/* ground.h - the wheel-to-ground query: which polygon each contact point is
 * on, where inside it, and how high the road is there.
 *
 * WHAT IT IS, READ FROM THE BINARY
 * --------------------------------
 * has the whole reading; in one paragraph:
 *
 *   FUN_0045B76A (44 b, the step's call at 0x00439C02, immediately before the
 *   surface solver) calls FUN_0045AE5D and then snapshots two of its outputs.
 *   FUN_0045AE5D is the PER-POINT DRIVER: for each of car+0x260 collision
 *   points it moves the car-local point into the world (the car matrix,
 *   FUN_0044B1AB, then 16.16 plus car+0x134), asks the QUERY for the polygon
 *   under it, adds a bilinear 64x64 bump map scaled by car+0x060, and stores
 *   what the surface solver reads.  The query is FUN_00440293 -> FUN_00440299:
 *   the previous step's polygon first, then the 3 x 3 cells around the point
 *   (zube), every polygon in each cell tested as one or
 *   two triangles in the x/z plane with 64-bit integer cross products.  The
 *   first hit wins.  It returns the polygon, WHICH TRIANGLE of a quad
 *   ([0x005819B4] - the flag the solver mirrors on), the point's BARYCENTRIC
 *   (u, v) in that triangle, and the plane height at the point.
 *
 * So the "x/z are polygon-local, 0..1" is confirmed from the producer's
 * side and made precise: x is v (the weight of the triangle's third corner),
 * z is u (the second's), and on a unit-square quad triangle 1's (u, v) is
 * (1-x, 1-z) of the square - which is exactly what the solver's flag==1
 * mirror undoes.
 *
 * THE CARRY FLAG, INVERTED
 * ------------------------
 * The query's helpers are in's family but they return
 * CF = 1 for FOUND (0x0044078B STC), the opposite sense to the file layer.
 * The one exception inside the family is FUN_00420DA8, the bucketed cell
 * lookup, which sets CF for NOT found.  Every helper below returns an int and
 * says which way round it is.
 *
 * TWO BRANCHES - AND RETAIL RUNS THE ONE THE PORT CAN COPY EXACTLY
 * -----------------------------------------------------------------
 * FUN_0044078D computes (u, v) one of two ways, chosen by [0x005D4788]:
 *
 *   == 0   x87:  u = num_u / det   (FDIV)
 *   != 0   SSE:  u = num_u * RCPSS(det)  - an APPROXIMATE reciprocal, 12 bits,
 *                whose exact bits are the CPU's own and differ by vendor
 *
 * [0x005D4788] is 0 in the image.  Its only writer, 0x00439973, stores
 * [0x005D45E8] & [0x00512140] - "a debug toggle" AND "SSE present, no
 * ignorekatmai" (0x00411288; CPUID leaf 1 EDX bit 25 and an XORPS probe at
 * 0x0041076A) - and it sits INSIDE a debug-key block (0x00439945: test
 * [0x004F1CF5] / je past it).  So it runs only when that key is pressed, and
 * the toggle starts at 1, so it takes TWO presses to reach SSE.  Retail's
 * default is the x87 branch.
 *
 * ~~So on any SSE machine the game takes the RCPSS branch.~~  That was this
 * header's first reading, from the toggle's initial value, before the
 * conditional around the store was read -.  The
 * capture settled it independently of the reading: through the solver and
 * yaw model, the x87 branch reproduces 178 of 178 blended frames and the
 * HOST'S RCPSS, on the machine the capture was made on, 4 (s15).  SIM_GROUND_SSE stays, for a replay recorded with the key pressed;
 * sim_ground_host_rcpss() returns the host's instruction where there is one
 * (x86) and NULL where there is not (aarch64).
 *
 * THE ARITHMETIC
 * --------------
 * The control word is FNINIT's 0x037F: 64-bit
 * significand, round to nearest.  Unlike the rest of port/sim this file does
 * NOT approximate that with double - the query's inputs are int32 16.16
 * coordinates, and the products that matter (cross products, determinants)
 * are exact in 64 bits and NOT in 53.  ground.c carries a small exact model
 * of the x87's 64-bit significand, so the result is the same bits on x86-64
 * and aarch64, and ground_selftest checks that model against the host's own
 * long double where the host has one.
*/

#ifndef RALLY_SIM_GROUND_H
#define RALLY_SIM_GROUND_H

#include <stddef.h>
#include <stdint.h>

#include "car.h"
#include "surface.h"

/* car+0xAF0/+0xBF0/+0xCF0 are three 0x100-byte arrays: 64 points.  The
 * per-car polygon array [0x006CDA54] (car+0xDF0) is 64 dwords too, and
 * FUN_0045B76A snapshots exactly 0x40 of it. */
#define SIM_GROUND_POINTS   64

/* FUN_0045AE5D sets [0x005D31A0] = 0x80 before the query: a polygon whose
 * pols +0x02 word ANDs non-zero with it is skipped (0x0044048F).  The other
 * six callers of FUN_00440293 set their own; none is ported. */
#define SIM_GROUND_MASK_CONTACT 0x80u
/* FUN_004375CA's: the camera's ground check (FUN_0043739C) - the last writer
 * of [0x005D31A0] in a player car's step (s14 item 4). */
#define SIM_GROUND_MASK_CAMERA  0x40u

typedef enum sim_ground_branch {
    SIM_GROUND_X87 = 0,     /* [0x005D4788] == 0: FDIV                      */
    SIM_GROUND_SSE = 1      /* [0x005D4788] != 0: RCPSS - debug key only;
                               retail runs X87                     */
} sim_ground_branch;

typedef float (*sim_rcp_fn)(float);

/* The host's RCPSS, or NULL if the host has none.  On x86 it is the CPU the
 * port happens to run on, which is the capture machine's only if the capture
 * was made on the same microarchitecture - see the header. */
sim_rcp_fn sim_ground_host_rcpss(void);

/* ------------------------------------------------------------------------ */
/* The level's ground geometry.                                             */

typedef struct sim_ground_geom {
    const uint8_t *zube;  int32_t nzube;   /* [0x00581A00], stride 0x20       */
    const uint8_t *pols;  int32_t npols;   /* [0x00581A04], stride 0x40       */
    const uint8_t *pnts;  int32_t npnts;   /* [0x00581A08], stride 0x0C       */

    /* FUN_00420B34 sorts zube into 256 hash buckets at load and FUN_00420DA8
     * searches the bucket.  With UNIQUE keys any key->record map gives the
     * same record, so the port keeps a sorted index instead of the sort -
     * and sim_ground_geom_build REFUSES a level with a duplicate key, where
     * the bucket order would start to matter (91,463
     * of 91,463 keys unique over 55 payloads). */
    uint32_t *key;
    int32_t  *rec;
    int32_t   nkey;

    sim_ground_branch branch;
    sim_rcp_fn        rcp;                 /* SIM_GROUND_SSE only           */
} sim_ground_geom;

/* Borrows the three arrays (they must outlive the geom).  Returns 0, -1 on a
 * duplicate cell key (the port's refusal, see above), -2 on allocation
 * failure.  branch is set to SIM_GROUND_X87; change it after. */
int  sim_ground_geom_build(sim_ground_geom *g,
                           const uint8_t *zube, int32_t nzube,
                           const uint8_t *pols, int32_t npols,
                           const uint8_t *pnts, int32_t npnts,
                           char *err, size_t errlen);
void sim_ground_geom_free(sim_ground_geom *g);

/* FUN_00420DA8 on its own: the zube record for a cell key, or -1 (the
 * original's CF = 1, NOT found).  The key is (x >> 21) + ((z >> 21) << 11),
 * both LOGICAL shifts - the ground query's and FUN_0045BDE9's.  Exported for
 * object collision (objects.c), which walks the same cells. */
int32_t sim_ground_cell_lookup(const sim_ground_geom *g, uint32_t key);

/* ------------------------------------------------------------------------ */
/* The query, FUN_00440293 -> FUN_00440299.                                 */

typedef struct sim_ground_hit {
    int32_t found;      /* [0x005EC6F0]  1 = a polygon holds the point       */
    int32_t poly;       /* [0x005819BC]  -1 unless found                     */
    int32_t tri;        /* [0x005819B4]  0: corners +4,+8,+C; 1: +10,+C,+8   */
    int32_t vert[3];    /* [0x005EC750..58]  the triangle's pnts indices     */
    float   u, v;       /* [0x005EC6D0], [0x005EC6D4]  barycentric: the
                         * weights of vert[1] and vert[2]                    */
    int32_t u16, v16;   /* [0x005EC6C0], [0x005EC6C4]  FISTP(u*65536) from the
                         * x87 register, NOT from the stored float           */
    int32_t height;     /* [0x005EC6E8]  the plane's y at the point, 16.16   */
} sim_ground_hit;

/* x, z: 16.16 world coordinates.  hint: the polygon to try first ([0x005819C0],
 * -1 for none; tried only if < npols, unsigned, as 0x00440281 does).  mask:
 * [0x005D31A0].  Returns 1 found, 0 not found, -1 if a polygon names a pnts
 * index outside the level (the original does not check - the refusal is the
 * port's) or the SSE branch has no rcp function.  On 0 only `found` and
 * `poly` are meaningful; the original leaves the rest of its globals stale. */
int sim_ground_query(const sim_ground_geom *g, int32_t x, int32_t z,
                     int32_t hint, uint32_t mask, sim_ground_hit *out);

/* ------------------------------------------------------------------------ */
/* The car's collision points - FUN_00425AAF, at model load.                */

typedef struct sim_collision_points {
    int32_t n;                      /* car+0x260  `numcollpts`             */
    float   x[SIM_GROUND_POINTS];   /* car+0xAF0                           */
    float   y[SIM_GROUND_POINTS];   /* car+0xBF0                           */
    float   z[SIM_GROUND_POINTS];   /* car+0xCF0  `fl/fr/bl/br z pos`      */
} sim_collision_points;

/* FUN_00425AAF's arithmetic from the model's own points: each 16.16 int / 65536
 * (FILD/FDIV/FSTP, one rounding), then entries 4..61 moved up two, then
 * POINT 4 = (0, 0, 0) and POINT 5 = (0, y[6], 0), and n = nsrc + 2.
 *
 * SO THE FIFTH CONTACT POINT IS THE CAR'S ORIGIN, DROPPED ONTO THE GROUND -
 * not a wheel.'s open question, answered from the
 * WRITER.  Point 5 is the origin at the height of the model's fifth point.
 *
 * The points that arrive here are the whole point array of the car's
 * COLLISION model, reordered by the head below; sim_car_collision_points
 * (physics.h) runs the chain.  Returns -1 if nsrc + 2 > 64 (the original
 * writes past the arrays; the refusal is the port's). */
int sim_collision_points_build(sim_collision_points *out,
                               const int32_t (*src)[3], int32_t nsrc);

/* FUN_00425777's HEAD, 0x00425777..0x00425ACA -.  (FUN_00425AAF
 * is its tail, reached by JMP; the entry is 0x00425777, CALLed once per car
 * from 0x004248A6.)  In place, on the 16.16 triples:
 *
 *   N = count clamped to [0, 0x3D]; y += 5.0 (0x50000) on the first N
 *   sort all N by y, the first 4 by z, points 0-1 by x, points 2-3 by x
 *   y -= 5.0 on the first N
 *
 * Each sort is the same hand-written quicksort on 12-byte records, keyed on
 * one dword compared UNSIGNED (JA/JC): y and z ascending, x descending.  So
 * the four lowest points become 0..3 - the wheels - front pair first (z > 0
 * is the smaller unsigned key), and within a pair -x first.  The original
 * sorts the shared model-point array [0x0057C224] itself, so the reorder is
 * permanent for every other reader of that type's points.
 *
 * Refuses (-1) count < 4 (the fixed 4-point z sort would read past the run;
 * count 0 takes the original's 0x00425BB4 path, which stores a stale count)
 * and a quicksort scan leaving its range (the original reads neighbouring
 * records; not reached by any shipped car). */
int sim_collision_points_select(int32_t (*pts)[3], int32_t count);

/* ------------------------------------------------------------------------ */
/* The per-point driver, FUN_0045AE5D, and its caller FUN_0045B76A.         */

typedef struct sim_ground_points {
    /* The original's GLOBAL scratch, 64 entries each.  In the game these are
     * shared by every car, so a point that MISSES keeps whatever the last car
     * to hit it left; here they are per car, which is the one difference and
     * it is outside the replayed prefix. */
    float   wx[SIM_GROUND_POINTS];     /* 0x00684F38  world, car-relative     */
    float   wy[SIM_GROUND_POINTS];     /* 0x00685038                          */
    float   wz[SIM_GROUND_POINTS];     /* 0x00685138                          */
    int32_t ix[SIM_GROUND_POINTS];     /* 0x00684B38  world 16.16             */
    int32_t iy[SIM_GROUND_POINTS];     /* 0x00684C38                          */
    int32_t iz[SIM_GROUND_POINTS];     /* 0x00684D38                          */
    int32_t ground[SIM_GROUND_POINTS]; /* 0x00684E38  road height + bump, 0 on
                                        * a miss                              */
    int32_t found[SIM_GROUND_POINTS];  /* 0x00685438                          */
    int32_t clear[SIM_GROUND_POINTS];  /* 0x00685238  iy - ground; wheels:
                                        * x max(matrix[5], 0)                 */
    int32_t clear_raw[SIM_GROUND_POINTS]; /* 0x00685338  wheels only          */
    int32_t tri[SIM_GROUND_POINTS];    /* 0x00685638  the solver's `flag`     */
    float   u[SIM_GROUND_POINTS];      /* 0x00685738  the solver's `z`        */
    float   v[SIM_GROUND_POINTS];      /* 0x00685838  the solver's `x`        */
    int32_t last_miss;                 /* 0x006868A8  -1, or the last point
                                        * that found no polygon               */
    float   last_wx, last_wz;          /* 0x0068634C, 0x00686354              */
    int32_t bump[SIM_GROUND_POINTS];   /* 0x00685D80 per point (the original
                                        * keeps only the last): the map's
                                        * bilinear sample, 0..255             */

    /* FUN_0045B76A's snapshot, taken after the driver returns. */
    int32_t prev_ixyz[3][SIM_GROUND_POINTS]; /* 0x00685938 <- 0x00684B38, 0xC0 */
    int32_t prev_poly[SIM_GROUND_POINTS];    /* 0x00685C38 <- car+0xDF0, 0x40  */

    /* PER CAR, and persistent: the polygon each point was last found on.
     * It is the next step's hint, and a point that misses KEEPS it. */
    int32_t poly[SIM_GROUND_POINTS];   /* car+0xDF0  [0x006CDA54]             */
    int32_t wheel_ix[4];               /* car+0xF10  [0x006CDB74]             */
    int32_t wheel_ground[4];           /* car+0xF20  [0x006CDB84]             */
    int32_t wheel_iz[4];               /* car+0xF30  [0x006CDB94]             */
} sim_ground_points;

/* Every poly = -1 (no hint).  NOT the game's value: in a fresh process the
 * image-backed car+0xDF0 is 0, and no setup writer exists (section 8).  Whether a later stage resets it is open.
 * Only the first step's hint can differ. */
void sim_ground_points_init(sim_ground_points *gp);

/* What FUN_0045AE5D reads that is not in the car record or the level.
 * susph/suspt are FUN_0043D033's - read, not
 * yet ported - so inputs, like bumpf. */
typedef struct sim_ground_inputs {
    const sim_collision_points *pts;
    const uint8_t *bump_map;   /* [0x009CAEE4], 64 x 64 (stage_fog_noise())   */
    float susph;               /* [0x005C7928] `susph`                         */
    float suspt;               /* [0x005C792C] `suspt`                         */
    float wheel_drop;          /* [0x005C6BCC]  sim_consts.wheel_drop          */
    float susp_half;           /* [0x005C6CC0]                                 */
    float bump_scale;          /* [0x0057B0B0]                                 */
    float bump_div;            /* [0x0057AF98]                                 */
} sim_ground_inputs;

/* FUN_0045B76A: the driver over car->numcollpts points (car+0x260, as the
 * original reads it; in->pts supplies their car-local positions), then the
 * snapshot.  Reads car->matrix, car->pos, car->bump_q (the PREVIOUS step's,
 * since the solver that writes it runs after this) and car->carpartsleft.
 * Returns 0, or -1 on a query refusal or more than 64 points. */
int sim_ground_contacts(const sim_ground_geom *g, const car_state *car,
                        const sim_ground_inputs *in, sim_ground_points *gp);

/* FUN_0045AE5D's query driver without FUN_0045B76A's previous-frame
 * snapshot.  Used by FUN_0043D66C inside collision passes. */
int sim_ground_query_contacts(const sim_ground_geom *g, const car_state *car,
                              const sim_ground_inputs *in,
                              sim_ground_points *gp);

/* The five things the surface solver takes, in its own vocabulary. */
void sim_ground_to_contacts(const sim_ground_points *gp,
                            sim_contact out[SIM_CONTACT_POINTS]);

/* ------------------------------------------------------------------------ */
/* Exposed for ground_selftest only: the x87 64-bit-significand model.      */

/* A value as the x87 holds it: (-1)^neg * m * 2^e, m < 2^64, cls 0 finite
 * (m == 0 is zero), 1 infinity, 2 NaN.  Every operation rounds its exact
 * result to a 64-bit significand, to nearest-even - FNINIT's control word.
 * NaN is always the x87's own "real indefinite": payload propagation is not
 * modelled, and no input on the query's path is ever a NaN. */
typedef struct sim_x87 { int neg; int cls; int32_t e; uint64_t m; } sim_x87;

sim_x87 sim_x87_from_float(float f);
sim_x87 sim_x87_from_i64(int64_t i);
sim_x87 sim_x87_add(sim_x87 a, sim_x87 b);
sim_x87 sim_x87_sub(sim_x87 a, sim_x87 b);
sim_x87 sim_x87_mul(sim_x87 a, sim_x87 b);
sim_x87 sim_x87_div(sim_x87 a, sim_x87 b);  /* FDIV; ZE masked: signed inf */
float   sim_x87_to_float(sim_x87 a);
int32_t sim_x87_fistp(sim_x87 a);

/* ------------------------------------------------------------------------ */
/* The car's ground light - FUN_0044CD28, called at 0x00439E92.             */

/* The three ramps it reads, built at level load by FUN_0044D0EF's LOCALDAT
 * arm (0x0044D157: a live race has [0x005D41B0] != 0):
 *   lut  [0x005E7FDC]  FUN_0044D1AD: lo + (hi - lo) * i / 255 per channel,
 *                      lo = LOCALDAT +0x600, hi = +0x60C
 *   c1   [0x005E83DC]  FUN_0044D200: c * i / 255, c = LOCALDAT +0x1898
 *   c2   [0x005E87DC]  the same, c = LOCALDAT +0x18A4
 * each 0xFFRRGGBB, IDIV truncating. */
typedef struct sim_ground_light {
    uint32_t lut[256], c1[256], c2[256];
} sim_ground_light;

/* lo, hi, c1, c2: R, G, B as the LOCALDAT dwords hold them. */
void sim_ground_light_build(sim_ground_light *gl, const int32_t lo[3],
                            const int32_t hi[3], const int32_t c1[3],
                            const int32_t c2[3]);

/* FUN_0044CD28: the polygon under the car's origin (car+0x134, +0x13C;
 * hint car+0xE00, the origin contact's polygon; `mask` the [0x005D31A0]
 * the step's last query left), its four corner shade bytes pols+0x14..0x17
 * bilinear by the hit's (u16, v16) - both 0x10000 - x on the quad's second
 * triangle (0x0044CD7E) - each first darkened by pols+0x22..0x25 when the
 * polygon's texinfo slot has second-layer mode 1 (0x0044CDEE); that is
 * car+0x244, and car+0x240 = lut[it].  Where pols+0x02 has bit 2, the same
 * bilinear of +0x28..0x2B through c1 and of +0x2C..0x2F through c2 are
 * added, saturating per byte (0x0044CF44..0x0044D0E9).  No polygon: both
 * fields are left as they were (0x0044CD78).
 *
 * The reset window (0x0044CED4): while car+0xFB4 ([0x006CDC18]; 8 at
 * 0x0043DFF6 in FUN_0043DFC4, one off a step at 0x0043B999) is non-zero,
 * the index moves at most 10 a step from car+0x244 as it came in: when
 * |new - prev| >= 10 (unsigned JB, so a difference of exactly 10 is the
 * target anyway) new = prev +- 10.  `reset_window` is that counter, kept in
 * sim_race.fb4_q[slot] (physics.h; the replay harness zeroes sim_world
 * each step).  The arm at 0x0044CEDD ([0x005D64B4] != 0: the index goes
 * back to prev) is the pause / retire flag, never set while a step runs,
 * so it is not ported.  The index is used masked to 0..255; the original
 * does not mask, and the shade bytes' bilinear cannot leave that range. */
void sim_car_ground_light_q(car_state *car, const sim_ground_geom *geom,
                            const sim_surface_level *level,
                            const sim_ground_light *gl, int32_t hint,
                            uint32_t mask, int32_t reset_window);

#endif /* RALLY_SIM_GROUND_H */
