/* RDB: RDSK/PART reading, checksums; pfs3aio into the RDB */
#include "f2p.h"

#define RESCUE_MAGIC 0x46325052UL       /* 'F2PR' */
#define RESCUE_SCAN 65536UL
#define LSEG_DATA (123 * 4)

int rdb_checksum(u8 *b)
{
    u32 n = L(b, 1), s = 0, i;
    if (n < 3 || n > 128)
        return 1;
    wr32(b + 8, 0);
    for (i = 0; i < n; i++)
        s += L(b, i);
    wr32(b + 8, (u32)(0 - s));
    return 0;
}

int rdb_valid(const u8 *b)
{
    u32 n = L(b, 1), s = 0, i;
    if (n < 3 || n > 128)
        return 0;
    for (i = 0; i < n; i++)
        s += L(b, i);
    return s == 0;
}

int rdb_find_rdsk(blockdev *d, u32 *blk, u8 *rdsk)
{
    u32 k;
    for (k = 0; k < 16 && k < d->nblocks; k++) {
        if (d->read(d, k, 1, rdsk))
            return 0;
        if (L(rdsk, 0) == 0x5244534bUL && rdb_valid(rdsk)) {     /* 'RDSK' */
            *blk = k;
            return 1;
        }
    }
    return 0;
}

int rdb_partitions(blockdev *d, partinfo *out, int max)
{
    u8 r[BS];
    u32 rblk;
    if (!rdb_find_rdsk(d, &rblk, r))
        return -1;
    return rdb_partitions_from(d, r, out, max);
}

int rdb_partitions_from(blockdev *d, const u8 *r, partinfo *out, int max)
{
    u8 p[BS];
    u32 nxt;
    int n = 0, guard = 0;
    nxt = L(r, 7);
    while (nxt != NOBLK && n < max && guard++ < 1000) {
        partinfo *q = &out[n++];
        memset(q, 0, sizeof(*q));
        q->block = nxt;
        if (nxt >= d->nblocks || d->read(d, nxt, 1, p) || L(p, 0) != 0x50415254UL || !rdb_valid(p)) {
            q->invalid = 1;                                   /* 'PART' */
            break;
        }
        {
            u32 surf = L(p, 32 + 3), bpt = L(p, 32 + 5), lo = L(p, 32 + 9), hi = L(p, 32 + 10);
            u32 nl = p[36] < 31 ? p[36] : 31;
            memcpy(q->name, p + 37, nl);
            q->name[nl] = 0;
            q->start = lo * surf * bpt;
            q->blocks = (hi - lo + 1) * surf * bpt;
            q->dostype = L(p, 32 + 16);
            q->spb = L(p, 32 + 4) ? L(p, 32 + 4) : 1;
            q->reserved = L(p, 32 + 6);
            q->prealloc = L(p, 32 + 7);
            if (L(p, 32 + 1) != 128)           /* SizeBlock: only 512-byte sectors */
                q->spb = 0;
        }
        nxt = L(p, 4);
    }
    return n;
}

/* ------------------------------------------------------------------ pfs3aio into the RDB */

typedef struct { u32 *blk; u32 n, max; } blist;

static int bl_add(blist *l, u32 k)
{
    u32 i;
    for (i = 0; i < l->n; i++)
        if (l->blk[i] == k)
            return f2p_fail("RDB block %lu used twice", UL(k));
    if (l->n == l->max)
        return f2p_fail("too many RDB blocks");
    l->blk[l->n++] = k;
    return 0;
}

static int bl_has(const blist *l, u32 k)
{
    u32 i;
    for (i = 0; i < l->n; i++)
        if (l->blk[i] == k)
            return 1;
    return 0;
}

/* walk one RDB chain, adding its blocks to used; *last = the last block, *lastb its contents */
static int chain(blockdev *d, u32 nxt, u32 magic, blist *used, u32 *last, u8 *lastb, blist *fshds)
{
    u8 b[BS];
    while (nxt != NOBLK) {
        if (nxt >= d->nblocks || d->read(d, nxt, 1, b))
            return f2p_fail("RDB pointer %lu out of range", UL(nxt));
        if (L(b, 0) != magic || !rdb_valid(b))
            return f2p_fail("RDB block %lu is not a valid %c%c%c%c", UL(nxt),
                            (int)(magic >> 24), (int)(magic >> 16) & 255, (int)(magic >> 8) & 255, (int)magic & 255);
        if (bl_add(used, nxt))
            return 1;
        if (fshds && bl_add(fshds, nxt))
            return 1;
        if (last)
            *last = nxt;
        if (lastb)
            memcpy(lastb, b, BS);
        nxt = L(b, 4);
    }
    return 0;
}

static int make_inner(blockdev *d, const u8 *bin, u32 binlen, u32 version, rdbfs_plan *r, u32 *usedbuf);

int rdbfs_make(blockdev *d, const u8 *bin, u32 binlen, u32 version, rdbfs_plan *r)
{
    u32 *usedbuf = f2p_alloc(sizeof(u32) * 4096);          /* not on the stack: small Amiga stacks */
    int e;
    if (!usedbuf)
        return f2p_fail("out of memory");
    e = make_inner(d, bin, binlen, version, r, usedbuf);
    f2p_free(usedbuf);
    return e;
}

static int make_inner(blockdev *d, const u8 *bin, u32 binlen, u32 version, rdbfs_plan *r, u32 *usedbuf)
{
    u8 rd[BS], b[BS];
    u32 rblk, lo, hi, limit, need, i, k, nfree, last = NOBLK;
    u32 fshdbuf[64], *freeblk;
    blist used = { NULL, 0, 4096 }, fshds = { fshdbuf, 0, 64 };
    used.blk = usedbuf;
    partinfo parts[32];
    int np;

    memset(r, 0, sizeof(*r));
    if (binlen % 4)
        return f2p_fail("filesystem binary length is not a multiple of 4");
    if (!rdb_find_rdsk(d, &rblk, rd))
        return f2p_fail("no valid RDSK block");
    if (bl_add(&used, rblk) || chain(d, L(rd, 7), 0x50415254UL, &used, NULL, NULL, NULL)
        || chain(d, L(rd, 8), 0x46534844UL, &used, &last, r->linkorig, &fshds))
        return 1;
    for (i = 0; i < fshds.n; i++) {               /* LSEG chains; a PFS\3 entry already there? */
        if (d->read(d, fshds.blk[i], 1, b))
            return f2p_fail("read error");
        if (L(b, 8) == DOSTYPE_PFS3) {
            r->existing = 1;
            r->fshd = fshds.blk[i];
            return 0;
        }
        if (chain(d, L(b, 18), 0x4c534547UL, &used, NULL, NULL, NULL))
            return 1;
    }
    if (chain(d, L(rd, 9), 0x4c534547UL, &used, NULL, NULL, NULL) || chain(d, L(rd, 6), 0x42414442UL, &used, NULL, NULL, NULL))
        return 1;
    np = rdb_partitions(d, parts, 32);
    lo = L(rd, 32);
    hi = L(rd, 33);
    limit = L(rd, 34) * L(rd, 36);             /* below LoCylinder and every partition */
    for (i = 0; i < (u32)np; i++)
        if (!parts[i].invalid && parts[i].start < limit)
            limit = parts[i].start;
    r->nlseg = (binlen + LSEG_DATA - 1) / LSEG_DATA;
    need = 1 + r->nlseg + 2;
    freeblk = f2p_alloc(need * sizeof(u32));
    r->lsegs = f2p_alloc(r->nlseg * sizeof(u32));
    r->bin = f2p_alloc(binlen);
    if (!freeblk || !r->lsegs || !r->bin)
        return f2p_fail("out of memory");
    nfree = 0;
    for (k = lo > 16 ? lo : 16; k <= hi && k < limit && nfree < need; k++)
        if (!bl_has(&used, k))
            freeblk[nfree++] = k;
    for (k = hi + 1 > 16 ? hi + 1 : 16; k < limit && nfree < need; k++) {   /* grow the range: all-zero blocks */
        if (bl_has(&used, k))
            continue;
        if (d->read(d, k, 1, b))
            return f2p_fail("read error");
        for (i = 0; i < BS && !b[i]; i++)
            ;
        if (i == BS)
            freeblk[nfree++] = k;
    }
    if (nfree < need) {
        f2p_free(freeblk);
        return f2p_fail("room for %lu RDB blocks before the first partition, pfs3aio needs %lu", UL(nfree), UL(need));
    }
    r->fshd = freeblk[0];
    for (i = 0; i < r->nlseg; i++)
        r->lsegs[i] = freeblk[1 + i];
    r->rescue[0] = freeblk[1 + r->nlseg];
    r->rescue[1] = freeblk[2 + r->nlseg];
    f2p_free(freeblk);
    r->rdsk = rblk;
    memcpy(r->rdskorig, rd, BS);
    r->linkblk = last;
    memcpy(r->bin, bin, binlen);
    r->binlen = binlen;
    r->nwrites = r->nlseg + 1 + (last != NOBLK) + 5;
    /* HighRDSKBlock = max(old, RDB blocks in use, FSHD, LSEGs);
       RDBBlocksHi = max(old, every new block incl. the rescue pair) */
    r->high = L(rd, 38);
    for (i = 0; i < used.n; i++)
        if (used.blk[i] > r->high)
            r->high = used.blk[i];
    r->rdbhi = hi;
    for (i = 0; i < need; i++) {
        u32 k2 = i == 0 ? r->fshd : i <= r->nlseg ? r->lsegs[i - 1] : r->rescue[i - 1 - r->nlseg];
        if (i <= r->nlseg && k2 > r->high)
            r->high = k2;
        if (k2 > r->rdbhi)
            r->rdbhi = k2;
    }
    return 0;
}

static void new_rdsk(const rdbfs_plan *r, u8 *out)
{
    memcpy(out, r->rdskorig, BS);
    if (r->linkblk == NOBLK)
        wr32(out + 32, r->fshd);                         /* FileSysHeaderList */
    wr32(out + 38 * 4, r->high);
    wr32(out + 33 * 4, r->rdbhi);
    rdb_checksum(out);
}

/* write order: LSEGs, FSHD, [link], rescue copy, rescue marker, RDSK, marker := 0, copy := 0 */
void rdbfs_marker(const rdbfs_plan *r, const u8 *rdsk, u8 *out)
{
    memset(out, 0, BS);
    wr32(out, RESCUE_MAGIC);
    wr32(out + 4, r->rdsk);
    wr32(out + 8, r->rescue[0]);
    wr32(out + 12, crc32(rdsk, BS));
    wr32(out + BS - 4, crc32(out, BS - 4));
}

void rdbfs_write_n(const rdbfs_plan *r, u32 i, u32 *blk, u8 *out)
{
    memset(out, 0, BS);
    if (i < r->nlseg) {
        u32 off = i * LSEG_DATA, n = r->binlen - off < LSEG_DATA ? r->binlen - off : LSEG_DATA;
        *blk = r->lsegs[i];
        wr32(out, 0x4c534547UL);
        wr32(out + 4, 5 + n / 4);
        wr32(out + 12, L(r->rdskorig, 3));
        wr32(out + 16, i + 1 < r->nlseg ? r->lsegs[i + 1] : NOBLK);
        memcpy(out + 20, r->bin + off, n);
        rdb_checksum(out);
        return;
    }
    i -= r->nlseg;
    if (i == 0) {
        *blk = r->fshd;
        wr32(out, 0x46534844UL);
        wr32(out + 4, 64);
        wr32(out + 12, L(r->rdskorig, 3));
        wr32(out + 16, NOBLK);
        wr32(out + 32, DOSTYPE_PFS3);
        wr32(out + 36, (19UL << 16) | 2);
        wr32(out + 40, 0x180);
        wr32(out + 72, r->lsegs[0]);
        wr32(out + 76, NOBLK);
        rdb_checksum(out);
        return;
    }
    i--;
    if (r->linkblk != NOBLK) {
        if (i == 0) {
            *blk = r->linkblk;
            memcpy(out, r->linkorig, BS);
            wr32(out + 16, r->fshd);
            rdb_checksum(out);
            return;
        }
        i--;
    }
    switch (i) {
    case 0:                                              /* rescue copy: the new RDSK */
        *blk = r->rescue[0];
        new_rdsk(r, out);
        return;
    case 1: {                                            /* rescue marker */
        u8 nr[BS];
        new_rdsk(r, nr);
        *blk = r->rescue[1];
        rdbfs_marker(r, nr, out);
        return;
    }
    case 2:
        *blk = r->rdsk;
        new_rdsk(r, out);
        return;
    case 3:
        *blk = r->rescue[1];                             /* zeros */
        return;
    default:
        *blk = r->rescue[0];
        return;
    }
}

int rdbfs_find_rescue(blockdev *d, u32 *rblk, u8 *rdsk)
{
    u8 b[BS];
    u32 k;
    for (k = 16; k < RESCUE_SCAN && k < d->nblocks; k++) {
        if (d->read(d, k, 1, b))
            return 0;
        if (L(b, 0) != RESCUE_MAGIC || L(b, 127) != crc32(b, BS - 4))
            continue;
        if (L(b, 1) >= 16 || L(b, 2) >= d->nblocks || d->read(d, L(b, 2), 1, rdsk))
            continue;
        if (crc32(rdsk, BS) == L(b, 3)) {
            *rblk = L(b, 1);
            return 1;
        }
    }
    return 0;
}
