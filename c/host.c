/* Host front end for tests: an image file as the disk, with crash and error injection.
 *   f2p_host IMAGE [--name P] [--bin PFS3AIO] [--dry] [--rollback] [--crash-after N]
 *            [--torn off|garbage|OFFSET] [--fail-at N[,N...]]
 *   f2p_host IMAGE --probe      (each partition and where it stands, reading only)
 * --crash-after N: the (N+1)th 512-byte block write is not done (or done torn)
 * and the program exits with status 3, as a power cut would leave it.
 * --fail-at N: block write attempt N (from 0) is not done and returns an error, once.
 * Exit status: 0 done, 11 failed but the volume is as it was, 5 stopped, 10 error. */
#define _FILE_OFFSET_BITS 64
#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "f2p.h"

static long long crash_after = -1, writes = 0, attempts = 0, fail_at[16];
static int nfail = 0;
static int torn_mode = 0;          /* 0 clean, -1 garbage, >0 byte offset */

void f2p_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout); }
void *f2p_alloc(size_t n) { return calloc(1, n ? n : 1); }
void f2p_free(void *p) { free(p); }
int f2p_break(void) { return 0; }
void f2p_progress(const char *step, u32 done, u32 total, const char *item) { (void)step; (void)done; (void)total; (void)item; }

static int fd_read(blockdev *d, u32 blk, u32 n, u8 *buf)
{
    int fd = *(int *)d->ctx;
    if (blk + n > d->nblocks)
        return 1;
    return pread(fd, buf, (size_t)n * BS, (off_t)blk * BS) != (ssize_t)n * BS;
}

static int fd_write(blockdev *d, u32 blk, u32 n, const u8 *buf)
{
    int fd = *(int *)d->ctx;
    u32 i;
    if (blk + n > d->nblocks)
        return 1;
    for (i = 0; i < n; i++) {
        const u8 *b = buf + (size_t)i * BS;
        int f;
        for (f = 0; f < nfail; f++)
            if (fail_at[f] == attempts) {
                attempts++;
                return 1;
            }
        attempts++;
        if (crash_after >= 0 && writes >= crash_after) {
            if (torn_mode) {
                u8 t[BS];
                if (torn_mode < 0) {
                    u32 j, x = (blk + i) * 2654435761UL + 1;
                    for (j = 0; j < BS; j++) { x = x * 1103515245UL + 12345; t[j] = (u8)(x >> 16); }
                } else {
                    pread(fd, t, BS, (off_t)(blk + i) * BS);
                    memcpy(t, b, torn_mode);
                }
                pwrite(fd, t, BS, (off_t)(blk + i) * BS);
            }
            fsync(fd);
            exit(3);
        }
        if (pwrite(fd, b, BS, (off_t)(blk + i) * BS) != BS)
            return 1;
        writes++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *img = NULL, *name = NULL, *binpath = NULL;
    int mode = F2P_CONVERT, i, fd, r, probe = 0;
    u8 *bin = NULL;
    u32 binlen = 0;
    blockdev d;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--bin") && i + 1 < argc) binpath = argv[++i];
        else if (!strcmp(argv[i], "--dry")) mode = F2P_DRY;
        else if (!strcmp(argv[i], "--rollback")) mode = F2P_ROLLBACK;
        else if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--fail-at") && i + 1 < argc) {
            char *p = argv[++i];
            while (*p && nfail < 16) {
                fail_at[nfail++] = strtoll(p, &p, 10);
                if (*p == ',') p++;
            }
        }
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc) crash_after = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--torn") && i + 1 < argc) {
            i++;
            torn_mode = !strcmp(argv[i], "garbage") ? -1 : atoi(argv[i]);
        } else img = argv[i];
    }
    if (!img) {
        fprintf(stderr, "usage: f2p_host IMAGE [--name P] [--bin PFS3AIO] [--dry] [--rollback] [--crash-after N]\n"
                        "                [--torn off|garbage|OFFSET] [--fail-at N[,N...]]\n");
        return 20;
    }
    if (binpath) {
        FILE *f = fopen(binpath, "rb");
        if (!f) { perror(binpath); return 20; }
        fseek(f, 0, SEEK_END);
        binlen = (u32)ftell(f);
        fseek(f, 0, SEEK_SET);
        bin = malloc(binlen);
        if (fread(bin, 1, binlen, f) != binlen) { perror(binpath); return 20; }
        fclose(f);
    }
    fd = open(img, mode == F2P_DRY || probe ? O_RDONLY : O_RDWR);
    if (fd < 0) { perror(img); return 20; }
    d.ctx = &fd;
    d.nblocks = (u32)(lseek(fd, 0, SEEK_END) / BS);
    d.read = fd_read;
    d.write = fd_write;
    d.flush = NULL;                /* writes go straight to the file, in order */
    if (probe) {                   /* one line per partition: name state step DosType-before */
        static const char *st[] = { "none", "pfs3", "stopped", "undo", "finish", "bad" };
        f2p_part pp[32];
        int n = f2p_probe(&d, 1, pp, 32);
        if (n < 0) { printf("no RDSK\n"); return 10; }
        for (i = 0; i < n; i++)
            printf("%s %s %lu %08lx%s%s\n", pp[i].p.invalid ? "?" : pp[i].p.name, st[pp[i].state], UL(pp[i].step),
                   UL(pp[i].orig_dostype), pp[i].why[0] ? " " : "", pp[i].why);
        close(fd);
        return 0;
    }
    r = f2p_run(&d, name, bin, binlen, mode);
    if (r)
        fprintf(stderr, "ffs2pfs: %s%s\n", f2p_error, r == E_ROLLEDBACK ? " (the volume is as it was)" : "");
    printf("block writes: %lld\n", writes);
    close(fd);
    return r == E_ROLLEDBACK ? 11 : r == E_STOP ? 5 : r ? 10 : 0;
}
