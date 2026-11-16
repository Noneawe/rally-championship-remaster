/* cdat.c - see cdat.h. */

#include "cdat.h"
#include "sq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const unsigned cdat_normal_scratch[6] = { 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c };

/* store_norms_q's two triangles: x at +0x18/+0x1c, y at +0x30/+0x34,
 * z at +0x38/+0x3c. */
const unsigned cdat_normal_xyz[2][3] = {
    { 0x18, 0x30, 0x38 },
    { 0x1c, 0x34, 0x3c }
};

/* FUN_00420c64 loads eax, ebx, ecx in this order before each call to
 * FUN_00420e36, and the early demo's 503,381 unit normals confirm the
 * winding from outside the disassembly. */
const unsigned cdat_triangle_corners[2][3] = {
    { 0x04, 0x08, 0x0c },
    { 0x10, 0x0c, 0x08 }
};

uint32_t cdat_u32(const uint8_t *rec, unsigned off)
{
    return (uint32_t)rec[off] | ((uint32_t)rec[off + 1] << 8) |
           ((uint32_t)rec[off + 2] << 16) | ((uint32_t)rec[off + 3] << 24);
}

uint16_t cdat_u16(const uint8_t *rec, unsigned off)
{
    return (uint16_t)((unsigned)rec[off] | ((unsigned)rec[off + 1] << 8));
}

float cdat_f32(const uint8_t *rec, unsigned off)
{
    uint32_t bits = cdat_u32(rec, off);
    float f;

    /* memcpy, not a union or a cast: the port builds at -O2 with strict
     * aliasing on, and this is the one idiom that is defined. */
    memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t cdat_cell_of(int32_t coord)
{
    return (uint32_t)coord >> CDAT_CELL_SHIFT;
}

void cdat_section(const void *buf, size_t len, cdat_payload *p)
{
    const uint8_t *b = (const uint8_t *)buf;
    int i;

    memset(p, 0, sizeof(*p));
    p->buf = b;
    p->len = len;
    for (i = 0; i < CDAT_HDR_WORDS; i++) {
        p->hdr[i] = (len >= (size_t)(i + 1) * 4)
                        ? (int32_t)cdat_u32(b, (unsigned)i * 4) : 0;
    }
    p->nzube = p->hdr[1];
    p->npols = p->hdr[2];
    p->npnts = p->hdr[3];

    {
        /* 64-bit throughout, so that a header word near UINT32_MAX - which is
         * exactly what a corrupt payload carries - cannot wrap the
         * multiplication and produce an `end` that looks plausible. On a
         * 32-bit host size_t arithmetic would do precisely that. */
        uint64_t nz = (uint32_t)p->nzube, np = (uint32_t)p->npols;
        uint64_t nv = (uint32_t)p->npnts;
        uint64_t zube_off = CDAT_HDR_BYTES;
        uint64_t pols_off = zube_off + nz * CDAT_ZUBE_SIZE;
        uint64_t pnts_off = pols_off + np * CDAT_POLS_SIZE;
        uint64_t end      = pnts_off + nv * CDAT_PNTS_SIZE;

        p->fits = (p->nzube >= 0 && p->npols >= 0 && p->npnts >= 0 &&
                   end <= (uint64_t)len);
        p->zube_off = (size_t)zube_off;
        p->pols_off = (size_t)pols_off;
        p->pnts_off = (size_t)pnts_off;
        p->end      = (size_t)end;
        if (!p->fits) {
            /* Saturate rather than truncate: cdat_closes must not accidentally
             * report a wrapped `end` as equal to `len`. */
            if (end > (uint64_t)(size_t)-1) {
                p->end = (size_t)-1;
            }
        }
    }
}

int cdat_closes(const cdat_payload *p)
{
    return p->end == p->len;
}

int cdat_open_buffer(const void *buf, size_t len, cdat_payload *p,
                     char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;

    memset(p, 0, sizeof(*p));
    if (len >= 4 && memcmp(b, "SQSL", 4) == 0) {
        sq_level lv;
        uint8_t *raw;
        size_t i;

        if (sqsl_decode(buf, len, &lv, NULL, err, errlen) != 0) {
            return -1;
        }
        raw = (uint8_t *)malloc(lv.n ? lv.n * 4 : 1);
        if (!raw) {
            sq_level_free(&lv);
            if (err && errlen) {
                snprintf(err, errlen, "cannot allocate the decompressed payload");
            }
            return -1;
        }
        /* Serialise little-endian by hand: the parser ships, and the port
         * targets a host whose byte order it does not get to assume. */
        for (i = 0; i < lv.n; i++) {
            raw[i * 4 + 0] = (uint8_t)(lv.word[i]);
            raw[i * 4 + 1] = (uint8_t)(lv.word[i] >> 8);
            raw[i * 4 + 2] = (uint8_t)(lv.word[i] >> 16);
            raw[i * 4 + 3] = (uint8_t)(lv.word[i] >> 24);
        }
        cdat_section(raw, lv.n * 4, p);
        p->owned = raw;
        sq_level_free(&lv);
        return 0;
    }
    cdat_section(buf, len, p);
    return 0;
}

void cdat_free(cdat_payload *p)
{
    free(p->owned);
    p->owned = NULL;
    p->buf = NULL;
    p->len = 0;
}

void cdat_zube(const cdat_payload *p, int32_t i, int32_t out[8])
{
    const uint8_t *r = p->buf + p->zube_off + (size_t)i * CDAT_ZUBE_SIZE;
    int k;

    for (k = 0; k < 8; k++) {
        out[k] = (int32_t)cdat_u32(r, (unsigned)k * 4);
    }
}

uint32_t cdat_zube_key(const cdat_payload *p, int32_t i)
{
    return cdat_u32(p->buf + p->zube_off + (size_t)i * CDAT_ZUBE_SIZE, 0);
}

const uint8_t *cdat_pol(const cdat_payload *p, int32_t i)
{
    return p->buf + p->pols_off + (size_t)i * CDAT_POLS_SIZE;
}

void cdat_pnt(const cdat_payload *p, int32_t i, int32_t out[3])
{
    const uint8_t *r = p->buf + p->pnts_off + (size_t)i * CDAT_PNTS_SIZE;

    out[0] = (int32_t)cdat_u32(r, 0);
    out[1] = (int32_t)cdat_u32(r, 4);
    out[2] = (int32_t)cdat_u32(r, 8);
}
