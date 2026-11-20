/* The conversion plan: PFS3 layout, where data goes, the checks
 * before anything is written, and the plan's on-disk form.
 *
 * On disk ('F2PB'): a 16-byte header (magic, version, length, CRC32 of the
 * rest), then the model, all big-endian. Written and read through a 512-byte
 * buffer: the plan never needs a second copy in memory. */
#include <stdlib.h>
#include "f2p.h"

#define MAXNUMRESERVED (4096UL + 255UL * 1024 * 8)
#define MAXSMALLDISK (5UL * 253 * 253 * 32)
#define MAXDISKSIZE1K (104UL * 253 * 253 * 32)
#define MAXDISKSIZE2K (104UL * 509 * 509 * 32)
#define PLAN_MAGIC 0x46325042UL              /* 'F2PB' */
#define PLAN_VERSION 3

void pfs3_layout(u32 total, u32 *resblocksize, u32 *numreserved, u32 *end)
{
    u32 rbs = 1024, taken = 32, i;
    if (total > MAXSMALLDISK)
        rbs = total <= MAXDISKSIZE1K ? 1024 : total <= MAXDISKSIZE2K ? 2048 : 4096;
    for (i = 2048; i / 2 < total; i <<= 1) {   /* i = 2048 .. 2^31, as format.c */
        taken += taken * (i >= 512UL * 2048 ? 10 : 14) / 16;
        if (i == 0x80000000UL)
            break;
    }
    taken /= rbs / 1024;
    taken = (taken - 1 < MAXNUMRESERVED ? taken - 1 : MAXNUMRESERVED);
    *numreserved = (taken + 31) & ~31UL;
    *resblocksize = rbs;
    *end = (rbs / BS) * *numreserved + 2;
}

/* ------------------------------------------------------------------ names */
static const model *sort_m;

static int up(int c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) ? c - 32 : c;
}

static int namecmp(const void *a, const void *b)
{
    const entry *x = &sort_m->e[*(const u32 *)a], *y = &sort_m->e[*(const u32 *)b];
    const u8 *p = (const u8 *)sort_m->pool + x->name, *q = (const u8 *)sort_m->pool + y->name;
    u32 i;
    if (x->parent != y->parent)
        return x->parent < y->parent ? -1 : 1;
    for (i = 0;; i++) {
        int c = up(p[i]), d = up(q[i]);
        if (c != d || !c)
            return c - d;
    }
}

static int check_names(model *m)
{
    u32 *ix = f2p_alloc(sizeof(u32) * (m->nentries + 1)), i;
    int err = 0;
    if (!ix)
        return f2p_fail("out of memory");
    for (i = 0; i < m->nentries; i++)
        ix[i] = i;
    sort_m = m;
    qsort(ix, m->nentries, sizeof(u32), namecmp);
    for (i = 1; i < m->nentries && !err; i++)
        if (!namecmp(&ix[i - 1], &ix[i]))
            err = f2p_fail("'%s' and '%s' are the same name to PFS3", m->pool + m->e[ix[i - 1]].name, m->pool + m->e[ix[i]].name);
    f2p_free(ix);
    return err;
}

/* ------------------------------------------------------------------ plan */
static int add_px(model *m, u32 *max, entry *e, u32 k)
{
    if (e->npext && m->px[e->pext + e->npext - 1].start + m->px[e->pext + e->npext - 1].count == k) {
        m->px[e->pext + e->npext - 1].count++;
        return 0;
    }
    if (m->npext == *max) {
        u32 nmax = *max ? *max * 2 : 256;
        extent *q = f2p_alloc(sizeof(extent) * nmax);
        if (!q)
            return f2p_fail("out of memory");
        if (m->px) {
            memcpy(q, m->px, sizeof(extent) * m->npext);
            f2p_free(m->px);
        }
        m->px = q;
        *max = nmax;
    }
    m->px[m->npext].start = k;
    m->px[m->npext].count = 1;
    m->npext++;
    e->npext++;
    return 0;
}

static int movecmp(const void *a, const void *b)
{
    u32 x = ((const move *)a)->src, y = ((const move *)b)->src;
    return x < y ? -1 : x > y;
}

int plan_build(blockdev *d, model *m)
{
    u32 n = m->part_blocks, i, k, need = 0, inside = 0, cur, maxpx = 0, nfree = 0;
    int err;
    pfs3_layout(n, &m->resblocksize, &m->numreserved, &m->end);
    if (m->end >= n)
        return f2p_fail("partition too small for PFS3");
    if (check_names(m))
        return 1;
    u32 files = 0, nf = 0;
    for (i = 0; i < m->nentries; i++)
        files += m->e[i].kind == K_FILE;
    for (i = 0; i < m->nentries; i++) {
        entry *e = &m->e[i];
        if (e->kind == K_SOFT && e->size >= BS)
            return f2p_fail("soft link %s: path too long for PFS3", m->pool + e->name);
        if (e->kind == K_HARD && m->e[e->link].kind != K_FILE && m->e[e->link].kind != K_DIR)
            return f2p_fail("hard link %s points at a soft link: PFS3 has no such link", m->pool + e->name);
        if (e->kind == K_FILE) {
            e->crc = file_crc(d, m->part_start, m, e, &err);   /* reads and checks all data */
            if (err)
                return 1;
            need += (e->size + BS - 1) / BS;
        }
        if (e->kind == K_FILE) {
            nf++;
            f2p_progress_file("Checking files", nf, files, nf, files, m->pool + e->name);
        }
        if (f2p_break())
            return E_STOP;
    }
    for (k = m->end; k < n; k++)
        nfree += BM_GET(&m->freemap, k);
    cur = m->end;                               /* forward cursor over free blocks past the reserved area */
#define NEXTFREE() do { while (cur < n && !BM_GET(&m->freemap, cur)) cur++; } while (0)
    if (m->ofs) {
        if (nfree < need)
            return f2p_fail("OFS: %lu free blocks outside the PFS3 reserved area, the packed data needs %lu", UL(nfree), UL(need));
        for (i = 0; i < m->nentries; i++) {
            entry *e = &m->e[i];
            u32 c = (e->size + BS - 1) / BS;
            if (e->kind != K_FILE)
                continue;
            e->pext = m->npext;
            while (c--) {
                NEXTFREE();
                if (add_px(m, &maxpx, e, cur))
                    return 1;
                BM_CLR(&m->freemap, cur);           /* taken: not for the journal or the plan */
                cur++;
            }
        }
    } else {
        /* FFS: data blocks inside the reserved area move to the top free blocks */
        for (i = 0; i < m->nextents; i++)
            for (k = m->x[i].start; k < m->x[i].start + m->x[i].count; k++)
                inside += k >= 2 && k < m->end;
        if (nfree < inside)
            return f2p_fail("not enough free space to move %lu blocks out of the reserved area", UL(inside));
        m->mv = f2p_alloc(sizeof(move) * (inside + 1));
        if (!m->mv)
            return f2p_fail("out of memory");
        for (i = 0; i < m->nextents; i++)
            for (k = m->x[i].start; k < m->x[i].start + m->x[i].count; k++)
                if (k >= 2 && k < m->end)
                    m->mv[m->nmoves++].src = k;
        qsort(m->mv, m->nmoves, sizeof(move), movecmp);
        for (i = m->nmoves, k = n; i > 0;) {        /* the highest free blocks, ascending */
            while (!BM_GET(&m->freemap, k - 1))
                k--;
            k--;
            m->mv[--i].dst = k;
            BM_CLR(&m->freemap, k);
        }
    }
    for (i = 0; i < m->nentries; i++) {           /* soft links: one data block each */
        entry *e = &m->e[i];
        if (e->kind != K_SOFT)
            continue;
        NEXTFREE();
        if (cur >= n)
            return f2p_fail("no free blocks for the soft links");
        e->pext = m->npext;
        if (add_px(m, &maxpx, e, cur))
            return 1;
        BM_CLR(&m->freemap, cur);
        cur++;
    }
#undef NEXTFREE
    return 0;
}

/* ------------------------------------------------------------------ serialising */
typedef struct {
    blockdev *d;
    u32 at, pos, len;            /* next block to write, bytes so far */
    u32 crc;                     /* over everything after the 16-byte header */
    int write;                   /* 0: only count and CRC */
    u8 buf[BS];
    int err;
} sink;

static void put(sink *s, const u8 *p, u32 n)
{
    while (n--) {
        u8 c = *p++;
        if (s->pos >= 16)
            s->crc = crc32_update(s->crc, &c, 1);
        s->buf[s->pos % BS] = c;
        s->pos++;
        if (s->pos % BS == 0 && s->write && !s->err)
            s->err = s->d->write(s->d, s->at++, 1, s->buf);
    }
}

static void put32(sink *s, u32 v)
{
    u8 b[4];
    wr32(b, v);
    put(s, b, 4);
}

static void put_model(sink *s, const model *m, u32 len, u32 crc)
{
    const rdbfs_plan *r = &m->rdb;
    u32 i;
    static const u8 zero[4];
    put32(s, PLAN_MAGIC); put32(s, PLAN_VERSION); put32(s, len); put32(s, crc);
    put32(s, m->part_blocks); put32(s, m->part_start); put32(s, m->part_block); put32(s, m->orig_dostype);
    put32(s, m->dostype); put32(s, m->ofs); put32(s, m->resblocksize); put32(s, m->numreserved); put32(s, m->end);
    put32(s, m->spb);
    for (i = 0; i < 3; i++) { put32(s, m->rootdate[i]); put32(s, m->voldate[i]); put32(s, m->voldate_alt[i]); }
    put32(s, m->nentries); put32(s, m->nextents); put32(s, m->npext); put32(s, m->nmoves); put32(s, m->poolsize);
    put(s, (const u8 *)m->volname, 32);
    put(s, m->part_orig, BS);
    put(s, m->boot_orig[0], BS);
    put(s, m->boot_orig[1], BS);
    put32(s, m->nflag); put32(s, m->nbk); put32(s, m->nbkx);
    put32(s, r->existing); put32(s, r->fshd); put32(s, r->nlseg); put32(s, r->rescue[0]); put32(s, r->rescue[1]);
    put32(s, r->rdsk); put32(s, r->linkblk); put32(s, r->high); put32(s, r->rdbhi); put32(s, r->binlen); put32(s, r->nwrites);
    put(s, r->linkorig, BS);
    put(s, r->rdskorig, BS);
    for (i = 0; i < r->nlseg; i++)
        put32(s, r->lsegs[i]);
    if (r->binlen)
        put(s, r->bin, r->binlen);
    for (i = 0; i < m->nentries; i++) {
        const entry *e = &m->e[i];
        u8 t[4];
        put32(s, e->key); put32(s, e->protect); put32(s, e->days); put32(s, e->mins); put32(s, e->ticks);
        put32(s, e->size); put32(s, (u32)e->parent); put32(s, e->name); put32(s, e->comment); put32(s, e->link);
        put32(s, e->ext); put32(s, e->next); put32(s, e->pext); put32(s, e->npext); put32(s, e->next_link); put32(s, e->crc);
        t[0] = e->kind; t[1] = e->nlen; t[2] = e->clen; t[3] = e->llen;
        put(s, t, 4);
    }
    for (i = 0; i < m->nextents; i++) { put32(s, m->x[i].start); put32(s, m->x[i].count); }
    for (i = 0; i < m->npext; i++) { put32(s, m->px[i].start); put32(s, m->px[i].count); }
    for (i = 0; i < m->nmoves; i++) { put32(s, m->mv[i].src); put32(s, m->mv[i].dst); }
    for (i = 0; i < m->nbkx; i++) { put32(s, m->bkx[i].start); put32(s, m->bkx[i].count); }
    if (m->nflag)
        put(s, m->bkflag, (m->nflag + 7) / 8);
    if (m->poolsize)
        put(s, (const u8 *)m->pool, m->poolsize);
    put(s, zero, (4 - s->pos % 4) % 4);
}

u32 plan_size(const model *m)
{
    sink s;
    memset(&s, 0, sizeof(s));
    put_model(&s, m, 0, 0);
    return (s.pos + BS - 1) / BS * BS;
}

int plan_write(blockdev *d, u32 at, const model *m, u32 *crc)
{
    sink s;
    u32 len;
    memset(&s, 0, sizeof(s));
    put_model(&s, m, 0, 0);                    /* the length, then the CRC with the length in */
    len = s.pos;
    memset(&s, 0, sizeof(s));
    put_model(&s, m, len, 0);
    *crc = s.crc;
    memset(&s, 0, sizeof(s));
    s.d = d; s.at = at; s.write = 1;
    put_model(&s, m, len, *crc);
    if (s.pos % BS && !s.err) {
        memset(s.buf + s.pos % BS, 0, BS - s.pos % BS);
        s.err = d->write(d, s.at++, 1, s.buf);
    }
    return s.err ? f2p_fail("write error (plan)") : 0;
}

/* ------------------------------------------------------------------ reading */
typedef struct {
    blockdev *d;
    u32 at, pos, end, crc;
    u8 buf[BS];
    int err;
} src;

static void get(src *s, u8 *p, u32 n)
{
    while (n--) {
        if (s->pos % BS == 0 && !s->err) {
            if (s->pos >= s->end)
                s->err = 1;
            else
                s->err = s->d->read(s->d, s->at++, 1, s->buf);
        }
        *p = s->err ? 0 : s->buf[s->pos % BS];
        if (s->pos >= 16)
            s->crc = crc32_update(s->crc, p, 1);
        p++;
        s->pos++;
    }
}

static u32 get32(src *s)
{
    u8 b[4];
    get(s, b, 4);
    return rd32(b);
}

static void *alloc_n(u32 n, u32 size, int *err)
{
    void *p;
    if (!n)
        return NULL;
    p = f2p_alloc((size_t)n * size);
    if (!p)
        *err = 1;
    return p;
}

int plan_read(blockdev *d, u32 at, u32 nblocks, model *m)
{
    src s;
    rdbfs_plan *r = &m->rdb;
    u32 i, len, crc;
    int err = 0;
    memset(&s, 0, sizeof(s));
    memset(m, 0, sizeof(*m));
    s.d = d; s.at = at; s.end = nblocks * BS;
    if (get32(&s) != PLAN_MAGIC || get32(&s) != PLAN_VERSION)
        return f2p_fail("no plan here");
    len = get32(&s);
    crc = get32(&s);
    if (len > s.end)
        return f2p_fail("plan length wrong");
    m->part_blocks = get32(&s); m->part_start = get32(&s); m->part_block = get32(&s); m->orig_dostype = get32(&s);
    m->dostype = get32(&s); m->ofs = get32(&s); m->resblocksize = get32(&s); m->numreserved = get32(&s); m->end = get32(&s);
    m->spb = get32(&s);
    for (i = 0; i < 3; i++) { m->rootdate[i] = get32(&s); m->voldate[i] = get32(&s); m->voldate_alt[i] = get32(&s); }
    m->nentries = get32(&s); m->nextents = get32(&s); m->npext = get32(&s); m->nmoves = get32(&s); m->poolsize = get32(&s);
    get(&s, (u8 *)m->volname, 32);
    m->volname[31] = 0;
    get(&s, m->part_orig, BS);
    get(&s, m->boot_orig[0], BS);
    get(&s, m->boot_orig[1], BS);
    m->nflag = get32(&s); m->nbk = get32(&s); m->nbkx = get32(&s);
    r->existing = get32(&s); r->fshd = get32(&s); r->nlseg = get32(&s); r->rescue[0] = get32(&s); r->rescue[1] = get32(&s);
    r->rdsk = get32(&s); r->linkblk = get32(&s); r->high = get32(&s); r->rdbhi = get32(&s); r->binlen = get32(&s); r->nwrites = get32(&s);
    get(&s, r->linkorig, BS);
    get(&s, r->rdskorig, BS);
    if (s.err || m->nentries > len / 68 || r->nlseg > 4096 || r->binlen > len || m->nbkx > len / 8 || m->nflag / 8 > len) {
        model_free(m);
        return f2p_fail("plan is bad");
    }
    r->lsegs = alloc_n(r->nlseg, 4, &err);
    r->bin = alloc_n(r->binlen, 1, &err);
    m->e = alloc_n(m->nentries, sizeof(entry), &err);
    m->x = alloc_n(m->nextents, sizeof(extent), &err);
    m->px = alloc_n(m->npext, sizeof(extent), &err);
    m->mv = alloc_n(m->nmoves, sizeof(move), &err);
    m->bkx = alloc_n(m->nbkx, sizeof(extent), &err);
    m->bkflag = alloc_n((m->nflag + 7) / 8, 1, &err);
    m->pool = alloc_n(m->poolsize + 1, 1, &err);
    if (err) {
        model_free(m);
        return f2p_fail("out of memory reading the plan");
    }
    for (i = 0; i < r->nlseg; i++)
        r->lsegs[i] = get32(&s);
    if (r->binlen)
        get(&s, r->bin, r->binlen);
    for (i = 0; i < m->nentries; i++) {
        entry *e = &m->e[i];
        u8 t[4];
        e->key = get32(&s); e->protect = get32(&s); e->days = get32(&s); e->mins = get32(&s); e->ticks = get32(&s);
        e->size = get32(&s); e->parent = (s32)get32(&s); e->name = get32(&s); e->comment = get32(&s); e->link = get32(&s);
        e->ext = get32(&s); e->next = get32(&s); e->pext = get32(&s); e->npext = get32(&s); e->next_link = get32(&s); e->crc = get32(&s);
        get(&s, t, 4);
        e->kind = t[0]; e->nlen = t[1]; e->clen = t[2]; e->llen = t[3];
    }
    for (i = 0; i < m->nextents; i++) { m->x[i].start = get32(&s); m->x[i].count = get32(&s); }
    for (i = 0; i < m->npext; i++) { m->px[i].start = get32(&s); m->px[i].count = get32(&s); }
    for (i = 0; i < m->nmoves; i++) { m->mv[i].src = get32(&s); m->mv[i].dst = get32(&s); }
    for (i = 0; i < m->nbkx; i++) { m->bkx[i].start = get32(&s); m->bkx[i].count = get32(&s); }
    if (m->nflag)
        get(&s, m->bkflag, (m->nflag + 7) / 8);
    if (m->poolsize)
        get(&s, (u8 *)m->pool, m->poolsize);
    {
        u8 pad[4];
        get(&s, pad, (4 - s.pos % 4) % 4);
    }
    if (s.err || s.pos != len || s.crc != crc) {
        model_free(m);
        return f2p_fail("plan copy is damaged");
    }
    return 0;
}

/* a plan copy on disk is complete and intact: length and CRC, read through one block */
int plan_check(blockdev *d, u32 at, u32 nblocks)
{
    u8 b[BS];
    u32 len, crc, c = 0, pos, k;
    if (d->read(d, at, 1, b) || L(b, 0) != PLAN_MAGIC || L(b, 1) != PLAN_VERSION)
        return f2p_fail("plan copy does not read back");
    len = L(b, 2);
    crc = L(b, 3);
    if (len > nblocks * BS || len < 16)
        return f2p_fail("plan copy length wrong");
    c = crc32_update(c, b + 16, (len < BS ? len : BS) - 16);
    for (k = 1, pos = BS; pos < len; k++, pos += BS) {
        if (d->read(d, at + k, 1, b))
            return f2p_fail("read error (plan)");
        c = crc32_update(c, b, len - pos < BS ? len - pos : BS);
    }
    return c == crc ? 0 : f2p_fail("plan copy does not read back as written");
}
