/* lwo.c - see lwo.h. */

#include "lwo.h"

#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int16_t be16s(const uint8_t *p)
{
    return (int16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

static uint16_t be16u(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

/* POLS, transcribed: [count][count * index][surface], and a NEGATIVE surface
 * is followed by a detail-polygon count that must be stepped over. Getting
 * that wrong silently shifts every following polygon. */
static int parse_pols(lwo_file *f, const uint8_t *body, size_t len)
{
    size_t off = 0;

    while (off + 2 <= len) {
        int n = be16s(body + off);
        size_t i;
        off += 2;
        if (n <= 0 || off + (size_t)n * 2 + 2 > len) break;

        if (f->npolys == f->_polycap) {
            size_t nc = f->_polycap ? f->_polycap * 2 : 256;
            lwo_poly *p = realloc(f->polys, nc * sizeof *p);
            if (!p) return -1;
            f->polys = p; f->_polycap = nc;
        }
        if (f->_poolused + (size_t)n > f->_poolcap) {
            size_t nc = f->_poolcap ? f->_poolcap : 1024;
            uint16_t *q;
            while (nc < f->_poolused + (size_t)n) nc *= 2;
            q = realloc(f->idx_pool, nc * sizeof *q);
            if (!q) return -1;
            f->idx_pool = q; f->_poolcap = nc;
        }
        for (i = 0; i < (size_t)n; i++)
            f->idx_pool[f->_poolused + i] = be16u(body + off + i * 2);
        off += (size_t)n * 2;

        /* Stored as an OFFSET, not a pointer: the pool moves under realloc.
         * Resolved once, in lwo_parse, after the last chunk. */
        f->polys[f->npolys].idx     = (uint16_t *)(uintptr_t)f->_poolused;
        f->polys[f->npolys].n       = n;
        f->polys[f->npolys].surface = be16s(body + off);
        off += 2;
        if (f->polys[f->npolys].surface < 0) off += 2;
        f->npolys++;
        f->_poolused += (size_t)n;
    }
    return 0;
}

static int parse_srfs(lwo_file *f, const uint8_t *body, size_t len)
{
    size_t i, start = 0;

    f->srf_pool = malloc(len + 1);
    if (!f->srf_pool) return -1;
    memcpy(f->srf_pool, body, len);
    f->srf_pool[len] = 0;

    for (i = 0; i <= len; i++) {
        if (i == len || f->srf_pool[i] == 0) {
            if (i > start) {
                if (f->nsrfs == f->_srfcap) {
                    size_t nc = f->_srfcap ? f->_srfcap * 2 : 16;
                    char **s = realloc(f->srfs, nc * sizeof *s);
                    if (!s) return -1;
                    f->srfs = s; f->_srfcap = nc;
                }
                f->srf_pool[i] = 0;
                f->srfs[f->nsrfs++] = f->srf_pool + start;
            }
            start = i + 1;
        }
    }
    return 0;
}

int lwo_parse(const void *vbuf, size_t len, lwo_file *f)
{
    const uint8_t *buf = (const uint8_t *)vbuf;
    size_t off, end;

    memset(f, 0, sizeof(*f));
    if (len < 12 || memcmp(buf, "FORM", 4) != 0 || memcmp(buf + 8, "LWOB", 4) != 0)
        return -1;

    end = (size_t)be32(buf + 4) + 8;
    if (end > len) end = len;

    for (off = 12; off + 8 <= end; ) {
        const uint8_t *tag = buf + off;
        size_t ln = be32(buf + off + 4);
        const uint8_t *body = buf + off + 8;
        if (off + 8 + ln > end) break;
        if (memcmp(tag, "POLS", 4) == 0) {
            if (parse_pols(f, body, ln) != 0) { lwo_free(f); return -1; }
        } else if (memcmp(tag, "SRFS", 4) == 0) {
            /* One SRFS per file. A second would orphan the first pool, so it
             * is ignored rather than silently leaked. */
            if (!f->srf_pool && parse_srfs(f, body, ln) != 0) { lwo_free(f); return -1; }
        } else if (memcmp(tag, "PNTS", 4) == 0) {
            f->npnts = ln / 12;
        }
        off += 8 + ln + (ln & 1);
    }

    /* Resolve the offsets left by parse_pols now the pool has stopped moving. */
    {
        size_t i;
        for (i = 0; i < f->npolys; i++)
            f->polys[i].idx = f->idx_pool + (size_t)(uintptr_t)f->polys[i].idx;
    }
    return 0;
}

void lwo_free(lwo_file *f)
{
    free(f->polys);
    free(f->idx_pool);
    free(f->srfs);
    free(f->srf_pool);
    memset(f, 0, sizeof(*f));
}
