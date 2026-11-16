/* wipers.h - the wipers' motion in DRIVER and INTERIOR,
 * 2026-10-20; is the reading.
 *
 * The original has no wiper draw call: every engine update 0x004ADB8F
 * re-poses the blade vertices of the car's own model (type 0x50, the one
 * entry of the snapshot's object list, 0x00454D0C) and the ordinary car
 * draw shows them.  This module does the same to the port's copy of that
 * model (objscene's type 0x50 points), in 16.16 as the original's global
 * vertex array holds them.
 *
 *   load   0x004ACC36 -> 0x004ACDBE: per blade, the vertices of every polygon
 *          whose material byte (+0x07) is [0x0083D094 + car*4] (30) or
 *          [0x0083D0F0 + car*4] (29; 16 for car 21), at most 64 each, in
 *          polygon order; the pivot = the lowest-y one (the LAST of equals,
 *          the loop runs downward); the rest = round(Rx(-tilt) (v - pivot)),
 *          tilt = [0x007BA1C7 + car*4] * [0x0085CCB1] (0x004B0D0F), the
 *          table's entry as 0x004B0B5A leaves it: [0x0085CC9A] - t, 90 - t
 *          degrees (read raw until then).
 *   update 0x004ADB8F, once per engine update: the step from the phase,
 *          the pose, then the intermittency (two LCGs, 0x004ADBB6) and the
 *          phase's advance, acc = round(dt * 11553 + acc), phase =
 *          (acc >> 16) & 0xFF - one sweep 1,452 ms - parked in phase 0..7
 *          unless block +0x274C is 0 ((rain || snow) && inside).
 *   pose   0x004ACD3F -> 0x004AE545 / 0x004AD20A: the step's angle eased
 *          h - h cos(pi k / 128), h = sweep / 2, sweep = [0x0083D1A8 /
 *          0x0083D14C + car*4] degrees, k the step (or step + 1 on the
 *          return stroke); v = round(Rx(tilt) Rz(angle) rest) + pivot.
 *
 * `car` is FUN_00454CEC(shauncarnum1): 6 -> 11 (block +0x26A8,
 * [0x007115A2]). is kept: the pose writes record 0 for every entry of
 * the object list, which has one entry.
 *
 * THE PORT'S, not the original's: the arithmetic is `ext` (ext.h): long double,
 * the x87's 80 bits, on x86 (the gates' host); `double` elsewhere (quad
 * is software on aarch64), where a vertex may differ by one 16.16 unit.  Every constant is read from the image.
 *
 * THE WIPE-OFF'S LOAD, wipers_masks_build, the
 * rest of 0x004ADAB7 after the blade build - reference/cockpit-moving-
 * is the reading:
 *   0x004AD93A  per blade, the end points: the farthest pair of rest
 *               vertices first added by the same polygon (the LAST found of
 *               equals wins);
 *   0x004AE742  129 poses of the two end points (no pivot, +0xB2A7 on y),
 *               the inner one first (x, y swapped), projected into the
 *               windscreen's 256 x 128 grid through the per-car table
 *               0x007B9D77 (converted by 0x004B0B5A) and quantised to 1/256;
 *   0x004AE086  per step two triangles, scan-converted under CHOP
 *               (0x004ADD6A; the arc it computes first is dead);
 *   0x004AE2DF  the masks 0x007DBD12 / 0x007E3D12: 0xFF, then each step's
 *               spans set to the step - the highest wins;
 *   0x004AE36F  the flow field: read only by an uncalled debug blit, but it
 *               draws 2-4 times a cell from the shared generator [0x006CF334]
 *              only the draws are ported.
 * The x87 work is `ext` (ext.h: long double on x86, double elsewhere)
 * under the original's rounding modes (fesetround), FSIN / FCOS the x87's own
 * on x86 (x87.h): on x86 it agrees with the original's code run in x86vm.c
 * byte for byte (gate wiper-vm); on aarch64 a mask cell or the draw count
 * may differ (look at it on the device). */
#ifndef RALLY_WIPERS_H
#define RALLY_WIPERS_H

#include "image.h"
#include "ro3.h"

#include <stdint.h>

#define WIPER_VERTS 64

typedef struct {
    int      n;                          /* rec +0x38 / +0x3C               */
    int32_t  id[WIPER_VERTS];            /* model vertex indices            */
    int32_t  group[WIPER_VERTS];         /* the polygon that added it:
                                          * [0x0082D144] / [0x0082D244]     */
    int32_t  rest[WIPER_VERTS][3];       /* rec +0x440 / +0x740             */
    int32_t  pivot[3];                   /* rec +0xA40 / +0xA4C             */
    float    sweep;                      /* [0x0082CF14] / [0x0082CF18]     */
    float    lo;                         /* [0x0083D22C] / [0x0083D234]     */
} wiper_blade;

typedef struct {
    int         have;                    /* both tables read, a model given */
    int         car;
    wiper_blade b[2];
    float       tilt;                    /* [0x007B9D4F]                    */
    int32_t     table[256];              /* 0x007EC560                      */
    uint32_t    mask[8];                 /* 0x007ECD84                      */
    float       rate_park, rate_run;     /* [0x0083D254], [0x0083D258]      */
    int32_t     half_div, step_div;      /* [0x0083D27C], [0x0083D280]      */
    float       half;                    /* [0x0083D278]                    */
    /* the state */
    int32_t     acc;                     /* [0x007D62DA]                    */
    int32_t     phase;                   /* [0x007EC55C]                    */
    int32_t     step[2];                 /* [0x007D7BA6], [0x007D7BCA]      */
    int32_t     stroke[2];               /* [0x007EC554], [0x007EC558]      */
    int32_t     next_ms;                 /* [0x007ECD80]                    */
    int32_t     running;                 /* [0x007ECD7C]                    */
    uint32_t    lcg[2];                  /* [0x0083D24C], [0x0083D250]      */
    unsigned long poses;                 /* updates that posed the blades   */
    int         seeded;                  /* the state above is live        */
    int32_t     ends[2][2];              /* [0x0082CF04..10]: globals, so a
                                          * blade with no pair keeps the
                                          * last load's (as `seeded`)       */
} wipers;

#define WIPER_MASK_W  256
#define WIPER_MASK_H  128
#define WIPER_POSES   129

/* The wipe-off's load: what the kills read, and what the gate compares. */
typedef struct {
    int       have;
    int32_t   steps[2];                  /* [0x007D7BAA], [0x007D7BCE]: 128 */
    uint8_t   mask[2][WIPER_MASK_W * WIPER_MASK_H]; /* 0x007DBD12, 0x007E3D12 */
    uint32_t  end[2][WIPER_POSES][6];    /* E1 0x007D6376 / E2 0x007D6F8E: per
                                          * step p0, p1 = x y (float bits) z */
    unsigned long draws;                 /* the flow field's, both blades   */
    int32_t   tri[256];                  /* 0x007EC960: 0..127..0, the rain
                                          * wipe-off's phase -> step         */
    int32_t   cur0;                      /* [0x007BC664] as the image has it:
                                          * the wipe-offs' cursor, never reset */
    unsigned long clipped;               /* span cells outside a mask: 0 if
                                          * the projection's clamps hold    */
} wiper_masks;

/* Reads the tables and builds the two blades from `m` (16.16 is taken from
 * its points).  `carnum1` is the viewing car's shauncarnum1.  The state is
 * the image's (a process starts with it; the original never resets it).
 * 0, or -1 with a message in `err` (the wipers then never move). */
int  wipers_load(wipers *w, const ral_image *im, const ro3_model *m,
                 int32_t carnum1, char *err, size_t errlen);

/* 0x004ADB8F: one engine update.  `clock_ms` is [0x005C6568], `dt`
 * [0x0070D340], `run` is block +0x274C == 0.  Writes the posed blades into
 * `m`'s points. */
void wipers_update(wipers *w, ro3_model *m, int32_t clock_ms, int32_t dt,
                   int run);

/* The pose alone for a given phase, written into `out` (n x 3, 16.16) for
 * blade `k` - the gate's view (RALLY_WIPER_DUMP). */
int  wipers_pose_blade(const wipers *w, int k, int32_t phase,
                       int32_t out[][3]);

/* 0x004AD93A .. 0x004AE36F after wipers_load: the end points into `w`, the
 * masks into `mk`, and the flow field's draws on `*gen` ([0x006CF334],
 * between the race's seed and rain3d's table).  0, or -1 with a message
 * (no masks: the kills then do nothing). */
int  wipers_masks_build(wipers *w, const ral_image *im, wiper_masks *mk,
                        uint32_t *gen, char *err, size_t errlen);

/* The wipe-offs, once a frame while the effects advance, over any array of
 * records (`n` of `stride` bytes; `o_alive` an int32 that is 1 when live,
 * `o_x` / `o_y` the floats on the glass), so the port's pools and the
 * original's own (the gate) run the same code.  `*cur` is [0x007BC664],
 * the cursor both share.
 *
 *   0x004B0280, screen snow's: for each phase from the cursor up to the
 *   phase, blade 1's step at it (0x004AE4E0); a record on a cell holding it
 *   in mask 1 - or in mask 2, against the same step - dies.
 *   0x004AC6ED, screen rain's: per record the evaporation draw first
 *   (`*evap`, [0x007D6206]: 1 in 2048 dies), then the phases since the
 *   cursor, newest first, through 0x007EC960 against either mask.  `mk`
 *   NULL: the evaporation alone, the cursor untouched. */
void wipers_snow_wipe(const wiper_masks *mk, const wipers *w, int32_t *cur,
                      void *rec, size_t n, size_t stride, size_t o_alive,
                      size_t o_x, size_t o_y);
void wipers_rain_wipe(const wiper_masks *mk, int32_t phase, int32_t *cur,
                      uint32_t *evap, void *rec, size_t n, size_t stride,
                      size_t o_alive, size_t o_x, size_t o_y);

#endif
