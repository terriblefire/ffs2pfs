/* Crash-safe OFS/FFS -> PFS3 conversion: anchors, journal, phases, resume
 * Every write may be the last or be torn; the state is always found again from
 * the disk alone.
 *
 *   START     plan A, plan B, journal A, journal B, anchor B (block 1)
 *   LOCK      anchor A over the boot block, RDB DosType := 'F2P\0'
 *   RELOCATE  FFS: data blocks out of the reserved area / OFS: every file packed
 *             into free blocks; soft link paths into their blocks
 *   BACKUP    the rollback copy: every block BUILD and RDBFS will overwrite, into free blocks
 *   BUILD     PFS3 metadata into the reserved area
 *   RDBFS     pfs3aio into the RDB
 *   COMMIT    block 1 := done mark, block 0 := PFS3 boot, RDB DosType := PFS\3
 *             (the point of no return: PFS3 may mount it from here), block 1 := zeros
 *   ROLLBACK  on an error, or when asked, up to the point of no return: the rollback
 *             copy back, the RDB as it was, RDB DosType as it was, block 1 := undo mark,
 *             blocks 0 and 1 as they were. Once begun it always runs to the end.
 * Blocks 0 and 1 always hold a mark (anchor, done, undo) that leads to the plan,
 * until the volume is FFS or PFS3 again. Each phase ends with a read-back. */
#include "f2p.h"

#define ANCHOR 0x46325041UL      /* 'F2PA' */
#define JOURNAL 0x4632504aUL     /* 'F2PJ' */
#define DONEMAGIC 0x46325044UL   /* 'F2PD' */
#define UNDOMAGIC 0x46325055UL   /* 'F2PU' */
#define CHECKPOINT 64
enum { PH_RELOCATE = 1, PH_BACKUP, PH_BUILD, PH_RDBFS, PH_COMMIT, PH_ROLLBACK };
#define OFS_DATA 488

typedef struct { u32 version, part_start, part_blocks, planA, planB, plan_blocks, jA, jB, partblock, orig_dostype; } anchor;

typedef struct {
    blockdev *d;
    partinfo p;
    u32 m0, m1;                  /* the marks in blocks 0 / 1 (ANCHOR, DONEMAGIC, UNDOMAGIC), 0: none */
    anchor A0, A1;               /* what they say */
    u8 b0[BS], b1[BS];
    model m;
    anchor a;
    u32 seq, plan_crc;
    struct batch *wb;            /* writes not yet on the disk: flushed before every checkpoint */
} conv;

static void crcblock(u8 *b, u32 magic)
{
    wr32(b, magic);
    wr32(b + BS - 4, crc32(b, BS - 4));
}

static int checkcrc(const u8 *b, u32 magic)
{
    return L(b, 0) == magic && L(b, 127) == crc32(b, BS - 4);
}

/* a mark: the anchor fields, under one of three magics */
static void make_mark(const anchor *a, u32 magic, u8 *b)
{
    memset(b, 0, BS);
    wr32(b + 4, a->version); wr32(b + 8, a->part_start); wr32(b + 12, a->part_blocks); wr32(b + 16, a->planA);
    wr32(b + 20, a->planB); wr32(b + 24, a->plan_blocks); wr32(b + 28, a->jA); wr32(b + 32, a->jB);
    wr32(b + 36, a->partblock); wr32(b + 40, a->orig_dostype);
    crcblock(b, magic);
}

static void make_anchor(const anchor *a, u8 *b)
{
    make_mark(a, ANCHOR, b);
}

static u32 read_mark(const u8 *b, anchor *a)
{
    u32 magic = L(b, 0);
    if ((magic != ANCHOR && magic != DONEMAGIC && magic != UNDOMAGIC) || !checkcrc(b, magic))
        return 0;
    a->version = L(b, 1); a->part_start = L(b, 2); a->part_blocks = L(b, 3); a->planA = L(b, 4);
    a->planB = L(b, 5); a->plan_blocks = L(b, 6); a->jA = L(b, 7); a->jB = L(b, 8);
    a->partblock = L(b, 9); a->orig_dostype = L(b, 10);
    return magic;
}

static int is_dos_boot(const u8 *b)
{
    return b[0] == 'D' && b[1] == 'O' && b[2] == 'S' && b[3] <= 5;
}

/* ------------------------------------------------------------------ I/O on the partition */
static int pread(conv *c, u32 k, u8 *b)
{
    return c->d->read(c->d, c->p.start + k, 1, b) ? f2p_fail("read error at block %lu", UL(k)) : 0;
}

static int pwrite(conv *c, u32 k, const u8 *b)
{
    return c->d->write(c->d, c->p.start + k, 1, b) ? f2p_fail("write error at block %lu", UL(k)) : 0;
}

/* write, then read back; rewrite if it differs (a few tries) */
static int readback(conv *c, u32 k, const u8 *b, int absolute)
{
    u8 t[BS];
    int i;
    u32 blk = absolute ? k : c->p.start + k;
    for (i = 0; i < 4; i++) {
        if (c->d->read(c->d, blk, 1, t))
            return f2p_fail("read error at block %lu", UL(blk));
        if (!memcmp(t, b, BS))
            return 0;
        if (i == 0)
            f2p_log("read-back: block %lu differs, rewriting\n", UL(blk));
        if (c->d->write(c->d, blk, 1, b))
            return f2p_fail("write error at block %lu", UL(blk));
    }
    return f2p_fail("block %lu does not read back as written", UL(blk));
}

/* write barrier: what came before is on the medium before anything after it is written */
static int flush(conv *c)
{
    return c->d->flush && c->d->flush(c->d) ? f2p_fail("flush failed") : 0;
}

/* ------------------------------------------------------------------ batched I/O
 * Runs of consecutive sectors go to the disk in one transfer (up to BATCH sectors).
 * A write batch is flushed before every checkpoint, so the journal never claims
 * more than is on the disk. A check batch reads its run back in one transfer and
 * compares; a sector that differs is rewritten on its own. */
#define BATCH 128
typedef struct batch {
    conv *c;
    u32 start, n;
    int check;
    u8 *buf, *tmp;
} batch;

static int b_init(batch *w, conv *c, int check)
{
    memset(w, 0, sizeof(*w));
    w->c = c;
    w->check = check;
    w->buf = f2p_alloc(BATCH * BS);
    w->tmp = check ? f2p_alloc(BATCH * BS) : NULL;
    return !w->buf || (check && !w->tmp) ? f2p_fail("out of memory") : 0;
}

static void b_free(batch *w)
{
    if (w->buf) f2p_free(w->buf);
    if (w->tmp) f2p_free(w->tmp);
    w->buf = w->tmp = NULL;
}

static int b_flush(batch *w)
{
    conv *c = w->c;
    u32 i;
    if (!w->n)
        return 0;
    if (!w->check) {
        if (c->d->write(c->d, c->p.start + w->start, w->n, w->buf)) {
            w->n = 0;
            return f2p_fail("write error at block %lu", UL(w->start));
        }
    } else {
        if (c->d->read(c->d, c->p.start + w->start, w->n, w->tmp)) {
            w->n = 0;
            return f2p_fail("read error at block %lu", UL(w->start));
        }
        for (i = 0; i < w->n; i++)
            if (memcmp(w->tmp + i * BS, w->buf + i * BS, BS) && readback(c, w->start + i, w->buf + i * BS, 0)) {
                w->n = 0;
                return 1;
            }
    }
    w->n = 0;
    return 0;
}

static int b_add(batch *w, u32 k, const u8 *data)
{
    if (w->n && (k != w->start + w->n || w->n == BATCH) && b_flush(w))
        return 1;
    if (!w->n)
        w->start = k;
    memcpy(w->buf + w->n * BS, data, BS);
    w->n++;
    return 0;
}

/* read-ahead: sector k, reading up to `ahead` sectors from k when it is not cached */
typedef struct { conv *c; u32 start, n; u8 *buf; } rcache;

static const u8 *rc_getn(rcache *r, u32 k, u32 need, u32 ahead)
{
    conv *c = r->c;
    if (k >= r->start && k + need <= r->start + r->n)
        return r->buf + (k - r->start) * BS;
    if (ahead > BATCH) ahead = BATCH;
    if (ahead < need) ahead = need;
    if (k + ahead > c->p.blocks) ahead = c->p.blocks - k;
    r->n = 0;
    if (c->d->read(c->d, c->p.start + k, ahead, r->buf)) {
        f2p_fail("read error at block %lu", UL(k));
        return NULL;
    }
    r->start = k;
    r->n = ahead;
    return r->buf;
}

static const u8 *rc_get(rcache *r, u32 k, u32 ahead)
{
    return rc_getn(r, k, 1, ahead);
}

static int checkpoint(conv *c, u32 phase, u32 nxt)
{
    if (c->wb && b_flush(c->wb))
        return 1;
    u8 b[BS];
    c->seq++;
    memset(b, 0, BS);
    wr32(b + 4, c->seq); wr32(b + 8, phase); wr32(b + 12, nxt); wr32(b + 16, c->plan_crc);
    crcblock(b, JOURNAL);
    return flush(c) || pwrite(c, c->seq % 2 ? c->a.jA : c->a.jB, b) || flush(c);
}

static int set_part(conv *c, u32 dostype)
{
    u8 b[BS];
    memcpy(b, c->m.part_orig, BS);
    wr32(b + (32 + 16) * 4, dostype);
    if (dostype == DOSTYPE_PFS3)
        wr32(b + (32 + 4) * 4, 1);                /* PFS3 here: 512-byte blocks (SectorPerBlock 1) */
    rdb_checksum(b);
    if (c->d->write(c->d, c->m.part_block, 1, b))
        return f2p_fail("write error (RDB)");
    c->p.dostype = dostype;
    c->p.invalid = 0;
    return 0;
}

/* ------------------------------------------------------------------ OFS data as a byte stream */
typedef struct {
    conv *c;
    const entry *e;
    u32 seq, left, xi, xk;       /* next OFS block: extent index, block within it */
    const u8 *b;                 /* the current OFS block (spb sectors), in the cache */
    u32 have, pos;               /* data in b */
    u32 spb, data;               /* sectors per block, data bytes per block */
    rcache *rc;                  /* read-ahead along the extent */
} ofsstream;

static int os_block(ofsstream *s)
{
    const model *m = &s->c->m;
    const extent *x;
    u32 k, n;
    if (s->xi >= s->e->next)
        return f2p_fail("%s: data ends early", m->pool + s->e->name);
    x = &m->x[s->e->ext + s->xi];
    k = x->start + s->xk;
    {
        u32 left_in_extent = x->count - s->xk;
        if (++s->xk == x->count) { s->xi++; s->xk = 0; }
        if (!(s->b = rc_getn(s->rc, k * s->spb, s->spb, left_in_extent * s->spb)))
            return 1;
    }
    s->seq++;
    n = s->left < s->data ? s->left : s->data;
    {
        u32 sum = 0, i;
        for (i = 0; i < s->spb * 128; i++)
            sum += L(s->b, i);
        if (L(s->b, 0) != 8 || L(s->b, 1) != s->e->key || L(s->b, 2) != s->seq || sum || L(s->b, 3) != n)
            return f2p_fail("%s: OFS data block %lu (#%lu) is bad", m->pool + s->e->name, UL(k), UL(s->seq));
    }
    s->left -= n;
    s->have = n;
    s->pos = 0;
    return 0;
}

/* start at byte offset off of the file */
static int os_open(ofsstream *s, conv *c, const entry *e, u32 off, rcache *rc)
{
    u32 skip, i;
    memset(s, 0, sizeof(*s));
    s->c = c; s->e = e; s->left = e->size; s->rc = rc;
    s->spb = c->m.spb ? c->m.spb : 1;
    s->data = s->spb * BS - 24;
    skip = off / s->data;
    for (i = 0; i < skip; i++) {                  /* step over whole OFS blocks without reading them */
        const extent *x = &c->m.x[e->ext + s->xi];
        if (++s->xk == x->count) { s->xi++; s->xk = 0; }
        s->seq++;
        s->left -= s->data;
    }
    if (off % s->data) {
        if (os_block(s))
            return 1;
        s->pos = off % s->data;
    }
    return 0;
}

static int os_read(ofsstream *s, u8 *out, u32 n)
{
    while (n) {
        u32 t;
        if (s->pos == s->have && os_block(s))
            return 1;
        t = s->have - s->pos < n ? s->have - s->pos : n;
        memcpy(out, s->b + 24 + s->pos, t);       /* OFS data follows a 24-byte header */
        s->pos += t; out += t; n -= t;
    }
    return 0;
}

/* ------------------------------------------------------------------ phases */
static int relocate(conv *c, u32 nxt)
{
    model *m = &c->m;
    batch w, chk;
    rcache rc;
    u32 i;
    int r = 1;
    rc.c = c; rc.n = 0; rc.buf = f2p_alloc(BATCH * BS);
    if (!rc.buf || b_init(&w, c, 0) || b_init(&chk, c, 1))
        goto out;
    c->wb = &w;
    for (i = nxt; i < m->nmoves; i++) {
        const u8 *src = rc_get(&rc, m->mv[i].src, m->nmoves - i);
        if (!src || b_add(&w, m->mv[i].dst, src))
            goto out;
        f2p_progress("Moving data", i + 1, m->nmoves, NULL);
        if ((i + 1) % CHECKPOINT == 0 && checkpoint(c, PH_RELOCATE, i + 1))
            goto out;
        if (f2p_break()) {
            r = checkpoint(c, PH_RELOCATE, i + 1) ? 1 : E_STOP;
            goto out;
        }
    }
    if (b_flush(&w))
        goto out;
    c->wb = NULL;
    rc.n = 0;
    for (i = 0; i < m->nmoves; i++) {
        const u8 *src = rc_get(&rc, m->mv[i].src, m->nmoves - i);
        if (!src || b_add(&chk, m->mv[i].dst, src))
            goto out;
        f2p_progress("Checking moved data", i + 1, m->nmoves, NULL);
    }
    r = b_flush(&chk);
out:
    c->wb = NULL;
    b_free(&w); b_free(&chk);
    if (rc.buf) f2p_free(rc.buf);
    return r;
}

/* OFS: every file packed into its PFS3 blocks (pass 0), then all of it read back (pass 1).
   ob holds one OFS block. */
static int copy_files(conv *c, u32 nxt, rcache *rc, batch *w, batch *chk)
{
    model *m = &c->m;
    u8 b[BS];
    u32 i, base = 0, pass, total = 0, files = 0, nf;
    for (i = 0; i < m->nentries; i++)
        if (m->e[i].kind == K_FILE) {
            total += (m->e[i].size + BS - 1) / BS;
            files++;
        }
    for (pass = 0; pass < 2; pass++) {            /* 0: copy from nxt on, 1: read-back everything */
        base = 0;
        nf = 0;
        for (i = 0; i < m->nentries; i++) {
            const entry *e = &m->e[i];
            u32 nb = (e->size + BS - 1) / BS, j, xj = 0, xk = 0, from;
            ofsstream s;
            if (e->kind != K_FILE)
                continue;
            nf++;
            if (pass == 0 && base + nb <= nxt) {
                base += nb;
                continue;
            }
            from = pass == 0 && nxt > base ? nxt - base : 0;
            if (os_open(&s, c, e, from * BS, rc))
                return 1;
            for (j = 0; j < from; j++)               /* target block from: walk the PFS3 runs */
                if (++xk == m->px[e->pext + xj].count) { xj++; xk = 0; }
            for (j = from; j < nb; j++) {
                u32 k = m->px[e->pext + xj].start + xk, n = e->size - j * BS < BS ? e->size - j * BS : BS;
                if (++xk == m->px[e->pext + xj].count) { xj++; xk = 0; }
                memset(b, 0, BS);
                if (os_read(&s, b, n))
                    return 1;
                f2p_progress_file(pass ? "Checking the copy" : "Copying data", base + j + 1, total, nf, files, m->pool + e->name);
                if (pass == 0) {
                    if (b_add(w, k, b))
                        return 1;
                    if ((base + j + 1) % CHECKPOINT == 0 && checkpoint(c, PH_RELOCATE, base + j + 1))
                        return 1;
                    if (f2p_break())
                        return checkpoint(c, PH_RELOCATE, base + j + 1) ? 1 : E_STOP;
                } else {
                    if (b_add(chk, k, b))
                        return 1;
                    if (f2p_break())
                        return E_STOP;
                }
            }
            base += nb;
        }
        if (b_flush(pass ? chk : w))             /* the end of the pass: everything on the disk / checked */
            return 1;
        rc->n = 0;
    }
    return 0;
}

static int copy(conv *c, u32 nxt)
{
    batch w, chk;
    rcache rc;
    int r = 1;
    rc.c = c; rc.n = 0; rc.start = 0; rc.buf = f2p_alloc(BATCH * BS);
    memset(&w, 0, sizeof(w)); memset(&chk, 0, sizeof(chk));
    if (rc.buf && !b_init(&w, c, 0) && !b_init(&chk, c, 1)) {
        c->wb = &w;
        r = copy_files(c, nxt, &rc, &w, &chk);
        if (r == E_STOP || r)
            b_flush(&w);
    } else if (!f2p_error[0])
        f2p_fail("out of memory");
    c->wb = NULL;
    b_free(&w); b_free(&chk);
    if (rc.buf) f2p_free(rc.buf);
    return r;
}

static int softlinks(conv *c)
{
    model *m = &c->m;
    u8 b[BS];
    u32 i, k;
    for (i = 0; i < m->nentries; i++)
        if (m->e[i].kind == K_SOFT) {
            softlink_block(m, &m->e[i], &k, b);
            if (pwrite(c, k, b))
                return 1;
        }
    for (i = 0; i < m->nentries; i++)
        if (m->e[i].kind == K_SOFT) {
            softlink_block(m, &m->e[i], &k, b);
            if (readback(c, k, b, 0))
                return 1;
        }
    return 0;
}

static int build(conv *c, u32 nxt)
{
    pfs3b *pb = pfs3_begin(&c->m);
    batch w, chk;
    u8 b[BS];
    u32 i, n, k;
    int r = 1;
    if (!pb)
        return 1;
    memset(&w, 0, sizeof(w)); memset(&chk, 0, sizeof(chk));
    if (b_init(&w, c, 0) || b_init(&chk, c, 1))
        goto out;
    c->wb = &w;
    n = pfs3_nwrites(pb);
    for (i = nxt; i < n; i++) {
        pfs3_block(pb, i, &k, b);
        if (b_add(&w, k, b))
            goto out;
        if ((i + 1) % CHECKPOINT == 0 && checkpoint(c, PH_BUILD, i + 1))
            goto out;
        f2p_progress("Writing PFS3", i + 1, n, NULL);
        if (f2p_break()) {
            r = checkpoint(c, PH_BUILD, i + 1) ? 1 : E_STOP;
            goto out;
        }
    }
    if (b_flush(&w))
        goto out;
    c->wb = NULL;
    for (i = 0; i < n; i++) {
        pfs3_block(pb, i, &k, b);
        if (b_add(&chk, k, b))
            goto out;
        f2p_progress("Checking PFS3", i + 1, n, NULL);
    }
    r = b_flush(&chk);
out:
    c->wb = NULL;
    b_free(&w); b_free(&chk);
    pfs3_end(pb);
    return r;
}

static int rdbfs(conv *c, u32 nxt)
{
    const rdbfs_plan *r = &c->m.rdb;
    u8 b[BS];
    u32 i, j, k, k2;
    if (r->existing)
        return 0;
    for (i = nxt; i < r->nwrites; i++) {
        rdbfs_write_n(r, i, &k, b);
        if (k == r->rdsk && flush(c))             /* the rescue copy is on the disk before the RDSK changes */
            return 1;
        if (c->d->write(c->d, k, 1, b))
            return f2p_fail("write error (RDB)");
        if (k == r->rdsk && flush(c))
            return 1;
        f2p_progress("RDB", i + 1, r->nwrites, NULL);
        if ((i + 1) % CHECKPOINT == 0 && checkpoint(c, PH_RDBFS, i + 1))
            return 1;
    }
    for (i = 0; i < r->nwrites; i++) {            /* read back what each block holds at the end */
        int later = 0;
        rdbfs_write_n(r, i, &k, b);
        for (j = i + 1; j < r->nwrites && !later; j++) {
            u8 t[BS];
            rdbfs_write_n(r, j, &k2, t);
            later = k2 == k;
        }
        if (!later && readback(c, k, b, 1))
            return 1;
    }
    return 0;
}

static int commit(conv *c)
{
    u8 b[BS];
    make_mark(&c->a, DONEMAGIC, b);
    if (flush(c) || pwrite(c, 1, b) || flush(c))  /* anchor B gone, anchor A and the done mark lead to the plan */
        return 1;
    pfs3_boot0(b);
    if (pwrite(c, 0, b) || flush(c))              /* anchor A gone, the done mark remains */
        return 1;
    if (set_part(c, DOSTYPE_PFS3) || flush(c))    /* the point of no return */
        return 1;
    memset(b, 0, BS);
    return pwrite(c, 1, b) || flush(c);           /* as format leaves it */
}

/* ------------------------------------------------------------------ the rollback copy
 * Candidate i < nw: PFS3 write #i's sector (partition-relative, in write order),
 * kept when FFS has it in use (or it is a boot block). Then the new RDB blocks
 * (absolute): the LSEGs, the FSHD, the rescue copy and marker, kept when they were
 * not all zeros. The RDSK and the old last FSHD are in the plan itself. Kept
 * candidates fill the copy's slots in order. */
static u32 rdb_ncand(const rdbfs_plan *r)
{
    return r->existing ? 0 : r->nlseg + 3;
}

static u32 rdb_cand(const rdbfs_plan *r, u32 j)
{
    return j < r->nlseg ? r->lsegs[j] : j == r->nlseg ? r->fshd : r->rescue[j - r->nlseg - 1];
}

#define KEPT(m, i) (((m)->bkflag[(i) >> 3] >> ((i) & 7)) & 1)

/* the sectors the PFS3 build writes, in order; NULL on error */
static u32 *pfs3_sectors(conv *c, u32 *nw)
{
    pfs3b *pb = pfs3_begin(&c->m);
    u32 *l, i;
    if (!pb)
        return NULL;
    *nw = pfs3_nwrites(pb);
    l = f2p_alloc((*nw + 1) * sizeof(u32));
    if (!l)
        f2p_fail("out of memory");
    else
        for (i = 0; i < *nw; i++)
            l[i] = pfs3_sector(pb, i);
    pfs3_end(pb);
    return l;
}

static int all_zero(const u8 *b)
{
    u32 i;
    for (i = 0; i < BS && !b[i]; i++)
        ;
    return i == BS;
}

static int alloc_backup(model *m, u32 n);

/* the plan for the copy (before anything is written): which candidates to keep */
static int plan_copy(conv *c)
{
    model *m = &c->m;
    const rdbfs_plan *rp = &m->rdb;
    u32 nw = 0, nr = rdb_ncand(rp), i, n = 0, *l = pfs3_sectors(c, &nw);
    u8 b[BS];
    if (!l)
        return 1;
    m->nflag = nw + nr;
    if (!(m->bkflag = f2p_alloc(m->nflag / 8 + 1))) {
        f2p_free(l);
        return f2p_fail("out of memory");
    }
    for (i = 0; i < m->nflag; i++) {
        int keep;
        if (i < nw)
            keep = !BM_GET(&m->freemap, l[i]);
        else if (c->d->read(c->d, rdb_cand(rp, i - nw), 1, b)) {
            f2p_free(l);
            return f2p_fail("read error (RDB block %lu)", UL(rdb_cand(rp, i - nw)));
        } else
            keep = !all_zero(b);
        if (keep) {
            m->bkflag[i >> 3] |= (u8)(1 << (i & 7));
            n++;
        }
    }
    f2p_free(l);
    return alloc_backup(m, n);
}

typedef struct { const model *m; u32 xi, xk; } bkcur;

static void bk_seek(bkcur *q, const model *m, u32 slot)
{
    q->m = m;
    q->xi = 0;
    while (q->xi < m->nbkx && slot >= m->bkx[q->xi].count)
        slot -= m->bkx[q->xi++].count;
    q->xk = slot;
}

/* the block of the next slot; *left: blocks from it to the end of its run */
static u32 bk_next(bkcur *q, u32 *left)
{
    const extent *x = &q->m->bkx[q->xi];
    u32 k = x->start + q->xk;
    if (left)
        *left = x->count - q->xk;
    if (++q->xk == x->count) {
        q->xi++;
        q->xk = 0;
    }
    return k;
}

/* sector l[i], reading ahead along the run of consecutive sectors in l */
static const u8 *rc_list(rcache *r, const u32 *l, u32 i, u32 n)
{
    u32 a = 1, k = l[i];
    if (k >= r->start && k < r->start + r->n)
        return r->buf + (k - r->start) * BS;
    while (i + a < n && a < BATCH && l[i + a] == k + a)
        a++;
    return rc_get(r, k, a);
}

/* BACKUP: copy, then read the copy back. Nothing it reads changes before BUILD,
   so after a crash it simply runs again. */
static int backup(conv *c)
{
    model *m = &c->m;
    const rdbfs_plan *rp = &m->rdb;
    u32 nw = 0, i, pass, done, *l = pfs3_sectors(c, &nw);
    batch w, chk;
    rcache rc;
    bkcur q;
    u8 t[BS];
    int r = 1;
    memset(&w, 0, sizeof(w)); memset(&chk, 0, sizeof(chk));
    rc.c = c; rc.n = 0; rc.start = 0; rc.buf = f2p_alloc(BATCH * BS);
    if (!l || b_init(&w, c, 0) || b_init(&chk, c, 1))
        goto out;
    if (!rc.buf) {
        f2p_fail("out of memory");
        goto out;
    }
    if (nw + rdb_ncand(rp) != m->nflag) {
        f2p_fail("rollback copy: %lu blocks planned, %lu now", UL(m->nflag), UL(nw + rdb_ncand(rp)));
        goto out;
    }
    for (pass = 0; pass < 2; pass++) {
        bk_seek(&q, m, 0);
        rc.n = 0;
        for (i = done = 0; i < m->nflag; i++) {
            const u8 *src = t;
            if (i < nw) {
                if (!KEPT(m, i))
                    continue;
                if (!(src = rc_list(&rc, l, i, nw)))
                    goto out;
            } else {
                u32 k = rdb_cand(rp, i - nw);
                if (c->d->read(c->d, k, 1, t)) {
                    f2p_fail("read error (RDB block %lu)", UL(k));
                    goto out;
                }
                if (!KEPT(m, i)) {
                    if (!all_zero(t)) {
                        f2p_fail("RDB block %lu changed since the plan was made", UL(k));
                        goto out;
                    }
                    continue;
                }
            }
            if (b_add(pass ? &chk : &w, bk_next(&q, NULL), src))
                goto out;
            f2p_progress(pass ? "Checking the rollback copy" : "Saving a rollback copy", ++done, m->nbk, NULL);
        }
        if (b_flush(pass ? &chk : &w))
            goto out;
    }
    r = 0;
out:
    b_free(&w); b_free(&chk);
    if (rc.buf) f2p_free(rc.buf);
    if (l) f2p_free(l);
    return r;
}

/* the reserved area's kept sectors as they were, then read back */
static int undo_build(conv *c, const u32 *l, u32 nw)
{
    const model *m = &c->m;
    batch w, chk;
    rcache rc;
    bkcur q;
    u32 i, pass, left, done, kept = 0;
    int r = 1;
    memset(&w, 0, sizeof(w)); memset(&chk, 0, sizeof(chk));
    rc.c = c; rc.n = 0; rc.start = 0; rc.buf = f2p_alloc(BATCH * BS);
    for (i = 0; i < nw; i++)
        kept += KEPT(m, i);
    if (b_init(&w, c, 0) || b_init(&chk, c, 1))
        goto out;
    if (!rc.buf) {
        f2p_fail("out of memory");
        goto out;
    }
    for (pass = 0; pass < 2; pass++) {
        bk_seek(&q, m, 0);
        rc.n = 0;
        for (i = done = 0; i < nw; i++) {
            u32 k;
            const u8 *src;
            if (!KEPT(m, i))
                continue;                          /* free to FFS: what PFS3 left there does not matter */
            k = bk_next(&q, &left);
            if (!(src = rc_get(&rc, k, left)) || b_add(pass ? &chk : &w, l[i], src))
                goto out;
            f2p_progress(pass ? "Checking the rollback" : "Rolling back", ++done, kept, NULL);
        }
        if (b_flush(pass ? &chk : &w))
            goto out;
    }
    r = 0;
out:
    b_free(&w); b_free(&chk);
    if (rc.buf) f2p_free(rc.buf);
    return r;
}

static int awrite(conv *c, u32 k, const u8 *b)
{
    return c->d->write(c->d, k, 1, b) ? f2p_fail("write error (RDB block %lu)", UL(k)) : 0;
}

/* the RDB as it was. First a rescue copy of the old RDSK, so a torn RDSK write
   comes back as the old one; then the old last FSHD (the new FSHD drops out of
   the list), the RDSK, the FSHD and LSEG blocks, and the rescue pair last. */
static int undo_rdb(conv *c, u32 nw)
{
    const model *m = &c->m;
    const rdbfs_plan *rp = &m->rdb;
    u32 nr = rdb_ncand(rp), j, slot = 0;
    u8 *o, mk[BS];
    bkcur q;
    int r = 1;
    if (!nr)
        return 0;
    if (!(o = f2p_alloc(nr * BS)))
        return f2p_fail("out of memory");
    for (j = 0; j < nw; j++)
        slot += KEPT(m, j);
    bk_seek(&q, m, slot);
    for (j = 0; j < nr; j++)                       /* kept ones from the copy, the rest were zeros */
        if (KEPT(m, nw + j) && pread(c, bk_next(&q, NULL), o + j * BS))
            goto out;
    rdbfs_marker(rp, rp->rdskorig, mk);
    if (flush(c) || awrite(c, rp->rescue[0], rp->rdskorig) || awrite(c, rp->rescue[1], mk) || flush(c))
        goto out;
    if (rp->linkblk != NOBLK && awrite(c, rp->linkblk, rp->linkorig))
        goto out;
    if (flush(c) || awrite(c, rp->rdsk, rp->rdskorig) || flush(c))
        goto out;
    for (j = 0; j <= rp->nlseg; j++)
        if (awrite(c, rdb_cand(rp, j), o + j * BS))
            goto out;
    if (flush(c) || awrite(c, rp->rescue[1], o + (nr - 1) * BS) || flush(c) || awrite(c, rp->rescue[0], o + (nr - 2) * BS) || flush(c))
        goto out;
    if (readback(c, rp->rdsk, rp->rdskorig, 1) || (rp->linkblk != NOBLK && readback(c, rp->linkblk, rp->linkorig, 1)))
        goto out;
    for (j = 0; j < nr; j++)
        if (readback(c, rdb_cand(rp, j), o + j * BS, 1))
            goto out;
    r = 0;
out:
    f2p_free(o);
    return r;
}

/* blocks 0 and 1 as they were. The undo mark in block 1 leads to the plan
   while block 0 is rewritten; block 1 itself is last (FFS does not read it). */
static int undo_boot(conv *c)
{
    u8 b[BS];
    make_mark(&c->a, UNDOMAGIC, b);
    if (flush(c) || pwrite(c, 1, b) || flush(c))
        return 1;
    if (pwrite(c, 0, c->m.boot_orig[0]) || flush(c) || readback(c, 0, c->m.boot_orig[0], 0) || flush(c))
        return 1;
    return pwrite(c, 1, c->m.boot_orig[1]) || flush(c) || readback(c, 1, c->m.boot_orig[1], 0) || flush(c);
}

/* back to the old volume from phase `from` (what had been written by then) */
static int rollback(conv *c, u32 from)
{
    u32 nw = 0, *l;
    int r;
    if (c->p.dostype == DOSTYPE_PFS3 && !c->p.invalid)
        return f2p_fail("the RDB says PFS\\3 already: past the point of no return, run ffs2pfs again to finish");
    f2p_log("Rolling back: %s goes back to %s, as it was\n", c->p.name[0] ? c->p.name : "the partition", c->m.ofs ? "OFS" : "FFS");
    if (checkpoint(c, PH_ROLLBACK, from))         /* from here on a resume carries on rolling back */
        return 1;
    if (from >= PH_BUILD) {
        if (!(l = pfs3_sectors(c, &nw)))
            return 1;
        r = undo_build(c, l, nw);
        f2p_free(l);
        if (r)
            return r;
    }
    if (from >= PH_RDBFS && undo_rdb(c, nw))
        return 1;
    if (flush(c) || awrite(c, c->m.part_block, c->m.part_orig) || flush(c) || readback(c, c->m.part_block, c->m.part_orig, 1))
        return 1;
    c->p.dostype = c->m.orig_dostype;
    c->p.invalid = 0;
    return undo_boot(c);
}

/* ------------------------------------------------------------------ start */
static int alloc_runs(model *m, u32 *out, const u32 *sizes, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        u32 k, run = 0, at = 0;
        for (k = 0; k < m->part_blocks; k++) {
            if ((k >= 2 && k < m->end) || !BM_GET(&m->freemap, k)) {
                run = 0;
                continue;
            }
            if (!run)
                at = k;
            if (++run == sizes[i])
                break;
        }
        if (run < sizes[i])
            return f2p_fail("no free run of %lu blocks for the conversion journal", UL(sizes[i]));
        out[i] = at;
        for (k = at; k < at + sizes[i]; k++)
            BM_CLR(&m->freemap, k);
    }
    return 0;
}

/* the rollback copy: n free blocks past the reserved area, lowest first, in runs */
static int alloc_backup(model *m, u32 n)
{
    u32 k, left, runs, pass, prev;
    for (pass = 0; pass < 2; pass++) {
        left = n;
        runs = 0;
        prev = NOBLK;
        for (k = m->end; k < m->part_blocks && left; k++) {
            if (!BM_GET(&m->freemap, k))
                continue;
            if (prev == NOBLK || k != prev + 1) {
                if (pass) {
                    m->bkx[runs].start = k;
                    m->bkx[runs].count = 0;
                }
                runs++;
            }
            if (pass) {
                m->bkx[runs - 1].count++;
                BM_CLR(&m->freemap, k);
            }
            prev = k;
            left--;
        }
        if (left)
            return f2p_fail("no room for the rollback copy: it needs %lu more free blocks", UL(left));
        if (!pass) {
            if (!(m->bkx = f2p_alloc((runs + 1) * sizeof(extent))))
                return f2p_fail("out of memory");
            m->nbkx = runs;
        }
    }
    m->nbk = n;
    return 0;
}

static int analyse(conv *c, const u8 *bin, u32 binlen)
{
    model *m = &c->m;
    pfs3b *pb;
    u32 i, files = 0, dirs = 0, links = 0, soft = 0, used = 0, nfree = 0;
    f2p_log("Step 1/6: reading the volume\n");
    if (!c->p.spb)
        return f2p_fail("partition %s: sectors are not 512 bytes", c->p.name);
    if (c->p.reserved < 2)
        return f2p_fail("partition %s: %lu boot blocks (Reserved); FFS may keep data in block 1, which the conversion needs",
                        c->p.name, UL(c->p.reserved));
    if (c->p.prealloc)
        return f2p_fail("partition %s: PreAlloc is %lu, not supported yet", c->p.name, UL(c->p.prealloc));
    if (ffs_read(c->d, c->p.start, c->p.blocks, c->p.spb, c->p.reserved, m))
        return 1;
    for (i = 0; i < m->nentries; i++) {
        files += m->e[i].kind == K_FILE; dirs += m->e[i].kind == K_DIR;
        links += m->e[i].kind == K_HARD; soft += m->e[i].kind == K_SOFT;
    }
    for (i = 0; i < m->part_blocks; i++)
        nfree += BM_GET(&m->freemap, i);
    f2p_log("%s volume '%s' (DOS\\%d): %lu files, %lu directories, %lu hard links, %lu soft links; %lu of %lu blocks free\n",
            m->ofs ? "OFS" : "FFS", m->volname, (int)(m->dostype & 0xff), UL(files), UL(dirs), UL(links), UL(soft), UL(nfree), UL(m->part_blocks));
    f2p_log("Step 2/6: checking every file (all data is read once)\n");
    i = plan_build(c->d, m);
    if (i)
        return (int)i;
    m->part_block = c->p.block;
    m->orig_dostype = c->p.dostype;
    if (c->d->read(c->d, c->p.block, 1, m->part_orig))
        return f2p_fail("read error (RDB)");
    if (rdbfs_make(c->d, bin, binlen, (19UL << 16) | 2, &m->rdb))
        return 1;
    pb = pfs3_begin(m);
    if (!pb)
        return 1;
    used = pfs3_nwrites(pb);
    pfs3_end(pb);
    memcpy(m->boot_orig[0], c->b0, BS);
    memcpy(m->boot_orig[1], c->b1, BS);
    if (plan_copy(c))
        return 1;
    f2p_log("plan: PFS3 reserved area blocks 2..%lu, %lu PFS3 metadata blocks, RDB: %s\n",
            UL(m->end - 1), UL(used), m->rdb.existing ? "pfs3aio already there" : "pfs3aio added");
    f2p_log("  rollback copy: %lu blocks in free space (until the last step the volume can go back as it was)\n", UL(m->nbk));
    if (m->ofs)
        f2p_log("  OFS: every file is copied, packed, into free space\n");
    else
        f2p_log("  %lu data blocks move out of the reserved area\n", UL(m->nmoves));
    return 0;
}

/* 0: locked. E_ROLLEDBACK: failed, nothing changed (the plan is in free space).
   1: failed and blocks 0/1 could not be put back (the marks lead to the plan). */
static int start(conv *c, const u8 *bin, u32 binlen)
{
    model *m = &c->m;
    u8 b[BS];
    u32 sizes[4], at[4], pb, i;
    int r = analyse(c, bin, binlen);
    if (r)
        return r == E_STOP ? E_STOP : E_ROLLEDBACK;
    pb = plan_size(m) / BS;
    sizes[0] = 1; sizes[1] = 1; sizes[2] = pb; sizes[3] = pb;
    if (alloc_runs(m, at, sizes, 4))
        return E_ROLLEDBACK;
    c->a.version = 1; c->a.part_start = c->p.start; c->a.part_blocks = c->p.blocks;
    c->a.jA = at[0]; c->a.jB = at[1]; c->a.planA = at[2]; c->a.planB = at[3]; c->a.plan_blocks = pb;
    c->a.partblock = c->p.block; c->a.orig_dostype = c->p.dostype;
    f2p_log("start: plan %lu blocks at %lu/%lu, journal %lu/%lu\n", UL(pb), UL(at[2]), UL(at[3]), UL(at[0]), UL(at[1]));
    if (plan_write(c->d, c->p.start + at[2], m, &c->plan_crc) || plan_write(c->d, c->p.start + at[3], m, &c->plan_crc))
        return E_ROLLEDBACK;
    for (i = 0; i < 2; i++) {
        memset(b, 0, BS);
        wr32(b + 4, i + 1); wr32(b + 8, PH_RELOCATE); wr32(b + 12, 0); wr32(b + 16, c->plan_crc);
        crcblock(b, JOURNAL);
        if (pwrite(c, at[i], b))
            return E_ROLLEDBACK;
    }
    for (i = 2; i < 4; i++)                       /* the plan copies read back before anything depends on them */
        if (plan_check(c->d, c->p.start + at[i], pb))
            return E_ROLLEDBACK;
    make_anchor(&c->a, b);
    if (flush(c) || pwrite(c, 1, b) || flush(c) || pwrite(c, 0, b) || flush(c)) {   /* anchor B, then A: LOCK */
        char why[200];
        strcpy(why, f2p_error);                   /* not locked: blocks 0 and 1 back as they were */
        r = flush(c) || pwrite(c, 0, c->b0) || flush(c) || pwrite(c, 1, c->b1) || flush(c);
        f2p_fail("%s", why);
        return r ? 1 : E_ROLLEDBACK;
    }
    return 0;
}

/* ------------------------------------------------------------------ state and run */
enum { S_FFS, S_LOCKED, S_BOOTFIX, S_PFS3, S_UNDOFIX };

static int state(conv *c, anchor *a)
{
    int any = c->m0 || c->m1;
    *a = c->m0 ? c->A0 : c->A1;
    if (c->p.invalid) {
        if (!any)
            return f2p_fail("PART block bad and no anchor"), -1;
        return S_LOCKED;
    }
    if (c->m1 == UNDOMAGIC)                       /* rolled back but for blocks 0 and 1 */
        return S_UNDOFIX;
    if (c->m1 == DONEMAGIC && c->p.dostype == DOSTYPE_PFS3)
        return S_BOOTFIX;
    if (any && (c->p.dostype == DOSTYPE_CONVERTING || c->p.dostype == DOSTYPE_PFS3 || c->m0 || c->m1 == DONEMAGIC
                || !is_dos_boot(c->b0)))
        return S_LOCKED;
    if (c->p.dostype == DOSTYPE_CONVERTING)
        return f2p_fail("partition marked converting but no anchor found"), -1;
    if (c->p.dostype == DOSTYPE_PFS3)
        return S_PFS3;
    return S_FFS;                                 /* maybe with the anchor B of a start cut short */
}

static int load_partition(conv *c)
{
    if (c->d->read(c->d, c->p.start, 1, c->b0) || c->d->read(c->d, c->p.start + 1, 1, c->b1))
        return f2p_fail("read error");
    c->m0 = read_mark(c->b0, &c->A0);
    c->m1 = read_mark(c->b1, &c->A1);
    if (c->m0 && c->A0.part_start != c->p.start)
        c->m0 = 0;
    if (c->m1 && c->A1.part_start != c->p.start)
        c->m1 = 0;
    return 0;
}

static int in_progress(conv *c)
{
    anchor a;
    int s = state(c, &a);
    return s != S_FFS && s != S_PFS3;             /* -1 too: its error shows when it is worked on */
}

/* a torn PART block: the partition, found by its mark at a cylinder start (reads only) */
static int find_by_mark(conv *c, const u8 *r)
{
    u32 cyl, cb = L(r, 36);
    for (cyl = L(r, 34); cyl <= L(r, 35); cyl++) {
        u8 b0[BS], b1[BS];
        anchor A, B;
        int a, bb;
        if (c->d->read(c->d, cyl * cb, 1, b0) || c->d->read(c->d, cyl * cb + 1, 1, b1))
            continue;
        a = read_mark(b0, &A) && A.part_start == cyl * cb;
        bb = read_mark(b1, &B) && B.part_start == cyl * cb;
        if (a || bb) {
            anchor *x = a ? &A : &B;
            memset(&c->p, 0, sizeof(c->p));
            c->p.block = x->partblock; c->p.start = cyl * cb; c->p.blocks = x->part_blocks; c->p.invalid = 1;
            return 1;
        }
    }
    return 0;
}

/* the partition to work on: one in progress, else the named one */
static int discover(conv *c, const char *name)
{
    u8 r[BS];
    u32 rblk;
    partinfo parts[32];
    int n, i, pick = -1;
    if (!rdb_find_rdsk(c->d, &rblk, r)) {
        if (!rdbfs_find_rescue(c->d, &rblk, r))
            return f2p_fail("no valid RDSK and no rescue copy");
        f2p_log("RDSK restored from the rescue copy\n");
        if (c->d->write(c->d, rblk, 1, r))
            return f2p_fail("write error (RDSK)");
    }
    n = rdb_partitions(c->d, parts, 32);
    for (i = 0; i < n; i++) {
        if (parts[i].invalid) {
            if (find_by_mark(c, r))
                return load_partition(c);
            continue;
        }
        c->p = parts[i];
        if (load_partition(c))
            return 1;
        if (in_progress(c)) {
            if (name && *name && strcmp(parts[i].name, name))
                return f2p_fail("%s is part way through a conversion: run ffs2pfs %s to finish it (or with ROLLBACK to undo it) first",
                                parts[i].name, parts[i].name);
            return 0;
        }
        if (pick < 0 && (!name || !*name || !strcmp(parts[i].name, name)))
            pick = i;
    }
    if (pick < 0)
        return f2p_fail(name ? "no partition named %s" : "no partition", name);
    c->p = parts[pick];
    return load_partition(c);
}

static u32 rd_journal(conv *c, const u8 *b, u32 *phase, u32 *nxt, u32 *crc)
{
    if (!checkcrc(b, JOURNAL))
        return 0;
    *phase = L(b, 2); *nxt = L(b, 3); *crc = L(b, 4);
    return L(b, 1);
}

/* block 1 holds the anchor of a start that was cut short (block 0 is still the
   boot block): block 1 back as that plan saved it, or cleared if the plan is gone */
static int stale_anchor(conv *c)
{
    model m;
    int ok = !plan_read(c->d, c->p.start + c->A1.planA, c->A1.plan_blocks, &m)
          || !plan_read(c->d, c->p.start + c->A1.planB, c->A1.plan_blocks, &m);
    if (ok) {
        memcpy(c->b1, m.boot_orig[1], BS);
        model_free(&m);
    } else
        memset(c->b1, 0, BS);
    f2p_log("block 1 held the anchor of a conversion that never began: %s\n", ok ? "put back as it was" : "cleared");
    c->m1 = 0;
    return pwrite(c, 1, c->b1) || flush(c);
}

static const char *pname(const conv *c)
{
    return c->p.name[0] ? c->p.name : "the partition";
}

/* roll back from phase `from`; 0 = done */
static int do_rollback(conv *c, u32 from)
{
    int r = rollback(c, from);
    if (!r)
        f2p_log("rolled back: %s is %s again, as it was before the conversion\n", pname(c), c->m.ofs ? "OFS" : "FFS");
    return r;
}

int f2p_run(blockdev *d, const char *name, const u8 *bin, u32 binlen, int mode)
{
    conv c;
    u8 jA[BS], jB[BS];
    u32 phase = 0, nxt = 0, crc = 0, sA, sB, p2, n2, c2;
    int st, r;
    memset(&c, 0, sizeof(c));
    c.d = d;
    if (discover(&c, name))
        return 1;
    st = state(&c, &c.a);
    if (st < 0)
        return 1;
    if (mode == F2P_DRY) {
        if (st != S_FFS)
            return f2p_fail("partition %s: a conversion is in progress or done", c.p.name);
        r = analyse(&c, bin, binlen);
        model_free(&c.m);
        return r;
    }
    if (st == S_PFS3) {
        if (mode == F2P_ROLLBACK)
            return f2p_fail("%s is PFS3: the conversion is finished, there is nothing to roll back", pname(&c));
        f2p_log("partition %s is PFS3: nothing to do\n", c.p.name);
        return 0;
    }
    if (st == S_BOOTFIX) {
        u8 b[BS];
        if (mode == F2P_ROLLBACK)
            return f2p_fail("%s: the RDB says PFS\\3 already, past the point of no return; run ffs2pfs without ROLLBACK to finish",
                            pname(&c));
        pfs3_boot0(b);
        if (pwrite(&c, 0, b) || flush(&c))
            return 1;
        memset(b, 0, BS);
        return pwrite(&c, 1, b) || flush(&c);
    }
    if (st == S_FFS) {
        if (c.m1 == ANCHOR && stale_anchor(&c))
            return 1;
        if (mode == F2P_ROLLBACK) {
            f2p_log("%s is not being converted: nothing to roll back\n", pname(&c));
            return 0;
        }
        r = start(&c, bin, binlen);
        model_free(&c.m);
        if (r)
            return r;
    } else if (st == S_UNDOFIX)
        f2p_log("finishing the rollback of %s\n", pname(&c));
    else
        f2p_log("%s the conversion of %s\n", mode == F2P_ROLLBACK ? "rolling back" : "resuming", pname(&c));
    /* from here on everything comes from the disk: mark, plan, journal */
    if (plan_read(d, c.p.start + c.a.planA, c.a.plan_blocks, &c.m) && plan_read(d, c.p.start + c.a.planB, c.a.plan_blocks, &c.m))
        return f2p_fail("both plan copies are bad");
    if (st == S_UNDOFIX) {
        r = undo_boot(&c);
        if (!r)
            f2p_log("rolled back: %s is %s again, as it was before the conversion\n", pname(&c), c.m.ofs ? "OFS" : "FFS");
        model_free(&c.m);
        return r ? r : mode == F2P_ROLLBACK ? 0 : E_ROLLEDBACK;
    }
    if (pread(&c, c.a.jA, jA) || pread(&c, c.a.jB, jB))
        goto fail;
    sA = rd_journal(&c, jA, &phase, &nxt, &crc);
    sB = rd_journal(&c, jB, &p2, &n2, &c2);
    if (!sA && !sB) {
        f2p_fail("both journal copies are bad");
        goto fail;
    }
    if (sB > sA) { phase = p2; nxt = n2; crc = c2; }
    c.seq = sA > sB ? sA : sB;
    c.plan_crc = crc;
    if (phase == PH_ROLLBACK || mode == F2P_ROLLBACK) {   /* a rollback, once begun, runs to the end */
        if (phase == PH_ROLLBACK && mode != F2P_ROLLBACK)
            f2p_log("a rollback was under way: finishing it\n");
        r = do_rollback(&c, phase == PH_ROLLBACK ? nxt : phase);
        model_free(&c.m);
        return r ? r : mode == F2P_ROLLBACK ? 0 : E_ROLLEDBACK;
    }
    r = 0;
    if (c.p.invalid && set_part(&c, DOSTYPE_CONVERTING))
        r = 1;
    if (!r && phase < PH_COMMIT && c.p.dostype != DOSTYPE_CONVERTING) {
        u8 b[BS];
        make_anchor(&c.a, b);
        if (pwrite(&c, 0, b) || flush(&c) || set_part(&c, DOSTYPE_CONVERTING) || flush(&c))   /* LOCK, part 2 */
            r = 1;
    }
    if (!r && phase == PH_RELOCATE) {
        f2p_log("Step 3/6: %s\n", c.m.ofs ? "copying the OFS data into free space, packed" : "moving data out of the PFS3 reserved area");
        r = c.m.ofs ? copy(&c, nxt) : relocate(&c, nxt);
        if (!r)
            r = softlinks(&c);
        if (!r && checkpoint(&c, PH_BACKUP, 0))
            r = 1;
        if (!r)
            phase = PH_BACKUP, nxt = 0;
    }
    if (!r && phase == PH_BACKUP) {
        f2p_log("Step 4/6: saving a copy of what the next steps overwrite (for a rollback)\n");
        r = backup(&c);
        if (!r && checkpoint(&c, PH_BUILD, 0))
            r = 1;
        if (!r)
            phase = PH_BUILD, nxt = 0;
    }
    if (!r && phase == PH_BUILD) {
        f2p_log("Step 5/6: writing the PFS3 structures\n");
        r = build(&c, nxt);
        if (!r && checkpoint(&c, PH_RDBFS, 0))
            r = 1;
        if (!r)
            phase = PH_RDBFS, nxt = 0;
    }
    if (!r && phase == PH_RDBFS) {
        f2p_log("Step 6/6: putting pfs3aio into the RDB and committing\n");
        r = rdbfs(&c, nxt);
        if (!r && checkpoint(&c, PH_COMMIT, 0))
            r = 1;
        if (!r)
            phase = PH_COMMIT;
    }
    if (!r)
        r = commit(&c);
    if (!r) {
        model_free(&c.m);
        f2p_log("done: %s is PFS3\n", pname(&c));
        return 0;
    }
    if (r == E_STOP) {
        model_free(&c.m);
        return E_STOP;
    }
    {                                             /* failed: back to the old volume */
        char why[200], why2[200];
        strcpy(why, f2p_error);
        f2p_log("the conversion failed: %s\n", why);
        if (c.p.dostype == DOSTYPE_PFS3 && !c.p.invalid) {   /* past the point of no return: only block 1 is left */
            model_free(&c.m);
            return f2p_fail("%s; the PFS3 volume is complete: run ffs2pfs %s again to finish", why, c.p.name);
        }
        r = do_rollback(&c, phase);
        model_free(&c.m);
        if (r) {
            strcpy(why2, f2p_error);
            f2p_fail("%s; the rollback failed too (%s): run ffs2pfs %s ROLLBACK to try it again", why, why2, c.p.name);
            return 1;
        }
        f2p_fail("%s", why);
        return E_ROLLEDBACK;
    }
fail:
    model_free(&c.m);
    return 1;
}

/* ------------------------------------------------------------------ probe (front ends) */
int f2p_probe(blockdev *d, int rescue, f2p_part *out, int max)
{
    conv c;
    u8 r[BS], jA[BS], jB[BS];
    u32 rblk, ph, p2, nx, cr, sA, sB;
    partinfo parts[32];
    int n, i, k = 0;
    if (!rdb_find_rdsk(d, &rblk, r) && (!rescue || !rdbfs_find_rescue(d, &rblk, r)))
        return -1;
    n = rdb_partitions_from(d, r, parts, 32);
    for (i = 0; i < n && k < max; i++) {
        f2p_part *o = &out[k++];
        memset(o, 0, sizeof(*o));
        memset(&c, 0, sizeof(c));
        c.d = d;
        c.p = parts[i];
        if (parts[i].invalid && !find_by_mark(&c, r)) {
            strcpy(o->why, "its PART block is damaged");
            o->state = P_BAD;
            o->p = parts[i];
            continue;
        }
        o->p = c.p;                               /* a torn PART block: as its mark says, no name */
        o->orig_dostype = c.p.dostype;
        if (load_partition(&c)) {
            memcpy(o->why, f2p_error, sizeof(o->why) - 1);     /* o is zeroed: the last byte stays 0 */
            o->state = P_BAD;
            continue;
        }
        if (c.m0 || c.m1)
            o->orig_dostype = (c.m0 ? c.A0 : c.A1).orig_dostype;
        switch (state(&c, &c.a)) {
        case S_FFS:
            o->state = P_NONE;
            o->orig_dostype = c.p.dostype;
            break;
        case S_PFS3:
            o->state = P_PFS3;
            break;
        case S_BOOTFIX:
            o->state = P_FINISH;
            break;
        case S_UNDOFIX:
            o->state = P_UNDO;
            break;
        case S_LOCKED:
            ph = p2 = sA = sB = 0;
            if (!pread(&c, c.a.jA, jA))
                sA = rd_journal(&c, jA, &ph, &nx, &cr);
            if (!pread(&c, c.a.jB, jB))
                sB = rd_journal(&c, jB, &p2, &nx, &cr);
            if (sB > sA)
                ph = p2;
            if (!sA && !sB) {
                strcpy(o->why, "both journal copies are bad");
                o->state = P_BAD;
            } else if (ph == PH_ROLLBACK)
                o->state = P_UNDO;
            else {
                o->state = P_STOPPED;
                o->step = ph >= PH_RELOCATE && ph <= PH_RDBFS ? ph + 2 : 6;
            }
            break;
        default:
            memcpy(o->why, f2p_error, sizeof(o->why) - 1);
            o->state = P_BAD;
        }
    }
    return k;
}
