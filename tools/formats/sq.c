/* sq.c - see sq.h. */

#include "sq.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const sq_arm_name[SQ_ARM_N] = {
    "0", "10", "1100", "1101", "1110", "1111"
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sq_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || errlen == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* Python's repr() of a bytes object, which is what sq.py's "bad magic %r"
 * interpolates. Only the four magic bytes ever reach this, but the wording is
 * part of the compared surface, so it is reproduced rather than approximated.
 * Python picks a single quote unless the value contains one and no double. */
static void py_bytes_repr(const uint8_t *b, size_t n, char *out, size_t outlen)
{
    size_t i, w = 0;
    int has_sq = 0, has_dq = 0;
    char q;

    for (i = 0; i < n; i++) {
        if (b[i] == '\'') has_sq = 1;
        if (b[i] == '"')  has_dq = 1;
    }
    q = (has_sq && !has_dq) ? '"' : '\'';

#define PUT(c) do { if (w + 1 < outlen) out[w] = (char)(c); w++; } while (0)
    PUT('b');
    PUT(q);
    for (i = 0; i < n; i++) {
        uint8_t c = b[i];

        if (c == (uint8_t)q || c == '\\') {
            PUT('\\'); PUT(c);
        } else if (c == '\t') {
            PUT('\\'); PUT('t');
        } else if (c == '\n') {
            PUT('\\'); PUT('n');
        } else if (c == '\r') {
            PUT('\\'); PUT('r');
        } else if (c >= 0x20 && c < 0x7f) {
            PUT(c);
        } else {
            static const char hex[] = "0123456789abcdef";
            PUT('\\'); PUT('x'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
        }
    }
    PUT(q);
#undef PUT
    out[w < outlen ? w : outlen - 1] = 0;
}

int sq_read_header(const void *buf, size_t len, sq_header *h,
                   char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;

    memset(h, 0, sizeof(*h));
    if (len < SQ_HDR_SIZE) {
        sq_err(err, errlen, "file shorter than the %d-byte header", SQ_HDR_SIZE);
        return -1;
    }
    if (memcmp(b, "SQSL", 4) == 0) {
        h->magic = SQ_MAGIC_LEVEL;
    } else if (memcmp(b, "SQSM", 4) == 0) {
        h->magic = SQ_MAGIC_MAP;
    } else {
        char r[32];

        py_bytes_repr(b, 4, r, sizeof r);
        sq_err(err, errlen, "bad magic %s", r);
        return -1;
    }
    h->pad     = rd32(b + 4);
    h->version = rd32(b + 8);
    h->count   = rd32(b + 12);
    if (h->version != SQ_VERSION) {
        sq_err(err, errlen, "version 0x%08x, expected 0x%08x",
               h->version, SQ_VERSION);
        return -1;
    }
    return 0;
}

/* ---- the bit reader ----------------------------------------------------
 *
 * MSB-first over little-endian 32-bit words, and it mirrors the game's macro
 * exactly: `n` starts at 1, so the first read refills before it takes a bit,
 * and each refill then yields 32 bits.
 */
typedef struct {
    const uint8_t *d;
    size_t         len;
    size_t         p;
    uint32_t       w;
    int            n;
    int            eof;
} sq_bits;

static void bits_init(sq_bits *bs, const void *d, size_t len, size_t pos)
{
    bs->d = (const uint8_t *)d;
    bs->len = len;
    bs->p = pos;
    bs->w = 0;
    bs->n = 1;
    bs->eof = 0;
}

static uint32_t bits_bit(sq_bits *bs)
{
    uint32_t b;

    bs->n--;
    if (bs->n == 0) {
        if (bs->p + 4 > bs->len) {
            bs->eof = 1;
            bs->n = 1;      /* stay put; the caller checks `eof` and stops */
            return 0;
        }
        bs->w = rd32(bs->d + bs->p);
        bs->p += 4;
        bs->n = 32;
    }
    b = (bs->w >> 31) & 1u;
    bs->w <<= 1;
    return b;
}

static uint32_t bits_take(sq_bits *bs, int k)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < k; i++) {
        v = (v << 1) | bits_bit(bs);
    }
    return v;
}

int sqsl_decode(const void *buf, size_t len, sq_level *out, sq_tokens *census,
                char *err, size_t errlen)
{
    sq_header h;
    sq_bits bs;
    uint32_t *w;
    size_t i;

    memset(out, 0, sizeof(*out));
    if (census) {
        memset(census, 0, sizeof(*census));
    }
    if (sq_read_header(buf, len, &h, err, errlen) != 0) {
        return -1;
    }
    if (h.magic != SQ_MAGIC_LEVEL) {
        char r[32];

        py_bytes_repr((const uint8_t *)buf, 4, r, sizeof r);
        sq_err(err, errlen, "not an SQSL file (%s)", r);
        return -1;
    }

    w = (uint32_t *)malloc((h.count ? h.count : 1) * sizeof *w);
    if (!w) {
        sq_err(err, errlen, "cannot allocate %u words", (unsigned)h.count);
        return -1;
    }
    bits_init(&bs, buf, len, SQ_HDR_SIZE);

    for (i = 0; i < h.count; i++) {
        uint32_t d;
        /* int64_t, not long: `long` is 32-bit under LLP64 and this file is
         * part of what ships. The same reasoning as cdat.c's 64-bit section
         * bounds - a width that is right on the dev host and wrong on a target
         * is the hardest kind of bug to see. */
        int64_t src;

        if (bits_bit(&bs) == 0) {
            if (census) census->arm[SQ_ARM_ZERO]++;
            w[i] = 0;
            goto emitted;
        }
        if (bits_bit(&bs) == 0) {
            d = bits_take(&bs, 8);
            if (census) {
                census->arm[SQ_ARM_REF8]++;
                census->zero[0] += (d == 0);
                if (d > census->dmax[0]) census->dmax[0] = d;
            }
            goto backref;
        }
        if (bits_bit(&bs) == 0) {
            if (bits_bit(&bs) == 0) {
                d = bits_take(&bs, 12);
                if (census) {
                    census->arm[SQ_ARM_REF12]++;
                    census->zero[1] += (d == 0);
                    if (d > census->dmax[1]) census->dmax[1] = d;
                }
                goto backref;
            }
            if (census) census->arm[SQ_ARM_LIT17]++;
            w[i] = bits_take(&bs, 17);
            goto emitted;
        }
        if (bits_bit(&bs) == 0) {
            /* 28 bits signed: the sign bit first, then 27 more. The Python
             * seeds the accumulator with -bit and shifts, which sign-extends
             * for free; int32_t does the same here. */
            int32_t v = -(int32_t)bits_bit(&bs);
            int k;

            if (census) census->arm[SQ_ARM_LIT28S]++;
            for (k = 0; k < 27; k++) {
                v = (int32_t)(((uint32_t)v << 1) | bits_bit(&bs));
            }
            w[i] = (uint32_t)v;
            goto emitted;
        }
        if (census) census->arm[SQ_ARM_LIT32]++;
        w[i] = bits_take(&bs, 32);
        goto emitted;

    backref:
        /* EOF is tested BEFORE the distance is used. The Python raises out of
         * `bit()` the moment the stream runs out, so a truncated file must
         * report EOF here and not a back-reference computed from the zeros
         * bits_bit() returns once it has given up. */
        if (bs.eof) {
            goto ran_past_eof;
        }
        src = (int64_t)i - (int64_t)d - 1;
        if (src < 0) {
            /* sq.h documents this divergence: tools/sq.py reads out[-k] here
             * and gets a wrong word rather than an error. */
            sq_err(err, errlen,
                   "back-reference before the start of the output at word %lu",
                   (unsigned long)i);
            free(w);
            return -1;
        }
        w[i] = w[(size_t)src];

    emitted:
        if (bs.eof) {
            goto ran_past_eof;
        }
    }

    out->word = w;
    out->n = h.count;
    out->consumed = bs.p;
    return 0;

ran_past_eof:
    sq_err(err, errlen, "bitstream ran past EOF at %lu", (unsigned long)bs.p);
    free(w);
    return -1;
}

void sq_level_free(sq_level *l)
{
    free(l->word);
    l->word = NULL;
    l->n = 0;
}

int sqsm_decode(const void *buf, size_t len, sq_map *out,
                char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    sq_header h;
    size_t end;

    memset(out, 0, sizeof(*out));
    if (sq_read_header(buf, len, &h, err, errlen) != 0) {
        return -1;
    }
    if (h.magic != SQ_MAGIC_MAP) {
        char r[32];

        py_bytes_repr(b, 4, r, sizeof r);
        sq_err(err, errlen, "not an SQSM file (%s)", r);
        return -1;
    }
    end = SQ_HDR_SIZE + SQ_MAP_INDEX_BYTES + (size_t)h.count * SQ_MAP_TILE_BYTES;
    if (end > len) {
        sq_err(err, errlen, "declares %u tiles, needs %lu bytes, file is %lu",
               (unsigned)h.count, (unsigned long)end, (unsigned long)len);
        return -1;
    }
    /* The cell table is 16-bit little-endian and this host may not be, so it
     * is NOT aliased through a uint16_t* - sq_map_cell does the read. */
    out->cell = b + SQ_HDR_SIZE;
    out->tiles = b + SQ_HDR_SIZE + SQ_MAP_INDEX_BYTES;
    out->ntiles = h.count;
    out->consumed = end;
    return 0;
}

unsigned sq_map_cell(const sq_map *m, size_t i)
{
    return (unsigned)m->cell[i * 2] | ((unsigned)m->cell[i * 2 + 1] << 8);
}
