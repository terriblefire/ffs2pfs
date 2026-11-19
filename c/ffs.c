/* OFS/FFS (DOS\0..DOS\5) reader: the whole volume into the model (ffs.py).
 * The directory walk is iterative (small Amiga stacks) and visits entries in
 * the same order as the Python model: hash slot order, chain order, a
 * directory's contents straight after its own entry. */
#include "f2p.h"

#define T_HEADER 2
#define T_DATA 8
#define T_LIST 16

/* the block layout: N longs per block; trailing fields counted back from the end
   (for 512-byte blocks: T(1) = long 127 = sec_type ... T(50) = long 78 = bm_flag) */
static u32 NL = 128, HT = 72, BSZ = 512, SPB = 1;
#define T(b, k) L(b, NL - (k))
#define OFS_DATA (BSZ - 24)

static void set_blocksize(u32 spb)
{
    SPB = spb;
    BSZ = 512 * spb;
    NL = BSZ / 4;
    HT = NL - 56;
}

static u32 csum(const u8 *b)
{
    u32 s = 0, i;
    for (i = 0; i < NL; i++)
        s += L(b, i);
    return s;
}

typedef struct {
    blockdev *d;
    u32 start, n;                  /* n: filesystem blocks */
    u32 reserved;                  /* boot blocks: the bitmap starts after them */
    model *m;
    bitmap claimed;                /* in filesystem blocks */
    u32 maxent, maxext;
    u32 maxpool;
} reader;

static int rd(reader *r, u32 k, u8 *b)
{
    if (k >= r->n)
        return f2p_fail("block %lu outside the partition", UL(k));
    return r->d->read(r->d, r->start + k * SPB, SPB, b) ? f2p_fail("read error at block %lu", UL(k)) : 0;
}

static int claim(reader *r, u32 k)
{
    if (k >= r->n)
        return f2p_fail("block %lu outside the partition", UL(k));
    if (BM_GET(&r->claimed, k))
        return f2p_fail("block %lu is used twice", UL(k));
    BM_SET(&r->claimed, k);
    return 0;
}

static void *grow(void *p, u32 *max, u32 used, u32 size)
{
    u32 nmax = *max ? *max * 2 : 256;
    void *q = f2p_alloc((size_t)nmax * size);
    if (!q)
        return NULL;
    if (p) {
        memcpy(q, p, (size_t)used * size);
        f2p_free(p);
    }
    *max = nmax;
    return q;
}

static int pool_add(reader *r, const u8 *s, u32 n, u32 *off)
{
    model *m = r->m;
    while (m->poolsize + n + 1 > r->maxpool) {
        char *q = grow(m->pool, &r->maxpool, m->poolsize, 1);
        if (!q)
            return f2p_fail("out of memory (names)");
        m->pool = q;
    }
    *off = m->poolsize;
    memcpy(m->pool + m->poolsize, s, n);
    m->pool[m->poolsize + n] = 0;
    m->poolsize += n + 1;
    return 0;
}

static int add_run(reader *r, entry *e, u32 k);

/* a data block of e: OFS keeps blocks; FFS gives its sectors, up to the file size */
static int add_extent(reader *r, entry *e, u32 k)
{
    u32 i, have = 0, want;
    if (r->m->ofs || SPB == 1)
        return add_run(r, e, k);
    for (i = 0; i < e->next; i++)
        have += r->m->x[e->ext + i].count;
    want = (e->size + BS - 1) / BS;
    for (i = 0; i < SPB && have + i < want; i++)
        if (add_run(r, e, k * SPB + i))
            return 1;
    return 0;
}

static int add_run(reader *r, entry *e, u32 k)
{
    model *m = r->m;
    if (e->next && m->x[e->ext + e->next - 1].start + m->x[e->ext + e->next - 1].count == k) {
        m->x[e->ext + e->next - 1].count++;
        return 0;
    }
    if (m->nextents == r->maxext) {
        extent *q = grow(m->x, &r->maxext, m->nextents, sizeof(extent));
        if (!q)
            return f2p_fail("out of memory (extents)");
        m->x = q;
    }
    m->x[m->nextents].start = k;
    m->x[m->nextents].count = 1;
    m->nextents++;
    e->next++;
    return 0;
}

static u32 bstr(const u8 *b, u32 off, u32 max, const u8 **s)
{
    *s = b + off + 1;
    return b[off] < max ? b[off] : max;
}

static int read_file(reader *r, entry *e, const u8 *h, u8 *b)
{
    u32 nblk = 0, i, cnt, ext, guard = 0, bpd = r->m->ofs ? OFS_DATA : BSZ;
    const u8 *hdr = h;
    e->ext = r->m->nextents;
    e->next = 0;
    e->size = T(h, 47);
    for (;;) {
        cnt = L(hdr, 2);
        if (cnt > HT)
            return f2p_fail("bad block count in a file header");
        for (i = 0; i < cnt; i++) {
            u32 k = L(hdr, 6 + HT - 1 - i);
            if (k == 0 || k >= r->n)
                return f2p_fail("data block %lu out of range", UL(k));
            if (claim(r, k) || add_extent(r, e, k))
                return 1;
            nblk++;
        }
        ext = T(hdr, 2);
        if (!ext)
            break;
        if (++guard > 1000000 || rd(r, ext, b) || L(b, 0) != T_LIST || L(b, 1) != ext || csum(b))
            return f2p_fail("bad extension block %lu", UL(ext));
        if (claim(r, ext))
            return 1;
        hdr = b;
    }
    if (nblk != (e->size + bpd - 1) / bpd)
        return f2p_fail("a file has %lu blocks for %lu bytes", UL(nblk), UL(e->size));
    return 0;
}

typedef struct { u32 *ht; s32 idx; u32 slot; u32 next; } frame;

static int walk(reader *r, u32 root, int count_only, u32 *expected)
{
    model *m = r->m;
    u8 *b, *xb;
    frame *st;
    u32 *hts;
    int sp = 0, maxdepth = 101;
    u32 i;
    st = f2p_alloc(sizeof(frame) * maxdepth);
    hts = f2p_alloc(sizeof(u32) * HT * maxdepth);
    b = f2p_alloc(BSZ);
    xb = f2p_alloc(BSZ);
    if (!st || !hts || !b || !xb) {
        if (st) f2p_free(st);
        if (hts) f2p_free(hts);
        if (b) f2p_free(b);
        if (xb) f2p_free(xb);
        return f2p_fail("out of memory");
    }
    for (i = 0; i < (u32)maxdepth; i++)
        st[i].ht = hts + i * HT;
    if (rd(r, root, b))
        goto fail;
    for (i = 0; i < HT; i++)
        st[0].ht[i] = L(b, 6 + i);
    st[0].idx = -1; st[0].slot = 0; st[0].next = 0;
    while (sp >= 0) {
        frame *f = &st[sp];
        u32 k;
        s32 sec;
        entry *e;
        const u8 *s;
        u32 n;
        if (!f->next) {
            while (f->slot < HT && !f->ht[f->slot])
                f->slot++;
            if (f->slot == HT) {
                sp--;
                continue;
            }
            f->next = f->ht[f->slot++];
        }
        k = f->next;
        if (rd(r, k, b) || L(b, 0) != T_HEADER || L(b, 1) != k || csum(b)) {
            f2p_fail("bad header block %lu", UL(k));
            goto fail;
        }
        f->next = T(b, 4);
        if (count_only) {                      /* the first pass: count, nothing else */
            ++*expected;
            f2p_progress("Counting", *expected, 0, NULL);
            if ((s32)T(b, 1) == 2) {
                if (sp + 1 >= maxdepth) { f2p_fail("directories nested too deep"); goto fail; }
                sp++;
                for (i = 0; i < HT; i++)
                    st[sp].ht[i] = L(b, 6 + i);
                st[sp].idx = -1; st[sp].slot = 0; st[sp].next = 0;
            }
            continue;
        }
        if (m->nentries == r->maxent) {
            entry *q = grow(m->e, &r->maxent, m->nentries, sizeof(entry));
            if (!q) { f2p_fail("out of memory (entries)"); goto fail; }
            m->e = q;
        }
        e = &m->e[m->nentries];
        memset(e, 0, sizeof(*e));
        e->key = k;
        e->parent = f->idx;
        e->protect = T(b, 48);
        e->days = T(b, 23); e->mins = T(b, 22); e->ticks = T(b, 21);
        e->next_link = T(b, 10);
        n = bstr(b, BSZ - 80, 30, &s);
        if (pool_add(r, s, n, &e->name)) goto fail;
        e->nlen = (u8)n;
        n = bstr(b, BSZ - 184, 79, &s);
        if (pool_add(r, s, n, &e->comment)) goto fail;
        e->clen = (u8)n;
        if (claim(r, k)) goto fail;
        sec = (s32)T(b, 1);
        m->nentries++;
        f2p_progress("Reading", m->nentries, *expected, m->pool + e->name);
        if (sec == 2) {
            e->kind = K_DIR;
            if (sp + 1 >= maxdepth) { f2p_fail("directories nested too deep"); goto fail; }
            sp++;
            for (i = 0; i < HT; i++)
                st[sp].ht[i] = L(b, 6 + i);
            st[sp].idx = (s32)(m->nentries - 1);
            st[sp].slot = 0;
            st[sp].next = 0;
        } else if (sec == -3) {
            e->kind = K_FILE;
            if (read_file(r, e, b, xb)) goto fail;
        } else if (sec == 3) {
            e->kind = K_SOFT;
            for (n = 0; n < BSZ - 224 && b[24 + n]; n++)
                ;
            if (pool_add(r, b + 24, n, &e->link)) goto fail;
            e->size = n;                       /* soft links: size = path length */
        } else if (sec == 4 || sec == -4) {
            e->kind = K_HARD;
            e->link = T(b, 11);                /* real entry key, resolved later */
        } else {
            f2p_fail("unknown entry type %ld at %lu", (long)sec, UL(k));
            goto fail;
        }
    }
    f2p_free(st); f2p_free(hts); f2p_free(b); f2p_free(xb);
    return 0;
fail:
    f2p_free(st); f2p_free(hts); f2p_free(b); f2p_free(xb);
    return 1;
}

static int read_bitmap(reader *r, const u8 *rb)
{
    model *m = r->m;
    u8 *b = f2p_alloc(BSZ);
    u32 bit = 0, i, j, k, ext, guard = 0, np = 0, maxp = 0, *pages = NULL;
    int err = 0;
    if (!b)
        return f2p_fail("out of memory");
    if (T(rb, 50) != 0xffffffffUL) {
        f2p_free(b);
        return f2p_fail("bitmap not valid: the volume needs validating first");
    }
    if (bm_init(&m->freemap, r->n * SPB)) {   /* free map in sectors */
        f2p_free(b);
        return 1;
    }
    /* bitmap block list: 25 in the root, then extension blocks of 127 (next at 127) */
#define ADDPAGE(v) do { if (np == maxp) { u32 *q_ = grow(pages, &maxp, np, sizeof(u32)); \
        if (!q_) { err = f2p_fail("out of memory"); goto done; } pages = q_; } pages[np++] = (v); } while (0)
    for (i = 0; i < 25; i++)
        if (T(rb, 49 - i))
            ADDPAGE(T(rb, 49 - i));
    for (ext = T(rb, 24); ext; ext = L(b, NL - 1)) {
        if (++guard > 100000 || rd(r, ext, b) || claim(r, ext)) {
            err = f2p_fail("bad bitmap extension block %lu", UL(ext));
            goto done;
        }
        for (i = 0; i < NL - 1; i++)
            if (L(b, i))
                ADDPAGE(L(b, i));
    }
#undef ADDPAGE
    for (i = 0; i < np; i++) {
        if (rd(r, pages[i], b) || claim(r, pages[i]) || csum(b)) {
            err = f2p_fail("bitmap block %lu is bad", UL(pages[i]));
            goto done;
        }
        for (j = 1; j < NL; j++) {
            u32 v = L(b, j);
            for (k = 0; k < 32; k++, bit++)
                if (r->reserved + bit < r->n && (v & (1UL << k))) {
                    u32 sct, blk = r->reserved + bit;
                    if (BM_GET(&r->claimed, blk)) {
                        err = f2p_fail("block %lu is in use but marked free: the volume needs validating", UL(blk));
                        goto done;
                    }
                    for (sct = 0; sct < SPB; sct++)
                        BM_SET(&m->freemap, blk * SPB + sct);
                }
        }
    }
done:
    if (pages)
        f2p_free(pages);
    f2p_free(b);
    return err;
}

static int links(reader *r)
{
    model *m = r->m;
    u32 *keys, i, *cnt;
    int err = 0;
    keys = f2p_alloc(sizeof(u32) * 2 * (m->nentries + 1));
    cnt = f2p_alloc(sizeof(u32) * (m->nentries + 1));
    if (!keys || !cnt)
        return f2p_fail("out of memory");
    for (i = 0; i < m->nentries; i++) {
        keys[2 * i] = m->e[i].key;
        keys[2 * i + 1] = i;
    }
    /* sort (key, index) pairs by key */
    {
        u32 gap, j;
        for (gap = m->nentries / 2; gap > 0; gap /= 2)
            for (i = gap; i < m->nentries; i++)
                for (j = i; j >= gap && keys[2 * (j - gap)] > keys[2 * j]; j -= gap) {
                    u32 t0 = keys[2 * j], t1 = keys[2 * j + 1];
                    keys[2 * j] = keys[2 * (j - gap)]; keys[2 * j + 1] = keys[2 * (j - gap) + 1];
                    keys[2 * (j - gap)] = t0; keys[2 * (j - gap) + 1] = t1;
                }
    }
#define FIND(k, out) do { u32 lo_ = 0, hi_ = m->nentries; out = NOBLK; \
        while (lo_ < hi_) { u32 mid_ = (lo_ + hi_) / 2; \
            if (keys[2 * mid_] < (k)) lo_ = mid_ + 1; else hi_ = mid_; } \
        if (lo_ < m->nentries && keys[2 * lo_] == (k)) out = keys[2 * lo_ + 1]; } while (0)
    for (i = 0; i < m->nentries && !err; i++) {
        entry *e = &m->e[i];
        if (e->kind == K_HARD) {
            u32 t;
            FIND(e->link, t);
            if (t == NOBLK || m->e[t].kind == K_HARD)
                err = f2p_fail("hard link %s: target %lu is not a file or directory", m->pool + e->name, UL(e->link));
            else {
                e->link = t;
                cnt[t]++;
            }
        }
    }
    for (i = 0; i < m->nentries && !err; i++) {
        entry *e = &m->e[i];
        u32 k = e->next_link, steps = 0, t;
        if (e->kind == K_HARD)
            continue;
        while (k) {
            FIND(k, t);
            if (t == NOBLK || m->e[t].kind != K_HARD || m->e[t].link != i || ++steps > cnt[i]) {
                err = f2p_fail("%s: link chain broken", m->pool + e->name);
                break;
            }
            k = m->e[t].next_link;
        }
        if (!err && steps != cnt[i])
            err = f2p_fail("%s: link chain does not match its links", m->pool + e->name);
    }
#undef FIND
    f2p_free(keys);
    f2p_free(cnt);
    return err;
}

int ffs_read(blockdev *d, u32 start, u32 nblocks, u32 spb, u32 reserved, model *m)
{
    reader r;
    u8 *boot = NULL, *rb = NULL;
    u32 root = 0, cand[2], i;
    const u8 *s;
    u32 n;
    if (spb < 1 || spb > 64 || (spb & (spb - 1)))
        return f2p_fail("filesystem block size %lu bytes is not supported", UL(spb * 512));
    set_blocksize(spb);
    memset(&r, 0, sizeof(r));
    r.d = d; r.start = start; r.n = nblocks / spb; r.m = m;
    m->part_start = start;
    m->part_blocks = nblocks;
    m->spb = spb;
    boot = f2p_alloc(BSZ);
    rb = f2p_alloc(BSZ);
    if (!boot || !rb || bm_init(&r.claimed, r.n))
        goto fail;
    if (spb > 1)
        f2p_log("  %lu-byte filesystem blocks\n", UL(BSZ));
    if (rd(&r, 0, boot))
        goto fail;
    if (L(boot, 0) >> 8 != 0x444f53UL || boot[3] > 5) {
        f2p_fail("not an OFS/FFS volume (boot block %02x%02x%02x%02x)", boot[0], boot[1], boot[2], boot[3]);
        goto fail;
    }
    m->dostype = L(boot, 0);
    m->ofs = boot[3] == 0 || boot[3] == 2 || boot[3] == 4;
    cand[0] = (r.n - 1 + reserved) / 2;
    cand[1] = L(boot, 2);
    for (i = 0; i < 2 && !root; i++)
        if (cand[i] > 0 && cand[i] < r.n && !rd(&r, cand[i], rb)
            && L(rb, 0) == T_HEADER && (s32)T(rb, 1) == 1 && !csum(rb))
            root = cand[i];
    if (!root) {
        f2p_fail("no valid root block");
        goto fail;
    }
    n = bstr(rb, BSZ - 80, 30, &s);
    memcpy(m->volname, s, n);
    m->volname[n] = 0;
    for (i = 0; i < 3; i++) {
        m->rootdate[i] = T(rb, 23 - i);
        m->voldate_alt[i] = T(rb, 10 - i);
        m->voldate[i] = T(rb, 7 - i);
    }
    {
        u32 expected = 0;
        if (walk(&r, root, 1, &expected))
            goto fail;
        f2p_log("  %lu files and directories\n", UL(expected));
        if (expected && !(m->e = f2p_alloc(sizeof(entry) * expected))) {   /* once, the exact size */
            f2p_fail("out of memory (%lu KB for the entry table)", UL(sizeof(entry) * expected / 1024));
            goto fail;
        }
        r.maxent = expected;
        for (i = 0; i < reserved; i++)            /* the boot blocks */
        if (claim(&r, i))
            goto fail;
    r.reserved = reserved;
    if (claim(&r, root) || walk(&r, root, 0, &expected)
            || read_bitmap(&r, rb) || links(&r))
            goto fail;
    }
    bm_free(&r.claimed);
    f2p_free(boot); f2p_free(rb);
    return 0;
fail:
    bm_free(&r.claimed);
    if (boot) f2p_free(boot);
    if (rb) f2p_free(rb);
    if (!f2p_error[0]) f2p_fail("out of memory");
    return 1;
}

int ofs_file_data(blockdev *d, u32 start, const model *m, const entry *e, u8 *buf)
{
    u8 *b;
    u32 seq = 0, left = e->size, i, k;
    int err = 0;
    set_blocksize(m->spb);
    if (!(b = f2p_alloc(BSZ)))
        return f2p_fail("out of memory");
    for (i = 0; i < e->next && !err; i++) {
        const extent *x = &m->x[e->ext + i];
        for (k = x->start; k < x->start + x->count && !err; k++) {
            u32 n = left < OFS_DATA ? left : OFS_DATA;
            if (d->read(d, start + k * SPB, SPB, b)) {
                err = f2p_fail("read error at block %lu", UL(k));
                break;
            }
            seq++;
            if (L(b, 0) != T_DATA || L(b, 1) != e->key || L(b, 2) != seq || csum(b) || L(b, 3) != n) {
                err = f2p_fail("%s: OFS data block %lu (#%lu) is bad", m->pool + e->name, UL(k), UL(seq));
                break;
            }
            memcpy(buf, b + 24, n);
            buf += n;
            left -= n;
        }
    }
    f2p_free(b);
    if (err)
        return 1;
    return left ? f2p_fail("%s: %lu bytes missing", m->pool + e->name, UL(left)) : 0;
}

/* CRC32 of a file's contents, every block checked on the way (OFS headers).
   FFS extents are sectors; OFS extents are OFS blocks. Extents are read in
   runs of up to 128 sectors. */
#define CRCRUN 128
u32 file_crc(blockdev *d, u32 start, const model *m, const entry *e, int *err)
{
    u8 *b;
    u32 c = 0, left = e->size, i, k, seq = 0, per;
    *err = 0;
    set_blocksize(m->spb);
    per = m->ofs ? SPB : 1;                       /* sectors per extent unit */
    if (!(b = f2p_alloc(CRCRUN * BS + BSZ))) {
        *err = f2p_fail("out of memory");
        return 0;
    }
    for (i = 0; i < e->next && !*err; i++) {
        const extent *x = &m->x[e->ext + i];
        for (k = x->start; k < x->start + x->count && !*err;) {
            u32 units = x->start + x->count - k, j;
            if (units * per > CRCRUN)
                units = CRCRUN / per ? CRCRUN / per : 1;
            if (d->read(d, start + k * per, units * per, b)) {
                *err = f2p_fail("read error at block %lu", UL(k));
                break;
            }
            for (j = 0; j < units; j++) {
                const u8 *p = b + j * per * BS;
                if (m->ofs) {
                    u32 n = left < OFS_DATA ? left : OFS_DATA;
                    seq++;
                    if (L(p, 0) != T_DATA || L(p, 1) != e->key || L(p, 2) != seq || csum(p) || L(p, 3) != n) {
                        *err = f2p_fail("%s: OFS data block %lu (#%lu) is bad", m->pool + e->name, UL(k + j), UL(seq));
                        break;
                    }
                    c = crc32_update(c, p + 24, n);
                    left -= n;
                } else {
                    u32 n = left < BS ? left : BS;
                    c = crc32_update(c, p, n);
                    left -= n;
                }
            }
            k += units;
        }
    }
    f2p_free(b);
    return c;
}

void model_free(model *m)
{
    if (m->e) f2p_free(m->e);
    if (m->x) f2p_free(m->x);
    if (m->px) f2p_free(m->px);
    if (m->mv) f2p_free(m->mv);
    if (m->pool) f2p_free(m->pool);
    if (m->rdb.lsegs) f2p_free(m->rdb.lsegs);
    if (m->rdb.bin) f2p_free(m->rdb.bin);
    if (m->bkx) f2p_free(m->bkx);
    if (m->bkflag) f2p_free(m->bkflag);
    bm_free(&m->freemap);
    memset(m, 0, sizeof(*m));
}
