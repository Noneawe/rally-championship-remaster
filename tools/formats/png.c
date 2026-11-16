/* png.c - see png.h. */
#include "png.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void perr(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    if (!err || !n) return;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- CRC-32 (PNG chunks) ------------------------------------------------- */

static uint32_t crc_tab[256];
static int crc_ready;

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t i;
    if (!crc_ready) {
        uint32_t k, j, v;
        for (k = 0; k < 256; k++) {
            v = k;
            for (j = 0; j < 8; j++) v = (v & 1u) ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_tab[k] = v;
        }
        crc_ready = 1;
    }
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* ---- DEFLATE (RFC 1951) -------------------------------------------------- */

typedef struct {
    const uint8_t *src;
    size_t   len, pos;
    uint32_t bitbuf;
    int      bitcnt;
    uint8_t *out;
    size_t   outlen, outcap;
    int      overrun;            /* read past the end of the input */
} inflater;

static int getbit(inflater *s)
{
    if (s->bitcnt == 0) {
        if (s->pos >= s->len) { s->overrun = 1; return 0; }
        s->bitbuf = s->src[s->pos++];
        s->bitcnt = 8;
    }
    s->bitcnt--;
    {
        int b = (int)(s->bitbuf & 1u);
        s->bitbuf >>= 1;
        return b;
    }
}

static uint32_t getbits(inflater *s, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) v |= (uint32_t)getbit(s) << i;
    return v;
}

/* A canonical Huffman code, decoded a bit at a time (RFC 1951 3.2.2): the
 * count of codes of each length and the symbols in code order. */
typedef struct {
    short count[16];
    short symbol[320];
} huff;

static int build(huff *h, const short *lengths, int n)
{
    short offs[16];
    int i, left = 1;
    memset(h->count, 0, sizeof h->count);
    for (i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;          /* no codes: complete enough */
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;             /* over-subscribed */
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
    return 0;
}

static int decode_sym(inflater *s, const huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int count;
        code |= getbit(s);
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->overrun) return -1;
    }
    return -1;
}

static int put(inflater *s, uint8_t b)
{
    if (s->outlen >= s->outcap) {
        size_t nc = s->outcap ? s->outcap * 2 : 1u << 16;
        uint8_t *p = (uint8_t *)realloc(s->out, nc);
        if (!p) return -1;
        s->out = p;
        s->outcap = nc;
    }
    s->out[s->outlen++] = b;
    return 0;
}

static const short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
    65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const short dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
    5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(inflater *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = decode_sym(s, lit);
        if (sym < 0 || s->overrun) return -1;
        if (sym < 256) {
            if (put(s, (uint8_t)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            int n, d;
            size_t i, from;
            sym -= 257;
            if (sym >= 29) return -1;
            n = len_base[sym] + (int)getbits(s, len_extra[sym]);
            sym = decode_sym(s, dist);
            if (sym < 0 || sym >= 30) return -1;
            d = dist_base[sym] + (int)getbits(s, dist_extra[sym]);
            if ((size_t)d > s->outlen) return -1;
            from = s->outlen - (size_t)d;
            for (i = 0; i < (size_t)n; i++)
                if (put(s, s->out[from + i])) return -1;
        }
    }
}

static int inflate_all(inflater *s)
{
    int last;
    do {
        int type;
        last = getbit(s);
        type = (int)getbits(s, 2);
        if (s->overrun) return -1;
        if (type == 0) {                     /* stored */
            unsigned n, nn;
            s->bitcnt = 0;
            if (s->pos + 4 > s->len) return -1;
            n  = s->src[s->pos] | (s->src[s->pos + 1] << 8);
            nn = s->src[s->pos + 2] | (s->src[s->pos + 3] << 8);
            s->pos += 4;
            if ((n ^ 0xffffu) != nn || s->pos + n > s->len) return -1;
            while (n--) if (put(s, s->src[s->pos++])) return -1;
        } else if (type == 1) {              /* fixed Huffman */
            static huff lit, dist;
            static int ready;
            if (!ready) {
                short l[288];
                int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                build(&lit, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                build(&dist, l, 30);
                ready = 1;
            }
            if (codes(s, &lit, &dist)) return -1;
        } else if (type == 2) {              /* dynamic Huffman */
            static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                             11, 4, 12, 3, 13, 2, 14, 1, 15 };
            short lengths[320];
            huff lencode, lit, dist;
            int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1,
                ncode = (int)getbits(s, 4) + 4, i = 0;
            if (nlen > 286 || ndist > 30) return -1;
            for (i = 0; i < 19; i++) lengths[order[i]] = 0;
            for (i = 0; i < ncode; i++) lengths[order[i]] = (short)getbits(s, 3);
            if (build(&lencode, lengths, 19)) return -1;
            i = 0;
            while (i < nlen + ndist) {
                int sym = decode_sym(s, &lencode), rep, val = 0;
                if (sym < 0) return -1;
                if (sym < 16) { lengths[i++] = (short)sym; continue; }
                if (sym == 16) {
                    if (i == 0) return -1;
                    val = lengths[i - 1];
                    rep = 3 + (int)getbits(s, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)getbits(s, 3);
                } else {
                    rep = 11 + (int)getbits(s, 7);
                }
                if (i + rep > nlen + ndist) return -1;
                while (rep--) lengths[i++] = (short)val;
            }
            if (lengths[256] == 0) return -1;
            if (build(&lit, lengths, nlen)) return -1;
            if (build(&dist, lengths + nlen, ndist)) return -1;
            if (codes(s, &lit, &dist)) return -1;
        } else {
            return -1;
        }
        if (s->overrun) return -1;
    } while (!last);
    return 0;
}

/* ---- PNG ----------------------------------------------------------------- */

static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

int png_size(const void *buf, size_t len, unsigned *w, unsigned *h,
             char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (len < 33 || memcmp(b, sig, 8) != 0 || memcmp(b + 12, "IHDR", 4) != 0) {
        perr(err, errlen, "not a PNG (no signature / IHDR)");
        return 1;
    }
    *w = be32(b + 16);
    *h = be32(b + 20);
    return 0;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

int png_decode(const void *buf, size_t len, uint32_t *out, size_t outcap,
               unsigned *pw, unsigned *ph, char *err, size_t errlen)
{
    const uint8_t *b = (const uint8_t *)buf;
    size_t p = 8, idat_len = 0;
    uint8_t *idat = NULL;
    unsigned w = 0, h = 0, depth = 0, ctype = 0, inter = 0, bpp, x, y;
    uint32_t pal[256];
    int have_ihdr = 0, have_iend = 0, rc = 1;
    inflater s;
    size_t stride;
    uint8_t *prev = NULL;

    memset(&s, 0, sizeof s);
    for (x = 0; x < 256; x++) pal[x] = 0xff000000u;
    if (len < 8 || memcmp(b, sig, 8) != 0) {
        perr(err, errlen, "not a PNG (signature)");
        return 1;
    }
    while (p + 12 <= len && !have_iend) {
        uint32_t n = be32(b + p);
        const uint8_t *type = b + p + 4, *data = b + p + 8;
        if (n > len - p - 12) {
            perr(err, errlen, "chunk at %lu runs past the end", (unsigned long)p);
            goto out;
        }
        if (crc32_of(type, n + 4u) != be32(data + n)) {
            perr(err, errlen, "chunk %.4s at %lu: CRC mismatch", type,
                 (unsigned long)p);
            goto out;
        }
        if (!memcmp(type, "IHDR", 4) && n >= 13) {
            w = be32(data); h = be32(data + 4);
            depth = data[8]; ctype = data[9]; inter = data[12];
            have_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            for (x = 0; x < n / 3 && x < 256; x++)
                pal[x] = 0xff000000u | ((uint32_t)data[3 * x] << 16) |
                         ((uint32_t)data[3 * x + 1] << 8) | data[3 * x + 2];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *q = (uint8_t *)realloc(idat, idat_len + n + 1);
            if (!q) { perr(err, errlen, "out of memory"); goto out; }
            idat = q;
            memcpy(idat + idat_len, data, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) {
            have_iend = 1;
        }
        p += 12u + n;
    }
    if (!have_ihdr || !have_iend || !idat_len) {
        perr(err, errlen, "missing IHDR, IDAT or IEND");
        goto out;
    }
    if (depth != 8 || inter != 0 ||
        (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 6)) {
        perr(err, errlen, "unsupported: depth %u, colour type %u, interlace %u",
             depth, ctype, inter);
        goto out;
    }
    if ((size_t)w * h > outcap) {
        perr(err, errlen, "output holds %lu of %lu pixels",
             (unsigned long)outcap, (unsigned long)((size_t)w * h));
        goto out;
    }
    bpp = ctype == 2 ? 3u : ctype == 6 ? 4u : 1u;
    stride = (size_t)w * bpp;

    /* zlib: CMF/FLG, DEFLATE, Adler-32 */
    if (idat_len < 6 || (idat[0] & 0x0f) != 8 ||
        ((idat[0] << 8) | idat[1]) % 31 != 0 || (idat[1] & 0x20)) {
        perr(err, errlen, "bad zlib header");
        goto out;
    }
    s.src = idat + 2;
    s.len = idat_len - 2;
    if (inflate_all(&s) != 0) {
        perr(err, errlen, "corrupt DEFLATE stream");
        goto out;
    }
    if (s.outlen != (stride + 1) * h) {
        perr(err, errlen, "inflated %lu bytes, expected %lu",
             (unsigned long)s.outlen, (unsigned long)((stride + 1) * h));
        goto out;
    }
    {
        uint32_t a = 1, c = 0;
        size_t i;
        for (i = 0; i < s.outlen; i++) {
            a = (a + s.out[i]) % 65521u;
            c = (c + a) % 65521u;
        }
        if (s.pos + 4 > s.len || ((c << 16) | a) != be32(s.src + s.pos)) {
            perr(err, errlen, "zlib Adler-32 mismatch");
            goto out;
        }
    }
    prev = (uint8_t *)calloc(stride, 1);
    if (!prev) { perr(err, errlen, "out of memory"); goto out; }
    for (y = 0; y < h; y++) {
        uint8_t *row = s.out + (size_t)y * (stride + 1);
        unsigned f = row[0];
        uint8_t *cur = row + 1;
        size_t i;
        for (i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, up = prev[i],
                c = i >= bpp ? prev[i - bpp] : 0, v;
            switch (f) {
            case 0: v = cur[i]; break;
            case 1: v = cur[i] + a; break;
            case 2: v = cur[i] + up; break;
            case 3: v = cur[i] + ((a + up) >> 1); break;
            case 4: v = cur[i] + paeth(a, up, c); break;
            default:
                perr(err, errlen, "row %u: filter %u", y, f);
                goto out;
            }
            cur[i] = (uint8_t)v;
        }
        for (x = 0; x < w; x++) {
            const uint8_t *q = cur + (size_t)x * bpp;
            uint32_t v;
            if (ctype == 2)      v = 0xff000000u | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
            else if (ctype == 6) v = ((uint32_t)q[3] << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
            else if (ctype == 3) v = pal[q[0]];
            else                 v = 0xff000000u | ((uint32_t)q[0] * 0x010101u);
            out[(size_t)y * w + x] = v;
        }
        memcpy(prev, cur, stride);
    }
    *pw = w;
    *ph = h;
    rc = 0;
out:
    free(prev);
    free(s.out);
    free(idat);
    return rc;
}
