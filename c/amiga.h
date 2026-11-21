/* ffs2pfs for AmigaOS: what the Shell front end (amiga.c) and the window (gui.c) share */
#ifndef AMIGA_H
#define AMIGA_H

#include <exec/types.h>
#include <exec/ports.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>
#include "f2p.h"

/* a disk through its exec device */
typedef struct {
    struct MsgPort *port, *tport;
    struct IOExtTD *io;
    struct timerequest *tr;
    int open, topen;
    int mode;                            /* 0: 32-bit offsets, 1: TD64, 2: NSD */
    u8 *buf;                             /* XFER sectors, of the partition's BufMemType */
    u32 maxsec;                          /* sectors per transfer (MaxTransfer, at most XFER) */
    int noupdate;
} disk;

int disk_open(disk *k, const char *dev, u32 unit, u32 flags, u32 memtype, u32 maxtransfer);
void disk_close(disk *k);                /* also on a disk that did not open (zeroed) */
void disk_blockdev(disk *k, blockdev *d);   /* d reads and writes k */

/* one run: what the Shell arguments say */
typedef struct {
    const char *part;                    /* PARTITION: "DH1" or "DH1:" */
    int mode;                            /* F2P_CONVERT, F2P_DRY (ANALYSE), F2P_ROLLBACK */
    const char *bin;                     /* PFS3AIO=, or NULL: the built-in one */
    const char *device;                  /* DEVICE=, or NULL: from the DOS list */
    u32 unit;
    int noremount, boot;
    const char *log;
    const char *image, *torn, *failat;   /* tests: a disk image file (IMAGE=, TORN=, FAILAT=) */
    long crash_after;                    /* CRASHAFTER=, -1: none */
} runopts;

int ffs2pfs_run(const runopts *o);       /* the AmigaDOS return code: 0, 5, 10 or 20 */
extern char run_why[200];                /* why the last run did not finish, for the window */
const char *ffs2pfs_version(void);       /* "ffs2pfs 0.3 (20.11.2026)" */

/* set by the window: the messages and the progress go there instead of the Shell */
extern void (*fe_log)(const char *text);
extern void (*fe_progress)(const char *step, u32 done, u32 total, const char *item);

int gui_main(void);                      /* gui.c: the window; the return code */

#endif
