/* gfx2d.c - sprite_blit, draw_text and draw_list, transcribed.
 *
 * The reading is
 * Every block
 * below carries the address of the instructions it came from; where the
 * transcription DIVERGES from the original the comment says so and why.
 *
 * The four hazards says a port must not
 * get wrong are handled at the lines marked HAZARD 1..4, each carrying the
 * section number that states it.
*/

#include "gfx2d.h"
#include "plat.h"

#include <stdlib.h>
#include <string.h>

/* ---- little-endian readers ---------------------------------------------
 * The bundle is a byte image and its integers are little-endian on disk.
 * Reading them a byte at a time rather than by cast is not pedantry: the
 * target is aarch64 and the offset table is not guaranteed 4-aligned inside a
 * caller-supplied buffer. */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* ---- 1. the surface ---------------------------------------------------- */

void gfx2d_surface_init(gfx2d_surface *s, uint32_t *px, int w, int h)
{
    if (!s) {
        return;
    }
    s->px = px;
    s->w  = w;
    s->h  = h;
    gfx2d_set_clip(s, 0, 0, w - 1, h - 1);
}

void gfx2d_set_clip(gfx2d_surface *s, int x0, int y0, int x1, int y1)
{
    if (!s) {
        return;
    }
    /* Clamped to the surface, which the original does NOT do - it keeps four
     * bare globals and trusts whoever wrote them.  Here the clip is the ONLY
     * thing that keeps the blitter's writes inside the buffer, so it is the
     * one place the bound is established. */
    if (x0 < 0)      { x0 = 0; }
    if (y0 < 0)      { y0 = 0; }
    if (x1 > s->w - 1) { x1 = s->w - 1; }
    if (y1 > s->h - 1) { y1 = s->h - 1; }
    s->clip_x0 = x0;
    s->clip_y0 = y0;
    s->clip_x1 = x1;
    s->clip_y1 = y1;
}

/* 0x00405E18 - the original clears its 800x600 canvas with
 * `rep stosd` of 0x75300 dwords.  Same operation, same shape. */
void gfx2d_clear(gfx2d_surface *s, uint32_t argb)
{
    long i, n;

    if (!s || !s->px) {
        return;
    }
    n = (long)s->w * s->h;
    for (i = 0; i < n; i++) {
        s->px[i] = argb;
    }
}

/* ---- 2. sprite_blit, 0x004066F9 ---------------------------------------- */

/* The blit's per-row state, so the three loops below read like the original's
 * three blocks rather than like one function with six extra arguments. */
typedef struct {
    gfx2d_surface  *s;
    const uint8_t  *p;      /* the run cursor - the original's ESI */
    const uint8_t  *end;    /* one past the bundle; the original has no bound */
    const uint32_t *lut;
    long            row;    /* pixel index of the row base - the original's EBX */
    int             clipl;  /* [0x004F0020] */
    int             visw;   /* [0x004F0024] */
    int             clipt;  /* [0x004F0028] */
    int             vish;   /* [0x004F002C] */
    unsigned        unit;   /* payload bytes per pixel: 1 (LUT), 4 (verbatim) */
    int             oops;   /* a write escaped the surface - see emit() */
} blit;

/* Reads one payload byte/dword and returns the surface pixel for it.  The
 * compact form's byte is a PALETTE INDEX expanded through the LUT
 * (0x00406A8C `MOV EBX,[EBP + EAX*4]`); the extended form's dword is already
 * in the display's format and is copied verbatim (0x00406D5B `REP MOVSD`). */
static uint32_t take_pixel(blit *b)
{
    uint32_t v;

    if (b->unit == 1) {
        v = b->lut[*b->p];
        b->p += 1;
    } else {
        v = rd32(b->p);
        b->p += 4;
    }
    return v;
}

/* Emits `n` pixels at `di`.  The bounds check must NEVER fire: the clip
 * arithmetic above already guarantees every written column lies in
 * [clip_x0, clip_x1] and every row in [clip_y0, clip_y1], both clamped to the
 * surface by gfx2d_set_clip.  It is here because "guaranteed by arithmetic
 * three blocks up" is exactly the kind of guarantee this project has been
 * wrong about, and a loud error beats a corrupted heap.  If it fires, the clip
 * transcription is wrong, not the caller. */
static long emit(blit *b, long di, int n)
{
    long cap = (long)b->s->w * b->s->h;

    while (n-- > 0) {
        if (b->p + b->unit > b->end) {
            b->oops = GFX2D_ERR_TRUNCATED;
            return di;
        }
        if (di < 0 || di >= cap) {
            b->oops = GFX2D_ERR_TRUNCATED;
            return di;
        }
        b->s->px[di++] = take_pixel(b);
    }
    return di;
}

/* Consumes one packet header.  Returns 0 at the row terminator. */
/* count and skip are UNIT-wide, like the pixels: a byte in the compact form
 * (0x00406ABB `MOV CL,[ESI]`), a dword in the 4-byte extended form
 * (0x00406D42 `MOV ECX,[ESI]` / `ADD ESI,4`, 0x00406D4B `MOV EAX,[ESI]` /
 * `ADD ESI,4`). said so; until this read a BYTE for
 * every form, which no gate could see because no shipped .bun is extended -
 * the sheet cut's 32-bit buttons were the first. */
static uint32_t rd_unit(const uint8_t *p, unsigned unit)
{
    if (unit == 4) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return p[0];
}

static int take_packet(blit *b, int *count, int *skip)
{
    if (b->p + b->unit > b->end) {
        b->oops = GFX2D_ERR_TRUNCATED;
        return 0;
    }
    *count = (int)rd_unit(b->p, b->unit);
    b->p += b->unit;
    if (*count == 0) {
        return 0;               /* 0x00406ABB `TEST CL,CL / JZ` */
    }
    if (b->p + b->unit > b->end) {
        b->oops = GFX2D_ERR_TRUNCATED;
        return 0;
    }
    *skip = (int)rd_unit(b->p, b->unit);
    b->p += b->unit;
    return 1;
}

/* Walks to the end of the current row without drawing - 0x00406AB8.  Used
 * both for the TOP clip and for the tail of a row that has run past the right
 * edge. */
static void skip_row(blit *b)
{
    int count, skip;

    while (take_packet(b, &count, &skip)) {
        if (b->p + (size_t)count * b->unit > b->end) {
            b->oops = GFX2D_ERR_TRUNCATED;
            return;
        }
        b->p += (size_t)count * b->unit;
    }
}

/* The container, 0x00406713: the entry count IS the first offset divided by
 * four.  There is no header, no magic and no count field. */
int gfx2d_entry_count(const void *bundle, size_t len)
{
    if (!bundle || len < 4) {
        return -1;
    }
    return (int)(rd32((const uint8_t *)bundle) >> 2);
}

/* One entry's header, 0x00406723 `CMP dword [ESI],-4 / JNC`.  Returns the
 * offset of the first payload byte, or a negative GFX2D_ERR_* code. */
static long entry_header(const uint8_t *base, size_t len, unsigned index,
                         gfx2d_entry *e)
{
    uint32_t nentries, off, tag;
    const uint8_t *p;

    if (!base || len < 4) {
        return -GFX2D_ERR_NO_BUNDLE;    /* guru 0x004D444C */
    }
    nentries = rd32(base) >> 2;
    if (index >= nentries) {
        return -GFX2D_ERR_INDEX;        /* guru 0x004D4465 */
    }
    if ((size_t)index * 4 + 4 > len) {
        return -GFX2D_ERR_TRUNCATED;
    }
    off = rd32(base + (size_t)index * 4);
    if (off + 4 > len) {
        return -GFX2D_ERR_TRUNCATED;
    }
    p   = base + off;
    tag = rd32(p);
    e->tag = tag;
    if (tag >= 0xFFFFFFFCu) {
        if (off + 12 > len) {
            return -GFX2D_ERR_TRUNCATED;
        }
        e->extended = 1;
        e->xoff = (int)rd16(p + 4);
        e->yoff = (int)rd16(p + 6);
        e->w    = (int)rd16(p + 8);
        e->h    = (int)rd16(p + 10);
        /* 0x00406BB6 discriminates ONLY 0xFFFFFFFE, whose units are two bytes
         * of the DISPLAY's 16bpp format. */
        e->unit = (tag == 0xFFFFFFFEu) ? 2u : 4u;
        return (long)off + 12;
    }
    e->extended = 0;
    e->xoff = p[0];
    e->yoff = p[1];
    e->w    = p[2];
    e->h    = p[3];
    e->unit = 1;
    return (long)off + 4;
}

int gfx2d_entry_info(const void *bundle, size_t len, unsigned index,
                     gfx2d_entry *e)
{
    long r;

    if (!e) {
        return GFX2D_ERR_NO_BUNDLE;
    }
    memset(e, 0, sizeof *e);
    r = entry_header((const uint8_t *)bundle, len, index, e);
    return r < 0 ? (int)-r : GFX2D_OK;
}

int gfx2d_blit(gfx2d_surface *s, const void *bundle, size_t len,
               unsigned index, const uint32_t *lut, int x, int y)
{
    const uint8_t *base = (const uint8_t *)bundle;
    gfx2d_entry    ent;
    int            xoff, yoff, w, h, t, i;
    long           payload;
    blit           b;

    if (!s || !s->px) {
        return GFX2D_ERR_NO_SURFACE;    /* guru 0x004D4433 */
    }
    payload = entry_header(base, len, index, &ent);
    if (payload < 0) {
        return (int)-payload;
    }
    /* A 16bpp payload has no meaning in a 32-bit surface, so it is refused
     * rather than guessed at.  It is also unexercised: 0 of 98 shipped
     * entries use ANY extended form. */
    if (ent.unit == 2) {
        return GFX2D_ERR_UNSUPPORTED;
    }
    xoff = ent.xoff; yoff = ent.yoff; w = ent.w; h = ent.h;
    b.unit = ent.unit;
    b.p    = base + payload;

    x += xoff;
    y += yoff;

    /* 0x00406737-0x004067B4.  Note the right and bottom tests are
     * `pos + size - 1 - limit`, so the clip limits are INCLUSIVE. */
    b.clipl = 0;
    t = x - s->clip_x0;
    if (t < 0) {
        b.clipl = -t;
    }
    t = x + w - 1 - s->clip_x1;
    if (t > 0) {
        w -= t;
    }
    w -= b.clipl;
    if (w <= 0) {
        return GFX2D_ERR_CLIPPED;
    }
    b.visw = w;

    b.clipt = 0;
    t = y - s->clip_y0;
    if (t < 0) {
        b.clipt = -t;
    }
    t = y + h - 1 - s->clip_y1;
    if (t > 0) {
        h -= t;
    }
    h -= b.clipt;
    if (h <= 0) {
        return GFX2D_ERR_CLIPPED;
    }
    b.vish = h;

    b.s    = s;
    b.lut  = lut;
    b.end  = base + len;
    b.oops = 0;

    /* 0x004067B9: the destination is the UNCLIPPED x on the first visible
     * row.  x may be negative here; the left-clip walk below advances the
     * cursor through the clipped columns, so the first pixel actually written
     * lands on clip_x0.  Keeping the index signed is what makes that legal. */
    b.row = (long)(y + b.clipt) * s->w + x;

    /* HAZARD 2 (section 8) - the TOP clip is done by WALKING the run stream,
     * 0x004069E7.  Packet lengths vary per row, so seeking by byte offset
     * desynchronises. */
    for (i = 0; i < b.clipt && !b.oops; i++) {
        skip_row(&b);
    }

    /* The row loop, 0x00406A12-0x00406AC6.
     *
     * The original has a SECOND, faster copy of this for the case
     * `[0x004F0024] == sprite width` (0x00406A04 `CMP [0x4F0024],EDI / JE`),
     * i.e. no horizontal clipping at all.  It is not transcribed, and that is
     * a deliberate omission rather than an oversight: with clipl == 0 and
     * visw == w the general path below consumes exactly the same packets and
     * writes exactly the same pixels - traced instruction by instruction on a
     * full-width row, where `edx` reaches zero on the last packet and the
     * right-edge arm at 0x00406A9A emits all of it.  One path is one thing to
     * get wrong. */
    while (b.vish > 0 && !b.oops) {
        long di    = b.row;
        int  left  = b.clipl;    /* EDX in the left-clip arm  */
        int  rem   = b.visw;     /* EDX in the visible arm    */
        int  count = 0, skip = 0;/* ECX, EAX                  */
        /* Which of the original's three entry points into the visible arm
         * this row starts at.  Naming them rather than inlining them is what
         * keeps the two left-clip exits honest: they arrive at DIFFERENT
         * instructions and one of them has already counted its run.
         *   0 = 0x00406A69, need a packet
         *   1 = 0x00406A78, a packet is in hand and NOT yet counted
         *   2 = 0x00406A7C, a packet is in hand and ALREADY counted */
        int  at = 0;
        int  row_ended = 0;

        /* ---- the left margin, 0x00406A30 ---------------------------------
         * Every packet is consumed and nothing is written until the cursor
         * passes clip_x0.  The run that STRADDLES the boundary is re-entered
         * by backing the source cursor up (0x00406A4C `ADD ESI,EDX` with EDX
         * negative), which is the second half of HAZARD 2: this cannot be
         * done by arithmetic either. */
        while (left > 0) {
            if (!take_packet(&b, &count, &skip)) {
                row_ended = 1;              /* 0x00406A35 */
                break;
            }
            di   += skip;
            left -= skip;
            if (left <= 0) {                /* 0x00406A5B - the SKIP cleared it */
                rem = b.visw + left;
                at  = 1;
                break;
            }
            b.p  += (size_t)count * b.unit; /* 0x00406A41 */
            di   += count;
            left -= count;
            if (left <= 0) {                /* 0x00406A4A - the RUN straddles */
                b.p  -= (size_t)(-left) * b.unit;
                di   += left;
                count = -left;              /* the visible remainder, ECX */
                rem   = b.visw + left;
                at    = 2;
                break;
            }
        }

        /* ---- the visible span, 0x00406A63 onward ------------------------ */
        while (!row_ended && !b.oops) {
            if (at == 0) {
                if (!take_packet(&b, &count, &skip)) {
                    break;                  /* the row terminator */
                }
                di  += skip;
                rem -= skip;
                at   = 1;
            }
            if (at == 1) {
                if (rem <= 0) {             /* 0x00406A78 - the SKIP overran */
                    b.p += (size_t)count * b.unit;
                    skip_row(&b);
                    break;
                }
                rem -= count;
                at   = 2;
            }
            if (rem <= 0) {                 /* 0x00406A9A - the run straddles
                                             * the right edge */
                int n = count + rem;

                if (n > 0) {
                    di = emit(&b, di, n);
                }
                b.p += (size_t)(-rem) * b.unit;   /* 0x00406AB6 `SUB ESI,EDX` */
                skip_row(&b);
                break;
            }
            if (count > 0) {
                di = emit(&b, di, count);
            }
            at = 0;
        }

        b.row += s->w;
        b.vish--;
    }

    return b.oops ? b.oops : GFX2D_OK;
}

void gfx2d_list(gfx2d_surface *s, const void *bundle, size_t len,
                const gfx2d_sprite_rec *rec, unsigned n, int ox, int oy)
{
    unsigned i;

    /* 0x004025FF / 0x00402603: the origin comes in ECX/EDX and is added to
     * every record.  The installer's one call site passes (0, 0). */
    for (i = 0; i < n; i++) {
        gfx2d_blit(s, bundle, len, rec[i].index, rec[i].lut,
                   (int)rec[i].x + ox, (int)rec[i].y + oy);
    }
}

/* ---- 3. draw_text, 0x0040239C ------------------------------------------ */

/* The kerning profile, 0x008E84D0: 64 rows of {right margin, left margin}.
 * 0x0040252A clears it once per line; 0x0040253E folds one glyph into it and
 * returns the minimum of the 64 left-margin bytes, which draw_text SUBTRACTS
 * from x.
 *
 * JADE NEVER EXERCISES IT: its four font records at 0x00517958 carry
 * flags == 0.  The FNT records DO: the race's
 * message banner (0x00517DEC.., FNT22_00) and the frontend's fonts
 * (0x00517ECC..) carry bit 0x04, so the first FNT string the port draws
 * is the first data this transcription meets.  Until a picture has
 * checked it, TENTATIVE. */

#define KERN_ROWS 64

typedef struct {
    uint8_t prof[KERN_ROWS * 2];
} kern_state;

static void kern_reset(kern_state *k)
{
    memset(k->prof, 0, sizeof k->prof);     /* 0x0040252A, 0x20 dwords */
}

static int kern_step(kern_state *k, const uint8_t *base, size_t len,
                     uint32_t nentries, unsigned glyph)
{
    const uint8_t *e;
    uint8_t *d = k->prof;
    uint8_t *lim = k->prof + sizeof k->prof;
    uint8_t  width, bl, bh, ah;
    uint32_t off;
    int      rows, best, i;

    if (glyph >= nentries || (size_t)glyph * 4 + 4 > len) {
        return 0;
    }
    off = rd32(base + (size_t)glyph * 4);
    if (off + 4 > len) {
        return 0;
    }
    e = base + off;

    width = e[2];
    if (width == 0) {
        return 0;                            /* 0x00402553 `TEST AL,AL / JZ` */
    }

    /* 0x00402562: the glyph's yoff rows carry no ink, so each one records a
     * right margin of 0 and a left margin of (previous right + width). */
    for (i = 0; i < e[1] && d + 1 < lim; i++) {
        ah   = d[0];
        d[0] = 0;
        d[1] = (uint8_t)(ah + width);
        d   += 2;
    }

    {
        const uint8_t *p = e + 4;
        int h = e[3];

        bl = width;
        bh = width;
        while (h-- > 0 && d + 1 < lim) {
            int first = 1;

            for (;;) {
                uint8_t count, skip;

                if (p >= base + len) {
                    return 0;
                }
                count = *p++;
                if (count == 0) {
                    break;
                }
                if (p >= base + len) {
                    return 0;
                }
                skip = *p++;
                if (first) {
                    bl    = skip;       /* 0x00402581 `MOV BL,[ESI]` */
                    first = 0;
                }
                bh = (uint8_t)(bh - count - skip);
                p += count;
            }
            bl   = (uint8_t)(bl + d[0]);
            d[0] = bh;
            d[1] = bl;
            d   += 2;
            bh   = width;
            bl   = width;
        }
    }

    /* 0x004025A8: every row past the glyph's height gets half the width. */
    rows = (int)((lim - d) / 2);
    for (i = 0; i < rows; i++) {
        ah   = d[0];
        d[0] = (uint8_t)(width >> 1);
        d[1] = ah;
        d   += 2;
    }

    /* 0x004025C2: the minimum left margin over all 64 rows. */
    best = 0x7fffffff;
    for (i = 0; i < KERN_ROWS; i++) {
        if (k->prof[i * 2 + 1] < best) {
            best = k->prof[i * 2 + 1];
        }
    }
    return best;
}

/* 0x004023E5 / 0x004024A5 - the glyph's own advance.  A zero WIDTH byte means
 * the advance is taken from byte 0 instead, which is how the space glyph's
 * `06 07 00 01 00` five bytes advance six pixels. */
static int glyph_advance(const uint8_t *base, size_t len, uint32_t nentries,
                         unsigned glyph)
{
    uint32_t off;

    if (glyph >= nentries || (size_t)glyph * 4 + 4 > len) {
        return 0;
    }
    off = rd32(base + (size_t)glyph * 4);
    if (off + 3 > len) {
        return 0;
    }
    return base[off + 2] ? base[off + 2] : base[off];
}

/* One step of the character walk, shared by the measure and draw passes
 * because the original shares it too: 0x004023C5 and 0x00402475 are the same
 * ladder with a different accumulator.  Returns the glyph index in *glyph and
 * the gap to use in *gap, or a negative code for the control characters. */
enum { CH_GLYPH = 0, CH_END = -1, CH_NEWLINE = -2, CH_DX = -3, CH_DY = -4,
       CH_BACK = -5, CH_SKIP = -6 };

static int next_char(const gfx2d_font *f, const uint8_t **pp,
                     unsigned *glyph, int *gap, int *amount)
{
    const uint8_t *p = *pp;
    unsigned c = *p++;

    *pp = p;
    if (c >= f->first) {
        /* HAZARD 3 (section 4) - the original's bound is INCLUSIVE (`JA` at
         * 0x004023CF skips only bytes strictly above `last`), so a byte of
         * exactly 0x80 indexes glyph 96 of a 96-entry bundle and guru-panics
         * 0x80A7DF32.  This clamps to `< last`, which is strictly safer and
         * identical on every string the game actually emits - ASCII contains
         * no 0x80. */
        if (c >= f->last) {
            return CH_SKIP;
        }
        *glyph = c - f->first;
        *gap   = f->gap;
        return CH_GLYPH;
    }
    if (c == 0x00) { return CH_END; }
    if (c == 0x0A) { return CH_NEWLINE; }
    if (c == 0x0E) { *amount = (int)(int8_t)*p; *pp = p + 1; return CH_DX; }
    if (c == 0x0F) { *amount = (int)(int8_t)*p; *pp = p + 1; return CH_DY; }
    if (c < 0x10)  { *amount = (int)c; return CH_BACK; }

    /* The two-byte escape, 0x00402432 / 0x00402504.  The index it computes is
     * `(last - first + 1) + (escape >> 1)`, which for JADE is 97 and up -
     * past the end of a 96-entry bundle, EVERY time (section 4).  Transcribed
     * as written; the bound check in glyph_advance and gfx2d_blit turns what
     * would be a guru panic into a skipped character. */
    *glyph = (unsigned)((((c - 0x10u) << 8) | *p) >> 1) +
             (unsigned)(f->last - f->first + 1);
    *pp    = p + 1;
    *gap   = f->esc_gap;
    return CH_GLYPH;
}

int gfx2d_text(gfx2d_surface *surf, gfx2d_font *f, const char *str,
               int x, int y)
{
    const uint8_t *base;
    const uint8_t *p;
    uint32_t nentries;
    kern_state kern;
    int cy;

    if (!surf || !f || !str || !f->bundle || f->bundle_len < 4) {
        return 0;
    }
    base     = (const uint8_t *)f->bundle;
    nentries = rd32(base) >> 2;
    p        = (const uint8_t *)str;
    cy       = y + f->y_origin;          /* 0x004023A8, added ONCE */

    for (;;) {                           /* one iteration per LINE */
        int cx = x;                      /* 0x004023AB `PUSH ECX` */
        const uint8_t *line = p;
        int ended = CH_END;

        if (f->flags & GFX2D_FONT_KERN) {
            kern_reset(&kern);
        }

        /* ---- pass 1, the measure, 0x004023C1 -------------------------- */
        if (f->align != GFX2D_ALIGN_LEFT) {
            int width = 0, gap = 0;
            const uint8_t *q = p;

            for (;;) {
                unsigned glyph = 0;
                int amount = 0;
                int kind = next_char(f, &q, &glyph, &gap, &amount);

                if (kind == CH_END || kind == CH_NEWLINE) {
                    ended = kind;
                    break;
                }
                if (kind == CH_SKIP)  { continue; }
                if (kind == CH_DX)    { width += amount; continue; }
                if (kind == CH_DY)    { continue; }
                if (kind == CH_BACK)  { width -= amount; continue; }

                if (f->flags & GFX2D_FONT_KERN) {
                    width -= kern_step(&kern, base, f->bundle_len,
                                       nentries, glyph);
                }
                width += glyph_advance(base, f->bundle_len, nentries, glyph);
                width += gap;

                /* 0x00402400: the width limit, and it is the ONE place
                 * draw_text sets carry (0x00402522 `STC`).  The comparison is
                 * UNSIGNED in the original, so a negative accumulator trips it
                 * rather than passing - transcribed, not tidied. */
                if ((f->flags & GFX2D_FONT_LIMIT) &&
                    (uint32_t)width > (uint32_t)f->field_w) {
                    return 1;
                }
            }
            width -= gap;                /* 0x0040244D `SUB EDX,EBP` */

            if (f->align == GFX2D_ALIGN_MEASURE) {
                /* 0x004024C2.  The original increments the record's +0x14 -
                 * its LUT POINTER - once per line, which a port holding that
                 * field as a real pointer cannot do; the count goes in its own
                 * field.  field_w is a MAX ACCUMULATOR and is never reset
                 * here, so a caller that wants one string's width must zero it
                 * first.  That is the original's behaviour, not a convenience. */
                f->measured++;
                if (f->field_w < width) {
                    f->field_w = width;
                }
                p = q;                   /* mode 4 does not rewind: it does
                                          * not draw, so it walks on */
                if (ended == CH_END) {
                    return 0;
                }
                cy += f->line_h;
                continue;
            }

            {
                int slack = f->field_w - width;

                if (f->align == GFX2D_ALIGN_RIGHT)  { cx += slack; }
                if (f->align == GFX2D_ALIGN_CENTRE) { cx += slack >> 1; }
            }

            if (f->flags & GFX2D_FONT_KERN) {
                kern_reset(&kern);       /* 0x0040246A, before the draw pass */
            }
        }

        /* ---- pass 2, the draw, 0x00402475 ----------------------------- */
        p = line;
        for (;;) {
            unsigned glyph = 0;
            int gap = 0, amount = 0;
            int kind = next_char(f, &p, &glyph, &gap, &amount);

            if (kind == CH_END)     { return 0; }
            if (kind == CH_NEWLINE) { break; }
            if (kind == CH_SKIP)    { continue; }
            if (kind == CH_DX)      { cx += amount; continue; }
            if (kind == CH_DY)      { cy += amount; continue; }
            if (kind == CH_BACK)    { cx -= amount; continue; }

            if (f->flags & GFX2D_FONT_KERN) {
                cx -= kern_step(&kern, base, f->bundle_len, nentries, glyph);
            }
            /* HAZARD 1 (section 8) - one path, expanded through the LUT.
             * HAZARD 4 (section 5) - f->lut may legitimately be ONE dword. */
            gfx2d_blit(surf, f->bundle, f->bundle_len, glyph, f->lut, cx, cy);
            cx += glyph_advance(base, f->bundle_len, nentries, glyph);
            cx += gap;
        }

        cy += f->line_h;                 /* 0x004024E8 */
    }
}

/* ---- 4. presenting ----------------------------------------------------- */

/* two textures, alternated frame by frame - the GPU may still be
 * reading last frame's, and a TexSubImage into a texture in flight makes a
 * tile-based driver copy it.  And where the context takes the canvas as it is
 * (plat_gfx_tex_bgra) there is no swizzle and no scratch copy. */
static plat_tex   g_tex[2];
static int        g_tex_cur;
static int        g_tex_w, g_tex_h;
static uint32_t  *g_scratch;
static size_t     g_scratch_n;

/* Not in plat.h: plat_gl.c's BGRA upload, plat_null.c's refusal. */
int      plat_gfx_tex_bgra(void);
plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px);
void     plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px);

void gfx2d_present_release(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (g_tex[i]) {
            plat_gfx_tex_destroy(g_tex[i]);
            g_tex[i] = 0;
        }
    }
    g_tex_w = g_tex_h = 0;
    free(g_scratch);
    g_scratch   = NULL;
    g_scratch_n = 0;
}

static void present(const gfx2d_surface *s, int screen_w, int screen_h,
                    int over);

void gfx2d_present(const gfx2d_surface *s, int screen_w, int screen_h)
{
    present(s, screen_w, screen_h, 0);
}

void gfx2d_present_over(const gfx2d_surface *s, int screen_w, int screen_h)
{
    present(s, screen_w, screen_h, 1);
}

static void present(const gfx2d_surface *s, int screen_w, int screen_h,
                    int over)
{
    const uint32_t *lv[1];
    const uint32_t *src;
    plat_tex *t;
    plat_vertex v[4];
    size_t i, n;
    int k, bgra;

    if (!s || !s->px || s->w <= 0 || s->h <= 0) {
        return;
    }
    n = (size_t)s->w * s->h;

    /* plat.h's texture order is R, G, B, A in memory; the canvas is the
     * ORIGINAL's order, B, G, R, A.  One swizzle, at upload, exactly as
     * port/stage/plate.c does at its line 463 - or none, where the context
     * takes B, G, R, A itself. */
    bgra = plat_gfx_tex_bgra();
    if (bgra) {
        src = s->px;
    } else {
        if (g_scratch_n != n) {
            free(g_scratch);
            g_scratch   = (uint32_t *)malloc(n * sizeof *g_scratch);
            g_scratch_n = g_scratch ? n : 0;
        }
        if (!g_scratch) {
            return;
        }
        for (i = 0; i < n; i++) {
            uint32_t c = s->px[i];

            g_scratch[i] = (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16) |
                           ((c & 0x000000ffu) << 16);
        }
        src = g_scratch;
    }

    lv[0] = src;
    if (g_tex[0] && (g_tex_w != s->w || g_tex_h != s->h)) {
        plat_gfx_tex_destroy(g_tex[0]);
        plat_gfx_tex_destroy(g_tex[1]);
        g_tex[0] = g_tex[1] = 0;
    }
    g_tex_cur ^= 1;
    t = &g_tex[g_tex_cur];
    if (!*t) {
        *t = bgra ? plat_gfx_tex_create_bgra((unsigned)s->w, (unsigned)s->h, src)
                  : plat_gfx_tex_create((unsigned)s->w, (unsigned)s->h, lv, 1);
        g_tex_w = s->w;
        g_tex_h = s->h;
    } else if (bgra) {
        plat_gfx_tex_update_bgra(*t, src);
    } else {
        plat_gfx_tex_update(*t, src);
    }

    /* One screen-filling quad.  The corner order is CLOCKWISE in the shim's
     * y-down screen space, because the backends cull back faces with
     * front = CW as of - reverse two corners and the whole 2D layer
     * vanishes with no error anywhere.  Same trap as gfx_render_probe.c's
     * test triangle. */
    for (k = 0; k < 4; k++) {
        v[k].sz       = 0.0f;
        v[k].rhw      = 1.0f;
        v[k].diffuse  = 0xffffffffu;
        v[k].specular = 0x00000000u;
    }
    v[0].sx = 0.0f;              v[0].sy = 0.0f;
    v[0].tu = 0.0f;              v[0].tv = 0.0f;
    v[1].sx = (float)screen_w;   v[1].sy = 0.0f;
    v[1].tu = 1.0f;              v[1].tv = 0.0f;
    v[2].sx = (float)screen_w;   v[2].sy = (float)screen_h;
    v[2].tu = 1.0f;              v[2].tv = 1.0f;
    v[3].sx = 0.0f;              v[3].sy = (float)screen_h;
    v[3].tu = 0.0f;              v[3].tv = 1.0f;

    plat_gfx_set_state(PLAT_RS_ZTEST,      0u);
    plat_gfx_set_state(PLAT_RS_ZWRITE,     0u);
    plat_gfx_set_state(PLAT_RS_BLEND,      over ? 1u : 0u);
    if (over) {
        plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_INVSRCALPHA);
    }
    plat_gfx_set_state(PLAT_RS_FOGENABLE,  0u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND,   2u);   /* modulate */
    plat_gfx_set_state(PLAT_RS_TEXADDRESS, 3u);   /* clamp */
    plat_gfx_set_state(PLAT_RS_TEXMAG,     1u);   /* nearest: a 2D layer is
                                                   * pixel art and a linear
                                                   * magnify smears the glyph
                                                   * the probe then measures */
    /* but SHRUNK (the 800x600 canvas at 640x480, the handheld's
     * panel) nearest drops whole rows and columns, and the small font loses
     * strokes - "TYRES" read "TYRCS".  Linear only when minifying: at 1:1
     * and above GL takes the magnify filter, so nothing else changes. */
    plat_gfx_set_state(PLAT_RS_TEXMIN,     2u);
    plat_gfx_tex_bind(*t);
    plat_gfx_draw(PLAT_PRIM_TRIANGLE_FAN, v, 4);
}
