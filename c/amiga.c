/* ffs2pfs for AmigaOS: the Shell front end. With no arguments, or from
 * Workbench, it opens its window instead (gui.c).
 *
 *   ffs2pfs DH1: [ANALYSE] [ROLLBACK] [PFS3AIO=file] [DEVICE=scsi.device UNIT=n] [NOREMOUNT]
 *
 * Converts the OFS/FFS partition DH1 to PFS3 in place, or resumes a conversion
 * that was cut off. pfs3aio (the handler that goes into the RDB) is built in;
 * PFS3AIO= uses another one. If anything fails the volume is rolled back to what
 * it was; ROLLBACK does the same for a conversion that was cut off (power cut,
 * Ctrl-C), until its last step. DEVICE/UNIT find the disk when the partition is
 * not mounted (after a crash the RDB holds DosType 'F2P\0', which nothing mounts).
 * After the commit the partition is mounted again as PFS3, write-protected
 * until the next reboot. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/rdargs.h>
#include <dos/dostags.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <resources/filesysres.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "amiga.h"

static const char version[] = "$VER: ffs2pfs 0.3 (20.11.2026)";
extern const unsigned char pfs3aio_bin[];          /* pfs3aio_bin.c: ref/pfs3aio, made by the Makefile */
extern const unsigned long pfs3aio_len;
size_t __stack = 65536;                  /* the vbcc startup code gives us this much */

#define TEMPLATE "PARTITION,ANALYSE=DRY/S,ROLLBACK/S,PFS3AIO/K,DEVICE/K,UNIT/K/N,NOREMOUNT/S,LOG/K,BOOT/S,IMAGE/K," \
                 "CRASHAFTER/K/N,TORN/K,FAILAT/K"
enum { A_PART, A_DRY, A_ROLLBACK, A_BIN, A_DEV, A_UNIT, A_NOREMOUNT, A_LOG, A_BOOT, A_IMAGE, A_CRASH, A_TORN, A_FAILAT, A_N };

struct ExpansionBase *ExpansionBase;
void (*fe_log)(const char *text);
void (*fe_progress)(const char *step, u32 done, u32 total, const char *item);
char run_why[200];

const char *ffs2pfs_version(void)
{
    return version + 6;
}

/* ------------------------------------------------------------------ core hooks */
static BPTR logfile;                     /* LOG=: a copy of the messages */
static struct {
    const char *step;
    unsigned long long t0, last;         /* ticks (1/50 s) */
    int active, interactive, lastpct;
} prog;

static unsigned long long ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ((unsigned long long)ds.ds_Days * 1440 + ds.ds_Minute) * 3000 + ds.ds_Tick;
}

/* watchdog: what the main task is doing, for a second process that reports a stall */
static volatile u32 hb_count, hb_block;
static volatile const char *hb_where = "starting";
static volatile int wd_quit;
static struct Task *wd_task, *main_task;
static char wd_logname[256];

static void out(const char *s, int tolog)
{
    hb_where = "console output";
    PutStr((STRPTR)s);
    Flush(Output());
    if (tolog && logfile) {
        hb_where = "log write";
        Write(logfile, (APTR)s, strlen(s));
        Flush(logfile);
    }
    hb_where = "working";
    hb_count++;
}

static void wd_write(const char *m)
{
    BPTR f;
    if (wd_logname[0] && (f = Open((STRPTR)wd_logname, MODE_READWRITE))) {
        Seek(f, 0, OFFSET_END);
        Write(f, (APTR)m, strlen(m));
        Close(f);
    }
}

static void wd_entry(void)
{
    u32 last = hb_count, still = 0, t = 0;
    char m[160];
    while (!wd_quit) {
        Delay(250);                                   /* 5 s */
        if (wd_quit)
            break;
        if ((t += 5) % 30 == 0) {                     /* heartbeat: memory over time shows a leak */
            snprintf(m, sizeof(m), "heartbeat %lu s: %s, block %lu, free %lu KB, largest %lu KB\n", UL(t),
                     (const char *)hb_where, UL(hb_block), UL(AvailMem(MEMF_ANY) / 1024), UL(AvailMem(MEMF_LARGEST) / 1024));
            wd_write(m);
        }
        if (hb_count != last) {
            last = hb_count;
            still = 0;
            continue;
        }
        still += 5;
        if (still >= 10) {
            snprintf(m, sizeof(m), "watchdog: no progress for %lu s, main task in: %s, block %lu\n",
                     UL(still), (const char *)hb_where, UL(hb_block));
            wd_write(m);
        }
    }
    Forbid();
    wd_task = NULL;
    Signal(main_task, SIGBREAKF_CTRL_F);              /* tell main we are gone */
}

static void wd_start(const char *logname)
{
    main_task = FindTask(NULL);
    wd_quit = 0;
    if (!logname)
        return;
    strncpy(wd_logname, logname, sizeof(wd_logname) - 1);
    wd_task = (struct Task *)CreateNewProcTags(NP_Entry, (ULONG)wd_entry, NP_Name, (ULONG)"ffs2pfs watchdog",
                                               NP_StackSize, 8192, NP_Priority, 1, TAG_END);
}

static void wd_stop(void)
{
    if (!wd_task)
        return;
    wd_quit = 1;
    while (wd_task)
        Wait(SIGBREAKF_CTRL_F);
}

void f2p_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (fe_log) {                        /* the window */
        fe_log(buf);
        if (logfile) {
            Write(logfile, buf, strlen(buf));
            Flush(logfile);
        }
        return;
    }
    if (prog.active) {                   /* finish the progress line first */
        out("\n", 0);
        prog.active = 0;
    }
    out(buf, 1);
}

void f2p_progress(const char *step, u32 done, u32 total, const char *item)
{
    char line[160], eta[24] = "";
    unsigned long long now = ticks();
    int force = 0, pct = total ? (int)((unsigned long long)done * 100 / total) : 0;
    if (fe_progress) {                   /* the window */
        fe_progress(step, done, total, item);
        return;
    }
    if (step != prog.step) {
        if (prog.active)
            out("\n", 0);
        prog.active = 0;
        prog.step = step;
        prog.t0 = now;
        prog.lastpct = -1;
        force = 1;
    }
    if (prog.interactive) {
        if (!force && done != total && now - prog.last < 10)       /* five times a second */
            return;
    } else if (!force && done != total && pct / 10 == prog.lastpct / 10)
        return;                                                     /* not a console: every 10% */
    prog.last = now;
    prog.lastpct = pct;
    if (total && done && now - prog.t0 > 100) {
        unsigned long long el = (now - prog.t0) / 50, left = el * (total - done) / done;
        snprintf(eta, sizeof(eta), ", %lu:%02lu left", UL(left / 60), UL(left % 60));
    }
    if (total)
        snprintf(line, sizeof(line), "  %s %d%%%s  %s", step, pct, eta, item ? item : "");
    else
        snprintf(line, sizeof(line), "  %s %lu  %s", step, UL(done), item ? item : "");
    line[76] = 0;                         /* one line of an 80-column Shell: no wrapping */
    if (prog.interactive) {
        out("\r", 0);
        out(line, 0);
        out("\x9bK", 0);                /* CSI K: clear to the end of the line */
        prog.active = 1;
    } else {
        out(line, 0);
        out("\n", 0);
    }
    if (done == total && total && logfile) {    /* the log gets one line per finished step */
        Write(logfile, line, strlen(line));
        Write(logfile, "\n", 1);
        Flush(logfile);
    }
}

void *f2p_alloc(size_t n) { return AllocVec(n ? n : 1, MEMF_PUBLIC | MEMF_CLEAR); }
void f2p_free(void *p) { FreeVec(p); }
int f2p_break(void) { return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0; }

/* ------------------------------------------------------------------ the disk */

/* one I/O request, with a watchdog: a request that takes more than 5 s is
   reported on the console every 5 s (a hung or very slow drive is then visible) */
static int do_io(disk *k, u32 blk)
{
    u32 waited = 0;
    ULONG sigs, brk = 0;
    /* a reused request still says "replied" (NT_REPLYMSG) from last time; a device that
       answers later without resetting it would make CheckIO/WaitIO think this one is done */
    k->io->iotd_Req.io_Message.mn_Node.ln_Type = NT_UNKNOWN;
    if (!k->topen)
        return DoIO((struct IORequest *)k->io);
    SendIO((struct IORequest *)k->io);
    for (;;) {
        k->tr->tr_node.io_Message.mn_Node.ln_Type = NT_UNKNOWN;
        k->tr->tr_node.io_Command = TR_ADDREQUEST;
        k->tr->tr_time.tv_secs = 5;
        k->tr->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)k->tr);
        sigs = Wait((1UL << k->port->mp_SigBit) | (1UL << k->tport->mp_SigBit) | SIGBREAKF_CTRL_C);
        brk |= sigs & SIGBREAKF_CTRL_C;
        if (CheckIO((struct IORequest *)k->io)) {
            WaitIO((struct IORequest *)k->io);
            if (!CheckIO((struct IORequest *)k->tr))
                AbortIO((struct IORequest *)k->tr);
            WaitIO((struct IORequest *)k->tr);
            break;
        }
        if (CheckIO((struct IORequest *)k->tr)) {
            WaitIO((struct IORequest *)k->tr);
            waited += 5;
            f2p_log("waiting for the disk: block %lu, %lu s\n", UL(blk), UL(waited));
        } else {
            AbortIO((struct IORequest *)k->tr);
            WaitIO((struct IORequest *)k->tr);
        }
    }
    if (brk)
        SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);   /* keep Ctrl-C for the next safe point */
    return k->io->iotd_Req.io_Error;
}

#define XFER 128                                 /* sectors: 64 KB transfers */

static int io_rw(blockdev *d, u32 blk, u32 n, u8 *data, int wr)
{
    disk *k = d->ctx;
    u32 i, cnt;
    int r;
    hb_where = wr ? "disk write" : "disk read";
    r = 0;
    for (i = 0; i < n && !r; i += cnt, blk += cnt, data += cnt * BS) {
        u32 hi = blk >> 23, lo = blk << 9;            /* byte offset, 64 bits */
        cnt = n - i < k->maxsec ? n - i : k->maxsec;
        if (!hi && blk + cnt > 0x800000UL)           /* do not cross 4 GB in one 32-bit request */
            cnt = 0x800000UL - blk;
        hb_block = blk;
        hb_count++;
        if (wr)
            CopyMem(data, k->buf, cnt * BS);
        k->io->iotd_Req.io_Data = k->buf;
        k->io->iotd_Req.io_Length = cnt * BS;
        k->io->iotd_Req.io_Offset = lo;
        if (!hi)                                      /* below 4 GB: plain commands, as FFS and pfs3aio use */
            k->io->iotd_Req.io_Command = wr ? CMD_WRITE : CMD_READ;
        else if (k->mode == 2) {
            k->io->iotd_Req.io_Command = wr ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64;
            k->io->iotd_Req.io_Actual = hi;
        } else if (k->mode == 1) {
            k->io->iotd_Req.io_Command = wr ? TD_WRITE64 : TD_READ64;
            k->io->iotd_Req.io_Actual = hi;
        } else
            return 1;                                 /* beyond 4 GB and no 64-bit commands */
        if (do_io(k, blk) || k->io->iotd_Req.io_Actual != cnt * BS)
            r = 1;
        else if (!wr)
            CopyMem(k->buf, data, cnt * BS);
    }
    hb_where = "working";
    return r;
}

static int d_read(blockdev *d, u32 blk, u32 n, u8 *buf) { return io_rw(d, blk, n, buf, 0); }
static int d_write(blockdev *d, u32 blk, u32 n, const u8 *buf) { return io_rw(d, blk, n, (u8 *)buf, 1); }

static int d_flush(blockdev *d)
{
    disk *k = d->ctx;
    if (k->noupdate)
        return 0;
    k->io->iotd_Req.io_Command = CMD_UPDATE;
    k->io->iotd_Req.io_Length = 0;
    hb_where = "disk flush";
    hb_count++;
    if (do_io(k, 0)) {
        if (k->io->iotd_Req.io_Error == IOERR_NOCMD) {
            f2p_log("note: the device has no CMD_UPDATE: writes are not flushed through a drive cache\n");
            k->noupdate = 1;
            return 0;
        }
        return 1;
    }
    return 0;
}

void disk_blockdev(disk *k, blockdev *d)
{
    d->ctx = k;
    d->nblocks = 0xffffffffUL;
    d->read = d_read;
    d->write = d_write;
    d->flush = d_flush;
}

void disk_close(disk *k)
{
    if (k->topen)
        CloseDevice((struct IORequest *)k->tr);
    if (k->tr)
        DeleteIORequest((struct IORequest *)k->tr);
    if (k->tport)
        DeleteMsgPort(k->tport);
    if (k->open)
        CloseDevice((struct IORequest *)k->io);
    if (k->io)
        DeleteIORequest((struct IORequest *)k->io);
    if (k->port)
        DeleteMsgPort(k->port);
    if (k->buf)
        FreeMem(k->buf, XFER * BS);
}

int disk_open(disk *k, const char *dev, u32 unit, u32 flags, u32 memtype, u32 maxtransfer)
{
    memset(k, 0, sizeof(*k));
    k->maxsec = maxtransfer / BS;
    if (k->maxsec > XFER) k->maxsec = XFER;
    if (k->maxsec < 1) k->maxsec = 1;
    if (!(k->port = CreateMsgPort()) || !(k->io = (struct IOExtTD *)CreateIORequest(k->port, sizeof(struct IOExtTD)))
        || !(k->buf = AllocMem(XFER * BS, memtype | MEMF_PUBLIC)))
        return f2p_fail("out of memory");
    if (OpenDevice((STRPTR)dev, unit, (struct IORequest *)k->io, flags))
        return f2p_fail("cannot open %s unit %lu", dev, UL(unit));
    k->open = 1;
    if ((k->tport = CreateMsgPort()) && (k->tr = (struct timerequest *)CreateIORequest(k->tport, sizeof(struct timerequest)))
        && !OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)k->tr, 0))
        k->topen = 1;
    {   /* 64-bit access: NSD if the device says so, else TD64 if it takes the command */
        struct NSDeviceQueryResult q;
        memset(&q, 0, sizeof(q));
        k->io->iotd_Req.io_Message.mn_Node.ln_Type = NT_UNKNOWN;
        k->io->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
        k->io->iotd_Req.io_Data = &q;
        k->io->iotd_Req.io_Length = sizeof(q);
        if (!DoIO((struct IORequest *)k->io) && q.nsdqr_SupportedCommands) {
            UWORD *c;
            for (c = q.nsdqr_SupportedCommands; *c; c++)
                if (*c == NSCMD_TD_READ64)
                    k->mode = 2;
        }
        if (!k->mode) {
            k->io->iotd_Req.io_Message.mn_Node.ln_Type = NT_UNKNOWN;
            k->io->iotd_Req.io_Command = TD_READ64;
            k->io->iotd_Req.io_Data = k->buf;
            k->io->iotd_Req.io_Length = BS;
            k->io->iotd_Req.io_Offset = 0;
            k->io->iotd_Req.io_Actual = 0;
            if (!DoIO((struct IORequest *)k->io))
                k->mode = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ a disk image file (IMAGE=)
 * The disk is a file read through dos.library: for tests on a host under vamos,
 * or for an image on any Amiga volume. CRASHAFTER=n (tests): the (n+1)th 512-byte
 * block write is not done, or done torn (TORN=garbage|offset), and the program
 * exits with return code 3, as a power cut would leave the disk. FAILAT=n[,n..] (tests):
 * block write attempt n (from 0) fails, once. Up to 2 GB. */
static BPTR imgfh;
static long crash_after = -1, img_writes, fail_at[4] = { -1, -1, -1, -1 }, img_attempts;
static int torn_mode;                    /* 0 clean, -1 garbage, >0 bytes */

static int img_seek(u32 blk)
{
    if (blk >= 0x400000UL)
        return f2p_fail("block %lu: images over 2 GB are not supported", UL(blk));
    return Seek(imgfh, (LONG)(blk << 9), OFFSET_BEGINNING) < 0 ? f2p_fail("seek error") : 0;
}

static int img_read(blockdev *d, u32 blk, u32 n, u8 *buf)
{
    hb_where = "image read";
    if (img_seek(blk) || Read(imgfh, buf, (LONG)n * BS) != (LONG)n * BS)
        return 1;
    hb_count++;
    hb_where = "working";
    return 0;
}

static int img_write(blockdev *d, u32 blk, u32 n, const u8 *buf)
{
    u32 i;
    hb_where = "image write";
    for (i = 0; i < n; i++, blk++, buf += BS) {
        int f;
        for (f = 0; f < 4; f++)
            if (fail_at[f] == img_attempts) {
                img_attempts++;
                return f2p_fail("write error at block %lu (FAILAT)", UL(blk));
            }
        img_attempts++;
        if (crash_after >= 0 && img_writes >= crash_after) {
            if (torn_mode) {
                u8 t[BS];
                if (torn_mode < 0) {
                    u32 j, x = blk * 2654435761UL + 1;
                    for (j = 0; j < BS; j++) { x = x * 1103515245UL + 12345; t[j] = (u8)(x >> 16); }
                } else {
                    img_seek(blk);
                    Read(imgfh, t, BS);
                    memcpy(t, buf, torn_mode);
                }
                img_seek(blk);
                Write(imgfh, t, BS);
            }
            Close(imgfh);
            exit(3);
        }
        if (img_seek(blk) || Write(imgfh, (APTR)buf, BS) != BS)
            return f2p_fail("write error at block %lu", UL(blk));
        img_writes++;
    }
    hb_count++;
    hb_where = "working";
    return 0;
}

/* ------------------------------------------------------------------ the DOS side */
typedef struct {
    char dosname[34];                    /* "DH1:" */
    char partname[32];                   /* "DH1" */
    char device[64];
    u32 unit, flags, memtype, maxtransfer;
    int mounted;
} target;

static void bstrcpy(char *out, BSTR b, int max)
{
    UBYTE *s = BADDR(b);
    int n = s[0] < max - 1 ? s[0] : max - 1;
    memcpy(out, s + 1, n);
    out[n] = 0;
}

static int find_dos_device(target *t)
{
    struct DosList *dl;
    int ok = 0;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    dl = FindDosEntry(dl, (STRPTR)t->partname, LDF_DEVICES);
    if (dl) {
        struct FileSysStartupMsg *fssm = BADDR(dl->dol_misc.dol_handler.dol_Startup);
        if ((ULONG)fssm > 1024 && TypeOfMem(fssm)) {
            struct DosEnvec *env = BADDR(fssm->fssm_Environ);
            bstrcpy(t->device, fssm->fssm_Device, sizeof(t->device));
            t->unit = fssm->fssm_Unit;
            t->flags = fssm->fssm_Flags;
            t->memtype = env->de_TableSize >= DE_MEMBUFTYPE ? env->de_BufMemType : MEMF_PUBLIC;
            t->maxtransfer = env->de_TableSize >= DE_MAXTRANSFER ? env->de_MaxTransfer : 0x1fe00;
            t->mounted = 1;
            ok = 1;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return ok;
}

/* is `path` on the same volume as the target? */
static int on_target(const target *t, const char *path)
{
    BPTR a = Lock((STRPTR)path, SHARED_LOCK), b = Lock((STRPTR)t->dosname, SHARED_LOCK);
    int same = 0;
    if (a && b)
        same = SameLock(a, b) == LOCK_SAME_VOLUME || SameLock(a, b) == LOCK_SAME;
    if (a) UnLock(a);
    if (b) UnLock(b);
    return same;
}

static int is_boot_volume(const target *t)
{
    return on_target(t, "SYS:");
}

static int progdir_on_target(const target *t)
{
    BPTR pd = GetProgramDir(), b = Lock((STRPTR)t->dosname, SHARED_LOCK);
    int same = 0;
    if (pd && b)
        same = SameLock(pd, b) == LOCK_SAME_VOLUME || SameLock(pd, b) == LOCK_SAME;
    if (b) UnLock(b);
    return same;
}

static u8 *load_file(const char *path, u32 *len)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    u8 *p = NULL;
    LONG n;
    if (!f)
        return NULL;
    Seek(f, 0, OFFSET_END);
    n = Seek(f, 0, OFFSET_BEGINNING);
    if (n > 0 && (p = AllocVec(n, MEMF_PUBLIC)) && Read(f, p, n) != n) {
        FreeVec(p);
        p = NULL;
    }
    Close(f);
    *len = (u32)n;
    return p;
}

/* After the commit: take the old (FFS) entry out of the DOS list and mount the
 * partition again from its PART block with pfs3aio, write-protected until reboot.
 * FFS cannot be told to quit (it has no ACTION_DIE): it stays inhibited, idle. */
static void remount(const target *t, blockdev *d, const u8 *bin, u32 binlen)
{
    partinfo parts[32];
    u8 pb[BS];
    int n, i;
    BPTR seg = 0;
    ULONG *pkt;
    struct DeviceNode *dn;
    struct FileSysResource *fsr = OpenResource((STRPTR)FSRNAME);
    char tmp[] = "T:ffs2pfs.pfs3aio";
    n = rdb_partitions(d, parts, 32);
    for (i = 0; i < n && strcmp(parts[i].name, t->partname); i++)
        ;
    if (i == n || d->read(d, parts[i].block, 1, pb)) {
        f2p_log("remount: partition not found in the RDB; reboot to use it\n");
        return;
    }
    if (fsr) {                                         /* pfs3aio already loaded for another partition? */
        struct FileSysEntry *fse;
        Forbid();
        for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; fse->fse_Node.ln_Succ;
             fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ)
            if (fse->fse_DosType == DOSTYPE_PFS3 && fse->fse_SegList) {
                seg = fse->fse_SegList;
                break;
            }
        Permit();
    }
    if (!seg && bin) {                                 /* else the binary the RDB now holds */
        BPTR f = Open((STRPTR)tmp, MODE_NEWFILE);
        if (f) {
            Write(f, (APTR)bin, binlen);
            Close(f);
            seg = LoadSeg((STRPTR)tmp);
            DeleteFile((STRPTR)tmp);
        }
    }
    if (!seg) {
        f2p_log("remount: pfs3aio not loaded; reboot to use %s\n", t->dosname);
        return;
    }
    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 36)))
        return;
    pkt = AllocVec(sizeof(ULONG) * (4 + 20), MEMF_PUBLIC | MEMF_CLEAR);
    if (!pkt)
        goto out;
    pkt[0] = (ULONG)t->partname;
    pkt[1] = (ULONG)t->device;
    pkt[2] = t->unit;
    pkt[3] = t->flags;
    for (i = 0; i < 20 && i <= (int)rd32(pb + 128); i++)    /* the DosEnvec of the PART block */
        pkt[4 + i] = rd32(pb + 128 + 4 * i);
    pkt[4 + DE_DOSTYPE] = DOSTYPE_PFS3;
    {   /* the old entry out of the DOS list */
        struct DosList *dl = LockDosList(LDF_DEVICES | LDF_WRITE);
        dl = FindDosEntry(dl, (STRPTR)t->partname, LDF_DEVICES);
        if (dl)
            RemDosEntry(dl);
        UnLockDosList(LDF_DEVICES | LDF_WRITE);
    }
    dn = MakeDosNode(pkt);
    if (!dn) {
        f2p_log("remount: MakeDosNode failed; reboot to use %s\n", t->dosname);
        goto out;
    }
    dn->dn_SegList = seg;
    dn->dn_GlobalVec = -1;
    if (!AddDosNode((LONG)(s32)rd32(pb + 128 + 4 * DE_BOOTPRI), ADNF_STARTPROC, dn)) {
        f2p_log("remount: AddDosNode failed; reboot to use %s\n", t->dosname);
        goto out;
    }
    {
        struct MsgPort *hp = DeviceProc((STRPTR)t->dosname);
        if (hp && DoPkt(hp, ACTION_WRITE_PROTECT, DOSTRUE, 0x46325046UL, 0, 0, 0))
            f2p_log("%s is mounted as PFS3, read-only until the next reboot\n", t->dosname);
        else
            f2p_log("%s mounted as PFS3 (could not write-protect it: do not write to it before a reboot)\n", t->dosname);
    }
out:
    if (pkt)
        FreeVec(pkt);
    CloseLibrary((struct Library *)ExpansionBase);
}

/* "$VER: name x.y (date)" in a binary, without the "$VER: ". The search key has no "$" in
   it here, so that the Version command finds ffs2pfs's own string, not this one */
static const char *ver_of(const u8 *b, u32 n)
{
    static char v[80];
    u32 i, j;
    for (i = 0; i + 6 < n; i++)
        if (b[i] == '$' && !memcmp(b + i + 1, "VER: ", 5)) {
            for (j = 0; j < sizeof(v) - 1 && i + 6 + j < n && b[i + 6 + j] >= ' ' && b[i + 6 + j] != ')'; j++)
                v[j] = b[i + 6 + j];
            if (i + 6 + j < n && b[i + 6 + j] == ')' && j < sizeof(v) - 1)
                v[j++] = ')';
            v[j] = 0;
            return v;
        }
    return "no version string";
}

static void report(const target *t, int r)
{
    if (r == E_STOP)
        f2p_log("stopped: run ffs2pfs %s again to carry on, or ffs2pfs %s ROLLBACK to undo it\n", t->dosname, t->dosname);
    else if (r == E_ROLLEDBACK)
        f2p_log("ffs2pfs: %s\n%s is as it was before; nothing was converted\n", f2p_error, t->dosname);
    else if (r)
        f2p_log("ffs2pfs: %s\n", f2p_error);
}

/* AmigaDOS return codes: 0 done, 5 stopped, 10 failed but as it was, 20 failed */
static int rc_of(int r)
{
    return !r ? 0 : r == E_STOP ? 5 : r == E_ROLLEDBACK ? 10 : 20;
}

/* a run that cannot start: the message, kept for the window too */
static void why(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(run_why, sizeof(run_why), fmt, ap);
    va_end(ap);
    f2p_log("%s\n", run_why);
}

int ffs2pfs_run(const runopts *o)
{
    target t;
    disk k;
    blockdev d;
    u8 *loaded = NULL, rdsk[BS];
    const u8 *bin = pfs3aio_bin;
    u32 binlen = (u32)pfs3aio_len, rblk;
    int r = 20, inhibited = 0, len, bootvol = 0, mode = o->mode;
    memset(&t, 0, sizeof(t));
    memset(&k, 0, sizeof(k));
    run_why[0] = 0;
    prog.step = NULL;
    prog.active = 0;
    if (o->log && (logfile = Open((STRPTR)o->log, MODE_READWRITE)))
        Seek(logfile, 0, OFFSET_END);   /* resumed runs add to the same log */
    len = strlen(o->part);
    strncpy(t.partname, o->part, sizeof(t.partname) - 1);
    if (len && t.partname[len - 1] == ':')
        t.partname[len - 1] = 0;
    snprintf(t.dosname, sizeof(t.dosname), "%s:", t.partname);
    if (o->bin) {                              /* another pfs3aio than the one built in */
        if (!(loaded = load_file(o->bin, &binlen))) {
            why("cannot read %s (the pfs3aio handler that goes into the RDB)", o->bin);
            goto out;
        }
        bin = loaded;
    }
    if (mode != F2P_ROLLBACK)
        f2p_log("pfs3aio for the RDB: %s (%s)\n", ver_of(bin, binlen), loaded ? o->bin : "built in");
    if (o->image) {
        LONG size;
        if (!(imgfh = Open((STRPTR)o->image, MODE_OLDFILE))) {
            why("cannot open %s", o->image);
            goto out;
        }
        Seek(imgfh, 0, OFFSET_END);
        size = Seek(imgfh, 0, OFFSET_BEGINNING);
        crash_after = o->crash_after;
        img_writes = img_attempts = 0;
        if (o->failat) {
            char *p = (char *)o->failat;
            int f;
            for (f = 0; f < 4 && *p; f++) {
                fail_at[f] = strtol(p, &p, 10);
                if (*p == ',')
                    p++;
            }
        }
        torn_mode = !o->torn ? 0 : !strcmp(o->torn, "garbage") ? -1 : atoi(o->torn);
        d.ctx = NULL;
        d.nblocks = (u32)(size / BS);
        d.read = img_read;
        d.write = img_write;
        d.flush = NULL;
        f2p_log("%s: image %s, %lu blocks\n", t.partname, o->image, UL(d.nblocks));
        r = f2p_run(&d, t.partname, bin, binlen, mode);
        if (r)
            strncpy(run_why, r == E_STOP ? "stopped" : f2p_error, sizeof(run_why) - 1);
        report(&t, r);
        f2p_log("block writes: %ld\n", img_writes);
        r = rc_of(r);
        goto out;
    }
    if (!find_dos_device(&t) && !o->device) {
        why("%s is not mounted: give DEVICE and UNIT", t.dosname);
        goto out;
    }
    if (o->device) {                           /* the disk named directly (after a crash nothing mounts it) */
        strncpy(t.device, o->device, sizeof(t.device) - 1);
        t.unit = o->unit;
        if (!t.mounted) {
            t.memtype = MEMF_PUBLIC;
            t.maxtransfer = 0x1fe00;              /* the usual RDB value; 64 KB per request is used */
        }
    }
    if (t.mounted && is_boot_volume(&t)) {
        if (!o->boot) {
            why("%s is the boot volume. To convert it while running from it, copy ffs2pfs\n"
                "to RAM:, run it from there with BOOT, and reboot when it is done.", t.dosname);
            goto out;
        }
        if (progdir_on_target(&t) || (o->log && on_target(&t, o->log))) {
            why("BOOT: run ffs2pfs from RAM:, and put the LOG on another volume (nothing may need %s)", t.dosname);
            goto out;
        }
        bootvol = 1;
        f2p_log("converting the boot volume: anything that uses %s while this runs will get errors;\n"
                "reboot when it is done\n", t.dosname);
    }
    if (disk_open(&k, t.device, t.unit, t.flags, t.memtype, t.maxtransfer)) {
        why("ffs2pfs: %s", f2p_error);
        goto out;
    }
    disk_blockdev(&k, &d);
    if (rdb_find_rdsk(&d, &rblk, rdsk))
        d.nblocks = L(rdsk, 16) * L(rdsk, 17) * L(rdsk, 18);
    if (t.mounted) {
        if (!Inhibit((STRPTR)t.dosname, DOSTRUE)) {
            why("cannot inhibit %s: close everything that uses it", t.dosname);
            goto out;
        }
        inhibited = 1;
    }
    {
        struct Task *me = FindTask(NULL);
        f2p_log("stack %lu bytes, free memory %lu KB (largest block %lu KB)\n", UL((u8 *)me->tc_SPUpper - (u8 *)me->tc_SPLower),
                UL(AvailMem(MEMF_ANY) / 1024), UL(AvailMem(MEMF_LARGEST) / 1024));
    }
    f2p_log("%s %s unit %lu, %lu KB transfers, 32-bit commands below 4 GB, %s above\n", t.dosname, t.device, UL(t.unit),
            UL(k.maxsec / 2), k.mode == 2 ? "NSD" : k.mode == 1 ? "TD64" : "none");
    wd_start(o->log);
    if (mode == F2P_CONVERT)
        f2p_log(fe_log ? "Stop stops safely at any time; Convert carries on from there, or Roll back undoes it.\n"
                       : "Ctrl-C stops safely at any time; run the same command again to carry on, or with ROLLBACK to undo it.\n");
    r = f2p_run(&d, t.partname, bin, binlen, mode);
    if (r)
        strncpy(run_why, r == E_STOP ? "stopped" : f2p_error, sizeof(run_why) - 1);
    report(&t, r);
    if (inhibited && (r == E_ROLLEDBACK || (mode == F2P_ROLLBACK && !r))) {
        Inhibit((STRPTR)t.dosname, DOSFALSE);  /* FFS again, as it was: give it back */
        inhibited = 0;
    } else if (!t.mounted && (r == E_ROLLEDBACK || (mode == F2P_ROLLBACK && !r)))
        f2p_log("reboot to mount %s again\n", t.dosname);
    if (mode == F2P_ROLLBACK || r == E_ROLLEDBACK)
        ;
    else if (!r && mode != F2P_DRY && bootvol) {
        f2p_log("%s is PFS3 now. Reboot: the boot ROM loads pfs3aio from the RDB and boots from it.\n", t.dosname);
        inhibited = 0;                         /* leave it inhibited until the reboot */
    } else if (!r && mode != F2P_DRY && t.mounted && !o->noremount) {
        remount(&t, &d, bin, binlen);
        inhibited = 0;                         /* the FFS entry is gone; never uninhibit it */
    }
    r = rc_of(r);
    wd_stop();
    f2p_log("free memory at the end: %lu KB\n", UL(AvailMem(MEMF_ANY) / 1024));
out:
    wd_stop();
    if (inhibited && mode == F2P_DRY)
        Inhibit((STRPTR)t.dosname, DOSFALSE);  /* analyse only: give the volume back */
    disk_close(&k);
    if (imgfh) {
        Close(imgfh);
        imgfh = 0;
    }
    if (loaded)
        FreeVec(loaded);
    if (logfile) {
        Close(logfile);
        logfile = 0;
    }
    return r;
}

int main(int argc, char **argv)
{
    LONG args[A_N];
    struct RDArgs *rda;
    runopts o;
    int r, i, any = 0;
    if (argc == 0)                             /* started from Workbench */
        return gui_main();
    memset(args, 0, sizeof(args));
    rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"ffs2pfs");
        return 20;
    }
    for (i = 0; i < A_N; i++)
        any |= args[i] != 0;
    if (!any) {                                /* no arguments: the window */
        FreeArgs(rda);
        if ((r = gui_main()))
            PutStr((STRPTR)"ffs2pfs: cannot open its window (it needs Kickstart 2.04 and a 640x256 screen)\n"
                           "In the Shell: ffs2pfs PARTITION [ANALYSE] [ROLLBACK] ... (ffs2pfs ? lists them)\n");
        return r;
    }
    if (!args[A_PART] || (args[A_ROLLBACK] && args[A_DRY])) {
        PutStr((STRPTR)(!args[A_PART] ? "ffs2pfs: which partition? For example: ffs2pfs DH1: ANALYSE\n"
                                        "(with no arguments ffs2pfs opens its window)\n"
                                      : "ffs2pfs: ANALYSE and ROLLBACK do not go together\n"));
        FreeArgs(rda);
        return 20;
    }
    prog.interactive = IsInteractive(Output()) ? 1 : 0;
    memset(&o, 0, sizeof(o));
    o.part = (const char *)args[A_PART];
    o.mode = args[A_ROLLBACK] ? F2P_ROLLBACK : args[A_DRY] ? F2P_DRY : F2P_CONVERT;
    o.bin = (const char *)args[A_BIN];
    o.device = (const char *)args[A_DEV];
    o.unit = args[A_UNIT] ? (u32)*(LONG *)args[A_UNIT] : 0;
    o.noremount = args[A_NOREMOUNT] != 0;
    o.log = (const char *)args[A_LOG];
    o.boot = args[A_BOOT] != 0;
    o.image = (const char *)args[A_IMAGE];
    o.torn = (const char *)args[A_TORN];
    o.failat = (const char *)args[A_FAILAT];
    o.crash_after = args[A_CRASH] ? *(LONG *)args[A_CRASH] : -1;
    r = ffs2pfs_run(&o);
    FreeArgs(rda);
    return r;
}
