/* gfx2d.h - the game's 2D layer: the sprite blitter, the text renderer and the
 * one surface they both write to.
 *
 *
 *
 * Transcribed from sprite_blit (0x004066F9), draw_text (0x0040239C) and
 * draw_list (0x004025EF).  The reading is
 * the format the
 * blitter consumes is.
 *
 * WHY THIS IS A CPU SURFACE AND NOT A QUAD PER SPRITE
 * ---------------------------------------------------
 * The obvious GLES2 shape for a blitted sprite is a textured quad, and the
 * ticket that opened this work proposed exactly that.  The original is not
 * that shape and the difference is not cosmetic:
 *
 *   * sprite_blit writes PIXELS into a framebuffer, one RLE packet at a time,
 *     and CLIPS BY WALKING the run stream.  Its output is defined to the
 *     pixel, which is what makes it checkable against tools/bun.py.
 *   * the original has TWO 2D surfaces, not one.  [0x004EF994] is the
 *     framebuffer sprite_blit writes to.  [0x004EF9B0] is a separate 800x600
 *     32-bit canvas that the PCX decoder (FUN_00411704) writes to and that
 *     0x00405E2A then copies WHOLE into the framebuffer.  The PCX pixels
 *     never pass through sprite_blit. read that path; the finding is
 *
 *
 * So the faithful port is one 32-bit CPU canvas, rasterised exactly as the
 * original rasterises, uploaded ONCE per frame as one texture and drawn as one
 * screen-filling quad.  That is one upload and one draw call for the whole 2D
 * layer, which is cheaper than a quad per glyph would have been anyway, and it
 * is the only design in which "the drawn glyph's lit pixels" is a statement
 * about the original's output rather than about GL's sampler.
 *
 * PIXEL ORDER is 0xAARRGGBB - B, G, R, A in memory - which is the order the
 * original's own canvas is in and the order
 * tools/formats/pcx.c already produces.  It is NOT plat.h's texture order;
 * gfx2d_present swizzles once, at upload, exactly as port/stage/plate.c does.
 *
 * WHAT IS DELIBERATELY ABSENT
 *   The 8bpp blit path.  It exists so the original can REP MOVSB into a
 *   paletted framebuffer; on GLES2 there is one path and it expands through
 *   the LUT.
*/
#ifndef RAL_GFX2D_H
#define RAL_GFX2D_H

#include <stddef.h>
#include <stdint.h>

/* ---- 1. the surface ---------------------------------------------------- */

/* The clip rectangle is the original's four globals, and it is INCLUSIVE at
 * both ends because the original's is: 0x004F0010/0x14 are the x range and
 * 0x004F0018/0x1C the y range, and the right/bottom tests at 0x0040675E and
 * 0x0040679E are `x + w - 1 - limit`, i.e. the limit is the last usable
 * column.  A port that reads them as exclusive loses one column and one row
 * and nothing ever says so. */
typedef struct {
    uint32_t *px;               /* w * h, 0xAARRGGBB */
    int       w, h;
    int       clip_x0, clip_x1; /* inclusive; 0x004F0010 / 0x004F0014 */
    int       clip_y0, clip_y1; /* inclusive; 0x004F0018 / 0x004F001C */
} gfx2d_surface;

/* Points the surface at caller-owned storage and sets the clip to the whole
 * of it.  Nothing here allocates. */
void gfx2d_surface_init(gfx2d_surface *s, uint32_t *px, int w, int h);
void gfx2d_set_clip(gfx2d_surface *s, int x0, int y0, int x1, int y1);
void gfx2d_clear(gfx2d_surface *s, uint32_t argb);   /* was 0x00405E18 */

/* ---- 2. the blitter ---------------------------------------------------- */

/* Return codes.  The original's out-of-range cases are guru panics
 * (0x004D4433 / 0x004D444C / 0x004D4465) rather than return values; a port
 * that panicked would be faithful and useless, so they become errors and the
 * call sites keep the panic's address in a comment. */
enum {
    GFX2D_OK = 0,
    GFX2D_ERR_NO_SURFACE,   /* guru 0x004D4433 - framebuffer base unset */
    GFX2D_ERR_NO_BUNDLE,    /* guru 0x004D444C - bundle pointer null */
    GFX2D_ERR_INDEX,        /* guru 0x004D4465 - sprite index >= entry count */
    GFX2D_ERR_TRUNCATED,    /* the run stream ran off the end of the buffer */
    GFX2D_ERR_UNSUPPORTED,  /* the 0xFFFFFFFE extended form - 16bpp payload in
                             * a 32-bit surface; see gfx2d.c and */
    GFX2D_ERR_CLIPPED       /* not an error: nothing was visible */
};

/* sprite_blit, 0x004066F9.  `bundle` is the raw .bun image - this walks the
 * offset table and the run stream itself rather than going through bun.c,
 * because port/tests/sprite_render_probe.c compares the result against
 * tools/bun.py and a comparison between two calls into the same decoder is not
 * a comparison.
 *
 * `lut` is indexed by the payload byte and is passed through verbatim.  A
 * MONOCHROME font legitimately supplies a ONE-ENTRY lut: all 2,018 literal
 * pixels in JADE.BUN are 0x00, and [0x004F1694] is one dword sitting
 * immediately before a string. */
int gfx2d_blit(gfx2d_surface *s, const void *bundle, size_t len,
               unsigned index, const uint32_t *lut, int x, int y);

/* The container and one entry's header, read the way sprite_blit reads them
 * (0x00406713 and 0x00406723).  Exposed so a gate can name a sprite's own
 * bounding box WITHOUT going through bun.c, which is the whole point of the
 * comparison tools/spriteagree.sh makes.  gfx2d_entry_count returns -1 on a
 * buffer too short to hold a table. */
typedef struct {
    int      xoff, yoff, w, h;
    int      extended;          /* the 0xFFFFFFFC.. tag range */
    uint32_t tag;
    unsigned unit;              /* payload bytes per pixel */
} gfx2d_entry;

int gfx2d_entry_count(const void *bundle, size_t len);
int gfx2d_entry_info(const void *bundle, size_t len, unsigned index,
                     gfx2d_entry *e);

/* draw_list, 0x004025EF.  Records are 16 bytes {x, y, index, lut}; `ox`/`oy`
 * are the origin the original passes in ECX/EDX and adds to every record
 * (0x004025FF / 0x00402603), which did not
 * record. */
typedef struct {
    int32_t         x, y;
    uint32_t        index;
    const uint32_t *lut;
} gfx2d_sprite_rec;

void gfx2d_list(gfx2d_surface *s, const void *bundle, size_t len,
                const gfx2d_sprite_rec *rec, unsigned n, int ox, int oy);

/* ---- 3. the font record ------------------------------------------------ */

/* 32 bytes at 0x00517958, stride 0x20, four of them differing only in +0x02.
 * Held here as a struct with the
 * original's offsets in the comments rather than as a byte image, because the
 * bundle and lut fields are pointers and are 8 bytes on the target. */
enum {                       /* +0x02, the alignment mode */
    GFX2D_ALIGN_LEFT    = 0, /* also 1 */
    GFX2D_ALIGN_RIGHT   = 2,
    GFX2D_ALIGN_CENTRE  = 3,
    GFX2D_ALIGN_MEASURE = 4  /* draws nothing; returns the width in `field_w` */
};

enum {                          /* +0x03, the flags */
    GFX2D_FONT_KERN  = 0x04,    /* the 0x0040253E per-glyph kerning hook */
    GFX2D_FONT_LIMIT = 0x02     /* stop and set carry past `field_w` */
};

typedef struct {
    uint8_t         first;      /* +0x00  0x20 in JADE */
    uint8_t         last;       /* +0x01  0x80 in JADE - see section 4 */
    uint8_t         align;      /* +0x02 */
    uint8_t         flags;      /* +0x03 */
    int32_t         gap;        /* +0x04  inter-character advance */
    int32_t         line_h;     /* +0x08  added on '\n' */
    int32_t         y_origin;   /* +0x0C  added once at entry */
    int32_t         esc_gap;    /* +0x10  the gap used after an escape */
    const uint32_t *lut;        /* +0x14 */
    const void     *bundle;     /* +0x18 */
    size_t          bundle_len; /* not in the original: it trusts the pointer */
    int32_t         field_w;    /* +0x1C  the alignment box, and mode 4's out */
    int32_t         measured;   /* mode 4's line counter - the original
                                 * increments +0x14, the LUT POINTER, which a
                                 * port cannot do.  Section 3 of the doc. */
} gfx2d_font;

/* draw_text, 0x0040239C.  Draws `str` at (x, y) and returns 0, or 1 when the
 * original would return with CARRY SET - which it does at exactly one place,
 * 0x00402522, when GFX2D_FONT_LIMIT is set and the measured line passes
 * `field_w`.: this is one more function
 * whose status is in the carry flag and whose decompiled C shows no sign of
 * it. */
int gfx2d_text(gfx2d_surface *surf, gfx2d_font *f, const char *str,
               int x, int y);

/* ---- 4. presenting ----------------------------------------------------- */

/* Uploads the surface as one RGBA8 texture and draws it as one screen-filling
 * quad through plat.h.  `screen_w`/`screen_h` are the destination in the
 * shim's top-down screen space.  Call between plat_video_frame_begin and
 * plat_video_present. */
void gfx2d_present(const gfx2d_surface *s, int screen_w, int screen_h);
void gfx2d_present_release(void);   /* drops the texture the above created */

/* the same quad, ALPHA-BLENDED (SRCALPHA / INVSRCALPHA) over what is
 * already drawn - a canvas cleared to 0x00000000 shows the scene through it.
 * THE PORT'S: the original writes its 2D layer into the framebuffer after
 * the scene, which this reproduces only where a pixel is opaque.  Leaves the
 * states it sets set; a caller in the middle of a frame saves and restores
 * them (rally.c). */
void gfx2d_present_over(const gfx2d_surface *s, int screen_w, int screen_h);

#endif /* RAL_GFX2D_H */
