/* small helpers: CRC32 (zlib's), big-endian access, bit arrays, errors */
#include <stdarg.h>
#include <stdio.h>
#include "f2p.h"

char f2p_error[200];

int f2p_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f2p_error, sizeof(f2p_error), fmt, ap);
    va_end(ap);
    return 1;
}

static u32 crctab[256];

u32 crc32_update(u32 crc, const u8 *p, size_t n)
{
    if (!crctab[1]) {
        u32 i, j, c;
        for (i = 0; i < 256; i++) {
            for (c = i, j = 0; j < 8; j++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            crctab[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = crctab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

int bm_init(bitmap *m, u32 n)
{
    m->n = n;
    m->bits = f2p_alloc(n / 8 + 1);
    return m->bits ? 0 : f2p_fail("out of memory (%lu KB for a block map)", UL(n / 8192 + 1));
}

void bm_free(bitmap *m)
{
    if (m->bits)
        f2p_free(m->bits);
    m->bits = NULL;
}

void f2p_progress_file(const char *step, u32 done, u32 total, u32 file, u32 files, const char *name)
{
    char item[64];
    snprintf(item, sizeof(item), "file %lu/%lu %.30s", UL(file), UL(files), name ? name : "");
    f2p_progress(step, done, total, item);
}
