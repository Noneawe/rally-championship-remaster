/* hud.h - the in-race HUD, ported.
 *
 * The reading is; every
 * routine here was transcribed again from retail's bytes this session
 * (tools/ralimg.py disasm) and says where the bytes
 * said more than the document.  It replaces hud_standin.c, the text
 * overlay, everywhere but the message banner's GLYPHS.
 *
 * THE SHAPE IS THE ORIGINAL'S
 *   hud_frame   FUN_004521B6's HUD part, once per rendered frame: the element
 *               mask car+0x220 into the block, the message pickup
 *               (FUN_00459163) and the block fill (FUN_004588BC) - the
 *               tacho, the speed, the gear, the clock, the route distance,
 *               the countdown's size (FUN_00458D93) and the section colours
 *               (FUN_00458EF5).  It WRITES the sim's message slot (topre ->
 *               0, +0x728 = 1), as the original's frame does, so it runs
 *               whether or not anything is drawn.
 *   hud_draw    FUN_00458619: depth off, FUN_004C91D0's layout 0 - textured
 *               quads cut from the 97-record sprite table at 0x008DD370, at
 *               800x600 positions scaled to the display - then the banner
 *               (FUN_00459539).
 * The two STEP counters are the sim's: msgtimer (FUN_0043F3BA) and the
 * countdown plate olaydraw (FUN_0043F355), port/sim/race.c.
 *
 * THE BANNER is the original's too:
 *   hud_banner_load  loadgamefonts_q (0x0041A4AC): FNT22_00.PCX and
 *               FNT22_01.PCX MERGED by the cut's flag 0x20 (0x0041210D /
 *               0x004121B0) - _00's low nibble becomes the high nibble of
 *               _01's index - cut to 224 glyphs (sheet.h), drawn through
 *               0x004121EF's LUT: colour = palette[i & 0x0F], alpha = the
 *               high nibble doubled.  Record 0x00517E2C, read from the image.
 *   the pickup  FUN_0041A608 -> FUN_0045941D: the lexicon's text of the id,
 *               drawn three times into the 0xC4 surface (128 x 128): field
 *               360, centred, at (4, 0), (-116, 25), (-236, 50) - three
 *               120-px slices of one 360-px line.
 *   banner      FUN_00459539: the three slices side by side.
 *
 * WHAT IS NOT THE ORIGINAL'S, each named where it is made in hud.c:
 *  THE 0xC4 SURFACE: 128 x 128, READ (section
 *     14.7: size class 1 of 0x00564F5C's table is 128, 0x00423D31 /
 *     0x004211B6).  Its texture format is [0x00564F90], the third role
 *     0x0041933F picks - 4444 on a 16-bit display (the snapshots'), 8888 on
 *     a 32-bit one.  THE PORT'S: 32-bit pixels, always.
 *  THE CORNER LABELS' NAMES are drawn (item 5): 8 characters of the
 *     lexicon's name (gmode 1) or " CAR nn " (gmode 2), in bands 3 / 4.
 *  ~~THE MAP DISC~~ - drawn since 2026-10-19 (section 9.1).  The port's: the window is filled on the first frame
 *     whatever the car's place (the original's starts from +0xAD4 = 0).
 *  ~~THE PLAYER'S CAR PICTURE~~ - drawn since 2026-10-20: FUN_00424297 cuts it from tx1822 / tx1824.
 *     The port's: the mip chain is rebuilt after the cut (the original
 *     writes the top surface only).
 *  THE HALF-PIXEL.  Direct3D's pre-transformed vertex samples at the
 *     pixel's corner, GL at its centre; every vertex goes out +0.5 px so the
 *     port's pixels are Direct3D's (measured the
 *     original's at (+0.5, +0.5)).
 *  THE HUD'S RENDER STATE: READ.  The frame function
 *     0x00458619 sets depth off (0x0044C48C: ZENABLE 0, ZWRITE 0, ZFUNC 4) and
 *     then 0x004586C9: TEXTUREMAPBLEND 4 (MODULATEALPHA), ALPHABLENDENABLE 1,
 *     SRCBLEND 5 / DESTBLEND 6 (SRCALPHA / INVSRCALPHA), FOGENABLE 0 - the
 *     port's reading, confirmed.  NOT set there, so inherited from the 3D
 *     pass and still the port's choice: the alpha test, ref and func, and the
 *     texture filters.  (TEXTUREADDRESS 3, clamp, is set at 0x00458691,
 *     after the layout, for the banner.)
 *  "HOME: RACE AGAIN" when the race is over - the original goes to its
 *     results screen.
 * Inputs the port does not model, so their elements never draw, exactly as
 * the original's guards leave them: race position (-1 in gmode 0), the gaps
 * and thumbnails of the cars behind and ahead (-1, messagesystem_init), the
 * pacenote arrows (car+0xF98 = 0), bit 0x08's box (off in 0xF7). */
#ifndef RALLY_HUD_H
#define RALLY_HUD_H

#include "gfx2d.h"
#include "physics.h"
#include "plat.h"
#include "image.h"
#include "lexicon.h"

#include <stddef.h>
#include <stdint.h>

#define HUD_RECTS     0x61          /* 0x008DD370, indices 0x00..0x60      */
#define HUD_SECTIONS  0x100         /* 0x00684260: 64 dwords before 0x684360 */

/* FUN_004C91D0's HUD block, 0x00684152 - player 1's fields, by offset. */
typedef struct hud_block {
    int32_t  tacho;       /* +0x10  round(car+0x718)                         */
    int32_t  speed;       /* +0x14  car+0xAB0                                */
    int32_t  gear;        /* +0x18  car+0x1D4                                */
    int32_t  position;    /* +0x1C  -1 = not shown                           */
    int32_t  clock_ms;    /* +0x20  catitken x 10                            */
    int32_t  gap_behind;  /* +0x24                                           */
    int32_t  make_behind; /* +0x28  -1 = none                                */
    int32_t  gap_ahead;   /* +0x2C                                           */
    int32_t  make_ahead;  /* +0x30  -1 = none                                */
    int32_t  name_behind; /* +0x00  car+0xA24, a lexicon id (FUN_004588E5)   */
    int32_t  name_ahead;  /* +0x04  car+0xA20                                */
    float    total;       /* +0x68  the route's length, initoverlay_q        */
    float    done;        /* +0x6C  FUN_004588E5                             */
    int32_t  sections;    /* +0x74  numcheckposits, 1 if none                */
    int32_t  olaydraw;    /* +0x84  the countdown plate, -1 = none           */
    int32_t  siz, alp, top; /* +0x88 / +0x8C / +0x90  FUN_00458D93           */
    int32_t  pace[4];     /* +0xA4..+0xB0  pacenote ids + 1, 0 = none        */
    int32_t  pace_x;      /* +0xB4                                           */
    int32_t  dial2;       /* +0xCC  the 7k dial (FUN_00444EBA)               */
    int32_t  flags;       /* +0xF8  car+0x220, the element mask              */
    int32_t  panel;       /* [0x00684256] = flags & 0x40                     */
    float    len[HUD_SECTIONS];   /* 0x00684260                             */
    int32_t  state[HUD_SECTIONS]; /* 0x00684360: 12 / 8 / 4, byte offsets
                                   * into the colours at 0x008DD9A0          */
} hud_block;

typedef struct hud {
    int       have;               /* the plates loaded                       */
    int32_t   rect[HUD_RECTS][4]; /* {u, v, w, h}, from the image            */
    plat_tex  tex_c0, tex_c1, tex_c2;  /* tx1800, tx1802, tx1798             */
    plat_tex  tex_go[6];          /* 0xDC..0xE1: tx1804..tx1814, 5 4 3 2 1 GO */
    hud_block b;
    /* the route's cumulative distance, 0x00997CD8 (FUN_00443C7D) */
    uint32_t *dist;
    int32_t   ndist;
    const float *sin_table;       /* 0x009C3EE4, the sim's copy (the needle) */
    int       video;
    /* the banner (hud_banner_load): FNT22, record 0x00517E2C */
    int       have_banner;
    uint8_t  *fnt22;              /* [0x00517E44], the merged sheet's bundle */
    size_t    fnt22_len;
    uint32_t  fnt22_lut[256];     /* [0x00517E00], 0x004121EF's palette      */
    gfx2d_font bfont;
    /* the corner labels, FUN_00459492 - record 0x00517E4C (left
     * aligned, the banner's bundle) through the BRIGHTENED LUT [0x00517E60]
     * (0x0041A586), the names of the cars behind / ahead in bands 3 / 4 of
     * the same 0xC4 surface.  label[] are the two 8-byte strings
     * [0x0068458C] / [0x00684598] (+ a NUL: the byte after each is 0);
     * label_shown[] the copies at [0x006845A4..] FUN_00459163 compares. */
    gfx2d_font lfont;
    uint32_t  fnt22_lut_lit[256];    /* [0x00517E60]                         */
    char      label[2][9];
    char      label_shown[2][9];
    int       label_valid;
    int       diag_names;         /* RALLY_GAP_NAMES, a DIAGNOSTIC           */
    int32_t   diag_id[2];
    uint32_t  banner_px[128 * 128];  /* the 0xC4 surface                     */
    gfx2d_surface sb;
    plat_tex  tex_banner;
    int32_t   shown_id;           /* +0x724 as the surface last drew it      */
    /* the port's race-over prompt: JADE, a STAND-IN (see the header) */
    int       have_font;
    void     *jade;
    size_t    jade_len;
    gfx2d_font font;
    uint32_t  ink[1], shadow[1];
    uint32_t  canvas2[256 * 16];
    gfx2d_surface s2;
    plat_tex  tex_prompt;
    long      pickups;            /* FUN_00459163's renders, for a log       */
    /* the map disc. */
    const sim_route       *route; /* hud_init's, for the offset line       */
    const sim_ground_geom *geom;
    uint8_t  *map;                /* files\maps\MAPnn.SQ, whole            */
    int32_t   map_tiles;          /* header +0x0C                          */
    int32_t  *line_x, *line_z;    /* 0x0099CD40 / 0x009A1DA8, route.n each */
    int32_t   win_x, win_y;       /* car +0xAD4 / +0xAD8, map px, 16s      */
    int32_t   win_valid;          /* the port's: no window filled yet      */
    int32_t   car_wx, car_wy;     /* car +0xAE8 / +0xAEC                   */
    float     map_x, map_y, map_head;   /* the block's +0x34 / +0x38 / +0x3C */
    int       have_map;
    plat_tex  tex_map;            /* texture 0xC6, 128x128                 */
    uint32_t  map_px[128 * 128];  /* the window, B,G,R,A                   */
    /* the game's text (lexicon.h), or NULL: the banner is then blank.  The
     * caller owns it. */
    const lexicon *lex;
} hud;

/* Loads the HUD's plates (files\b\tx1798..tx1815, the odd one of each pair
 * as the inverted alpha, 3-plane: PLATE_ALPHA_FORCED) and the sprite table
 * out of the image; `jade_path` is the race-over prompt's stand-in font (may
 * fail: the prompt then draws nothing).  `route` / `geom` / `race` may be NULL (no
 * route: no bar).  Needs a GL context for the plates; without one (`video`
 * 0) only the frame logic runs.  0, or -1 with a message in `err`. */
int  hud_init(hud *h, const char *files_root, const ral_image *im,
              const char *jade_path, const car_params *p,
              const float *sin_table, const sim_route *route,
              const sim_ground_geom *geom, int32_t carnum, int video,
              char *err, size_t errlen);

/* loadgamefonts_q's banner half (0x0041A4AC): FNT22_00.PCX + FNT22_01.PCX
 * (`p00`, `p01`) merged, cut and given 0x004121EF's LUT; the record is
 * 0x00517E2C out of the image.  0, or -1 with a message in `err` - the
 * banner then draws nothing.  After hud_init. */
int  hud_banner_load(hud *h, const ral_image *im, const char *p00,
                     const char *p01, char *err, size_t errlen);

/* initoverlay_q (0x00458764): the section lengths from the route, at each
 * race start. */
void hud_race_start(hud *h, const sim_race *race);

/* the map disc's map, files\maps\MAPnn.SQ for stage `level`
 * (FUN_00437BCC writes the level's two digits into `mapxx`), and the route's
 * offset line (FUN_00444140 / FUN_004441C2).  After hud_init.  0, or -1 with
 * a message in `err`: the disc then draws nothing. */
int  hud_map_load(hud *h, const char *files_root, int level,
                  char *err, size_t errlen);

/* Once per rendered frame, before hud_frame: FUN_004592B8 (the window kept
 * round the car, refilled by FUN_004593F4 -> FUN_0041AC65 when the car
 * leaves its middle) and FUN_00458BA8's three fields.  `heading` is the
 * car's +0x9C0. */
void hud_map_frame(hud *h, const car_state *car, int32_t heading);

/* Once per rendered frame, before hud_draw.  `rc` may be NULL. */
void hud_frame(hud *h, const car_state *car, sim_race *rc, uint32_t slot);

/* Draws over the scene.  `race_over`: FUN_004436C1 stopped the batch (the
 * port's "HOME: RACE AGAIN" prompt).  Sets the render state it needs; the
 * caller saves and restores around it (rally.c). */
void hud_draw(hud *h, const sim_race *rc, uint32_t slot, int race_over,
              int screen_w, int screen_h);

void hud_free(hud *h);

/* a shadowed line of the prompt's font on a surface, and its width in
 * pixels (0 when the font did not load) - the replay viewer's strip. */
void hud_text(hud *h, gfx2d_surface *s, const char *str, int x, int y);
int  hud_text_width(hud *h, gfx2d_surface *s, const char *str);

#endif
