/* surface.h - the surface solver, sim_surface_solve_q (FUN_0043BA75)..
 *
 * WHAT THIS IS
 * ------------
 * The road's half of the per-car step.  For each of the five contact points it
 * takes (the polygon the point is on, where inside that polygon it is, a
 * per-point flag), finds the TWO surface classes the polygon's two texture
 * layers give that quadrant, blends the two surface records by a weight the
 * polygon's four corner bytes define, and writes the grip arrays the tyre force
 * model and the yaw model consume - plus six things no document had recorded
 * before:
 *
 *  a count of wheels standing on classes {1, 2, 3, 12}   -> car0+0x258
 *  the minimum over ALL FIVE points of record +0x28, x bumpf -> car0+0x060
 *  `gripf`, a lerp of two wetness-blended constants by set_s_1
 *  the drag pair sim_surface already carried, which is contact point 4's
 *     blended +0x20 and +0x24 - so the fifth point sets the drag
 *  point 4's +0x14 and +0x10 lerps copied to two more globals
 *  the per-point class, quadrant, weight and aux bytes, as scratch
 *
 * WHAT IT TAKES FROM OUTSIDE, AND WHY THAT IS EXACTLY ONE THING
 * ------------------------------------------------------------
 * the ticket measured it: every reference to the four arrays the head
 * reads - the polygon index [0x006CDA54], the contact point's x [0x00685838]
 * and z [0x00685738], and a flag [0x00685638] - other than this function's own
 * is inside ONE block at 0x0045AF81..0x0045B784.  That block is the
 * wheel-to-ground query, and it is NOT here - it is ground.h's, and
 * sim_car_step hands this function its answer.  (also found that only
 * ONE of those four sites is a store: 0x0045AF81, 0x0045B31B and 0x0045B784
 * are reads - `refs` reports where a value sits, not what the instruction
 * does..)
 *
 * The x/z pair is POLYGON-LOCAL, 0..1, and that is a reading of this
 * function: bit 15 of FISTP(x * 65536) picks a half of the polygon, and the
 * corner-weight plane below multiplies x and z by differences of 0..255
 * corner bytes and divides by 256.  CONFIRMED from the producer's side by
 * x and z are the BARYCENTRIC weights v and u of the triangle's third
 * and second corners (FUN_0044078D), which on a unit-square quad are the
 * square's own coordinates - and triangle 1's are mirrored, which is what
 * the flag == 1 mirror below undoes.
 *
 * WHAT IS LOADED AND WHAT IS BUILT
 * --------------------------------
 * The 16 x 0x30 table at 0x005C66B0, its 13-int32 curve pool at 0x005C69B0,
 * the 256-entry second-layer mode table at 0x005F0890 and the seven scalar
 * constants are LOADED from the user's own image at runtime, never
 * transcribed.  Each record's first dword is a POINTER into the curve pool and
 * is resolved through the image, not dereferenced.
 *
 * THE TRAP'S TICKET NAMED: the record stride is 0x30 and the CURVE
 * stride is 0x34.  Record 0's pointer IS the pool base, so a reader that treats
 * the pool as "stride 0x30 from here" gets record 0 right and every other
 * record wrong.  sim_surface_table_load's self-check exists to catch exactly
 * that: records 0, 13, 14 and 15 must resolve to the all-ones curve, 1..12
 * must not, and every pointer must land on a 0x34 boundary inside the pool.
 *
 * The per-level descriptor table at 0x00922330 is BUILT at level load by
 * initgametexinfbuf_q (0x00437B1D) out of LOCALDAT's texinfo index and the
 * TEXTINFO file; sim_surface_level_build is a port of the four fields of it
 * this function reads.  It is data from the user's own install, read at
 * runtime, like the table.
*/

#ifndef RALLY_SIM_SURFACE_H
#define RALLY_SIM_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#include "car.h"
#include "car_params.h"

#define SIM_CONTACT_POINTS     5

#define SIM_SURF_RECORDS      16      /* 0x005C66B0 .. 0x005C69B0, exactly     */
#define SIM_SURF_STRIDE     0x30
#define SIM_SURF_CURVE_LEN    13      /* int32 per curve                       */
#define SIM_SURF_CURVE_STRIDE 0x34
#define SIM_SURF_TEXSLOTS    256      /* the descriptor table, stride 0x40     */
#define SIM_SURF_LAYER_MODES 256      /* [0x005F0890 + byte*4], byte 0..255    */

/* The tyre-type clamp is THE PORT'S, NOT THE ORIGINAL'S.  [0x006CCC90] has
 * three references image-wide and none of them bounds it (section 5); six compounds is an argument from the curve layout, where k = 6
 * would read curve B's first entry as curve A's seventh.  An out-of-range
 * value in the original reads the next curve's data; the port clamps to 0..5
 * rather than reproduce an out-of-bounds read it cannot model. */
#define SIM_SURF_TYRE_MAX      5

/* ------------------------------------------------------------------------ */
/* The table.                                                               */

typedef struct sim_surf_rec {
    uint32_t curve_va;              /* +0x00  the POINTER, as stored          */
    int32_t  curve[SIM_SURF_CURVE_LEN];  /* ... and what it points at: entry 0
                                     * a default the solver loads and then
                                     * overwrites unread, 1..6 curve A, 7..12
                                     * curve B, both indexed by tyre type      */
    float    slide;                 /* +0x04  -> [0x5C6FB8]                    */
    float    accel;                 /* +0x08  -> [0x5C6FCC]                    */
    float    brake;                 /* +0x0C  -> [0x5C6FE0]                    */
    float    f10, f14;              /* +0x10 -> [0x5C7430], +0x14 -> [0x5C741C] */
    float    f18, f1c;              /* +0x18, +0x1C: READ BY NOTHING HERE      */
    float    f20, f24;              /* +0x20 -> [0x5C73F4], +0x24 -> [0x5C7408] */
    float    f28;                   /* +0x28 -> [0x5C6FF4], and the min() tail */
    float    f2c;                   /* +0x2C -> [0x6CD5AC]                     */
} sim_surf_rec;

typedef struct sim_surface_table {
    sim_surf_rec rec[SIM_SURF_RECORDS];

    /* [0x005F0890 + b*4], b = the SECOND layer's TEXTINFO +0x24.  Only 0..2
     * carry meaning ({0 -> 3, 1 -> 5, 2 -> 4}); the
     * corpus uses 0..3 in slots and 0..2 on polygons (measured),
     * and entries past 3 are whatever .data holds there.  Loaded whole so the
     * port reads what the original reads for any byte. */
    int32_t layer_mode[SIM_SURF_LAYER_MODES];

    float k_pct;         /* 0x0057AF80  0.01    - curve percent -> multiplier */
    float k_256;         /* 0x0057B0B0  256.0   - the corner-byte / set_s_1 divisor */
    float k_mean;        /* 0x005C6D24  0.0025  - [0x5C6F9C]'s mean scale     */
    float k_fix;         /* 0x0057B01C  65536.0 - the quadrant FISTP's scale  */
    float gripf_k[4];    /* 0x005C6C90/94/98/9C  1.0 0.9 0.85 1.0 - `gripf`   */
} sim_surface_table;

/* 0 on success.  -1/-2 the image could not be read (SKIP), -3 an address is
 * not backed by raw data, -4 the self-check failed: this is not the retail
 * 5.25 image, or the reader has the curve stride wrong. */
int sim_surface_table_load(const char *image_path, sim_surface_table *out,
                           char *err, size_t errlen);

/* ------------------------------------------------------------------------ */
/* The level: the four descriptor fields the solver reads, and the polygons. */

/* One slot of the descriptor table at 0x00922330 + id*0x40, as
 * initgametexinfbuf_q fills it from TEXTINFO record r = LOCALDAT texinfo[id]:
 *
 *     desc+0x10 dword  <- TI[r*0x80 + 0x20]   the four per-quadrant CLASSES
 *     desc+0x14 dword  <- TI[r*0x80 + 0x10]   four per-quadrant bytes, unnamed
 *     desc+0x18 dword  <- TI[r*0x80 + 0x14]   four per-quadrant bytes, unnamed
 *     desc+0x2C byte   <- TI[r*0x80 + 0x24]   the second-layer mode index
 *
 * 0x00437B69..0x00437B84.  Note the CROSSOVER: +0x14 takes TEXTINFO +0x10 and
 * +0x18 takes TEXTINFO +0x14.  The builder writes other fields too (the plate,
 * the slot, the corner uvs, +0x2D/+0x2E); the solver reads none of them. */
typedef struct sim_texdesc {
    uint8_t cls[4];      /* desc+0x10 */
    uint8_t b14[4];      /* desc+0x14  (TEXTINFO +0x10) */
    uint8_t b18[4];      /* desc+0x18  (TEXTINFO +0x14) */
    uint8_t mode;        /* desc+0x2C  (TEXTINFO +0x24) */
} sim_texdesc;

typedef struct sim_surface_level {
    sim_texdesc    desc[SIM_SURF_TEXSLOTS];
    const uint8_t *pols;        /* the payload's pols array, stride 0x40 -
                                 * [0x00581A04] in the original             */
    int32_t        npols;
} sim_surface_level;

/* texinfo is LOCALDAT +0x0100, 256 little-endian dwords (1,024 bytes).
 * Returns 0, or -1 if a texinfo index points past the end of TEXTINFO - which
 * no shipped level does (12,800 of 12,800 below 16,000) and which
 * the ORIGINAL does not check; the refusal is the port's. */
int sim_surface_level_build(sim_surface_level *lv,
                            const uint8_t *texinfo_1024,
                            const uint8_t *textinfo, size_t textinfo_len,
                            const uint8_t *pols, int32_t npols);

/* ------------------------------------------------------------------------ */
/* The solver.                                                              */

/* What the wheel-to-ground query leaves for the solver, per point. */
typedef struct sim_contact {
    int32_t poly;        /* [0x006CDA54 + i*4]  index into pols               */
    float   x;           /* [0x00685838 + i*4]  polygon-local, see the top     */
    float   z;           /* [0x00685738 + i*4]                                 */
    int32_t flag;        /* [0x00685638 + i*4]  == 1 mirrors the quadrant AND
                          * selects the far corner triple for the weight      */
} sim_contact;

/* What the solver writes.  The first six fields are what physics.h's
 * consumers read and have always been here; the rest are new and
 * each names the address it stands for. */
typedef struct sim_surface {
    float slide[SIM_CONTACT_POINTS];  /* [0x005C6FB8 + i*4]                   */
    float accel[SIM_CONTACT_POINTS];  /* [0x005C6FCC + i*4]                   */
    float brake[SIM_CONTACT_POINTS];  /* [0x005C6FE0 + i*4]                   */
    float mean_slide;                 /* [0x005C6F9C]  sum(slide[0..3])*0.0025 */
    float drag_k;                     /* [0x005C7494] = f20[4]                */
    float drag_vmin;                  /* [0x005C7498] = f24[4]                */

    int32_t cls_a[SIM_CONTACT_POINTS];   /* [0x006CD584]  layer 0's class     */
    int32_t cls_b[SIM_CONTACT_POINTS];   /* [0x006CD598]  layer 1's class     */
    int32_t quad_a[SIM_CONTACT_POINTS];  /* [0x005C7444]  0..3                */
    int32_t quad_b[SIM_CONTACT_POINTS];  /* [0x005C7458]                      */
    int32_t aux18_a[SIM_CONTACT_POINTS]; /* [0x006CD5C0]  desc+0x18 byte      */
    int32_t aux18_b[SIM_CONTACT_POINTS]; /* [0x006CD5D4]                      */
    int32_t aux14_a[SIM_CONTACT_POINTS]; /* [0x006CD5E8]  desc+0x14 byte      */
    int32_t aux14_b[SIM_CONTACT_POINTS]; /* [0x006CD5FC]                      */
    float   weight[SIM_CONTACT_POINTS];  /* [0x006CD610]  0 = all layer 0     */

    float f28[SIM_CONTACT_POINTS];    /* [0x005C6FF4]                         */
    float f2c[SIM_CONTACT_POINTS];    /* [0x006CD5AC]                         */
    float f20[SIM_CONTACT_POINTS];    /* [0x005C73F4]                         */
    float f24[SIM_CONTACT_POINTS];    /* [0x005C7408]                         */
    float f14[SIM_CONTACT_POINTS];    /* [0x005C741C]                         */
    float f10[SIM_CONTACT_POINTS];    /* [0x005C7430]                         */

    int32_t on_road[SIM_CONTACT_POINTS]; /* [0x005C6E88]  class in {1,2,3,12} */
    float   gripf;                    /* [0x005C6F64]  the overlay's `gripf`   */
    float   f14_pt4;                  /* [0x005D6C3C] = f14[4]                 */
    float   f10_pt4;                  /* [0x005D6C40] = f10[4]                 */
} sim_surface;

/* The three grip knobs [0x005C78F0]/[0x005C78E8]/[0x005C78EC] are the car
 * record's `sidegrip`/`acc grip`/`brk grip`, which sim_load_car_params is the
 * only writer of - so they come from `p`.  `bumpf`
 * [0x005C7938] is written by FUN_0043D033, unread, and is therefore an input
 * the way set_cgh and bbal already are.
 *
 * Reads car->tyretype (car0+0x2C), car->wetns (car0+0x280) and car->set_s_1
 * (car0+0x34).  WRITES car->road_wheels_q (car0+0x258) and car->bump_q
 * (car0+0x060) - both inside the replayed prefix, so the state-dump capture
 * carries the game's own values of both.
 *
 * Returns 0, or -1 if a contact point names a polygon outside the level - the
 * original has no such check (it indexes [0x00581A04] + poly*0x40 blind) and
 * the refusal is the port's; `out` is not trustworthy in that case. */
int sim_surface_solve_q(const sim_surface_table *t,
                        const sim_surface_level *lv,
                        const sim_contact contact[SIM_CONTACT_POINTS],
                        const car_params *p, float bumpf,
                        car_state *car, sim_surface *out);

#endif /* RALLY_SIM_SURFACE_H */
