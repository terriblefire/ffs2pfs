/* PFS3 metadata from the plan, as pfs3build.py builds it (pfs3aio 19.2 format).
 *
 * pfs3_begin() lays everything out: reserved block numbers, the anode table,
 * the directory block packing, the data bitmap. pfs3_block(i) then produces
 * write #i (one 512-byte sector) on demand, in the same order as the Python
 * builder: the rootblock cluster, then every reserved block in ascending order.
 * Nothing is kept per block, so BUILD can resume at any step. */
#include "f2p.h"

#define DBLKID 0x4442
#define ABLKID 0x4142
#define IBLKID 0x4942
#define BMBLKID 0x424D
#define BMIBLKID 0x4D49
#define DELDIRID 0x4444
#define EXTENSIONID 0x4558
#define SBLKID 0x5342
#define OPTIONS 0x77fUL                       /* harddisk, split anodes, dir extension, deldir, sizefield,
                                                 extension, datestamp, superdeldir, extroving, longfn */
#define MODE_SUPERINDEX 128
#define PFS2VERSION ((19UL << 16) | 2)
#define DATESTAMP 1
#define DELENTRY_PROT 5
#define FNSIZE 32
#define CMSIZE 80

typedef struct { u32 cs, blk, next; } anode;
typedef struct { s32 dir; u32 first, count; } dirblk;

struct pfs3b {
    const model *m;
    u32 n, rbs, rc, numreserved, end, supermode, anodesper, indexper, longsperbmb;
    u32 nroot, rblkcluster;
    u32 r_bmi, nbmi, r_bm, no_bmb, r_dd, r_dir, ndirblk, maxdirblk, r_sup, nsup, r_idx, nidx, r_ab, nab, next_res;
    bitmap used;
    u32 blocksfree, curranseqnr;
    anode *an;
    u32 nan, maxan;
    u32 *first_link;                       /* per entry: anode of its first hard link, 0 = none */
    u32 *child, *cstart;                   /* children of each directory (root at 0), in entry order */
    dirblk *db;
    u32 cache_r;                           /* reserved block in cache */
    u8 *cache;
};

static u32 res_sector(const pfs3b *b, u32 r) { return 2 + r * b->rc; }
static u32 head(u32 i) { return i + 6; }   /* the head anode index of entry i */

static u32 anodenr(const pfs3b *b, u32 i) { return ((i / b->anodesper) << 16) | (i % b->anodesper); }
static u32 anodeix(const pfs3b *b, u32 nr) { return (nr >> 16) * b->anodesper + (nr & 0xffff); }

static int new_anode(pfs3b *b, u32 *nr)
{
    if (b->nan == b->maxan) {
        u32 nmax = b->maxan * 2;
        anode *q = f2p_alloc(sizeof(anode) * nmax);
        if (!q)
            return f2p_fail("out of memory (anodes)");
        memcpy(q, b->an, sizeof(anode) * b->nan);
        f2p_free(b->an);
        b->an = q;
        b->maxan = nmax;
    }
    if (b->nan / b->anodesper > 0xffff)
        return f2p_fail("too many anodes");
    memset(&b->an[b->nan], 0, sizeof(anode));
    *nr = anodenr(b, b->nan++);
    return 0;
}

static void set_anode(pfs3b *b, u32 nr, u32 cs, u32 blk, u32 next)
{
    anode *a = &b->an[anodeix(b, nr)];
    a->cs = cs; a->blk = blk; a->next = next;
}

static u32 mapblock(const model *m, u32 k)
{
    u32 lo = 0, hi = m->nmoves;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (m->mv[mid].src < k) lo = mid + 1; else hi = mid;
    }
    return lo < m->nmoves && m->mv[lo].src == k ? m->mv[lo].dst : k;
}

/* the directory entry for e: size, and the bytes when out is given */
static u32 entry_bytes(const pfs3b *b, u32 i, u8 *out)
{
    const model *m = b->m;
    const entry *e = &m->e[i];
    u32 nlen = e->nlen > FNSIZE - 1 ? FNSIZE - 1 : e->nlen;
    u32 clen = e->clen > CMSIZE - 1 ? CMSIZE - 1 : e->clen;
    u32 base = (20 + nlen + clen) & ~1UL, link, prot = e->protect & 0xffffff00UL, words[11], nx = 0, flags = 0, j, size;
    u32 anode = anodenr(b, head(i)), fsize = 0;
    int typ;
    link = e->kind == K_HARD ? anodenr(b, head(e->link)) : b->first_link[i];
    memset(words, 0, sizeof(words));
    words[0] = link >> 16; words[1] = link & 0xffff; words[4] = prot >> 16; words[5] = prot & 0xffff;
    for (j = 0; j < 11; j++)
        if (words[j]) { nx++; flags |= 1UL << j; }
    size = base + 2 * nx + 2;
    if (!out)
        return size;
    switch (e->kind) {
    case K_DIR: typ = 2; break;
    case K_FILE: typ = -3; fsize = e->size; break;
    case K_SOFT: typ = 3; fsize = e->size; break;
    default:
        typ = m->e[e->link].kind == K_DIR ? 4 : -4;
        fsize = m->e[e->link].kind == K_FILE ? m->e[e->link].size : 0;
    }
    memset(out, 0, size);
    out[0] = (u8)size;
    out[1] = (u8)typ;
    wr32(out + 2, anode);
    wr32(out + 6, fsize);
    wr16(out + 10, (u16)e->days); wr16(out + 12, (u16)e->mins); wr16(out + 14, (u16)e->ticks);
    out[16] = (u8)(e->protect & 0xff);
    out[17] = (u8)nlen;
    memcpy(out + 18, m->pool + e->name, nlen);
    out[18 + nlen] = (u8)clen;
    memcpy(out + 19 + nlen, m->pool + e->comment, clen);
    {
        u32 p = base;
        for (j = 11; j-- > 0;)                     /* the highest field first, flags last */
            if (words[j]) { wr16(out + p, (u16)words[j]); p += 2; }
        wr16(out + p, (u16)flags);
    }
    return size;
}

static int dir_layout(pfs3b *b)
{
    const model *m = b->m;
    u32 space = b->rbs - 20, top = 0, depth = 0;
    struct { s32 dir; u32 pos; } *st;
    st = f2p_alloc(sizeof(*st) * 210);
    if (!st)
        return f2p_fail("out of memory");
    st[0].dir = -1; st[0].pos = NOBLK;
    while (1) {
        s32 dir = st[top].dir;
        u32 c0 = b->cstart[dir + 1], c1 = b->cstart[dir + 2], k;
        if (st[top].pos == NOBLK) {           /* entering: lay out this directory's blocks */
            u32 used = 0, first = c0, prev = 0, nb = 0, hd = dir < 0 ? 5 : anodenr(b, head((u32)dir));
            for (k = c0; k <= c1; k++) {
                u32 sz = k < c1 ? entry_bytes(b, b->child[k], NULL) : 0;
                if (k == c1 || used + sz + 1 > space) {
                    u32 nr, s;
                    {
                        if (b->ndirblk == b->maxdirblk) {
                            u32 nmax = b->maxdirblk * 2;
                            dirblk *q = f2p_alloc(sizeof(dirblk) * nmax);
                            if (!q) { f2p_free(st); return f2p_fail("out of memory"); }
                            memcpy(q, b->db, sizeof(dirblk) * b->ndirblk);
                            f2p_free(b->db);
                            b->db = q;
                            b->maxdirblk = nmax;
                        }
                        b->db[b->ndirblk].dir = dir;
                        b->db[b->ndirblk].first = first;
                        b->db[b->ndirblk].count = k - first;
                        s = res_sector(b, b->r_dir + b->ndirblk);
                        b->ndirblk++;
                        if (nb == 0)
                            nr = hd;
                        else if (new_anode(b, &nr)) { f2p_free(st); return 1; }
                        set_anode(b, nr, 1, s, 0);
                        if (nb)
                            b->an[anodeix(b, prev)].next = nr;
                        prev = nr;
                        nb++;
                    }
                    if (k == c1)
                        break;
                    first = k;
                    used = 0;
                }
                used += sz;
            }
            st[top].pos = c0;
        }
        /* descend into the next child directory */
        while (st[top].pos < c1 && m->e[b->child[st[top].pos]].kind != K_DIR)
            st[top].pos++;
        if (st[top].pos < c1) {
            u32 ch = b->child[st[top].pos++];
            if (++depth > 200 || top + 1 >= 210) { f2p_free(st); return f2p_fail("directories nested too deep"); }
            top++;
            st[top].dir = (s32)ch;
            st[top].pos = NOBLK;
            continue;
        }
        if (top == 0)
            break;
        top--;
        depth--;
    }
    f2p_free(st);
    return 0;
}

pfs3b *pfs3_begin(const model *m)
{
    pfs3b *b = f2p_alloc(sizeof(pfs3b));
    u32 i, k, nb1k, nbits, maxbmi;
    if (!b) { f2p_fail("out of memory"); return NULL; }
    b->m = m;
    b->n = m->part_blocks;
    b->rbs = m->resblocksize;
    b->rc = b->rbs / BS;
    b->numreserved = m->numreserved;
    b->end = m->end;
    b->supermode = b->n > 5UL * 253 * 253 * 32;
    b->anodesper = (b->rbs - 16) / 12;
    b->indexper = (b->rbs - 12) / 4;
    b->longsperbmb = b->rbs / 4 - 3;
    b->cache_r = NOBLK;
    nb1k = 1;
    for (i = 125; i < b->numreserved / 32; i += 256)
        nb1k++;
    b->nroot = (1024 * nb1k + b->rbs - 1) / b->rbs;
    b->rblkcluster = b->nroot * b->rc;
    b->cache = f2p_alloc(b->rbs > b->rblkcluster * BS ? b->rbs : b->rblkcluster * BS);
    if (!b->cache || bm_init(&b->used, b->n))
        goto fail;

    /* data in use: file data (moved, or the OFS copy) and soft link blocks */
    for (i = 0; i < m->nentries; i++) {
        const entry *e = &m->e[i];
        const extent *src = e->kind == K_FILE && !m->ofs ? m->x + e->ext : m->px + e->pext;
        u32 nsrc = e->kind == K_FILE && !m->ofs ? e->next : e->npext, j;
        if (e->kind != K_FILE && e->kind != K_SOFT)
            continue;
        for (j = 0; j < nsrc; j++)
            for (k = src[j].start; k < src[j].start + src[j].count; k++) {
                u32 t = e->kind == K_FILE && !m->ofs ? mapblock(m, k) : k;
                if (t < b->end || t >= b->n) { f2p_fail("%s: data block %lu outside the data area", m->pool + e->name, UL(t)); goto fail; }
                if (BM_GET(&b->used, t)) { f2p_fail("data block %lu used twice", UL(t)); goto fail; }
                BM_SET(&b->used, t);
                b->blocksfree++;                 /* counts used blocks for now */
            }
    }
    nbits = b->n - b->end;
    b->blocksfree = nbits - b->blocksfree;
    b->no_bmb = ((nbits + 31) / 32 + b->longsperbmb - 1) / b->longsperbmb;
    maxbmi = (b->supermode ? 103 : 4) + 1;
    if (b->no_bmb > maxbmi * b->indexper) { f2p_fail("partition too large for PFS3"); goto fail; }
    b->nbmi = (b->no_bmb + b->indexper - 1) / b->indexper;
    b->r_bmi = b->nroot + 1;                   /* nroot: the root block extension */
    b->r_bm = b->r_bmi + b->nbmi;
    b->r_dd = b->r_bm + b->no_bmb;
    b->r_dir = b->r_dd + 2;

    /* anodes: 0..4 reserved, 5 root, a head for every entry, then chains */
    b->maxan = 6 + m->nentries + 64;
    b->an = f2p_alloc(sizeof(anode) * b->maxan);
    b->first_link = f2p_alloc(sizeof(u32) * (m->nentries + 1));
    b->child = f2p_alloc(sizeof(u32) * (m->nentries + 1));
    b->cstart = f2p_alloc(sizeof(u32) * (m->nentries + 3));
    b->maxdirblk = 64;
    b->db = f2p_alloc(sizeof(dirblk) * b->maxdirblk);
    if (!b->an || !b->first_link || !b->child || !b->cstart || !b->db) { f2p_fail("out of memory"); goto fail; }
    for (i = 0; i < 5; i++)
        b->an[i].blk = 0xffffffffUL;
    b->nan = 6 + m->nentries;
    for (i = 0; i < m->nentries; i++) {
        const entry *e = &m->e[i];
        if (e->kind == K_SOFT)
            set_anode(b, anodenr(b, head(i)), 1, m->px[e->pext].start, 0);
    }
    /* hard links: node = (object's directory, link's directory, next link); chains in entry order */
    {
        u32 *last = f2p_alloc(sizeof(u32) * (m->nentries + 1));
        if (!last) { f2p_fail("out of memory"); goto fail; }
        for (i = 0; i < m->nentries; i++) {
            const entry *e = &m->e[i];
            if (e->kind != K_HARD)
                continue;
            {
                s32 tp = m->e[e->link].parent, lp = e->parent;
                u32 tdir = tp < 0 ? 5 : anodenr(b, head((u32)tp)), ldir = lp < 0 ? 5 : anodenr(b, head((u32)lp));
                u32 me = anodenr(b, head(i));
                set_anode(b, me, tdir, ldir, 0);
                if (!b->first_link[e->link])
                    b->first_link[e->link] = me;
                else
                    b->an[anodeix(b, last[e->link])].next = me;
                last[e->link] = me;
            }
        }
        f2p_free(last);
    }
    /* file chains: one anode per run of blocks (after the moves, or the OFS copy) */
    for (i = 0; i < m->nentries; i++) {
        const entry *e = &m->e[i];
        const extent *src = m->ofs ? m->px + e->pext : m->x + e->ext;
        u32 nsrc = m->ofs ? e->npext : e->next, j, prev = 0, runs = 0, rs = 0, rn = 0;
        if (e->kind != K_FILE)
            continue;
        for (j = 0; j <= nsrc; j++) {
            u32 kk = j < nsrc ? src[j].start : 0, kend = j < nsrc ? src[j].start + src[j].count : 1;
            for (; kk < kend; kk++) {
                u32 t = j == nsrc ? NOBLK : m->ofs ? kk : mapblock(m, kk);
                if (rn && t == rs + rn) {
                    rn++;
                    continue;
                }
                if (rn) {                          /* close the run [rs, rs + rn) */
                    u32 nr2 = anodenr(b, head(i));
                    if (runs) {
                        if (new_anode(b, &nr2))
                            goto fail;
                        b->an[anodeix(b, prev)].next = nr2;
                    }
                    set_anode(b, nr2, rn, rs, 0);
                    prev = nr2;
                    runs++;
                }
                rs = t;
                rn = 1;
            }
        }
        if (!runs)
            set_anode(b, anodenr(b, head(i)), 0, 0xffffffffUL, 0);
    }
    /* children lists, then the directory blocks (they take anodes too) */
    for (i = 0; i < m->nentries; i++)
        b->cstart[m->e[i].parent + 2]++;
    for (i = 1; i < m->nentries + 3; i++)
        b->cstart[i] += b->cstart[i - 1];
    {
        u32 *fill = f2p_alloc(sizeof(u32) * (m->nentries + 3));
        if (!fill) { f2p_fail("out of memory"); goto fail; }
        memcpy(fill, b->cstart, sizeof(u32) * (m->nentries + 3));
        for (i = 0; i < m->nentries; i++)
            b->child[fill[m->e[i].parent + 1]++] = i;
        f2p_free(fill);
    }
    if (dir_layout(b))
        goto fail;
    /* anode blocks and their index */
    b->nab = (b->nan + b->anodesper - 1) / b->anodesper;
    b->nidx = (b->nab + b->indexper - 1) / b->indexper;
    b->nsup = b->supermode ? (b->nidx + b->indexper - 1) / b->indexper : 0;
    if ((b->supermode && b->nsup > 16) || (!b->supermode && b->nidx > 99)) { f2p_fail("too many anodes"); goto fail; }
    b->r_sup = b->r_dir + b->ndirblk;
    b->r_idx = b->r_sup + b->nsup;
    b->r_ab = b->r_idx + b->nidx;
    b->next_res = b->r_ab + b->nab;
    b->curranseqnr = b->nab - 1;
    if (b->next_res > b->numreserved) { f2p_fail("reserved area full (%lu blocks)", UL(b->numreserved)); goto fail; }
    return b;
fail:
    pfs3_end(b);
    return NULL;
}

u32 pfs3_nwrites(const pfs3b *b)
{
    return b->rblkcluster + (b->next_res - b->nroot) * b->rc;
}

u32 pfs3_sector(const pfs3b *b, u32 i)
{
    if (i < b->rblkcluster)
        return 2 + i;
    i -= b->rblkcluster;
    return res_sector(b, b->nroot + i / b->rc) + i % b->rc;
}

static void hdr(u8 *p, u16 id, u32 seq)
{
    wr16(p, id); wr16(p + 2, 0); wr32(p + 4, DATESTAMP); wr32(p + 8, seq);
}

static void rootcluster(pfs3b *b, u8 *r)
{
    const model *m = b->m;
    u32 i, j, nl = 0;
    memset(r, 0, b->rblkcluster * BS);
    while (nl < 30 && m->volname[nl])
        nl++;
    wr32(r, ID_PFS1);
    wr32(r + 4, OPTIONS | (b->supermode ? MODE_SUPERINDEX : 0));
    wr32(r + 8, DATESTAMP);
    wr16(r + 12, (u16)m->voldate[0]); wr16(r + 14, (u16)m->voldate[1]); wr16(r + 16, (u16)m->voldate[2]);
    wr16(r + 18, 0xf0);
    r[20] = (u8)nl;
    memcpy(r + 21, m->volname, nl);
    wr32(r + 52, b->end - 1);
    wr32(r + 56, 2);
    wr32(r + 60, b->numreserved - b->next_res);
    wr16(r + 64, (u16)b->rbs);
    wr16(r + 66, (u16)b->rblkcluster);
    wr32(r + 68, b->blocksfree);
    wr32(r + 72, (b->n - b->end) / 20);
    wr32(r + 84, b->n);
    wr32(r + 88, res_sector(b, b->nroot));
    for (i = 0; i < b->nbmi; i++)
        wr32(r + 96 + 4 * i, res_sector(b, b->r_bmi + i));
    if (!b->supermode)
        for (i = 0; i < b->nidx; i++)
            wr32(r + 116 + 4 * i, res_sector(b, b->r_idx + i));
    hdr(r + 512, BMBLKID, 0);
    for (i = 0; i < b->numreserved / 32; i++) {
        u32 v = 0;
        for (j = 0; j < 32; j++)
            if (i * 32 + j >= b->next_res)
                v |= 0x80000000UL >> j;
        wr32(r + 524 + 4 * i, v);
    }
}

static void resblock(pfs3b *b, u32 r, u8 *p)
{
    const model *m = b->m;
    u32 i, j;
    memset(p, 0, b->rbs);
    if (r == b->nroot) {                                   /* root block extension */
        wr16(p, EXTENSIONID);
        wr32(p + 8, DATESTAMP);
        wr32(p + 12, PFS2VERSION);
        for (i = 0; i < 3; i++) {
            wr16(p + 16 + 2 * i, (u16)m->rootdate[i]);
            wr16(p + 22 + 2 * i, (u16)m->voldate_alt[i]);
        }
        wr32(p + 44, b->next_res);
        wr16(p + 50, (u16)b->curranseqnr);
        wr16(p + 54, 2);
        wr16(p + 56, FNSIZE);
        for (i = 0; i < b->nsup; i++)
            wr32(p + 64 + 4 * i, res_sector(b, b->r_sup + i));
        for (i = 0; i < 2; i++)
            wr32(p + 144 + 4 * i, res_sector(b, b->r_dd + i));
    } else if (r < b->r_bm) {                              /* bitmap index */
        u32 n = r - b->r_bmi;
        hdr(p, BMIBLKID, n);
        for (i = 0; i < b->indexper && n * b->indexper + i < b->no_bmb; i++)
            wr32(p + 12 + 4 * i, res_sector(b, b->r_bm + n * b->indexper + i));
    } else if (r < b->r_dd) {                              /* bitmap: 1 = free */
        u32 seq = r - b->r_bm, base = seq * b->longsperbmb * 32, nbits = b->n - b->end;
        hdr(p, BMBLKID, seq);
        for (i = 0; i < b->longsperbmb; i++) {
            u32 v = 0xffffffffUL;
            for (j = 0; j < 32; j++) {
                u32 bit = base + i * 32 + j;
                if (bit < nbits && BM_GET(&b->used, b->end + bit))
                    v &= ~(0x80000000UL >> j);
            }
            wr32(p + 12 + 4 * i, v);
        }
    } else if (r < b->r_dir) {                             /* deldir */
        hdr(p, DELDIRID, r - b->r_dd);
        wr32(p + 22, DELENTRY_PROT);
        wr16(p + 26, (u16)m->voldate[0]); wr16(p + 28, (u16)m->voldate[1]); wr16(p + 30, (u16)m->voldate[2]);
    } else if (r < b->r_sup) {                             /* directory block */
        const dirblk *d = &b->db[r - b->r_dir];
        u32 off = 20;
        s32 dir = d->dir;
        u32 hd = dir < 0 ? 5 : anodenr(b, head((u32)dir));
        s32 pp = dir < 0 ? -2 : m->e[dir].parent;
        wr16(p, DBLKID);
        wr32(p + 4, DATESTAMP);
        wr32(p + 12, hd);
        wr32(p + 16, pp == -2 ? 0 : pp < 0 ? 5 : anodenr(b, head((u32)pp)));
        for (i = 0; i < d->count; i++)
            off += entry_bytes(b, b->child[d->first + i], p + off);
    } else if (r < b->r_idx) {                             /* super index */
        u32 n = r - b->r_sup;
        hdr(p, SBLKID, n);
        for (i = 0; i < b->indexper && n * b->indexper + i < b->nidx; i++)
            wr32(p + 12 + 4 * i, res_sector(b, b->r_idx + n * b->indexper + i));
    } else if (r < b->r_ab) {                              /* anode index */
        u32 n = r - b->r_idx;
        hdr(p, IBLKID, n);
        for (i = 0; i < b->indexper && n * b->indexper + i < b->nab; i++)
            wr32(p + 12 + 4 * i, res_sector(b, b->r_ab + n * b->indexper + i));
    } else {                                               /* anode block */
        u32 seq = r - b->r_ab;
        hdr(p, ABLKID, seq);
        for (i = 0; i < b->anodesper && seq * b->anodesper + i < b->nan; i++) {
            const anode *a = &b->an[seq * b->anodesper + i];
            wr32(p + 16 + 12 * i, a->cs);
            wr32(p + 20 + 12 * i, a->blk);
            wr32(p + 24 + 12 * i, a->next);
        }
    }
}

int pfs3_block(pfs3b *b, u32 i, u32 *sector, u8 *out)
{
    if (i < b->rblkcluster) {
        if (b->cache_r != NOBLK - 1) {
            rootcluster(b, b->cache);
            b->cache_r = NOBLK - 1;
        }
        *sector = 2 + i;
        memcpy(out, b->cache + i * BS, BS);
        return 0;
    }
    i -= b->rblkcluster;
    {
        u32 r = b->nroot + i / b->rc;
        if (b->cache_r != r) {
            resblock(b, r, b->cache);
            b->cache_r = r;
        }
        *sector = res_sector(b, r) + i % b->rc;
        memcpy(out, b->cache + (i % b->rc) * BS, BS);
    }
    return 0;
}

void pfs3_end(pfs3b *b)
{
    if (!b)
        return;
    bm_free(&b->used);
    if (b->an) f2p_free(b->an);
    if (b->first_link) f2p_free(b->first_link);
    if (b->child) f2p_free(b->child);
    if (b->cstart) f2p_free(b->cstart);
    if (b->db) f2p_free(b->db);
    if (b->cache) f2p_free(b->cache);
    f2p_free(b);
}

void pfs3_boot0(u8 *b)
{
    memset(b, 0, BS);
    wr32(b, ID_PFS1);
}

int softlink_block(const model *m, const entry *e, u32 *blk, u8 *out)
{
    memset(out, 0, BS);
    *blk = m->px[e->pext].start;
    memcpy(out, m->pool + e->link, e->size);
    return 0;
}
