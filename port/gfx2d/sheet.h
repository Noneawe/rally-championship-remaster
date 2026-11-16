/* sheet.h - THE SHEET CUT: a picture of sprites becomes a sprite bundle.
 *
 * FUN_00411D4C, with FUN_00411CB3 / FUN_00411CDB around
 * it - the "FNT sheet cut" that, and each left unread.  The
 * frontend draws everything that is not a background or a 3D model from two
 * kinds of bundle this builds at load:
 *
 *   0x00478799   files\g\graphics\sprites\mbuts00.pcx, then mbuts01.pcx,
 *                appended into ONE bundle, [0x00707EED]: the buttons (160
 *                sprites from mbuts00) and the frame (sprite 0xA0)
 *   0x004150C9   files\d\fnt\fnt13/21/23/27_00.pcx, one bundle each, the
 *                glyphs of the frontend's font records (224 each)
 *
 * IT IS NOT A GRID.  It is a box finder, and the reading is in
 * with every instruction; in short:
 *
 *   BACKGROUND  the (0,0) pixel if the four corners agree, else 0
 *   KEY         0xFF00FF (three planes), or the first palette index whose
 *               colour is 0xFF00FF (one plane), else the background
 *   BOX         raster scan; a non-background pixel starts a box that grows
 *               until a full pass over its four edges moves nothing (two of
 *               the four edge walks skip their first pixel - transcribed);
 *               the box is encoded, then painted background
 *   ENCODE      rows wholly KEY-or-BACKGROUND trimmed top then bottom; the
 *               least leading and least trailing run trimmed left and right;
 *               {count, skip, pixels} packets per row, count 0 ends it
 *
 * The bundle is the format gfx2d_blit reads: a dword offset
 * table, then each entry.  A one-plane sheet gives COMPACT entries (byte
 * header, byte pixels = palette indices, drawn through a LUT).  A three-plane
 * sheet gives EXTENDED entries: the game's display was 16-bit, so its unit
 * was 2 ([0x00513144] = 2 when every channel mask fits 16 bits,
 * 0x00411DEC); the port's canvas is 32-bit and takes the OTHER arm of that
 * same branch, unit 4, tag 0xFFFFFFFC (0x00412468: `NEG` of the unit).  The
 * pixels are 0xFFRRGGBB - the original converts through the display format
 * (0x004059D1), which at 16 bits is a truncation the port's 5-6-5 compare
 * applies anyway.
 *
 * tools/menuart.py `cut` is the independent second reading; `check` there and
 * port/game/frontend.c's loaders assert the counts the CODE needs (160 then
 * the frame, 224 glyphs) - counts that are BLIND to three wrong readings
 * the screenshots are what see the order.
 * Nothing here allocates except through the builder.
*/
#ifndef RAL_GFX2D_SHEET_H
#define RAL_GFX2D_SHEET_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  *data;              /* the entries, back to back               */
    size_t    len, cap;
    size_t   *off;               /* each entry's offset into `data`         */
    unsigned  n, ncap;
} sheet_builder;

void sheet_builder_init(sheet_builder *b);
void sheet_builder_free(sheet_builder *b);

/* Cut one decoded sheet, APPENDING its sprites (0x00411D4C's outer loop
 * appends every numbered file to one bundle).  `px` is w*h values: palette
 * indices for a one-plane sheet (`pal` = its palette, 0x00RRGGBB), or
 * 0x00RRGGBB for a three-plane one (`pal` NULL).  `px` is not modified.
 * Returns the number of sprites appended, or -1 (out of memory). */
int sheet_cut(sheet_builder *b, const uint32_t *px, int w, int h,
              const uint32_t *pal);

/* The finished bundle: a dword offset table, then the entries.  Malloc'd;
 * the caller frees.  NULL on out of memory. */
uint8_t *sheet_bundle(const sheet_builder *b, size_t *len);

#endif
