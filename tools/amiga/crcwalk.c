/* crcwalk DIR TO - every entry under DIR: "path<TAB>kind<TAB>size<TAB>crc32" into TO.
 * kind: dir, file, linkdir, soft, unreadable. CRC32 as zlib's. Read only. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

size_t __stack = 32768;
static const char version[] = "$VER: crcwalk 1.0 (18.11.2026)";
static unsigned long tab[256];
static UBYTE *buf;
static BPTR out;
static unsigned long nfiles, ndirs, nbad;

static unsigned long crc(unsigned long c, const UBYTE *p, long n)
{
    c = ~c;
    while (n--)
        c = tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void line(const char *path, const char *kind, unsigned long size, unsigned long c)
{
    char b[400];
    snprintf(b, sizeof(b), "%s\t%s\t%lu\t%08lx\n", path, kind, size, c);
    Write(out, b, strlen(b));
}

/* a stack of directories still to list (relative paths) */
typedef struct node { struct node *next; char path[1]; } node;

int main(int argc, char **argv)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    node *todo = NULL;
    char *root;
    unsigned long i, j;
    if (argc != 3 || !fib) {
        PutStr("usage: crcwalk DIR TO\n");
        return 20;
    }
    for (i = 0; i < 256; i++) {
        unsigned long c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        tab[i] = c;
    }
    root = argv[1];
    buf = AllocVec(65536, MEMF_PUBLIC);
    out = Open(argv[2], MODE_NEWFILE);
    if (!buf || !out) {
        PutStr("crcwalk: no memory or cannot open the output\n");
        return 20;
    }
    todo = AllocVec(sizeof(node) + 1, MEMF_CLEAR);
    while (todo) {
        node *cur = todo;
        char full[400];
        BPTR lock;
        todo = cur->next;
        snprintf(full, sizeof(full), "%s%s", root,
                 cur->path + (cur->path[0] == '/' && root[strlen(root) - 1] == ':' ? 1 : 0));   /* "Work:dir", not "Work:/dir" */
        lock = Lock(full, SHARED_LOCK);
        if (lock && Examine(lock, fib)) {
            while (ExNext(lock, fib)) {
                char rel[400], fp[400];
                LONG t = fib->fib_DirEntryType;
                snprintf(rel, sizeof(rel), "%s/%s", cur->path, fib->fib_FileName);
                snprintf(fp, sizeof(fp), "%s%s", root, rel + (root[strlen(root) - 1] == ':' ? 1 : 0));
                if (t == ST_SOFTLINK)
                    line(rel, "soft", 0, 0);
                else if (t == ST_LINKDIR)
                    line(rel, "linkdir", 0, 0);
                else if (t > 0) {
                    node *n = AllocVec(sizeof(node) + strlen(rel), MEMF_CLEAR);
                    line(rel, "dir", 0, 0);
                    ndirs++;
                    if (n) {
                        strcpy(n->path, rel);
                        n->next = todo;
                        todo = n;
                    }
                } else {
                    BPTR f = Open(fp, MODE_OLDFILE);
                    unsigned long c = 0, size = 0;
                    LONG r = 0;
                    if (!f) {
                        line(rel, "unreadable", fib->fib_Size, 0);
                        nbad++;
                        continue;
                    }
                    while ((r = Read(f, buf, 65536)) > 0) {
                        c = crc(c, buf, r);
                        size += r;
                    }
                    Close(f);
                    line(rel, r < 0 ? "unreadable" : "file", size, c);
                    nfiles++;
                    if (nfiles % 500 == 0) {
                        Printf("crcwalk: %lu files, %lu directories\n", nfiles, ndirs);
                        Flush(Output());
                    }
                }
            }
        } else {
            line(cur->path, "unlistable", 0, 0);
            nbad++;
        }
        if (lock)
            UnLock(lock);
        FreeVec(cur);
    }
    Close(out);
    Printf("crcwalk %s: %lu files, %lu directories, %lu unreadable\n", root, nfiles, ndirs, nbad);
    return version[0] == '$' ? 0 : 0;
}
