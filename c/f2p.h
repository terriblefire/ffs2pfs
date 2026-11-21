/* ffs2pfs: crash-safe in-place OFS/FFS -> PFS3 conversion.
 * The portable core (no AmigaOS calls). The front end (host.c for the tests,
 * amiga.c for the real thing) supplies block I/O, logging and memory. */
#ifndef F2P_H
#define F2P_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;
#define UL(x) ((unsigned long)(x))          /* for %lu */

#define BS 512

/* ------------------------------------------------------------------ front end */
typedef struct blockdev {
    void *ctx;
    u32 nblocks;
    int (*read)(struct blockdev *d, u32 blk, u32 n, u8 *buf);          /* 0 = ok */
    int (*write)(struct blockdev *d, u32 blk, u32 n, const u8 *buf);   /* 0 = ok */
    int (*flush)(struct blockdev *d);  /* everything written so far is on the medium (drive cache too); 0 = ok */
} blockdev;

void f2p_log(const char *fmt, ...);            /* front end */
void *f2p_alloc(size_t n);                     /* front end: zeroed memory or NULL */
void f2p_free(void *p);
int f2p_break(void);                           /* front end: user asked to stop (Ctrl-C) */
/* front end: progress of a long step. total 0 = not known; item may be NULL.
   Called often: the front end decides how often to show it. */
void f2p_progress(const char *step, u32 done, u32 total, const char *item);
/* the same, with "file x of y" in front of the item */
void f2p_progress_file(const char *step, u32 done, u32 total, u32 file, u32 files, const char *name);

/* errors: functions return 0 on success, an error code otherwise; f2p_error holds the text */
extern char f2p_error[200];
int f2p_fail(const char *fmt, ...);            /* sets f2p_error, returns 1 */
#define E_STOP 2                               /* stopped at a safe point (Ctrl-C) */
#define E_ROLLEDBACK 3                         /* failed, but the volume is as it was (rolled back, or never changed) */

/* ------------------------------------------------------------------ helpers */
u32 crc32_update(u32 crc, const u8 *p, size_t n);
#define crc32(p, n) crc32_update(0, (p), (n))
u32 rd32(const u8 *p);
u16 rd16(const u8 *p);
void wr32(u8 *p, u32 v);
void wr16(u8 *p, u16 v);
#define L(b, i) rd32((b) + 4 * (i))            /* longword i of a block */

/* bit arrays */
typedef struct { u8 *bits; u32 n; } bitmap;
int bm_init(bitmap *m, u32 n);
void bm_free(bitmap *m);
#define BM_GET(m, k) (((m)->bits[(k) >> 3] >> ((k) & 7)) & 1)
#define BM_SET(m, k) ((m)->bits[(k) >> 3] |= (u8)(1 << ((k) & 7)))
#define BM_CLR(m, k) ((m)->bits[(k) >> 3] &= (u8)~(1 << ((k) & 7)))

/* ------------------------------------------------------------------ RDB */
#define NOBLK 0xffffffffUL
#define DOSTYPE_CONVERTING 0x46325000UL    /* 'F2P\0' */
#define DOSTYPE_PFS3 0x50465303UL          /* 'PFS\3' */
#define ID_PFS1 0x50465301UL

typedef struct {
    u32 block, start, blocks, dostype;
    u32 spb;                       /* sectors per filesystem block (DosEnvec SectorPerBlock) */
    u32 reserved, prealloc;        /* DosEnvec Reserved (boot blocks) and PreAlloc */
    char name[32];
    int invalid;
} partinfo;

int rdb_checksum(u8 *b);                       /* set the checksum, returns 0 */
int rdb_valid(const u8 *b);
int rdb_find_rdsk(blockdev *d, u32 *blk, u8 *rdsk);   /* 1 = found a valid RDSK */
int rdb_partitions(blockdev *d, partinfo *out, int max);   /* count, or -1 */
int rdb_partitions_from(blockdev *d, const u8 *rdsk, partinfo *out, int max);   /* the list of this RDSK */

/* pfs3aio into the RDB */
typedef struct {
    int existing;                  /* PFS\3 filesystem already in the RDB */
    u32 fshd, nlseg, *lsegs;       /* new FSHD and LSEG blocks */
    u32 rescue[2];                 /* rescue copy, marker */
    u32 rdsk;                      /* RDSK block */
    u32 linkblk;                   /* last FSHD (NOBLK: link from the RDSK) */
    u8 linkorig[BS], rdskorig[BS]; /* their contents before */
    u32 binlen; u8 *bin;           /* the pfs3aio binary */
    u32 high, rdbhi;               /* new HighRDSKBlock, RDBBlocksHi */
    u32 nwrites;
} rdbfs_plan;
int rdbfs_make(blockdev *d, const u8 *bin, u32 binlen, u32 version, rdbfs_plan *r);
void rdbfs_write_n(const rdbfs_plan *r, u32 i, u32 *blk, u8 *out);   /* write #i of r->nwrites */
int rdbfs_find_rescue(blockdev *d, u32 *rblk, u8 *rdsk);
void rdbfs_marker(const rdbfs_plan *r, const u8 *rdsk, u8 *out);   /* rescue marker for rdsk held in rescue[0] */

/* ------------------------------------------------------------------ the volume model */
enum { K_FILE, K_DIR, K_SOFT, K_HARD };

typedef struct {
    u32 key, protect, days, mins, ticks, size;
    s32 parent;                    /* entry index, -1 = root */
    u32 name, comment, link;       /* string pool offsets; link: soft link path, or target index (hard) */
    u32 ext, next;                 /* data extents (OFS/FFS blocks) */
    u32 pext, npext;               /* PFS3 extents: OFS copy target / soft link block */
    u32 next_link, crc;
    u8 kind, nlen, clen, llen;
} entry;

typedef struct { u32 start, count; } extent;
typedef struct { u32 src, dst; } move;

typedef struct {
    /* the partition */
    u32 part_blocks, part_start, part_block, orig_dostype;
    u8 part_orig[BS];
    u8 boot_orig[2][BS];           /* blocks 0 and 1 before the conversion */
    /* the old volume */
    u32 dostype, ofs;
    u32 spb;                       /* sectors (512 bytes) per OFS/FFS block: 1, 2, 4 .. 64 */
    char volname[32];
    u32 rootdate[3], voldate[3], voldate_alt[3];
    /* PFS3 layout (format.c) */
    u32 resblocksize, numreserved, end;
    /* contents */
    u32 nentries, nextents, npext, nmoves, poolsize;
    entry *e;
    extent *x, *px;
    move *mv;
    char *pool;
    rdbfs_plan rdb;
    /* rollback copy (free blocks): what the PFS3 build and the RDB install overwrite and
       a rollback needs back. Candidates: the PFS3 sectors in write order, then the new
       RDB blocks (LSEGs, FSHD, rescue pair); bkflag bit set = kept in the copy (an FFS
       block in use / an RDB block that was not all zeros). */
    u32 nflag, nbk, nbkx;          /* candidates, blocks kept, runs */
    u8 *bkflag;
    extent *bkx;
    /* while reading only */
    bitmap freemap;                /* old volume: 1 = free */
} model;

void model_free(model *m);
/* FFS: extents are 512-byte sector runs, trimmed to the file size.
   OFS: extents are OFS blocks (spb sectors each), as the copy reads them. */
int ffs_read(blockdev *d, u32 start, u32 nblocks, u32 spb, u32 reserved, model *m);
int ofs_file_data(blockdev *d, u32 start, const model *m, const entry *e, u8 *buf);   /* whole file into buf */
u32 file_crc(blockdev *d, u32 start, const model *m, const entry *e, int *err);

/* plan.c */
void pfs3_layout(u32 total, u32 *resblocksize, u32 *numreserved, u32 *end);
int plan_build(blockdev *d, model *m);         /* targets, moves, checks, CRCs */
u32 plan_size(const model *m);                 /* bytes, a multiple of BS */
int plan_write(blockdev *d, u32 at, const model *m, u32 *crc);   /* absolute block */
int plan_read(blockdev *d, u32 at, u32 nblocks, model *m);
int plan_check(blockdev *d, u32 at, u32 nblocks);   /* CRC only, no memory */

/* pfs3.c: the PFS3 metadata, block by block */
typedef struct pfs3b pfs3b;
pfs3b *pfs3_begin(const model *m);             /* NULL: f2p_error */
u32 pfs3_nwrites(const pfs3b *b);
u32 pfs3_sector(const pfs3b *b, u32 i);        /* the sector write #i goes to, without making it */
int pfs3_block(pfs3b *b, u32 i, u32 *sector, u8 *out);   /* partition-relative sector */
void pfs3_end(pfs3b *b);
void pfs3_boot0(u8 *b);
int softlink_block(const model *m, const entry *e, u32 *blk, u8 *out);

/* convert.c */
enum { F2P_CONVERT, F2P_DRY, F2P_ROLLBACK };
int f2p_run(blockdev *d, const char *name, const u8 *bin, u32 binlen, int mode);

/* where each partition of a disk stands, reading only (for a front end's list).
   P_NONE: no conversion on it (OFS, FFS, or any other file system: see p.dostype).
   P_STOPPED: cut off at step `step` (3..6 of 6): ffs2pfs carries on, or ROLLBACK.
   P_UNDO: a rollback was cut off: ROLLBACK (or ffs2pfs) finishes it.
   P_FINISH: past the point of no return, only the last write left: ffs2pfs finishes it.
   P_BAD: the partition or its marks cannot be read (f2p_error). */
enum { P_NONE, P_PFS3, P_STOPPED, P_UNDO, P_FINISH, P_BAD };
typedef struct {
    partinfo p;
    int state;
    u32 step;
    u32 orig_dostype;              /* the DosType before the conversion (p.dostype is the RDB's now) */
    char why[80];                  /* P_BAD: what could not be read */
} f2p_part;
/* rescue: a torn RDSK block is read from its rescue copy (a search of up to 32 MB) */
int f2p_probe(blockdev *d, int rescue, f2p_part *out, int max);   /* count, or -1: no valid RDSK */

#endif
