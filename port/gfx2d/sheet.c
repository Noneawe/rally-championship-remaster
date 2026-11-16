/* sheet.c - the sheet cut, FUN_00411D4C.  See sheet.h. */
#include "sheet.h"

#include <stdlib.h>
#include <string.h>

void sheet_builder_init(sheet_builder *b)
{
    memset(b, 0, sizeof *b);
}

void sheet_builder_free(sheet_builder *b)
{
    free(b->data);
    free(b->off);
    memset(b, 0, sizeof *b);
}

static int emit(sheet_builder *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        uint8_t *q;
        while (nc < b->len + n) nc *= 2;
        q = (uint8_t *)realloc(b->data, nc);
        if (!q) return -1;
        b->data = q;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int emit_unit(sheet_builder *b, int wide, uint32_t v)
{
    /* 0x0041263D: the unit is one byte or one dword - never 2 here, see
     * sheet.h on why the port takes the 32-bit arm */
    if (!wide) {
        uint8_t c = (uint8_t)v;
        return emit(b, &c, 1);
    }
    {
        uint8_t d[4];
        d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
        d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
        return emit(b, d, 4);
    }
}

typedef struct {
    uint32_t *px;
    int       w, h;
    uint32_t  bg, key;
    int       wide;
} cut_state;

/* 0x0041259A: out of the sheet reads as background */
static int nonbg(const cut_state *s, int x, int y)
{
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return 0;
    return s->px[(size_t)y * s->w + x] != s->bg;
}

static int transparent(const cut_state *s, uint32_t v)
{
    return v == s->key || v == s->bg;          /* 0x0041252E / 0x00412536 */
}

/* 0x0041250E: from (x, y), with `*left` pixels remaining in the row, count
 * the transparent pixels then the opaque ones.  Advances *x and *left. */
static void runs(const cut_state *s, int y, int *x, int *left, int *lead, int *run)
{
    const uint32_t *row = s->px + (size_t)y * s->w;
    *lead = *run = 0;
    while (*left > 0 && transparent(s, row[*x])) { (*x)++; (*lead)++; (*left)--; }
    while (*left > 0 && !transparent(s, row[*x])) { (*x)++; (*run)++; (*left)--; }
}

/* 0x00412226 with [0x512C90] & 1: trim, then the RLE. */
static int encode(sheet_builder *b, const cut_state *s, int bx, int by,
                  int bw, int bh)
{
    int top = 0, y0 = by, rows = bh - 1, yb, y, x, left, lead, run;
    uint32_t lmin = 0xFFFFFFFFu, rmin = 0xFFFFFFFFu;
    int x0, w, h;
    size_t *no;

    /* 0x0041229C: the top, while more than one row is left */
    while (rows != 0) {
        x = bx; left = bw;
        runs(s, y0, &x, &left, &lead, &run);
        if (run != 0) break;
        y0++; top++; rows--;
    }
    /* 0x004122BF: the bottom, the same way */
    yb = y0 + rows;
    while (rows != 0) {
        x = bx; left = bw;
        runs(s, yb, &x, &left, &lead, &run);
        if (run != 0) break;
        yb--; rows--;
    }
    /* 0x004122F0: the least leading and the least trailing run */
    for (y = y0; y <= y0 + rows; y++) {
        x = bx; left = bw;
        runs(s, y, &x, &left, &lead, &run);
        if ((uint32_t)lead < lmin) lmin = (uint32_t)lead;
        while (run != 0) runs(s, y, &x, &left, &lead, &run);
        if ((uint32_t)lead < rmin) rmin = (uint32_t)lead;
    }
    /* 0x00412335: x0 += lmin; w = (bw - 1) - lmin, then - rmin unless that
     * borrowed (JB 0x00412349); +1 */
    x0 = bx + (int)lmin;
    w = bw - 1 - (int)lmin;
    if (w >= 0) w -= (int)rmin;
    w += 1;
    h = rows + 1;

    if (b->n == b->ncap) {
        unsigned nc = b->ncap ? b->ncap * 2 : 256;
        no = (size_t *)realloc(b->off, nc * sizeof *no);
        if (!no) return -1;
        b->off = no;
        b->ncap = nc;
    }
    b->off[b->n++] = b->len;

    if (!s->wide) {                            /* 0x00412421: COMPACT        */
        uint8_t hd[4];
        hd[0] = (uint8_t)lmin; hd[1] = (uint8_t)top;
        hd[2] = (uint8_t)w;    hd[3] = (uint8_t)h;
        if (emit(b, hd, 4)) return -1;
    } else {                                   /* 0x00412468: EXTENDED       */
        uint8_t hd[12];
        uint32_t tag = (uint32_t)-4;           /* NEG of the unit, 4         */
        memcpy(hd, &tag, 4);
        hd[4] = (uint8_t)lmin; hd[5] = (uint8_t)(lmin >> 8);
        hd[6] = (uint8_t)top;  hd[7] = (uint8_t)(top >> 8);
        hd[8] = (uint8_t)w;    hd[9] = (uint8_t)(w >> 8);
        hd[10] = (uint8_t)h;   hd[11] = (uint8_t)(h >> 8);
        if (tag != 0xFFFFFFFCu) return -1;
        if (emit(b, hd, 12)) return -1;
    }
    /* 0x00412492: per row, {count, skip, pixels}...; count 0 ends it */
    for (y = y0; y < y0 + h; y++) {
        x = x0; left = w;
        for (;;) {
            int i, xs;
            runs(s, y, &x, &left, &lead, &run);
            if (emit_unit(b, s->wide, (uint32_t)run)) return -1;
            if (run == 0) break;
            if (emit_unit(b, s->wide, (uint32_t)lead)) return -1;
            xs = x - run;
            for (i = 0; i < run; i++) {
                uint32_t v = s->px[(size_t)y * s->w + xs + i];
                if (emit_unit(b, s->wide, s->wide ? (0xFF000000u | v) : v))
                    return -1;
            }
        }
    }
    return 0;
}

int sheet_cut(sheet_builder *b, const uint32_t *px, int w, int h,
              const uint32_t *pal)
{
    cut_state s;
    int x, y, count = 0;

    if (w <= 0 || h <= 0) return 0;
    s.w = w; s.h = h; s.wide = pal == NULL;
    s.px = (uint32_t *)malloc((size_t)w * h * sizeof *s.px);
    if (!s.px) return -1;
    memcpy(s.px, px, (size_t)w * h * sizeof *s.px);

    /* FUN_00411EB7 */
    {
        uint32_t v = s.px[0];
        s.bg = (s.px[w - 1] == v && s.px[(size_t)(h - 1) * w + w - 1] == v &&
                s.px[(size_t)(h - 1) * w] == v) ? v : 0;
    }
    /* FUN_00411EF9 */
    if (s.wide) {
        s.key = 0xFF00FFu;
    } else {
        int i;
        s.key = s.bg;
        for (i = 0; i < 256; i++)
            if (pal[i] == 0xFF00FFu) { s.key = (uint32_t)i; break; }
    }

    /* FUN_00411F32 */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int L, T, R, B, moved, t;
            if (s.px[(size_t)y * w + x] == s.bg) continue;
            L = x; T = y; R = x + 1; B = y + 1;
            do {
                moved = 0;
                for (t = T; t <= B;) {                    /* left, T..B     */
                    if (nonbg(&s, L, t)) { L--; t = T; moved++; } else t++;
                }
                for (t = L; t <= R;) {                    /* top, L..R      */
                    if (nonbg(&s, t, T)) { T--; t = L; moved++; } else t++;
                }
                for (t = B;;) {                           /* right, B..T+1  */
                    if (nonbg(&s, R, t)) { R++; t = B; moved++; continue; }
                    if (--t == T) break;
                }
                for (t = R;;) {                           /* bottom, R..L+1 */
                    if (nonbg(&s, t, B)) { B++; t = R; moved++; continue; }
                    if (--t == L) break;
                }
            } while (moved);
            {
                int bx = L + 1, by = T + 1, bw = R - L - 1, bh = B - T - 1, j, i;
                if (encode(b, &s, bx, by, bw, bh)) { free(s.px); return -1; }
                count++;
                for (j = by; j < by + bh; j++)            /* 0x00411FF6     */
                    for (i = bx; i < bx + bw; i++)
                        s.px[(size_t)j * w + i] = s.bg;
            }
        }
    }
    free(s.px);
    return count;
}

uint8_t *sheet_bundle(const sheet_builder *b, size_t *len)
{
    size_t tab = (size_t)b->n * 4u, i;
    uint8_t *out = (uint8_t *)malloc(tab + b->len + 4u);
    if (!out) return NULL;
    for (i = 0; i < b->n; i++) {
        uint32_t o = (uint32_t)(tab + b->off[i]);
        out[4 * i] = (uint8_t)o; out[4 * i + 1] = (uint8_t)(o >> 8);
        out[4 * i + 2] = (uint8_t)(o >> 16); out[4 * i + 3] = (uint8_t)(o >> 24);
    }
    if (b->len) memcpy(out + tab, b->data, b->len);
    *len = tab + b->len;
    return out;
}
