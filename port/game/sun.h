/* sun.h - the level's sun: where it is, whether the world hides it, and the
 * lens flare and glare it draws.  Read from retail's
 * bytes; addresses are
 * retail's.
 *
 * WHAT THE ORIGINAL DOES (the port's reading, each step measured against
 * run 32's LEVEL24):
 * 0x0045343E, after the panorama: when LOCALDAT +0x65C + +0x6AC is not
 *    zero, the level's sun direction (+0x684 + +0x680 a byte azimuth, +0x688
 *    the elevation, both 2*pi/256) is put on the dome's shell (radius 1.5),
 *    taken through the dome's matrix, and, if it is in front of the 4096
 *    near plane, projected: [0x005F4490/94] the screen point, [0x005F4488/8C]
 *    its integers, [0x005F4484] "visible" if the integers are on the screen,
 *    [0x005F449C] = max(0, 256 - distance_from_centre / 2).
 * 0x0044E0F3, at the head of every polygon batch of the world (0x00449AC7)
 *    after that: clears [0x005F4484] when the screen point lies inside the
 *    batch's polygon (every edge's cross product negative).  Trees, hills,
 *    cars: anything the world draws over the sun hides it.
 * 0x00453426, the frame's end: the flag goes to car+0xFB8.  Nothing in
 *    the executable's code reads +0xFB8 or writes +0xFBC; measured from run
 *    32's statehook2.bin, +0xFBC moves by exactly 4 a 100 Hz tick - up while
 *    +0xFB8 is 1, down while it is 0, held to 0..256.  sun_tick is that.
 *    ([0x005F4498] = car+0xFBC, read at 0x0045340C.)
 * 0x004B4ACE, the engine's additive pass after the spray: 17 sprites of
 *    TEXTINFO records 0x40..0x49 along the line from the sun through the
 *    screen centre (sun_flare_draw), alpha = level * (255 - dist/2) >> 8,
 *    all of it only when [0x005D675C] (the LENS FLARE option) is on.
 * 0x00453905, after the cockpit: a screen-sized quad of the level's
 *    +0x18BC.. colour, alpha ((closeness * level >> 4) * LOCALDAT +0x6AC >> 8)
 *    from the PREVIOUS frame's values (0x0044DBFC computes it before the
 *    sky does).  The option does not gate it.
 *
 * THE PORT'S: the polygon test is a tap on the platform's draws
 * (plat_gfx_set_tap) taking the triangles the world submits, where the
 * original tests its clipped polygons; the ramp is stepped from the sim's
 * tick count, where the original's tick writes it. */
#ifndef RALLY_SUN_H
#define RALLY_SUN_H

#include <stdint.h>

#include "localdat.h"
#include "objxform.h"
#include "plat.h"
#include "stage.h"

typedef struct sun_s {
    int      have;         /* the level has a sun: +0x65C + +0x6AC != 0     */
    int      option;       /* [0x005D675C], the LENS FLARE option           */
    int      yaw, az, el;  /* LOCALDAT +0x680, +0x684, +0x688               */
    int      glare;        /* LOCALDAT +0x6AC                               */
    uint32_t glare_rgb;    /* +0x18BC / +0x18C0 / +0x18C4 as 0xRRGGBB       */

    /* this frame's computation */
    int      front;        /* in front of the near plane                    */
    int      vis;          /* [0x005F4484], after the polygon test          */
    float    sx, sy;       /* [0x005F4490 / 94]                             */
    int      ix, iy;       /* [0x005F4488 / 8C]                             */
    int      close;        /* [0x005F449C]                                  */
    int      occluded;     /* polygons that covered the point               */

    /* what the ticks and the frame carry */
    int      level;        /* car+0xFBC, 0..256                             */
    int      flag;         /* car+0xFB8                                     */
    int      glare_alpha;  /* the frame's [0x005E94B8], from `close` and `level`
                            * as the last frame left them                  */
    unsigned long last_tick;
    int      have_tick;
    int      wid, hei;     /* [0x004EF920 / 24]                             */
    const stage_view *sv;  /* for the screen centre                         */
} sun;

/* The level's fields.  `ld` may be absent (no sun). */
void sun_init(sun *s, const ld_file *ld, int have_ld, int option);

/* 0x00453515..0x004536A8, from the dome's matrix as the frame's sky used
 * (objscene_backdrop_matrix).  Sets the tap when there is a sun to hide. */
void sun_frame_begin(sun *s);
void sun_sky(sun *s, const ral_mat4 *dome, const stage_view *v,
             int width, int height);

/* The world is drawn: stop testing, keep the flag.  0x00453426. */
void sun_world_end(sun *s);

/* The ticks since the last call (the sim's counter, 100 Hz). */
void sun_tick(sun *s, unsigned long tick);

/* 0x004B4ACE's draw: `atlas` the spray's texture, `aspect` the spray's
 * 256 x 9 table (spray.aspect), `near_bits` / `zscale` as the fireworks'
 * quad takes them. */
void sun_flare_draw(const sun *s, uint32_t atlas, const float *aspect,
                    uint32_t near_bits, float zscale);

/* 0x00453905. */
void sun_glare_draw(const sun *s, int width, int height);

#endif
