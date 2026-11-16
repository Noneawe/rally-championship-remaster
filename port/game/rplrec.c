/* rplrec.c -: the replay recorder (see rplrec.h). */

#include "rplrec.h"

#include <stdlib.h>
#include <string.h>

/* ---- fmt_rle_pack, 0x0041E1F7, transliterated ----------------------------
 * Register names are the original's: EDX the bytes left, ECX a run's or a
 * literal's length, AL the byte compared.  Three of its habits are kept
 * because the gate compares the packed bytes with the real files':
 *   - a run is capped at 128, a literal at 128;
 *   - the walk stops a literal BEFORE the first byte of a pair;
 *   - when the cap lands exactly on the end, one more empty literal (a zero
 *     count byte) is written before the terminator. */
size_t rplrec_pack(const uint8_t *src, size_t n, uint8_t *out)
{
    const uint8_t *esi = src;
    uint8_t *edi = out;
    size_t edx = n, ecx;
    uint8_t al;

    if (n < 3) {                                   /* 0x41E1FD: jb 0x41E272 */
        *edi++ = (uint8_t)(0u - (uint8_t)edx);
        memcpy(edi, esi, edx);
        edi += edx;
        *edi++ = 0;
        return (size_t)(edi - out);
    }
    for (;;) {
        al = esi[0];                               /* 0x41E202 */
        if (edx <= 1) {                            /* jle 0x41E272 */
            *edi++ = (uint8_t)(0u - (uint8_t)edx);
            memcpy(edi, esi, edx);
            edi += edx;
            break;
        }
        if (al == esi[1]) {                        /* a run */
            ecx = 2;
            for (;;) {
                if (ecx == edx) {                  /* 0x41E233: to the end */
                    esi += ecx;
                    *edi++ = (uint8_t)(ecx - 1);
                    *edi++ = al;
                    goto done;
                }
                if (al != esi[ecx]) break;
                ecx++;
                if (ecx >= 0x80) break;            /* jb 0x41E213 */
            }
            esi += ecx;                            /* 0x41E225 */
            edx -= ecx;
            *edi++ = (uint8_t)(ecx - 1);
            *edi++ = al;
            continue;
        }
        ecx = 1;                                   /* a literal, 0x41E23F */
        for (;;) {
            if (ecx == edx) {                      /* 0x41E267: to the end */
                *edi++ = (uint8_t)(0u - (uint8_t)ecx);
                memcpy(edi, esi, ecx);
                edi += ecx;
                goto done;
            }
            if (al == esi[ecx]) break;             /* 0x41E259 */
            al = esi[ecx];
            ecx++;
            if (ecx >= 0x81) break;
        }
        ecx--;                                     /* 0x41E259 */
        edx -= ecx;
        *edi++ = (uint8_t)(0u - (uint8_t)ecx);
        memcpy(edi, esi, ecx);
        esi += ecx;
        edi += ecx;
    }
done:
    *edi++ = 0;                                    /* 0x41E27B, a dword store */
    return (size_t)(edi - out);
}

size_t rplrec_unpack(const uint8_t *src, size_t srclen, uint8_t *out, size_t outcap)
{
    size_t i = 0, o = 0;
    while (i < srclen) {
        int n = (int8_t)src[i++];
        if (n == 0) break;
        if (n > 0) {
            size_t c = (size_t)n + 1;
            if (i >= srclen || o + c > outcap) return o;
            memset(out + o, src[i++], c);
            o += c;
        } else {
            size_t c = (size_t)(-n);
            if (i + c > srclen || o + c > outcap) return o;
            memcpy(out + o, src + i, c);
            i += c;
            o += c;
        }
    }
    return o;
}

/* ---- the device words, 0x00409227 / 0x00409237 -------------------------- */
uint32_t rplrec_word_pack(uint32_t ax, int32_t cx, uint32_t dx)
{
    uint32_t ecx = ((uint32_t)cx << 9) | dx;
    ecx <<= 9;
    return (ax | ecx) << 4;
}

void rplrec_word_unpack(uint32_t w, uint32_t *ax, int32_t *cx, uint32_t *dx)
{
    *ax = (w >> 4) & 0x1FFu;
    *dx = (w >> 13) & 0x1FFu;
    *cx = (int32_t)w >> 22;
}

uint32_t rplrec_stamp(int year, int month, int day, int hour, int minute)
{
    uint32_t v = (uint32_t)year;
    v = (v << 4) | (uint32_t)month;
    v = (v << 5) | (uint32_t)day;
    v = (v << 5) | (uint32_t)hour;
    return (v << 6) | (uint32_t)minute;
}

/* ---- the file ----------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int rplrec_begin(rplrec *r, const char *path, const uint8_t hdr[RPL_HDR_SIZE],
                 uint32_t stamp)
{
    uint8_t fh[RPL_FILE_HDR];

    memset(r, 0, sizeof *r);
    r->f = fopen(path, "wb");
    if (!r->f) return -1;
    memset(fh, 0, sizeof fh);
    memcpy(fh, "REPL", 4);                         /* 0x0041D986 */
    put32(fh + 0x08, RPL_VERSION);
    put32(fh + 0x0C, stamp);
    memcpy(fh + 0x10, "tmpreplay", 9);             /* 0x0041D9A4.. */
    memcpy(fh + 0x30, hdr, RPL_HDR_SIZE);
    r->mode20 = (int32_t)((uint32_t)hdr[RPLH_MODE20] | (uint32_t)hdr[RPLH_MODE20 + 1] << 8 |
                          (uint32_t)hdr[RPLH_MODE20 + 2] << 16 | (uint32_t)hdr[RPLH_MODE20 + 3] << 24);
    if (fwrite(fh, 1, sizeof fh, r->f) != sizeof fh) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    r->pos = RPL_FILE_HDR;
    r->open = 1;
    r->done = 0;
    return 0;
}

void rplrec_snapshot(rplrec *r, const uint8_t *const car[4],
                     const uint8_t *parts, const uint8_t *tab)
{
    uint8_t *p = r->chunk;
    int k;
    for (k = 0; k < 4; k++, p += RPL_CAR_BYTES) {
        if (car && car[k]) memcpy(p, car[k], RPL_CAR_BYTES);
        else memset(p, 0, RPL_CAR_BYTES);
    }
    if (parts) memcpy(p, parts, RPL_PARTS_BYTES); else memset(p, 0, RPL_PARTS_BYTES);
    p += RPL_PARTS_BYTES;
    if (tab) memcpy(p, tab, RPL_TAB_BYTES); else memset(p, 0, RPL_TAB_BYTES);
}

#define FRAMES_AT (4u * RPL_CAR_BYTES + RPL_PARTS_BYTES + RPL_TAB_BYTES)   /* 0x21D0 */

void rplrec_frame(rplrec *r, unsigned fr, unsigned p, uint32_t bx, uint32_t ax,
                  int32_t cx, uint32_t dx)
{
    uint8_t *q;
    if (p >= 2 || fr >= RPL_FRAMES) return;        /* 0x0045DCFD's guard */
    q = r->chunk + FRAMES_AT + fr * 16u + p * 8u;
    put32(q, bx);
    put32(q + 4, rplrec_word_pack(ax, cx, dx));
}

/* fmt_replay_write_block: pack, keep the packed form if it is smaller after
 * rounding up to a dword (0x0041E10C), else the raw chunk.  The padding is
 * the three zero bytes the terminator's dword store leaves behind. */
static int write_block(rplrec *r, int32_t *len_out)
{
    uint8_t packed[RPL_CHUNK + RPL_CHUNK / 128 + 16];
    size_t n = rplrec_pack(r->chunk, RPL_CHUNK, packed);
    size_t rounded = (n + 3u) & ~(size_t)3u;
    size_t k;

    for (k = n; k < rounded; k++) packed[k] = 0;
    if (rounded < RPL_CHUNK) {
        if (fwrite(packed, 1, rounded, r->f) != rounded) return -1;
        *len_out = -(int32_t)rounded;
    } else {
        if (fwrite(r->chunk, 1, RPL_CHUNK, r->f) != RPL_CHUNK) return -1;
        *len_out = (int32_t)RPL_CHUNK;
    }
    return 0;
}

int rplrec_commit(rplrec *r)
{
    int32_t len;
    if (!r->open) return 0;
    r->frames += (int32_t)RPL_FRAMES;              /* 0x0041DB4F */
    put32(r->chunk, 0);                            /* the duplicated store */
    if (r->mode20 != 1) {                          /* 0x0041DB74 */
        if (r->chunk[5] < 2) r->chunk[5] = 2;
        if (r->chunk[5] > 3) r->chunk[5] = 3;
    }
    r->idx_off[r->nchunks] = (int32_t)r->pos;      /* fmt_replay_flush_chunk */
    if (write_block(r, &len) != 0) { rplrec_end(r); return -1; }
    r->idx_len[r->nchunks] = len;
    r->pos += (uint32_t)(len < 0 ? -len : len);
    r->nchunks++;
    if (r->nchunks == RPL_MAX_CHUNKS) {            /* 0x0041DBFC */
        r->full = 1;
        rplrec_end(r);
        return -1;
    }
    return 0;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int rplrec_resave(const char *src, const char *dst, const uint8_t tag[32],
                  uint32_t stamp)
{
    FILE *f = fopen(src, "rb");
    uint8_t *in = NULL, *out = NULL, *t;
    uint8_t raw[RPL_CHUNK], packed[RPL_CHUNK + RPL_CHUNK / 128 + 16];
    long n;
    uint32_t nch, i, pos = RPL_FILE_HDR, cap;
    int32_t idx_off[RPL_MAX_CHUNKS], idx_len[RPL_MAX_CHUNKS];
    int rc = -1;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < (long)(RPL_FILE_HDR + RPL_TRAILER) ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    in = malloc((size_t)n);
    if (!in || fread(in, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(in); return -1; }
    fclose(f);
    t = in + n - RPL_TRAILER;
    if (memcmp(in, "REPL", 4) != 0 || get32(in + 8) != RPL_VERSION ||
        memcmp(t, "RCIB", 4) != 0) goto done;
    nch = get32(t + 8);
    if ((int32_t)nch <= 0) goto done;
    if (nch > RPL_MAX_CHUNKS) nch = RPL_MAX_CHUNKS;          /* the load clamps */
    cap = RPL_FILE_HDR + nch * (RPL_CHUNK + 4u) + RPL_TRAILER;
    out = malloc(cap);
    if (!out) goto done;
    memcpy(out, in, RPL_FILE_HDR);                            /* the header the load read */
    put32(out + 0x04, 0);                                     /* 0x0041DF41: EDX */
    put32(out + 0x08, RPL_VERSION);
    put32(out + 0x0C, stamp);
    memcpy(out + 0x10, tag, 32);                              /* 0x0041DF6A */
    for (i = 0; i < nch; i++) {
        int32_t off = (int32_t)get32(t + 0x10 + i * 8), len = (int32_t)get32(t + 0x14 + i * 8);
        size_t pl, rounded, k;
        if (off < (int32_t)RPL_FILE_HDR || len == 0 ||
            (uint32_t)off + (uint32_t)(len < 0 ? -len : len) > (uint32_t)(n - (long)RPL_TRAILER)) goto done;
        if (len > 0) {
            if (len != (int32_t)RPL_CHUNK) goto done;
            memcpy(raw, in + off, RPL_CHUNK);
        } else if (rplrec_unpack(in + off, (size_t)-len, raw, RPL_CHUNK) != RPL_CHUNK) goto done;
        pl = rplrec_pack(raw, RPL_CHUNK, packed);             /* fmt_replay_write_block */
        rounded = (pl + 3u) & ~(size_t)3u;
        for (k = pl; k < rounded; k++) packed[k] = 0;
        idx_off[i] = (int32_t)pos;
        if (rounded < RPL_CHUNK) {
            memcpy(out + pos, packed, rounded);
            idx_len[i] = -(int32_t)rounded;
            pos += (uint32_t)rounded;
        } else {
            memcpy(out + pos, raw, RPL_CHUNK);
            idx_len[i] = (int32_t)RPL_CHUNK;
            pos += RPL_CHUNK;
        }
    }
    memset(out + pos, 0, RPL_TRAILER);                        /* 0x0041E06E: the trailer */
    memcpy(out + pos, t, 0x10);                               /*   its own 16 bytes: RCIB, the count, the frames */
    for (i = 0; i < nch; i++) {
        put32(out + pos + 0x10 + i * 8, (uint32_t)idx_off[i]);
        put32(out + pos + 0x14 + i * 8, (uint32_t)idx_len[i]);
    }
    pos += RPL_TRAILER;
    f = fopen(dst, "wb");
    if (f) {
        rc = fwrite(out, 1, pos, f) == pos ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
done:
    free(in);
    free(out);
    return rc;
}

/* ---- the reader ---------------------------------------------------------- */
int rplrd_open(rplrd *r, const char *path, char *why, size_t whyn)
{
    FILE *f = fopen(path, "rb");
    long n;
    const uint8_t *t;
    uint32_t i, cnt;
    char tmp[64];

    memset(r, 0, sizeof *r);
    r->cur = -1;
    if (!why) { why = tmp; whyn = sizeof tmp; }
    if (!f) { snprintf(why, whyn, "cannot open %s", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < (long)(RPL_FILE_HDR + RPL_TRAILER) ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(why, whyn, "%s is too short for a replay", path);
        return -1;
    }
    r->data = malloc((size_t)n);
    if (!r->data || fread(r->data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(r->data);
        r->data = NULL;
        snprintf(why, whyn, "cannot read %s", path);
        return -1;
    }
    fclose(f);
    r->size = (size_t)n;
    t = r->data + n - RPL_TRAILER;
    if (memcmp(r->data, "REPL", 4) != 0 || get32(r->data + 8) != RPL_VERSION ||
        memcmp(t, "RCIB", 4) != 0) {
        snprintf(why, whyn, "%s is not a version 5.25 replay", path);
        rplrd_close(r);
        return -1;
    }
    cnt = get32(t + 8);
    if ((int32_t)cnt <= 0) { snprintf(why, whyn, "%s holds no chunk", path); rplrd_close(r); return -1; }
    if (cnt > RPL_MAX_CHUNKS) cnt = RPL_MAX_CHUNKS;
    r->nchunks = cnt;
    r->frames = (int32_t)get32(t + 12);
    for (i = 0; i < cnt; i++) {
        r->idx_off[i] = (int32_t)get32(t + 0x10 + i * 8);
        r->idx_len[i] = (int32_t)get32(t + 0x14 + i * 8);
        if (r->idx_off[i] < (int32_t)RPL_FILE_HDR || r->idx_len[i] == 0 ||
            (uint32_t)r->idx_off[i] + (uint32_t)(r->idx_len[i] < 0 ? -r->idx_len[i] : r->idx_len[i])
                > (uint32_t)(n - (long)RPL_TRAILER)) {
            snprintf(why, whyn, "%s: chunk %u lies outside the file", path, i);
            rplrd_close(r);
            return -1;
        }
    }
    return 0;
}

void rplrd_close(rplrd *r)
{
    free(r->data);
    r->data = NULL;
    r->nchunks = 0;
    r->cur = -1;
}

int rplrd_chunk(rplrd *r, uint32_t k)
{
    int32_t len;
    if (!r->data || k >= r->nchunks) return -1;
    if (r->cur == (int)k) return 0;
    len = r->idx_len[k];
    r->cur = -1;
    if (len > 0) {
        if (len != (int32_t)RPL_CHUNK) return -1;
        memcpy(r->raw, r->data + r->idx_off[k], RPL_CHUNK);
    } else if (rplrec_unpack(r->data + r->idx_off[k], (size_t)-len, r->raw, RPL_CHUNK) != RPL_CHUNK)
        return -1;
    r->cur = (int)k;
    return 0;
}

int32_t rplrd_hdr(const rplrd *r, unsigned off)
{
    return r->data ? (int32_t)get32(r->data + 0x30 + off) : 0;
}

void rplrd_tag(const rplrd *r, char out[33])
{
    memset(out, 0, 33);
    if (r->data) memcpy(out, r->data + 0x10, 32);
}

void rplrd_frame(const rplrd *r, unsigned fr, unsigned p, uint32_t *bx,
                 uint32_t *ax, int32_t *cx, uint32_t *dx)
{
    const uint8_t *q = r->raw + FRAMES_AT + (fr % RPL_FRAMES) * 16u + (p & 1u) * 8u;
    *bx = get32(q);
    rplrec_word_unpack(get32(q + 4), ax, cx, dx);
}

int rplrec_end(rplrec *r)
{
    uint8_t t[RPL_TRAILER];
    uint32_t i;
    int rc = 0;

    if (!r->open) return 0;
    memset(t, 0, sizeof t);
    memcpy(t, "RCIB", 4);                          /* 0x0041DA02 */
    put32(t + 0x08, r->nchunks);
    put32(t + 0x0C, (uint32_t)r->frames);
    for (i = 0; i < r->nchunks; i++) {
        put32(t + 0x10 + i * 8, (uint32_t)r->idx_off[i]);
        put32(t + 0x14 + i * 8, (uint32_t)r->idx_len[i]);
    }
    if (fwrite(t, 1, sizeof t, r->f) != sizeof t) rc = -1;
    if (fclose(r->f) != 0) rc = -1;
    r->f = NULL;
    r->open = 0;
    r->done = 1;                                   /* [0x574548] = 1 */
    return rc;
}
