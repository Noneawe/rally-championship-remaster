/* physics.h - the second half of the per-car step: the tyre force model, the
 * yaw model, the two integrators and the divergence guard.
 *
 * WHAT THIS ADDS TO port/sim
 * --------------------------
 * built the STRUCT (car.h) and the DRIVETRAIN (drivetrain.c), and
 * listed exactly what was missing.  This
 * header covers that list:
 *
 *   sim_tyre_forces_q      FUN_0043C48C
 *   sim_yaw_model_q        FUN_0043B254
 *   sim_integrate_position FUN_0043B18B
 *   sim_integrate_orient   FUN_0043C22F
 *   sim_divergence_guard   FUN_0043DC70
 *   sim_car_step           FUN_00439B37 (the order)
 *
 * WHAT IT DELIBERATELY DOES NOT COVER, AND WHY THAT IS A SHAPE NOT A GAP
 * ---------------------------------------------------------------------
 * The step's inputs that come from the STAGE were not ported, whose
 * own text excludes collision geometry.  ~~So the five contact points' grip
 * arrays arrive as a `sim_surface` the caller fills~~ -:
 * sim_surface_solve_q (FUN_0043BA75, surface.c) is ported and sim_car_step runs
 * it.  ~~What still arrives from the caller is the WHEEL-TO-GROUND QUERY's
 * answer~~ -: the query (FUN_0045B76A, ground.c) is ported
 * and sim_car_step runs it too; `sim_ground` now carries the level geometry
 * and the inputs the query reads. ports the setup, mass, load, drag and
 * suspension producers (FUN_0043D033, FUN_004351C9, FUN_0043D8AB/D9CC and
 * FUN_0043D198); the remaining caller-owned inputs are the model collision
 * points, level geometry, bump map and the query's image-backed constants.
 *
 * THE ARITHMETIC RULE THIS FILE IS WRITTEN UNDER
 * ----------------------------------------------
 * The original is x87.  A run of FLD/FMUL/FDIV/FSUB ending in ONE FSTP is ONE
 * rounding to float32, not one per operator -, and the
 * bug paid 2 values in 193 for.  Every place this file computes a
 * multi-term expression that the original stores once, it is written as
 * `(float)(double expression)` so the intermediates keep their precision, and
 * the comment says which FSTP it is matching.  Where the original DOES store
 * between terms, this file stores too.  Getting this backwards is invisible in
 * decimal and is the single most likely way for this port to be subtly wrong.
 *
 * THE GATE
 * --------
 * trace_check.c replays the state-dump hook's capture (section 5: 36,754 frames of the real game's own car0 block) and checks these
 * transcriptions against it FRAME BY FRAME, bitwise.  That is the first
 * port-side physics this project has ever had to diff against the hook, and it
 * is what reports.
*/

#ifndef RALLY_SIM_PHYSICS_H
#define RALLY_SIM_PHYSICS_H

#include <stddef.h>
#include <stdint.h>

#include "car.h"
#include "car_params.h"
#include "drivetrain.h"
#include "ground.h"
#include "surface.h"
#include "soundfeed.h"
#include "weather.h"

/* ------------------------------------------------------------------------
 * The force accumulator.
 *
 * 23 records of 0x20 bytes at 0x005C74A4, of which the first six dwords are
 * used: force (x,y,z) then application point (x,y,z).  FUN_0043D859 clears
 * exactly those six per record, and a sweep of every READ/WRITE landing in the
 * span found 130 accesses and none outside +0x00..+0x14 -
 * section 2, which corrected's "five records".
 *
 * The record ROLES are that document's, and they are TENTATIVE: inferred from
 * seven agreeing producers and cross-checked against the velocity triple's
 * middle component, but no string or table names them. */
#define SIM_FORCE_RECORDS     23
/* SIM_CONTACT_POINTS (5) is surface.h's, which owns the contact points. */

#define SIM_REC_DRAG          10
#define SIM_REC_LAT_FIRST     11   /* 11..14: `force fg` / `force rg`        */
#define SIM_REC_LONG_FRONT    15   /* 15,16 front pair; 17,18 rear pair      */
#define SIM_REC_LONG_REAR     17
#define SIM_REC_YAW_FIRST     19   /* 19,20: the yaw couple                  */
/* Record 21 is BODY CONTACT: FUN_0045C90A's impulse, applied at the origin
 * (point zeroed, so no torque).. */
#define SIM_REC_BODY_CONTACT  21

#define SIM_COLLISION_FORCE_RECORDS 7

typedef struct sim_force_rec {
    float f[3];              /* +0x00 +0x04 +0x08   force   x y z            */
    float p[3];              /* +0x0C +0x10 +0x14   point   x y z            */
    float _unused[2];        /* the rest of the 0x20 stride: never accessed  */
} sim_force_rec;

/* ------------------------------------------------------------------------
 * The sine table, which is what calls `steer_curve`.
 *
 * MEASURED THIS SESSION, and it upgrades the yaw model's headline from an
 * interpretation to an identity.  Section 4's first line is
 *
 *     [car0+0x210] = steer_curve[ USTEERANGLE >> 20 ] * CZSPD / [0x005C78D4]
 *
 * with no address given for `steer_curve`.  The bytes that document quotes at
 * 0x0043B2D1 decode to  FLD [EDX*4 + 0x009C3EE4], and 0x009C3EE4 is the
 * 5,120-entry sin(i*pi/2048) table read out of
 * its BUILDER at 0x0041E368 (it is past .data's raw extent, so the image holds
 * zeros there and dumping it finds nothing).
 *
 * So the steering term is literally  sin(delta) * v / L  - the bicycle model
 * exactly, not by analogy.  USTEERANGLE >> 20 is a LOGICAL shift, so the index
 * is 0..4095 and a full turn is 4,096 steps; the 1,024 entries of overrun
 * exist so the quarter-turn offset read never needs a bounds check. */
#define SIM_SIN_ENTRIES     5120
#define SIM_SIN_QUARTER     1024   /* the +0x1000 cosine offset              */

/* ------------------------------------------------------------------------
 * The image constants this half reads.  Same policy as dt_consts: they are
 * numbers out of the copyrighted image, so they are LOADED, not transcribed
 * The VAs are retail 5.25 and the retail/5.30 delta is not constant
 * so this table must be re-measured per
 * build rather than offset. */
#define SIM_DMG_PAGE_BASE  0x008DCD74u   /* sim_consts.dmg_page    */
#define SIM_DMG_PAGE_WORDS 512u          /* to 0x008DD574                  */
typedef struct sim_consts {
    float dt;            /* 0x005C65C4  0.01  - the timestep                 */
    float pos_scale;     /* 0x0057B01C  65536.0 - the 16.16 fixed-point scale */
    float zero;          /* 0x0057B014  0.0   - vec3_normalise's length test */

    /* The three sim_load_car_params loads and then OVERWRITES with globals -
     *(a).  The per-car values at record +0x50,
     * +0x54, +0x58 are dead: nothing jumps past the second store, measured
     * zero JUMP references each.  So the physics uses these for EVERY car. */
    float wheelbase;     /* 0x005C6C3C -> [0x005C78D4]  2.4                  */
    float half_q;        /* 0x005C6C40 -> [0x005C78D8]  0.5   (no reader yet) */
    float grip_k;        /* 0x005C6C48 -> [0x005C78DC]  4.0   - the `K`      */

    float clamp_ang;     /* 0x005C6BB0  4.0   angular, the guard             */
    float clamp_lin;     /* 0x005C6BB4  80.0  linear,  the guard             */
    float clamp_zspddif; /* 0x005C6BC0  10.0  - AND SEE THE WARNING BELOW    */
    float clamp_rotspd;  /* 0x005C6C08  15.0                                 */
    float gravity;       /* 0x005C6CFC  9.8                                  */

    float trac_num;      /* 0x005C6B88  800.0  sim_traction_limit            */
    float trac_add;      /* 0x005C6B8C  20.0                                 */
    float trac_sub;      /* 0x005C6B90  20.0                                 */

    /* The three per-axis torque->angular-velocity scales FUN_0045D6D7 applies
     * after the cross(force,point) torque -.  0.85 / 0.75 / 2.5 in the
     * image; not confirmed as an inertia tensor's reciprocal by anything
     * stronger than the arithmetic's shape (a per-axis multiplier applied to
     * a torque before it is summed into an angular velocity), so read as
     * "the constants at these three addresses", not as a physical claim. */
    float inv_inertia[3]; /* 0x005C6CDC/E0/E4                                */

    float contact_normal_floor;  /* 0x0057AF80  0.01                        */
    float contact_restitution;   /* 0x00684B18  1.1                         */
    float contact_push;          /* 0x00684B1C  0.1                         */
    float body_mu_base;          /* 0x0057AFBC  1.0                         */
    float body_mu_scale;         /* 0x0057AFC0  2.0                         */
    float body_yaw_divisor;      /* 0x0057AFE4  64.0                        */
    float body_contact_friction; /* 0x005C6FC8  0.0                         */
    int32_t body_contact_threshold; /* 0x005C6C38  0x1000 in 16.16          */
    /* FUN_0043F058's hard-impact test; one reference each, no writer. */
    float impact_speed_min;      /* 0x005C6B80  8.0                         */
    float impact_retain_ratio;   /* 0x005C6B84  0.5                         */

    /* The wheel-to-ground query's per-point driver, FUN_0045AE5D -,
     * ground.h.  Each has exactly the references listed and no writer. */
    float wheel_drop;    /* 0x005C6BCC  0.15  - added to a wheel's contact
                          * height unless BOTH its CPLEFT bits are set
                          * (0x0045AEAB, 0x0045AEC1)                          */
    float susp_half;     /* 0x005C6CC0  0.5   - x suspt (0x0045AEC7; also
                          * 0x0043D2D4, FUN_0043D198, unported)               */
    float wheel_radius;  /* 0x005C6BFC  0.65  - FUN_0043B1F5's divisor: the
                          * wheel spin angle                          */
    float inv_pi;        /* 0x0057B04C  1/pi  - and its last factor           */
    float wheel_ride;    /* 0x005C6C00  0.325 - added to a wheel's height in
                          * car+0x910. (0x0043D2C5)                   */
    float bump_scale;    /* 0x0057B0B0  256.0 - the bump map's FMUL           */
    float bump_div;      /* 0x0057AF98  100.0 - and its FDIV                  */

    /* FUN_0043D033 setup-slider endpoints, loaded rather than transcribed. */
    float bbal_low, bbal_high;       /* 0x005C6C50 / 0x005C6C54              */
    float ste_k_low, ste_k_high;     /* 0x005C6C58 / 0x005C6C5C              */
    float ste_j_low, ste_j_high;     /* 0x005C6C60 / 0x005C6C64              */
    float susph_low, susph_high;     /* 0x005C6C78 / 0x005C6C7C              */
    float set_cgh_low[2], set_cgh_high[2]; /* 0x005C6C80..0x005C6C8C         */
    float suspt_low, suspt_high;     /* 0x005C6CA0 / 0x005C6CA4              */
    float suspd_low, suspd_high;     /* 0x005C6CA8 / 0x005C6CAC              */
    float susp_factor_low, susp_factor_high; /* 0x005C6CB8 / 0x005C6CBC      */
    float bumpf_low, bumpf_high;     /* 0x005C6CB0 / 0x005C6CB4              */

    /* FUN_0043D8AB / D9CC / D198 producer constants. */
    float load_bump_low;             /* 0x0057AFBC  1.0                       */
    float load_bump_high;            /* 0x0057AFCC  5.0                       */
    float load_bump_step;            /* 0x0057B028  0.25                      */
    float drag_speed_scale;          /* 0x005C6CE8                             */
    float mass_nonzero_scale;        /* 0x005B538C                             */
    /* FUN_0043D79E, record 5: v*v * num / den per car-frame axis.  The same
     * two words dt_consts carries as drag_q/k10k for initaccels. */
    float body_drag_num;             /* 0x005C6D04  5.0                        */
    float body_drag_den;             /* 0x0057B0B8  10000.0                    */
    float suspension_force_scale;    /* 0x005C6CC4                             */
    float lateral_point_y;           /* 0x005C6F08                             */
    float longitudinal_point_y;      /* 0x005C6F0C                             */
    /* the tyre model's two grip helpers, FUN_0043F12D and
     * FUN_0043F1A5.  One read each (0x5C6BA4
     * two), no absolute or [reg + disp] writer. */
    float slip_grip_scale[2];        /* 0x005C6C24 0.0045 (class 0), 0x005C6C28
                                      * 0.0025 (class 1)                       */
    float lowspd_grip_ref;           /* 0x005C6BA4  20.0                       */
    float lowspd_grip_add;           /* 0x005C6BA8  1.0                        */
    float lowspd_grip_half;          /* 0x005C6BAC  0.5                        */
    /* The engine and wheels after the tyre model, 0x00439CAF..0x00439D0B
     * No writer. */
    float wheel_spin_down[2];        /* 0x005C6C74 0.01 (brake off), 0x005C6C70
                                      * 1.0 (brake on) - FUN_0043A471          */
    float idle_revs[2];              /* 0x005C6CEC 3000 (class 0), 0x005C6CF0
                                      * 2000 - [0x005C6E30] at load            */
    float overrev_scale;             /* 0x005C6D1C  1.2 - [0x005C6E3C] = maxrevs
                                      * x this, at load                        */
    float free_decay_scale;          /* 0x005C6D10  0.005 - FUN_0043CF56       */
    float free_decay_base;           /* 0x005C6D0C  5.0                        */
    /* The gearbox.  Data with no absolute writer;
     * each is shared with an unrelated reader, which is why they are named
     * for this use here and not merged with the other copy. */
    float gear_reverse_revs;         /* 0x0057B028  0.25 - revs x this on the
                                      * shift into reverse (0x0043A950)       */
    float gear_select_blend;         /* 0x005C6D18  0.99 - FUN_0043AD87's blend
                                      * of the old revs into the new gear's   */
    float stall_margin;              /* 0x0057AF98  100.0 - [0x005C6E34] =
                                      * idle + this, FUN_0043E235's threshold */
    /* Strategy 2, FUN_0043AABF (s3.5).  Each has one
     * absolute reference in the image, the reader named. */
    float mode2_select_frac;         /* 0x005C6C4C  0.95 - out of neutral when
                                      * maxrevs x this < revs (0x0043AB0B)    */
    float mode2_fwd_speed;           /* 0x005C6BD4  1.0 - 1st -> reverse on the
                                      * brake below this czspd (0x0043AD68)   */
    float mode2_back_speed;          /* 0x005C6BD8  -1.0 - reverse -> select
                                      * on the brake from this czspd up
                                      * (0x0043ACF8)                          */

    int32_t passes;      /* 0x005C6E84  10 - the collision re-integration cap */

    /* FUN_0045BCE8 (object collision) and FUN_0043A3DE (the
     * binary angles). sections 9-12.  Each is data with
     * no writer; the code immediates beside them (0xE666, 0x28000, ...) are
     * transcribed in objects.c. */
    int32_t car_model_y_q;       /* 0x005C6CF8  0xB333 (0.7) - FUN_0042566A's
                                  * y offset for files\b models (9 reads)   */
    float   object_speed_min;    /* 0x0057AFD4  8.0  - C535's class 1/9 test */
    float   bearing_scale;       /* 0x0057B0B0  256.0 - the hit-bearing byte */
    float   inv_two_pi;          /* 0x0057B058  1/(2 pi)                     */
    float   angle_steps;         /* 0x0057B0AC  4096.0 - FUN_0043A3DE        */
    float   asin_limit;          /* 0x0057AFBC  1.0 - FUN_0041FF84's clamp,
                                  * negated in place and back (section 12.2) */
    float   pivot_one;           /* 0x0057AFBC  1.0 - the yaw model's fade
                                  * (FSUB) and FUN_0043C2BA's weight clamp
                                  *                    */

    /* the closing session: FUN_0045D16A, car versus car (section 3.1).  All data with no absolute writer. */
    float   car_box[3];          /* 0x006868E0/E4/E8  0.9, 0.8, 1.8 - D258's
                                  * half-extents, compared strictly          */
    float   car_corner[3][8];    /* 0x00686904 x, 0x00686924 y, 0x00686944 z:
                                  * the same box's eight corners             */
    float   car_exchange_lo;     /* 0x006868D0  0.4 (0x3ECCCCCD)             */
    float   car_exchange_hi;     /* 0x006868D4  0.6 (0x3F19999A); the pair
                                  * sums to 1.0000000298, not 1              */
    float   car_push_half;       /* 0x006868AC  0.5                          */
    float   car_push_scale;      /* 0x0057AFC4  3.0 - FUN_00420010's factor  */

    /* FUN_0043A2D3, the spiral search (s17).  Data: one read
     * each, no writer (ralimg.py refs / disp, 2026-09-30). */
    float   spiral_speed_scale;  /* 0x005C6D14  0.9 (0x3F666666)             */
    int32_t spiral_x[16];        /* 0x005C6D4C  probe offsets, x, before the
                                  * shift (ebp >> 4) + 1                     */
    int32_t spiral_z[16];        /* 0x005C6D8C  probe offsets, z             */

    /* the car's collision points (FUN_00425777).  Data
     * with no absolute or [reg + disp] writer (ralimg.py refs / disp: two
     * reads each, both in FUN_0046102B/FUN_00461047). */
    int32_t car_model[24];       /* 0x006CEF1C  carnum -> display model
                                  * number; collision model is this + 100    */

    /* FUN_004C66EA's stage 1 (s3).
     * Data: no store to any of them in 0x004C2AF6..0x004C773F, and every
     * ralimg.py refs hit lies inside the damage functions. */
    float   dmg_pct;             /* 0x008DCDB8  0.01 - damage % -> fraction  */
    float   dmg_w[6];            /* 0x008DD0C4..D8: 0.6 dam_engine, 0.2
                                  * dam_exhaust, 0.3 dam_brake*, 1.0 btmp*,
                                  * 0.4 bwet, 0.2 wetness                    */
    float   dmg_turbo;           /* 0x008DCFD0  0.6 - a blown turbo, 4WD only */
    float   dmg_full;            /* 0x008DCD7C  100.0                        */
    float   dmg_eff_lo;          /* 0x008DCD80  0.0 - the clamp, and the
                                  * ramps' (x1 - x0) zero test                */
    float   dmg_eff_hi;          /* 0x008DCD78  1.0                          */
    float   dmg_half;            /* 0x008DCD90  0.5 - the tyre-wear average  */
    float   dmg_ramp_ttmp[4];    /* 0x008DCF18  (0,95) -> (100,100)          */
    float   dmg_ramp_tyre[4];    /* 0x008DCF28  (0,100) -> (100,80)          */
    float   dmg_ramp_steer[4];   /* 0x008DCF38  (50,0) -> (100,0.3)          */
    float   dmg_roll;            /* 0x008DCD70  65536.0 - stage 2's chance to
                                  * a 16-bit threshold (the rolls' ramps read
                                  * dmg_page)                                 */
    /* FUN_0043F422, the octant damage. */
    float   oct_min;             /* 0x005C7994  4.0 - least impact counted   */
    float   oct_div;             /* 0x0057AFE4  64.0 - impact per full share */
    float   oct_lo;              /* 0x0057B014  0.0 - sign test and clamp    */
    float   oct_hi;              /* 0x0057AFBC  1.0 - the clamp's top        */
    /* FUN_0043EEDD, driver 2's damage reset. */
    float   dmg_ai_one;          /* 0x0057AFBC  1.0 - the five efficiencies   */
    float   dmg_ai_div;          /* 0x0057B0B0  set_s / set_h divided by ...  */
    float   dmg_ai_mul;          /* 0x0057AF98  ... then times this           */
    /* the three damage accumulators' numbers (damage_acc.c), raw:
     * the .data page from 0x008DCD74, read by address - ~120 rates, ramps,
     * the tyre-wear table 0x008DD16C and its column pairs 0x008DD2F0. */
    uint32_t dmg_page[SIM_DMG_PAGE_WORDS];
} sim_consts;

/* >>> [0x005C6BC0] IS NEGATED IN PLACE BY THE RUNNING PHYSICS. <<<
 *
 * sim_yaw_model_q's ZSPDDIF clamp does not use two constants.  It does
 * FLD/FCHS/FSTP on 0x005C6BC0, compares, and flips it back ~40 instructions
 * later.  The value is restored so the
 * behaviour is correct, but THE PHYSICS CONSTANT TABLE IS NOT READ-ONLY AT
 * RUNTIME: a port that puts these in .rodata faults, and anything reading them
 * concurrently sees the wrong sign for those 40 instructions.
 *
 * This port does not reproduce the negation - it clamps to +/- the positive
 * value, which is what the negation achieves - because a self-modifying
 * constant is an artefact of x87 having no negate-on-load, not behaviour.  It
 * is recorded here because compares against a process where it is real. */

int sim_consts_load(const char *image_path, sim_consts *out,
                    char *err, size_t errlen);

/* ------------------------------------------------------------------------
 * What the STAGE contributes.
 *
 * ~~Filling this struct is the caller's job~~ -: sim_surface
 * now lives in surface.h and sim_surface_solve_q (FUN_0043BA75) fills it.
 * ~~What the caller still supplies is the WHEEL-TO-GROUND QUERY's answer~~ -
 * the query is ground.h's and sim_car_step runs it
 * (FUN_0045B76A, immediately before the solver, as the original does).  What
 * the caller supplies now is what the query READS and cannot derive: the
 * level's geometry, the car's collision points (model-load data), the bump
 * map and image-backed query constants. sim_car_step derives susph/suspt from
 * the car record through FUN_0043D033 before running the query.
 * `state` is per car and persistent: it holds each point's last polygon,
 * which is the next step's hint. */

/* ------------------------------------------------------------------------
 * The level's placed OBJECTS, for FUN_0045BCE8 -.
 * is the reading; section 12 the specification.
 *
 * sim_objects is load-time data (FUN_004252A5's per-type tables and the three
 * scalars BCE8 reads); sim_object_state is the TICK-LEVEL state every car's
 * step shares: C535's hit list, rnd_next's generator, and the writable
 * instance records whose bytes C535 latches.  In the game those records are
 * the level's own pols array, so `pols` must be the same buffer the ground
 * geometry borrows (objects_selftest checks the alias); the port never writes
 * anything but a class-0xFF record's +0x09 and +0x11..+0x13 (C535, C65B) and
 * its offset +0x30..+0x38 (C65B's class 4, C8C5's reset). */
#define SIM_OBJECT_TYPES     256
#define SIM_OBJECT_HITS      256     /* [0x00685F44], 0x100 at 0x0045C556     */

typedef struct sim_object_type {
    int32_t model;      /* [0x00581FC4 + type*4]: OBJSINFO index = model
                         * number, -1 when none is registered (0x0042490F)   */
    int32_t cls;        /* OBJSINFO[model] + 0x1C (0x0045BF64).  For model -1
                         * the game reads 128 bytes below OBJSINFO, texinfo
                         * slot 254 +0x1C = TEXTINFO[LOCALDAT+0x100 #254]
                         * +0x18 (0x00437B7E) - the CALLER supplies it       */
    int32_t start;      /* [0x005E4E00 + type*4] into sim_objects.points     */
    int32_t count;      /* [0x005E5200 + type*4]                             */
    int32_t radius;     /* [0x005E7A30 + type*4]  max FISTP(sqrt(x^2+z^2))   */
    int32_t material;   /* OBJSINFO[model] + 0x24 (byte): 0x0045A8B8 picks
                         * the hit's impact sound by its low four bits
                         * (0 until the loader sets it)              */
} sim_object_type;

typedef struct sim_objects {
    sim_object_type type[SIM_OBJECT_TYPES];
    int32_t (*points)[3];        /* the object part of [0x0057C224], 16.16   */
    int32_t npoints, cap;
    int32_t gmode;               /* [0x005D6750]  2 = arcade                 */
    int32_t countdown_skip;      /* [0x006CEF7C]                             */
    int32_t route_variant;       /* [0x006CE08C]                             */
} sim_objects;

typedef struct sim_object_state {
    uint8_t *pols;               /* the level's pols, WRITABLE, stride 0x40  */
    int32_t  npols;
    int32_t  list[SIM_OBJECT_HITS];  /* [0x00685F44] pols indices            */
    int32_t  count;                  /* [0x00686344]                         */
    uint32_t rnd;                    /* [0x004DB13C] rnd_next's state        */
    /* [0x005EC6E8], the ground query's plane height, as the last query left
     * it.  C65B's class 4 reads it even when its query MISSES (0x0045C86E),
     * so on a miss +0x34 takes a stale height.  In the game every query in
     * the program shares the global; here only C65B's own hits update it,
     * and the caller may seed it.  It reaches +0x34 only, which collision
     * never reads. */
    int32_t  ground_height;
    /* Not the game's: counts of what the port does not model, so a caller
     * and a test can see that it happened. */
    long     feedback_requests;      /* FUN_0045A8B8 calls - sound/feedback  */
    long     solid_hits, recorded_hits, tests;
} sim_object_state;

typedef struct sim_ground {
    const sim_surface_table *table;          /* sim_surface_table_load        */
    const sim_surface_level *level;          /* sim_surface_level_build       */
    const sim_ground_geom   *geom;           /* sim_ground_geom_build,  */
    sim_ground_inputs        in;             /* ground.h                      */
    sim_ground_points       *state;          /* per car; sim_ground_points_init */
    /* FUN_0045BCE8's inputs.  Both NULL: the step skips object collision,
     * which is what it did before step 4b.  Either alone is refused. */
    const sim_objects       *objects;
    sim_object_state        *object_state;
    /* FUN_0044CD28's ramps (sim_ground_light_build).  NULL: the step leaves
     * car+0x240 / +0x244 alone, which is what it did before 2026-10-19. */
    const sim_ground_light  *light;
} sim_ground;

typedef struct sim_collision_snapshot {
    int32_t pos[3];
    float matrix[16];
    int32_t poly[SIM_GROUND_POINTS];
} sim_collision_snapshot;

/* ------------------------------------------------------------------------
 * The per-step scratch the original keeps in globals.
 *
 * Every field names the address it stands for. The setup, mass and force
 * producer fields are computed by sim_car_step; active_frame is refreshed at
 * the same points as FUN_0043D630 in the retail order. */
/* The damage model's PROCESS GLOBALS that its consumer FUN_004C66EA reads
 * (damage.c; s3).  One copy in the game, not one per car:
 * each car's step reads them as the LAST car's step left them, which is why
 * sim_roster.damage points at sim_tick's.  Written at the step's end by
 * FUN_004C2AE6 (0x00439E54), and the port writes only these five of the
 * many stores its three accumulators make (sim_damage_globals_from_car_q). */
typedef struct sim_damage_globals {
    uint32_t drivetype;  /* [0x008DCE14]  car+0x40's BITS: FUN_004C2AF6 copies
                          * it FLD/FSTP, so class 1 is a denormal, not 1.0f
                          * (s2's port hazard)             */
    float    wetness;    /* [0x008DCE20]  car+0x280, MOV at 0x004C3221      */
    float    tyref;      /* [0x008DCE70]  (dam_tyre1 + dam_tyre2) x 0.5     */
    float    tyrer;      /* [0x008DCE74]  (dam_tyre3 + dam_tyre4) x 0.5,
                          * both at the tail of sim_damage_accumulate_q     */
    uint32_t framernd;   /* [0x008DCED0]  car+0x00C, sim_damage_wear_q's MOV;
                          * the consumer's stage 2 (unported) rolls on it    */
} sim_damage_globals;

struct sim_race;   /* the race rules' state, below */

typedef struct sim_world {
    sim_force_rec force[SIM_FORCE_RECORDS];  /* 0x005C74A4, stride 0x20      */
    sim_force_rec collision_force[SIM_COLLISION_FORCE_RECORDS];
                                                /* 0x005C7784, separate array */

    /* [0x006CD630] - FUN_0043D630's target, and the same address as
     * car0+0x9CC.  ~~not a per-car field, a GLOBAL scratch~~ PER CAR:
     * the working block's +0x9CC, which the 0xFE0-byte copy-in/copy-out
     * carries to and from each car's own record, so
     * every car keeps its last active frame between steps.  FUN_0045D16A reads
     * ANOTHER car's through the roster (sim_roster.active_frame), which is
     * why a caller stepping several cars keeps one sim_world per car.  It
     * lies above the replayed prefix, so it is not a car_state member.
     * sim_refresh_active_frame fills it
     * with the adjugate-orthonormalise of car->matrix (FUN_0041FA3B), which for
     * an orthonormal input IS the inverse -.  sim_force_to_velocity_q and
     * sim_force_to_angular_q both read it to move a vector between world space
     * and the car's own frame. */
    float active_frame[16];

    float yaw_couple[2];   /* [0x005C7704], [0x005C7724].  sim_yaw_model_q
                            * writes, sim_tyre_forces_q reads and ZEROES.
                            * 7 image-wide references each, all inside those
                            * two functions - the hand-off is provable.       */
    float yaw_pivot_fade;  /* [0x005C749C] = clamp(1 - car+0x1F4 / 50, 0, 1),
                            * the yaw model's first act; FUN_0043C2BA is its
                            * only other reader (6 references image-wide).
                            * A collision zeroes the pivot shift and it fades
                            * back over 50 steps.  The retry block's C2BA
                            * reads the yaw model's value, not a fresh one.  */

    /* Collision diagnostics and the carry-over pass count used by the
     * body-contact solver before the next step resets the collision loop. */
    int32_t collision_passes;
    int32_t body_contact;
    int32_t body_contact_index;
    int32_t body_contact_clearance;
    float weight_clearance;  /* [0x685D94] as D8AB read it, metres */
    int32_t body_contact_16_16[3];
    float body_contact_vt;
    int32_t collision_body_contact;
    int32_t collision_body_contact_retry;
    int32_t collision_missed_point;
    int32_t collision_missed_point_retry;
    float collision_missed_point_normal_speed;
    int32_t collision_retry_flag;       /* [0x005C746C], C0B0/C100 loop flag */
    float collision_peak_normal_speed; /* [0x006CCE94], reset each step    */
    int32_t collision_active;           /* [0x006CCE98], reset each step    */
    int32_t collision_response_timer;   /* [0x006CCE58], B28F sets 0x32    */
    float collision_saved_cspd[3];      /* [0x005C6DD4], FUN_0043F037      */
    int32_t collision_exhausted;        /* [0x005C6F04], ++ when all passes
                                         * retried; no absolute reader      */
    int32_t collision_spiral_found;     /* [0x005C7470], ++ when the spiral
                                         * search places the car; its one
                                         * absolute reference is the ++     */
    int32_t collision_spiral_probe;     /* port diagnostic: the probe (EBP)
                                         * that placed it, the first number
                                         * of retail's "porky" line; -1 when
                                         * the search did not run or failed */
    uint8_t collision_spiral_why[144];  /* port diagnostic: per probe from
                                         * the first, which responses set
                                         * the retry flag - 1 B28F, 2 B30E,
                                         * 4 D16A, 8 BCE8                   */
    uint8_t collision_pass_why[16];     /* port diagnostic: per PASS of the
                                         * main collision loop, which
                                         * responses set the retry flag - the
                                         * spiral's bits, plus 16 for the
                                         * body-contact loop (CB48). Cleared
                                         * each step;      */
    int32_t collision_first_pos[3];     /* port diagnostic: the position the
                                         * FIRST pass tested, and its body
                                         * clearance and index (s79)  */
    /* The FIRST missed-point response (FUN_0045B796) of the step: the normal
     * it used and the car's velocity going in, so a capture can ask which way
     * the game pushed (trace_check, twocar_check; s6).
     * collision_b796_seen is 0 when none ran. */
    float   collision_b796_n[3];
    float   collision_b796_v[3];
    int32_t collision_b796_seen;
    int32_t collision_first_clearance, collision_first_index;
    float collision_first_vel[3];       /* ... and newspd at that point     */
    int32_t collision_pass_clear[16];   /* per pass: body clearance at entry */
    float collision_pass_vy[16];        /* per pass: newspd y at entry      */
    int32_t collision_spiral_car_hits;  /* port diagnostic: how many of
                                         * collision_car_hits the search's
                                         * probes made                      */
    int32_t collision_spiral_lost;      /* port diagnostic: all 144 probes
                                         * overlapped, where retail calls
                                         * FUN_0043D4B3 (ported: the
                                         * step runs it after the search)   */
    int32_t collision_car_hits;         /* port diagnostic, reset each step:
                                         * FUN_0045D306 runs (car-vs-car
                                         * inside corners).  Each calls
                                         * FUN_0045A98A, the sound request,
                                         * once - counted, not modelled     */
    float collision_car_in[2][3];       /* port diagnostic: the FIRST
                                         * response's inputs, the current and
                                         * the other car's newspd before the
                                         * exchange (twocar_check)          */
    int32_t collision_car_first_pass;   /* port diagnostic: its pass        */

    float torqu;           /* [0x005C7484]  - sim_drive_torque's output, an
                            * ACCELERATION in m/s^2, which is why step 4 adds
                            * it straight onto a speed.                       */
    float mass;            /* [0x005C6EA4]  - FUN_004351C9's output           */
    float set_cgh;         /* [0x005C7930]  - FUN_0043D033                    */
    float bbal;            /* [0x005C7924]  - FUN_0043D033                    */
    float susph, suspt;     /* [0x005C7928/2C] - D033, consumed by ground.c   */
    float suspd;            /* [0x005C7934] - D033, damping term              */
    float susp_factor;      /* [0x005C7920] - D033, negative-delta scale      */
    float ste_j, ste_k;     /* [0x005C793C/40] - D033 steering scalars       */
    float tracspd;         /* [0x005C6EA0] as sim_traction_limit leaves it    */
    float speed_cap;       /* [0x005C6E38]  |velocity| before the collision
                            * loop; after a retry the guard rescales ~~newspd.x~~
                            * the whole newspd, and cxspd..czspd, back down to
                            * it.  A no-retry step is not capped
                            **/
    int   shaungmode;      /* the pre-start hold's other half, step 13.
                            * [0x005D6750]: sim_tick_run sets it from
                            * sim_tick.gmode (the harness had
                            * held the one global at two values).            */
    float rev_q;           /* [0x005C7478], q = revs/maxrevs * 0.25.  Written
                            * only by the torque curve's tail (0x0043B174), so
                            * on a rev-limiter step sim_drive_torque reads the
                            * value the LAST curve call left - a process
                            * global, which is why sim_roster.rev_q points at
                            * sim_tick's.  Used here when there is no roster. */
    float crank;           /* [0x5C7474], sim_drive_torque's crank torque,
                            * which FUN_0043CF56 reads later in the step     */
    int   d4b3_due;        /* the game called FUN_0043D4B3, the car reset, this
                            * step: FUN_0043D47E's counter reached 700, or
                            * the rescue key's hold reached 100 (input.c).
                            * ~~unported~~: place.c's sim_car_reset_q
                            * runs it when the step has a race context;
                            * d4b3_ran counts those                         */
    /* The gearbox's two input flags, [0x005C65D8] (up) and [0x005C65DC]
     * (down): the input stage's newly-pressed bits 0 and 1 of dannyebx,
     * zeroed by FUN_00438208 (sim_input_stage_q, input.c).  Process globals the step
     * only reads; a caller that has no input stage leaves them 0. */
    int32_t gear_up_in, gear_down_in;
    /* [0x005C65E0], the strategy-2 pedal arbitration's sign (+1 the
     * throttle, -1 the brake), written by FUN_00438208 only while the gear
     * is 0 and otherwise kept; FUN_0043AABF reads it out of neutral
     * (0x0043AB23: -1 takes reverse).. */
    int32_t gear_sign_in;
    int   input_439024_due;  /* edge bit 0x20 on a human car: the input stage
                              * called FUN_00439024 (binding slot 9, "cycle
                              * camera views") - run on `camera` below
                              * when it is set, flagged either way            */
    int   input_rival_due;   /* gmode 2, driver 2: FUN_0043895C handed the
                              * staging struct to the rival controller
                              * (FUN_004601BA) - NOT ported, so the car keeps
                              * the device's words                   */
    sim_damage_globals damage; /* used when there is no roster, as rev_q    */
    int   damage_e2d3_due;   /* driver == 2: FUN_00439F90 called FUN_0043E2D3
                              * instead of FUN_004C66EA - unread, NOT ported,
                              * so the six efficiencies keep the record's   */
    float bumpf;           /* [0x005C7938]  `bumpf` - FUN_0043D033.            */

    /* The surface solver's whole output for this step.  sim_car_step
     * fills it; the yaw and tyre models read it. */
    sim_surface surf;

    /* Values the game NAMES on its debug overlay and that the tyre model
     * leaves behind for other consumers.  Kept so trace_check can compare the
     * ones the car record carries, and so a reader can find them by the name
     * the developers used. */
    float gripx, gripx1, gripx2, gripzf, gripzb;
    float usegripx, ugrpzf, ugrpzb, maxugrpx;
    float usecghx, usecghz;
    float tiltx, tiltz, tiltz2;
    float wgripzf, wgripzb;

    /* Two fields of the car record ABOVE the replayed prefix, so not
     * car_state members, that FUN_0043A3DE reads and writes.  PER CAR and
     * persistent: the angles freeze while all four wheels are airborne, so a
     * caller stepping several cars keeps one sim_world per car. */
    int32_t binary_angle[3];  /* car+0x9C0 heading, +0x9C4 pitch, +0x9C8 roll
                               * ([0x006CD624/628/62C]), 2^32 per turn        */
    int32_t camera_view_2_3;  /* car+0x6F0 - FUN_0043D644: p1 view is 2 or 3 */
    uint32_t parts_off;       /* carpartsleft bits sim_parts_left_q
                               * cleared, ORed in until the caller zeroes it:
                               * the flying pieces (0x0045FC0C) are
                               * not ported; rally logs them               */

    /* the car's camera (camera.h), or NULL.  Non-NULL runs the
     * camera's step half at 0x00439E83 (a car with an input device), sets
     * camera_view_2_3 from its view at the step's start (FUN_0043D644, the
     * first thing FUN_0043D66C does), and gives FUN_0043E00B its camera half.
     * NULL is the step as it was before: trace_check passes NULL. */
    struct sim_camera_ctx *camera;

    /* FUN_0043A16A, after the surface solver - the level's weather
     * words (weather.h), or NULL, which skips it.  It samples at the CAMERA,
     * so it runs only with `camera` too; without them car->wetns is whatever
     * the caller holds, as before.  `wx` is car+0x900..+0x908. */
    const sim_weather_level *weather;
    sim_weather_words        wx;

    /* the race rules' context (race.c), set by the tick before the
     * input stage and the step.  NULL: the race rules are not modelled and
     * the step is exactly what it was before - the fixtures that step
     * a car alone rely on that.  `race_slot` is [0x006CE084], the tick's own
     * slot counter, which FUN_0043526C and FUN_00444639 read. */
    struct sim_race *race;
    uint32_t race_slot;
    int      d4b3_ran;        /* how many of this step's FUN_0043D4B3 calls
                               * the port performed (d4b3_due says the game
                               * made one; with no race context it cannot) */
} sim_world;

/* ------------------------------------------------------------------------ */
/* The primitives, each read in full from the image this session.            */

/* FUN_0041E368's table: 5,120 entries of sin(i*pi/2048), stored as float32.
 *
 * THE ONE PLACE THIS PORT CANNOT PROMISE BIT-EXACTNESS AND SAYS SO.  The
 * original computes each entry with x87 FSIN on an 80-bit argument built as
 * (i * FLDPI) / 2048; this builds it with C `sin()` in double and rounds once.
 * The two agree to well inside a float32 ULP for every argument in range, but
 * "well inside" is an argument, not a measurement, and FSIN's own error is
 * specified only to ~1 ULP of the 80-bit result.  trace_check measures the
 * consequence directly instead:. */
void sim_sin_build(float *table_5120);

/* steer_curve[USTEERANGLE >> 20].  The shift is LOGICAL - a negative steering
 * angle indexes the top of the table, which is where the negative half of the
 * sine lives, and that is why it works. */
float sim_steer_sin(const float *table_5120, int32_t usteerangle);

/* FUN_0041FC71.  len = sqrt(x^2+y^2+z^2), ONE FSTP; if len == 0.0 exactly the
 * output is zeroed, otherwise each component is divided by the STORED float32
 * length.  Then each output that is exactly 0x80000000 - negative zero - is
 * rewritten as +0.  That last step is real and it is three CMP/MOV pairs. */
void sim_vec3_normalise(const float in[3], float out[3], const sim_consts *k);

/* FUN_0041FFFB.  sqrt(x^2 + (y^2 + z^2)) - the FADDP order adds the LAST TWO
 * squares first - left on the FPU for the caller's one FSTP.  Its callers
 * include [0x005C6E38] (the speed cap, 0x00439D48) and FUN_0043A22F. */
float sim_vec3_len_q(const float v[3]);

/* FUN_0041E9C6, the in-place normaliser B453 and BCE8 use - NOT
 * sim_vec3_normalise (FUN_0041FC71), which divides by the STORED length.
 * L = sqrt(x^2 + (y^2 + z^2)) stays on the FPU; FCOM against 0.0 and C3
 * (equal OR unordered) zeroes the vector, so a NaN length does too; otherwise
 * each component is divided by the unrounded L and stored, and -0.0 is
 * rewritten +0.0.  Returns L, which the original leaves at [0x005D6EA0]. */
float sim_normalise_q(float v[3]);

/* FUN_0043A22F: car+0x1C4 = sim_vec3_len_q(newspd).  Retail calls it at
 * 0x00439B46 (step start) and 0x00439E4F (step end).
 * section 11.2; the capture holds this identity in 36,754 of 36,754 frames. */
void sim_speed_len_q(car_state *car);

/* FUN_0043A3DE: re-derive the binary angles from the matrix unless the camera
 * is not in view 2/3 AND all four wheels are airborne (car+0x254).  Retail
 * calls it at 0x00439D3D and, in the retry block, 0x00439DB6.
 */
void sim_binary_angles_q(const car_state *car, const sim_consts *k,
                         sim_world *w);

/* FUN_0041FD06.  out = cross(b, c), each component one FSTP. */
void sim_vec3_cross(const float b[3], const float c[3], float out[3]);

/* FUN_0041FD92 - and IT DOES NOT BUILD A ROTATION MATRIX.
 *
 * The diagonal of a Rodrigues matrix is  x*x*(1-cos a) + cos a.  This function
 * computes  sqrt(|x|)*(1-cos a) + cos a  -  FLD [axis.x] / FABS / FSQRT at
 * 0x0041FE32, 0x0041FE77 and 0x0041FEBD.  A square would be FMUL ST(0) (which
 * is exactly what vec3_len two functions away uses); FSQRT is d9 fa and FABS
 * is d9 e1, so this is not a mis-decode.
 *
 * The result is therefore NOT orthogonal, which is precisely why the caller
 * runs it through sim_matrix_orthonormalise before composing it - a line
 * recorded as a fact with no reason attached.  THE
 * REASON IS THIS.  A port must copy the sqrt, not repair it: repairing it
 * changes the delta rotation every car gets every step. */
void sim_axis_angle_matrix_q(const float axis[3], float angle, float m[16]);

/* FUN_0041FC0D.  Gram-Schmidt from row 2 (the +0x20 row) outward:
 *   row2' = normalise(row2)
 *   row0' = normalise(cross(row1, row2'))
 *   row1' = cross(row2', row0')
 * then a 16-dword copy back.  The fourth column and the fourth row are copied
 * from scratch this function never writes; measured over the capture they are
 * 0.0 in every one of 36,754 frames, so this port zeroes them explicitly
 * rather than reproducing a stale-scratch read it cannot model. */
void sim_matrix_orthonormalise(float m[16], const sim_consts *k);

/* FUN_00425E1D.  out = a * b, row-major, with b's fourth row added as a
 * translation.  Each output element is ONE FSTP over  a0*b0 + (a1*b1 + a2*b2)
 * - and that grouping is the x87 stack's, not a choice: the three products are
 * pushed, then faddp st(1) twice, so the LAST TWO add first. */
void sim_matrix_mul(const float a[16], const float b[16], float out[16]);

/* ------------------------------------------------------------------------ */
/* Four more primitives, read: the force->velocity step needs a
 * scale, a point-by-matrix transform and a matrix inverse that none of the
 * above cover. */

/* FUN_00420010 - "vec3_scale_q" in, and it names its
 * own opcodes there: FLD/FMUL ST1/FSTP per component, so each of the three
 * outputs is its OWN rounding - not one rounding over the whole triple. */
void sim_vec3_scale(float v[3], float k);

/* FUN_0044B1AB - v' = v . M, row-vector convention: v is taken on the FPU
 * stack (ST0=x, ST1=y, ST2=z, the caller's job) and M is four rows of four
 * floats, basis in rows 0-2 and a translation in row 3 that is ADDED to the
 * result (already names
 * this as "rotate a vec3 in place by the matrix at [0x6cd630]", used by
 * FUN_0043D5DC).  87 call sites project-wide - rendering and physics share
 * it.  Each output component is ONE FSTP over a0*b0 + (a1*b1 + a2*b2), the
 * same grouping as sim_matrix_mul because it is the same three-multiply,
 * two-faddp x87 shape applied to one row instead of four. */
void sim_vec3_by_matrix(const float v[3], const float m[16], float out[3]);

/* FUN_0041FA3B - NOT a transpose, though for an orthonormal input it produces
 * the same numbers.  It builds the ADJUGATE of the 3x3 part by nine
 * cofactor computations (each one FSTP over a product-of-products
 * difference, the same shape as sim_vec3_cross) and writes it row-major with
 * NO division by the determinant - which only leaves the true inverse
 * because every matrix this port ever passes it has already been through
 * sim_matrix_orthonormalise and so has det 1.  It then calls
 * sim_matrix_orthonormalise on its own output before returning - a second,
 * independent piece of evidence for the "always orthonormal" assumption,
 * since the original author added a cleanup pass a pure transpose would not
 * need.  Row 3 and column 3 are zeroed, matching every other matrix this
 * file builds. */
void sim_matrix_inverse_orthonormal(const float m[16], float out[16],
                                    const sim_consts *k);

/* ------------------------------------------------------------------------ */
/* FUN_0043D630, called from three sites inside FUN_00439B37 (section 4's call-order table lists it twice, both times as "(read)" with
 * no arithmetic given - the arithmetic is sim_matrix_inverse_orthonormal,
 * above).  Refreshes sim_world's active_frame from car->matrix; call it
 * before sim_force_to_velocity_q / sim_force_to_angular_q, which both read
 * the result. */
void sim_refresh_active_frame(sim_world *w, const car_state *car,
                              const sim_consts *k);

/* FUN_0043DA08 -> FUN_0045D636, the 23-record loop. -
 * section 4's "0043da08 0043db58  unread" row, and the reason
 * physics_selftest's rollout did not move a car: this is what turns the 23
 * force records into newspd.  Per record: impulse = force * dt (car-local),
 * rotated into world space by car->matrix (FUN_0043D612, folded in rather
 * than exposed separately - its whole body is "rotate a vec3 by car->matrix"
 * and it has no other caller), then added onto newspd.
 *
 * The ORIGINAL also re-derives cxspd/cyspd_q/czspd from the running newspd
 * total after EVERY record (FUN_0045D636's tail, through active_frame).  This
 * port derives them ONCE after the loop instead - not a guess: the
 * transform reads the current newspd and nothing else, so the 23rd call's
 * result is the only one that survives regardless of how many times the
 * intermediate ones ran, and a port that recomputed 23 times would reach the
 * bit-identical answer at a 23x cost.  The angular counterpart below CANNOT
 * take this shortcut - see its own comment. */
void sim_force_to_velocity_q(car_state *car, const sim_consts *k, sim_world *w);

/* FUN_0043DB58 -> FUN_0045D6D7, the 23-record loop's angular twin.  Per
 * record: torque = cross(force, point) (FUN_0041FD06, already ported as
 * sim_vec3_cross); the CURRENT car->rotspd is read, transformed into the
 * car's own frame by active_frame, has -torque*dt*inv_inertia added
 * PER AXIS, and is transformed back into world space by car->matrix - and
 * then STORED, so the next record's iteration reads back what this one
 * wrote.
 *
 * THIS ONE MUST STAY A 23-ITERATION ROUND TRIP.  Unlike sim_force_to_velocity_q,
 * each pass here both reads and writes car->rotspd, so the world<->local
 * conversion is re-rounded twenty-three times in the original and a port
 * that accumulated the twenty-three torques in one frame and transformed
 * once would not reach the same bits - it would be a different, more
 * accurate computation, which is exactly the kind of "improvement"
 * exists to catch. */
void sim_force_to_angular_q(car_state *car, const sim_consts *k, sim_world *w);

/* FUN_0043DA19 / FUN_0043DB69: integrate the separate seven-record collision
 * extension.  The angular path is gated to collision_passes == 0. */
void sim_collision_force_to_velocity_q(car_state *car, const sim_consts *k,
                                       sim_world *w);
void sim_collision_force_to_angular_q(car_state *car, const sim_consts *k,
                                      sim_world *w);

/* FUN_0045C90A / FUN_0045CCD3.  Tests the lowest non-wheel point against the
 * road and, on contact, writes force record 21.  Returns 1 for contact, 0 for
 * no contact, and -1 for invalid level geometry. */
int sim_body_contact_q(car_state *car, const sim_consts *k,
                       const sim_ground *ground, sim_world *w);
int sim_body_contact_loop_q(car_state *car, const sim_consts *k,
                            const sim_ground *ground, sim_world *w,
                            const sim_collision_snapshot *snapshot);
/* FUN_0045B796's carry-based polygon-edge selection for the last missed point.
 * Returns 1 with edge vertex ids, 0 when no point missed, or -1 on invalid
 * geometry/state. */
int sim_missed_point_edge_q(const sim_ground_geom *geom,
                            const sim_ground_points *points,
                            int32_t *edge_start, int32_t *edge_end);
/* FUN_0045B28F/B796. Writes the zero-friction response to extension record 1;
 * returns 1 for a missed point, 0 for none, or -1 for invalid inputs. */
int sim_missed_point_response_q(car_state *car, const sim_consts *k,
                                const sim_ground *ground,
                                const float *sin_table, sim_world *w);
int sim_collision_snapshot_save(const car_state *car,
                                const sim_ground *ground,
                                sim_collision_snapshot *snapshot);
int sim_missed_point_collision_q(car_state *car, const sim_consts *k,
                                 const sim_ground *ground,
                                 const float *sin_table, sim_world *w,
                                 const sim_collision_snapshot *snapshot);
int sim_polygon_transition_collision_q(car_state *car, const sim_consts *k,
                                       const sim_ground *ground,
                                       const float *sin_table, sim_world *w,
                                       const sim_collision_snapshot *snapshot);
/* ---- object collision, (objects.c) ------------------------ */

/* Empty tables: every type unregistered (model -1, count 0, radius 0). */
void sim_objects_init(sim_objects *o);
void sim_objects_free(sim_objects *o);

/* FUN_004252A5's point bracket for one type:
 * `pts` is the .ro3's whole point array as raw 16.16 ints, in file order.
 * Appends each through FUN_0042566A (models 400..599 become
 * (z, y - car_model_y_q, -x)), sets start/count, and the radius as the
 * largest FISTP(sqrt(x^2 + z^2)) (FUN_0042568F).  `cls` is OBJSINFO[model]
 * +0x1C.  Refuses (-1) type >= 0x50, where the loader keeps only the points
 * the polygon mask names - BCE8 never tests one - and a point coordinate
 * outside +-2^28, where the exact-integer sqrt below stops being exact;
 * -2 on allocation failure. */
int  sim_objects_set_type(sim_objects *o, const sim_consts *k, int type,
                          int32_t model, int32_t cls,
                          const int32_t (*pts)[3], int32_t npts);

/* the car's own collision points.
 * FUN_004247CD registers, per car, type 0x5B + 0x14 * car with model FUN_0046102B(carnum) + 100 - the car's COLLISION model, a files\b
 * .ro3 - and CALLs FUN_00425777 on it.  This is that model number. */
int32_t sim_car_collision_model_q(const sim_consts *k, int32_t carnum);

/* The chain from that model's whole .ro3 point array (raw 16.16 ints, file
 * order, npts of them - MODIFIED IN PLACE, as the original modifies
 * [0x0057C224]): FUN_0042566A's turn, sim_collision_points_select (the
 * head), sim_collision_points_build (the tail).  -1 on any refusal. */
int  sim_car_collision_points(sim_collision_points *out, const sim_consts *k,
                              int32_t model, int32_t (*pts)[3], int32_t npts);
/* A type with no model (the 0x00424997 early return): model -1, no points,
 * radius 0, and the class the caller read out of texinfo slot 254. */
void sim_objects_set_unloaded(sim_objects *o, int type, int32_t texinfo_cls);

/* Zero list and counters; rnd is left as the caller seeded it. */
void sim_object_state_init(sim_object_state *s, uint8_t *pols, int32_t npols,
                           uint32_t rnd_seed);

/* rnd_next, 0x004018A0 -> rnd_lfsr_step(state, 0) at 0x004018B4. */
uint32_t sim_rnd_next_q(uint32_t *state);
/* FUN_0043F3E0/FUN_0043F400: the car's own word, car+0x0C, stepped once at
 * the top of every sim_car_step - rnd_next's step XOR 0x87654321. */
uint32_t sim_car_rnd_next_q(uint32_t *state);

/* FUN_0045BCE8: the nine-cell walk, every class-0xFF instance, the anchor /
 * radius / collision-point tests, and on a hit the response (object-collision
 * sections 2-5, 11.2).  A solid hit rolls the car back through `snapshot`
 * (FUN_0043C100, which increments the retry flag), refreshes the ground
 * (FUN_0043D66C), records the hit (C535) and writes extension record 0.
 * Returns the number of hits, or -1 on invalid inputs. */
int  sim_object_collision_q(car_state *car, const sim_consts *k,
                            const sim_ground *ground, const float *sin_table,
                            sim_world *w,
                            const sim_collision_snapshot *snapshot);

/* FUN_0045C65B, the first call of FUN_0045C51F: animate C535's hit list ONCE
 * PER TICK, after every car's step (simstep_q calls it at 0x0045E47F and
 * 0x0045E5D8; the port has no tick body, so the caller does).  Object-
 * Class 1/9 wobble, 2 tilt, 3/8 turn then fall,
 * 4 slide along the hit bearing - moving +0x30/+0x38, which the next step's
 * FUN_0045BCE8 tests - and type 0x3B's countdown; the rate is 1..17 sub-steps
 * by distance to car[0].
 *   car0_pos:  roster slot 0's position, [0x006C2244]/[0x006C224C] (x, z)
 *   car0_hint: roster slot 0 +0xE00 = car+0xDF0 entry 4, the polygon under
 *              car[0]'s origin point ([0x006C2F10] -> [0x005819C0])
 *   mask:      [0x005D31A0] as it was left - C65B does not set it.  The
 *              tick's last stepped car leaves SIM_GROUND_MASK_CONTACT (0x80)
 *              from its ground driver, OR 0x40 if it has an input device and
 *              its camera ground check (FUN_0043739C, from the step at
 *              0x00439E83) queried; unported, so the caller supplies it
 *              (item 4)
 * Returns 0, or -1 on invalid inputs, a list entry or anchor outside the
 * level, or a ground-query refusal. */
int  sim_object_tick_q(const sim_objects *o, sim_object_state *s,
                       const sim_ground_geom *geom, const float *sin_table,
                       const int32_t car0_pos[3], int32_t car0_hint,
                       uint32_t mask);

/* FUN_0045C8C5, from raceinit_q (0x004444B0) and 0x00443820: zero +0x30,
 * +0x34, +0x38 and +0x11..+0x13 of every class-0xFF record, then the list
 * count (0x0045C52A).  Nothing else in the state is touched. */
void sim_object_reset_q(sim_object_state *s);

/* FUN_0043F037: save cxspd/cyspd_q/czspd before the extension integrates. */
void sim_collision_save_cspd_q(const car_state *car, sim_world *w);
/* FUN_0043F058, first call of the retry block.  If the saved car-frame speed
 * is at least impact_speed_min and the current car-frame velocity keeps less
 * than impact_retain_ratio of it along the saved direction, zero gear,
 * fwhlspd and rwhlspd.  Returns 1 when it fired. */
int sim_collision_impact_q(car_state *car, const sim_consts *k,
                           const sim_world *w);
/* store_norms_q, 0x00420BF4 - collision.c has the reading.  Fills the two
 * triangle normals of every drawable polygon IN PLACE (`pols` must be
 * writable: the shipped file holds zeros there and the collision responses
 * read them).  Call once per level load, before anything queries `pols`.
 * Returns the number of triangles stored from a stale scratch (a corner
 * index outside `pnts`), or -1 on bad arguments. */
int  sim_level_store_norms_q(uint8_t *pols, int32_t npols, const uint8_t *pnts,
                             int32_t npnts);
void sim_airborne_flags_q(car_state *car);
int sim_collision_refresh_ground_q(car_state *car, const sim_ground *ground,
                                   sim_world *w);

/* ------------------------------------------------------------------------ */
/* The step's pieces.                                                       */

/* FUN_0043B18B.  pos.{x,y,z} += (int32)(vel * 65536.0f * dt), and the FISTP
 * rounds to NEAREST under the prevailing control word, which the boot leaves
 * at round-to-nearest.  A port that writes (int) in C gets truncation and a
 * different answer - measured against the capture at 99.89% agreement for
 * round-to-nearest against 50.50% for truncation on the vertical axis. */
void sim_integrate_position(car_state *car, const sim_consts *k);

/* FUN_0043C22F.  |w|, then if it is not exactly zero: normalise it, build the
 * axis-angle matrix for |w|*dt, orthonormalise THAT, compose it onto the car
 * matrix, and orthonormalise the result.  The final orthonormalise runs on
 * both arms, including when |w| is zero. */
void sim_integrate_orientation(car_state *car, const sim_consts *k);

/* FUN_0043C2BA, after the orientation and before FUN_0043D630 (0x00439D33,
 * and 0x00439DAC in the retry block).  The car yaws about its axles, not its
 * origin: the origin is moved in x/z by (rotspd x lever) about the REAR
 * point (car+0xCFC's z) weighted by the steering and handbrake yaw terms'
 * share, and about the FRONT point (car+0xCF0) by the power and spin terms'
 * share, times sim_world.yaw_pivot_fade.  Two FISTPs per axis, y untouched.
 * Skipped when all four wheels are airborne (car+0x254) or the four terms
 * sum to zero. */
void sim_yaw_pivot_shift_q(car_state *car, const car_geometry *geom,
                           const sim_consts *k, const sim_world *w);

/* 0x0043B254..0x0043B2BE, the yaw model's first block: [0x005C749C] =
 * clamp(1 - timer / 50, 0, 1) over the x87, from car+0x1F4 as the yaw model
 * sees it (after FUN_0043B990's decrement).  sim_yaw_model_q stores it in
 * sim_world.yaw_pivot_fade; exported for trace_check. */
float sim_yaw_pivot_fade_q(int32_t timer, const sim_consts *k);

/* FUN_0043DC70.  The NaN/Inf test is on the INTEGER exponent field, not a
 * floating compare, so it is exact and cheap; a port must keep it, because
 * without it one bad frame is permanent.  Returns non-zero if it fired.
 * Retail runs it only after a collision retry or on exhaustion
 * (0x00439DD8, 0x00439DF8) and in the spiral search (0x0043A3D8); a NaN
 * born on a no-retry step is not caught there. */
int sim_divergence_guard(car_state *car, const sim_consts *k, float speed_cap);

/* FUN_0043B254.  Four yaw terms into
 * WANTYROTSPD, a 0.1-per-step rate limiter onto YROTSPD, and the change
 * emitted as a pure couple into force records 19 and 20 using car_geometry's
 * two lever arms. */
void sim_yaw_model_q(car_state *car, const car_params *p, const sim_consts *k,
                     const sim_surface *surf, const car_geometry *geom,
                     const float *sin_table, sim_world *w);

/* FUN_0043C48C, fourteen numbered steps.  The
 * largest single block in the update, and the first thing in the whole port
 * that turns the two axles at different speeds - which is what makes
 *'s second blind spot reachable at all. */
void sim_tyre_forces_q(car_state *car, const car_params *p,
                       const sim_consts *k, const dt_consts *dk,
                       const dt_gears *gears, const sim_surface *surf,
                       sim_world *w);

/* 0x00439CAF..0x00439D0B, right after the tyre model: the
 * airborne wheels' spin-down (FUN_0043A471), the free-revving engine in
 * neutral or in the air (FUN_0043CF2A/CF39 -> FUN_0043CF56), the idle floor
 * and the 1.2 x maxrevs cap, and the wheels re-derived from the revs
 * (FUN_0043A626).  `rev_q` is [0x005C7478], which FUN_0043CF56's curve call
 * writes.  0 on success, -1 on a NULL argument. */
int sim_engine_after_tyres_q(car_state *car, const car_params *p,
                             const sim_consts *k, const dt_consts *dk,
                             const dt_gears *gears, sim_world *w, float *rev_q);

/* FUN_0043A65E, the gearbox -, in the order of the
 * bytes: the race-finish and pre-start holds, the arcade rival's shift, the
 * direct gear selection (FUN_0043A6E5, when (dannyebx & 0xF00) != 0xE00 -
 * the one arm that skips the tail), the strategy switch on autom_1, and the
 * tail: revs x 0.95 when shift_result == 1, then the wheels re-derived from
 * the revs.  Strategies 0 (the stick) and 1 (the automatic) are
 * FUN_0043A7AB; strategy 2, the fully automatic, is FUN_0043AABF.
 * Reads w->gear_up_in/down_in, w->gear_sign_in and w->shaungmode. */
void sim_gearbox_update_q(car_state *car, const car_params *p,
                          const sim_consts *k, const dt_consts *dk,
                          const dt_gears *gears, sim_world *w);

/* FUN_0043E235, called near the step's end (0x00439E68): a car that has
 * collided this step (car+0x234) in gear with revs below idle + 100 drops
 * to neutral, with the torque cut and the cooldown. */
void sim_gearbox_stall_q(car_state *car, const sim_consts *k,
                         const dt_consts *dk);

/* THE TICK'S INPUT STAGE (input.c; s13): the
 * five calls FUN_0045E525 makes on the working car before the step -
 * FUN_0043895C, FUN_00438208, FUN_004383C3, FUN_00438616, FUN_004386E5.
 *
 * sim_input_globals is the process state they share: the staging struct at
 * 0x005C65C8 and ste k / ste j, which FUN_0043D033 writes inside the step of
 * the car that stepped LAST.  One per process, carried across ticks. */
typedef struct sim_input_globals {
    int32_t inputst;     /* [0x005C65C8]  dannyecx: the steer request       */
    int32_t pedal_a;     /* [0x005C65CC]  dannyeax: -> +0x638, throttle     */
    int32_t pedal_b;     /* [0x005C65D0]  dannyedx: -> +0x63C, brake        */
    int32_t hbrake;      /* [0x005C65D4]  (dannyebx & 4) * 0x40, or 0x1F4   */
    int32_t gear_up;     /* [0x005C65D8]  edge bit 0                        */
    int32_t gear_down;   /* [0x005C65DC]  edge bit 1                        */
    int32_t sign_q;      /* [0x005C65E0]  the autom 2 arbitration's winner;
                          * read by FUN_0043AABF (0x0043AB23), as
                          * w->gear_sign_in                         */
    float   ste_k;       /* [0x005C7940]  the ramp's rate scale             */
    float   ste_j;       /* [0x005C793C]  the direct path's scale           */
    int     seeded;      /* ste k / ste j hold a step's values              */
} sim_input_globals;

/* Run the stage on `car`.  `words` are FUN_00438880's four device words,
 * dannyeax..dannyedx, as the tick hands them over (a live keyboard, or a
 * capture's record).  `human` is the slot test [0x006CE084] <
 * [0x005F0A54].  Writes car+0x464..0x474, +0x644/+0x648, +0x208, +0x64C,
 * +0x650, +0x638/+0x63C/+0x640, +0x264, +0x088, +0x084 and the gear on a
 * reset; w->gear_up_in/down_in for the step, and w->d4b3_due,
 * w->input_439024_due (run on w->camera when set),
 * w->input_rival_due for what is not ported. */
void sim_input_stage_q(car_state *car, const uint32_t words[4], int human,
                       int32_t gmode, sim_input_globals *g, sim_world *w);

/* Seed ste k / ste j from `car` as its own step would leave them - what a
 * caller starting from a record does (the record does not hold them). */
void sim_input_ste_q(const car_state *car, const sim_consts *k,
                     sim_input_globals *g);

/* FUN_004C66EA, the damage model's consumer, which the step reaches at
 * +0x014 through FUN_0043D033 -> FUN_00439F90 -> 0x004C2AE0 when the car's
 * driver is not 2 (ss1, 3, 6; s30).  So it
 * runs BEFORE this step's accumulators, on the damage the previous step left.
 * Stage 1: the six efficiencies +0x3F4..+0x408 (three clamped to [0,1]).
 * Stage 3: gear0 (+0x2D4) = 0 and gearbits (+0x434).  Stage 4: the terminal
 * flag +0x29C, and engpower = 0 when dam_elect >= 100.  Stage 2:
 * the 18 rolls on framernd (sim_damage_globals.framernd, the last
 * sim_damage_wear_q's copy of a car's rnd): handbrake (+0x428), elecoff,
 * the six lights, stikgear and dropgear. */
void sim_damage_efficiency_q(car_state *car, const sim_consts *k,
                             const sim_damage_globals *g);

/* FUN_004C2AE6 (0x00439E54): the three damage accumulators (damage_acc.c,
 * - the six temperatures, the 44 damage slots, the actuators
 * +0x438..+0x45C and +0x460 from this step's car, and the globals the next
 * step's consumer reads.  `surf_class4` and `surf_958` are car+0x930 and
 * car+0x958 (sim_surface cls_a[4] and f2c[4]), above the record. */
void sim_damage_update_q(car_state *car, int32_t surf_class4, float surf_958,
                         const sim_consts *k, sim_damage_globals *g);

/* The four process-global stores of FUN_004C2AE6 that
 * sim_damage_efficiency_q reads, made from the car as it stands.  How a
 * harness resumes from a capture: applied to the previous record, it gives
 * what the game's globals held at this step's start (for one car). */
void sim_damage_globals_from_car_q(const car_state *car, const sim_consts *k,
                                   sim_damage_globals *g);

/* FUN_0043F422 (0x00439E9C, after the ground light): the step's impact
 * dents one octant of the body.  When the step's peak normal speed
 * car+0x230 reaches 4.0, the octant is the signs of the contact point
 * car+0x224..0x22C (x<0 -> 4, y<0 -> 2, z<0 -> 1) and
 * caroctdamage[octant] += (1 - caroctdamage[octant]) * clamp(peak / 64, 0, 1).
 * The engine's crush pass draws the car from these (objscene_car_windows). */
void sim_car_octant_damage_q(car_state *car, const sim_consts *k);

/* FUN_0043E2D3 -> FUN_0043EEDD: what driver 2's step runs in place of the
 * consumer - the damage, temperatures and dents zeroed, the efficiencies 1,
 * the actuators from the setup (damage.c). */
void sim_damage_reset_ai_q(car_state *car, const sim_consts *k);

/* FUN_00439B37's read subset, in's order, including
 * the ten-pass collision re-integration loop's structure.  Since it
 * runs the surface solver itself from `ground` and leaves the result in
 * w->surf; it returns sim_surface_solve_q's status (0, or -1 if a contact
 * point names a polygon or class the level does not have - in which case the
 * step is NOT run and the car is untouched). `roster` is FUN_0045D16A's
 * tick-level input (sim_car_overlap_q below): it runs once
 * per pass in its retail slot, between CB48 and BCE8.  NULL, or a roster in
 * which no other slot is driven, is the capture's case (numcars == 1 on every
 * frame) and changes nothing.  A roster sim_car_overlap_q refuses is refused
 * HERE, before the step touches the car (-1).  ~~`pairwise_penetrating`~~
 * (the callback stand-in, replaced in the closing session).
 * FUN_0045BCE8 (objects, extension record 0) runs in its
 * slot when `ground` carries objects (s13).  Its per-tick animator FUN_0045C65B is sim_object_tick_q, which the
 * CALLER runs once per tick after every car's step (object-collision s14).
 *
 * THE ROSTER, AND WHO FILLS IT.  gmode, numcars, the current slot and the
 * other cars' records are the TICK DRIVER's, and
 * the multi-car tick - roster order, copy-in/copy-out, sim_object_tick_q
 * after the loop - is the, not this function's.  The other cars are
 * written through their pointers: newspd, cxspd/cyspd_q/czspd and
 * collision_active.  Their active frames are only
 * read. */
#define SIM_ROSTER_SLOTS 8

typedef struct sim_roster {
    int32_t   gmode;                     /* [0x005D6750]; D16A runs in 0, 2  */
    uint32_t  numcars;                   /* [0x005F0A50], the tick's bound   */
    uint32_t  slot;                      /* [0x006CE084], the current car    */
    car_state *car[SIM_ROSTER_SLOTS];    /* [0x006CE044 + slot*4]            */
    const float *active_frame[SIM_ROSTER_SLOTS];
                                         /* each car's own +0x9CC:
                                          * its sim_world's active_frame     */
    float     *rev_q;                    /* [0x005C7478], the tick's; NULL
                                          * uses the car's sim_world.rev_q   */
    sim_damage_globals *damage;          /* the tick's; NULL uses the car's
                                          * sim_world.damage                 */
} sim_roster;

int  sim_car_step(car_state *car, const car_params *p, const sim_consts *k,
                  const dt_consts *dk, const sim_ground *ground,
                  const car_geometry *geom, const float *sin_table,
                  sim_world *w, const sim_roster *roster);

/* The OUTER TICK, FUN_0045E525's slot loop (tick.c has the reading): every
 * slot below numcars in order - copy the slot's record into a working car,
 * step it if its driver is nonzero, ++alivet and ++alivet_r whether or not,
 * copy it back - and then sim_object_tick_q ONCE.  One sim_world per car
 * (its active_frame is what the roster publishes), one sim_ground per car
 * (its `state` is that car's polygon hints; the geometry may be shared).
 * `object_mask` is [0x005D31A0] as the last step left it (see
 * sim_object_tick_q); objects/object_state NULL skips the object tick.
 * Returns 0, -1 on bad arguments, or the first step's error. */
typedef struct sim_tick_car {
    car_state          *rec;       /* the array slot [0x006CE044 + slot*4]   */
    const car_params   *params;
    const car_geometry *geom;
    const sim_ground   *ground;
    sim_world          *world;
    /* The input device's four words for this tick ([0x005C65FC..0x5C6608],
     * dannyeax..dannyedx).  Non-NULL runs sim_input_stage_q on the working
     * car before the step (the slot's has_device is its `human`); NULL
     * leaves the input stage unmodelled - the car's own +0x638..+0x650 go
     * into the step as they are. */
    const uint32_t     *input_words;
} sim_tick_car;

typedef struct sim_tick {
    const sim_consts *k;
    const dt_consts  *dk;
    const float      *sin_table;
    int32_t           gmode;       /* [0x005D6750]                           */
    uint32_t          numcars;     /* [0x005F0A50], 1..SIM_ROSTER_SLOTS      */
    sim_tick_car      car[SIM_ROSTER_SLOTS];
    const sim_objects *objects;
    sim_object_state  *object_state;
    /* [0x005D31A0], the ground-query mask FUN_0045C65B inherits.  sim_tick_run
     * sets it after the loop from the LAST DRIVEN car (s14 item 4); with no driven car it is left as the caller had it. */
    uint32_t           object_mask;
    /* The mask's inputs, per slot: does the car have an input device
     * ([0x005F4540 + slot*4] != -1), and the camera fields the step's
     * FUN_0043739C reads - car+0x684 (view) and car+0x694 (a flag), both
     * above the 1,668-byte prefix, so the caller supplies them. */
    uint8_t            has_device[SIM_ROSTER_SLOTS];
    int32_t            camera_view[SIM_ROSTER_SLOTS];
    int32_t            camera_flag[SIM_ROSTER_SLOTS];
    /* [0x005C7478], q as the last torque-curve call left it, whichever car
     * made it.  A caller that resumes from a capture seeds it. */
    float              rev_q;
    /* The damage model's process globals as the last stepped car left them.
     * A caller that resumes from a capture seeds them from the previous
     * record of the last car stepped (sim_damage_globals_from_car_q). */
    sim_damage_globals damage;
    /* The input stage's process state.  ste k / ste j are refreshed
     * from each driven car's step; a caller that starts from a record leaves
     * input.seeded 0 and the first driven slot seeds them from its car. */
    sim_input_globals  input;
    /* Non-zero runs the PRE-RACE SETTLE step FUN_0045E3FC instead of the
     * tick: the device words are zero,
     * only the input stage's first two calls run (sim_input_settle_q), and
     * alivet / alivet_r are NOT incremented.  The object tick runs after the
     * loop as in the tick. */
    int                settle;
    /* the race rules (race.c), handed to every car's input stage and
     * step through its sim_world.  NULL leaves them unmodelled. */
    struct sim_race   *race;
} sim_tick;

int  sim_tick_run(sim_tick *t);

/* 0x40 when the car's camera ground check queried (FUN_0043739C reached
 * FUN_004375CA: car+0xC8 < 0 - or unordered - and car+0x694 == 0 and the view
 * car+0x684 is 0 or 1, for a car with an input device), else 0x80 (the last
 * writer is FUN_0045AE5D's per-point query). s14. */
uint32_t sim_object_tick_mask_q(const car_state *car, int has_device,
                                int32_t camera_view, int32_t camera_flag);

/* FUN_0045D16A, car versus car - sections 2-4, in the order
 * of the bytes.  Exposed so fixtures can call it alone.
 *
 * In gmode 0 or 2, every roster slot below numcars (a do-while: slot 0 is
 * visited even when numcars is 0) other than the current one whose driver is
 * nonzero is tested.  M = cur.matrix x T(raw 16.16 difference) x
 * other.active_frame is built ONCE per other car; each of the current car's
 * eight corners is taken through it and tested strictly against the box,
 * an unordered (NaN) coordinate counting as inside.  Each inside corner
 * rolls the current car back (C100, which increments the retry flag), then
 * zeroes extension record 6's force, marks both cars active (no timer),
 * exchanges newspd 0.4/0.6, pushes the pair apart by 1.5 x their 3D
 * separation, and re-derives both cars' car-frame velocity.  The remaining
 * corners of that car are tested against the now STALE M.
 *
 * Returns 0, or -1 (nothing touched) when the roster names more than
 * SIM_ROSTER_SLOTS cars or a slot it would visit has no car or frame: the
 * original indexes 0x006CE044 without an AND 7 and would read past it. */
/* FUN_0043A2D3, the spiral search for a free position: sim_car_step calls
 * it when every pass retried, after the guard (tyre.c has the reading).
 * Exported for twocar_check, which runs it alone from a captured car.
 * Sets w->collision_spiral_probe to the placing probe and counts
 * w->collision_spiral_found, or counts w->collision_spiral_lost where
 * retail calls FUN_0043D4B3 (sim_car_step then runs sim_car_reset_q,
 * 0, or < 0 on a refusal. */
int  sim_spiral_search_q(car_state *car, const sim_consts *k, sim_ground *g,
                         const float *sin_table, sim_world *w,
                         const sim_collision_snapshot *snapshot,
                         const sim_roster *roster);

/* The spiral search's signature in a capture, for the check tools: the
 * probe that would move `before` to `after` - exactly one probe's offset in
 * x and z from (before's tick & 0xF) + 0x70, y unchanged - or -1.
 * Diagnostic, not part of the step. */
int  sim_spiral_probe_of_move_q(const car_state *before, const car_state *after,
                                const sim_consts *k);

int  sim_car_overlap_q(car_state *car, const sim_consts *k,
                       sim_ground_points *points, sim_world *w,
                       const sim_collision_snapshot *snapshot,
                       const sim_roster *roster);

/* Read producer slices of FUN_00439B37, exposed so fixed-input tests can
 * check each one independently of the capture's missing model points. */
void sim_setup_values(const car_state *car, const car_params *p,
                      const sim_consts *k, sim_world *w);
/* Its first two stores alone, ste k and ste j from ste s 1 (0x0043D038). */
void sim_setup_steer_q(const car_state *car, const sim_consts *k,
                       sim_world *w);
void sim_derive_mass(const car_state *car, const sim_consts *k, sim_world *w);
/* car+0x910..0x91C (FUN_0043D198, 0x0043D2E6): the four wheels' centre
 * heights, 16.16, for the draw (game/wheels.c).  y6 = the model's point 6
 * height. */
void sim_wheel_heights(const car_state *car, const sim_consts *k, float y6,
                       int32_t out[4]);
/* FUN_0043D8AB reads w->body_contact_clearance, [0x685D94]'s source: run
 * FUN_0043D66C (sim_collision_refresh_ground_q) first, as the step does. */
void sim_apply_weight(const sim_consts *k, sim_world *w);
void sim_apply_downforce(const car_state *car, const car_params *p,
                         const sim_consts *k, sim_world *w);
/* FUN_0043D79E, called at 0x00439D10 just before D198: force record 5 is a
 * quadratic drag against the car-frame velocity cxspd/cyspd_q/czspd,
 * -sign(v) * v*v * 5 / 10000 per axis.  v == 0 stores -0.0. */
void sim_apply_body_drag_q(const car_state *car, const sim_consts *k,
                           sim_world *w);
void sim_suspension_update(car_state *car, const sim_consts *k,
                           const sim_ground *ground, sim_world *w);

/* ------------------------------------------------------------------------
 * PUTTING A CAR ON THE START LINE - place.c.
 *
 * The route the start is taken from: the class-0xFF markers of one texture,
 * ordered by their +0x0C links (FUN_00443A33), each segment's heading
 * (FUN_00443B6B) and section ordinal (FUN_00443C37).  The same arrays the
 * checkpoints, the racing line and the arcade rivals read
 * (sections 1-2a). */
typedef struct sim_route {
    int32_t  n;          /* [0x005D650C]  the ordered count; 0 = no route     */
    int32_t  tex;        /* [0x005D6520]  0x3E, 0x3D or 0 (FUN_004438D6)      */
    int32_t *pol;        /* [0x00983B38]  pols indices, n of them             */
    int32_t *angle;      /* 0x0098DC04 + 4i: angle[i], the heading of the
                          * segment ENDING at i, 2^32 per turn; angle[-1] and
                          * angle[n] are valid too (0x0098DC00, the end clamp) */
    int32_t *ord;        /* [0x00988BA0]  the section ordinal                 */
    int32_t *angle_mem;  /* the allocation behind `angle`                     */
} sim_route;

/* `variant` is [0x006CE08C], the course direction (sim_objects.route_variant):
 * 0 selects texture 62, 1 texture 61, anything else texture 0.  `pnts` is the
 * level's point array, read through each marker's +0x04.  Returns 0; -1 if a
 * marker names a point outside the level (the original reads on - the
 * refusal is the port's); -2 out of memory. */
int  sim_route_build(sim_route *r, const uint8_t *pols, int32_t npols,
                     const uint8_t *pnts, int32_t npnts, int32_t variant);
void sim_route_free(sim_route *r);

/* The race's mode, as the start reads it.  The defaults (all zero but numpl
 * 1) are a one-player rally: run 18's. */
typedef struct sim_start_mode {
    int32_t gmode;           /* [0x005D6750]  0 rally, 2 arcade, 4 ...        */
    int32_t numpl;           /* [0x005F0A54]  local players                   */
    int32_t network;         /* [0x00524038]  a network game                  */
    int32_t no_route_q;      /* [0x005FE9D8]  set at 0x00441477; skips the
                              * route (and FUN_0043FAC8's nearest search).
                              * What sets it in play is not read             */
    int32_t countdown_skip;  /* [0x006CEF7C]  resettimers starts the clocks
                              * at 0x320 instead of 0                         */
} sim_start_mode;

/* FUN_00444639's placement for the car in `slot`, then FUN_0043E00B (the
 * leveller FUN_0043E0F6 and the rest), then its three clears - everything
 * initcarpositions_q does to one car that lands in the 1,668-byte record.
 * `car` is the record as the car's load left it (the caller's: see
 * sim_start_record_q); `w` receives the binary angles (+0x9C0..+0x9C8) and
 * the active frame; `ground` is the car's (its state's hints are the
 * leveller's queries', as in the game); `dmg` is FUN_0043D033's damage
 * input.  `slot_angle_out`, if non-NULL, gets [0x00581AA8], the route
 * heading the car was placed on.  Returns 0; -1 if the route is too short
 * to hold a start (the original reads before the array) or a ground query
 * refuses. */
int  sim_place_car_q(car_state *car, uint32_t slot, const sim_start_mode *m,
                     const sim_route *route, const sim_ground_geom *geom,
                     const car_params *p, const sim_consts *k,
                     const float *sin_table, const sim_ground *ground,
                     const sim_damage_globals *dmg, sim_world *w,
                     int32_t *slot_angle_out);

/* resettimers_q (FUN_00442FB0) and FUN_00442FEA for one car: alivet and
 * alivet_r = countdown_skip ? 0x320 : 0, +0x018 = +0x01C = 0, and
 * +0x00C = +0x010 = `seed` - the game's is rnd_next() on [0x004DB13C],
 * clock-derived, so the caller supplies it (0 becomes 1: 0x0044300D). */
void sim_reset_timers_q(car_state *car, const sim_start_mode *m, uint32_t seed);

/* The settle's input stage (FUN_0045E3FC): only FUN_0043895C and
 * FUN_00438208 of the tick's five, on zero device words. */
void sim_input_settle_q(car_state *car, int human, int32_t gmode,
                        sim_input_globals *g, sim_world *w);

/* raceinit_q's `singlecarinit`: fifty FUN_0045E3FC steps before the first
 * tick (0x00444569..0x0044459E, CMP ECX,0x32). */
#define SIM_SETTLE_STEPS 50

/* ---- place.c's route helpers, exposed for the race rules -------- */

/* FUN_0043F87C over [lo, hi): the nearest route marker to (x, z), as a
 * do-while with an UNSIGNED index test (JB), an index outside [0, n) costing
 * 0xFFFFFFFF (FUN_0043F8A1), and a tie going to the later index.  `*dist`
 * gets the winning distance (EDI, 16.16), `*at` the index (EBP, which starts
 * at 0).  FUN_0043F7D3 is this over [0, n). */
void sim_route_nearest_q(const sim_route *r, const sim_ground_geom *geom,
                         int32_t x, int32_t z, int32_t lo, int32_t hi,
                         int32_t *at, uint32_t *dist);

/* FUN_0041EA8B -> FUN_0041EC71: (x, z) rotated by the binary angle, each
 * coordinate FILD / FSTP float, one FSTP per output, FISTP back. */
void sim_table_rotate_q(const float *sin_table, uint32_t angle,
                        int32_t *x, int32_t *z);

/* The four registers FUN_0043D4B3 / FUN_0043DFC4 / FUN_00444639 need, which
 * the step and the tick both hold: the car's params and ground, the sine
 * table, the damage globals FUN_0043D033's consumer reads. */
typedef struct sim_place_ctx {
    const car_params         *p;
    const sim_consts         *k;
    const float              *sin_table;
    const sim_ground         *ground;
    const sim_damage_globals *dmg;
} sim_place_ctx;

/* ------------------------------------------------------------------------
 * THE RACE RULES - race.c.
 *
 * What the rules need that is not in the 1,668-byte car record: the level's
 * checkpoint list (derived from the route at load, initcheckpoints_q), the
 * mode, the split array, and - per car - the message slots and the live
 * position, which sit ABOVE the replayed prefix. */
#define SIM_RACE_CHECKPOINTS 513   /* 0x005B5390 .. 0x005B5B94: the array ends
                                    * where numcheckposits begins            */
#define SIM_RACE_SPLITS      64    /* 0x005B5FC8: gamemode_q clears 0x40     */
#define SIM_GHOSTS           90    /* FUN_0043560F: CMP ECX,0x5A             */

/* one of gmode 1's rivals as FUN_0043560F reads its 0x50-byte
 * record at [0x005D6770]: +0x00 the name id, +0x08 the car type, +0x0C the
 * stage total in centiseconds. */
typedef struct sim_ghost_rec {
    int32_t name, type, total;
} sim_ghost_rec;
#define SIM_RACE_ALLOWANCE_N 256   /* 0x005B5794..0x005B5B93, up to
                                    * numcheckposits                        */

/* car+0x71C..+0x730: msgtopre, three words FUN_00439EB6 clears, msgtimer and
 * msgflash.  The text of each id is the.
 * ~~What counts msgtimer down is not read.~~: FUN_0043F3BA in the
 * step (sim_message_timer_q); the FRAME picks a new id up (FUN_00459163,
 * port/game/hud.c) -. */
typedef struct sim_race_msg {
    int32_t topre;     /* +0x71C  [0x006CD380]  the message id; the frame
                        * clears it when it renders the text               */
    int32_t w720;      /* +0x720  [0x006CD384]  the frame's upload pending  */
    int32_t w724;      /* +0x724  [0x006CD388]  the id being shown          */
    int32_t w728;      /* +0x728  [0x006CD38C]  showing: the frame sets it,
                        * the step clears it when msgtimer runs out        */
    int32_t timer;     /* +0x72C  [0x006CD390]  steps to show it; -1 = until
                        * replaced                                          */
    int32_t flash;     /* +0x730  [0x006CD394]  flash rate                  */
} sim_race_msg;

/* ------------------------------------------------------------------------
 * THE CO-DRIVER'S PACENOTES - pacenote.c.
 *
 * init_pacenotes_q (0x0043FED0) turns the route's note codes into sentences
 * at level load; FUN_0043FC56 in the step speaks each one as the car passes
 * the end of the note before it.  The sentences are the game's arrays, but
 * the pool is the port's: zeroed on every build, so the original's run-on
 * defect (a shorter stage's last call reads on into
 * a longer stage's old pool) is NOT reproduced - a port decision. */
#define SIM_PACE_NOTES  1024    /* A/B/C/D, key[], offset[]: 1,024 dwords   */
#define SIM_PACE_POOL   16384   /* 0x005CEB48: the port's bound on the pool  */

typedef struct sim_pacenotes {
    int32_t count;                    /* [0x005C8B44]  sentences            */
    int32_t notes;                    /* [0x005C8B40]  n, entry 0 the dummy  */
    int32_t key[SIM_PACE_NOTES];      /* 0x005CCB48  the route index that
                                       * speaks sentence s                  */
    int32_t offset[SIM_PACE_NOTES];   /* 0x005CDB48  its first pool byte     */
    uint8_t pool[SIM_PACE_POOL];      /* 0x005CEB48  tokens, 0-terminated    */
    int32_t used;                     /* the pool bytes written (w)         */
    /* pacenotes.py's flags: reads a FRESH process sees as zero.  The port
     * reads them as zero too, and counts them. */
    int32_t stale_a;                  /* A[n] never written                  */
    int32_t past_route;               /* a code, cum[] or angle[] read past
                                       * the route - the port reads 0 / stops */
    int32_t holes;                    /* 0x62 bytes skipped in the pool      */
    int32_t unterminated;             /* a corner on a note with no end      */
    int32_t last_open;                /* the last sentence's missing 0 (1)   */
    int32_t wild;                     /* past SIM_PACE_NOTES / SIM_PACE_POOL:
                                       * the original writes on; the port
                                       * stops and counts                    */
} sim_pacenotes;

/* init_pacenotes_q over `r` (built from the same pols/pnts).  Returns 0 (a
 * route of < 2 markers leaves count 0, as the original's first test). */
int  sim_pacenotes_build(sim_pacenotes *pn, const sim_route *r,
                         const uint8_t *pols, int32_t npols,
                         const uint8_t *pnts, int32_t npnts);

/* A sound request the step makes, kept for the frame to hand to the audio
 * side: the original calls into the speech queue from the game thread
 * (0x0045AD2B, 0x0045AD14, 0x0045ACFD), which the simulation must not do.
 * `slot` is the car ([0x006CE084]); the player mapping ([0x005F4540]) is
 * the caller's. */
enum {
    SIM_SPEECH_QUEUE  = 0,   /* 0x0045AD2B -> 0x0040EADE: a token, appended */
    SIM_SPEECH_FRONT  = 1,   /* 0x0045AD14 -> 0x0040EB0A: a token, at once  */
    SIM_SPEECH_SAMPLE = 2    /* 0x0045ACFD -> 0x0040EABF: a sample id, now  */
};
typedef struct sim_speech {
    int32_t kind, slot, value;
} sim_speech;
#define SIM_SPEECH_RING 256

typedef struct sim_race {
    /* The level: the route the checkpoints index, and the geometry its
     * markers' points are read from.  Both the caller's, both must outlive
     * the race. */
    const sim_route       *route;
    const sim_ground_geom *geom;
    /* The mode, as raceinit_q saw it: gmode is [0x005D6750] (the step's
     * shaungmode is the same global and must agree), no_route_q is
     * [0x005FE9D8], numpl [0x005F0A54].  The false start re-places a car
     * with it. */
    sim_start_mode mode;

    /* initcheckpoints_q (FUN_00458715): the route-strip indices of the
     * tagged markers from index 1, and numcheckposits = the LAST VALID
     * INDEX, not the count (-1 when none is tagged). */
    int32_t numcheckposits;              /* [0x005B5B94]                     */
    int32_t cp[SIM_RACE_CHECKPOINTS];    /* [0x005B5390]                     */
    int32_t cp_overflow;                 /* tagged markers past the array -
                                          * the original writes on into
                                          * numcheckposits itself            */

    int32_t split[SIM_RACE_SPLITS];      /* [0x005B5FC8], FUN_00435576's -
                                          * arcade only (FUN_00435339 does
                                          * not record splits)              */

    /* Process globals the route tracker leaves for its own next call. */
    float   nearest_dist;                /* [0x005C7998] metres, FUN_0043F821
                                          * and FUN_0043F7D3                */
    int32_t reset_index, reset_frac;     /* [0x005E94A8] / [0x005E94B0]     */
    int32_t quiet_q;                     /* [0x00574558]: non-zero skips the
                                          * off-route sounds.  0 in the image;
                                          * its writers are not read        */

    /* Per car, above the replayed prefix. */
    sim_race_msg msg[SIM_ROSTER_SLOTS];
    int32_t currpos[SIM_ROSTER_SLOTS];   /* +0xA0C: -1 in gmode 0, where
                                          * FUN_004355D2 writes nothing     */
    int32_t fac_q[SIM_ROSTER_SLOTS];     /* +0xFAC: FUN_0043D4B3 and
                                          * FUN_0043DFC4 store 10; its
                                          * readers are not read            */
    /* +0xFB4 ([0x006CDC18]): FUN_0043DFC4 sets 8 (0x0043DFF6), the drivetrain
     * tick counts it down (0x0043B999), the ground light reads it
     * (0x0044CED4).  Here, not in sim_world: the replay harness zeroes the
     * world every step, and the counter outlives a step.  Not seeded by
     * --reseed (above the prefix).  +0xFB0, also 8 there, has no reader
     * ported.                                                           */
    int32_t fb4_q[SIM_ROSTER_SLOTS];
    /* the HUD block's `olaydraw` for players 1 and 2
     * ([0x006841D6] / [0x006841E6]), which FUN_0043F355 writes in the
     * step: the countdown plate, 0..5 = 5 4 3 2 1 GO!, -1 none.  The frame
     * redirects texture 0xDC to that plate (0x00452371). */
    int32_t olaydraw[2];

    /* gmode 2's clock.  `allowance` is [0x005B5794], the section
     * allowances in seconds as the load leaves them (FUN_004363C3's row,
     * FUN_00436222's bonus and car factor: port/game/car_load.c) - the
     * caller's, ALLOWANCE_N dwords; a checkpoint pays [0x5B5798 + nextchck*4]
     * (NULL: nothing is paid).  `cheat_word` is [0x0070AA78] (bit 16 stops
     * the out-of-time check); `dontrunoutoftime` [0x005B9228] can never be
     * set (its config key is broken) and stays 0. */
    const int32_t *allowance;            /* SIM_RACE_ALLOWANCE_N dwords      */
    uint32_t cheat_word;
    int32_t  dontrunoutoftime;
    /* The chime flags [0x00510334], [0x00510338], [0x0051033C] - CHECKPT,
     * CHECKTT, CHECKOT - set by 0x0045ACA9 / ACC5 / ACE1 for a local player's
     * car, cleared by the audio side (0x0040E0E1, worldsound.c) */
    int32_t  chime[3];

    /* the arcade's rivals (arcade.h), the caller's and NULL outside
     * gmode 2; and the tick's records [0x006CE044 + slot*4] as this tick
     * stands - the reset's +-64 search reads slots 1..3 through them. */
    struct sim_arcade *arcade;
    car_state *rec[SIM_ROSTER_SLOTS];
    /* A DIAGNOSTIC, not the original's: non-zero hands slot 0 to the rival
     * driver through its dead factor-1 entry (sim_arcade_autopilot_q) -
     * the arcade gate's way to drive a stage that passes rivals. */
    int        autopilot_q;

    /* What the port does not do, counted rather than skipped silently. */
    long    arcade_due;        /* ~~gmode 2's rule arms~~ (ported);
                                * now: gmode 2 steps with no allowance table */
    long    position_due;      /* gmode 3's live position, FUN_0043584A -
                                * NOT ported; gmode 1's
                                * is, below                         */

    /* gmode 1's ninety ghosts.
     * `ghost_recs` is [0x005D6770], graham_to_shaun's copy of the menus'
     * [0x0070CC38] - the caller's, SIM_GHOSTS records, NULL for none (then
     * FUN_0043560F's defaults stand).  The four tables are FUN_0043560F's
     * and FUN_00435688's, rebuilt at every checkpoint; `ghost_catitken` is
     * [0x005B6668] (no reader but the name table).  Per slot, the car's
     * +0xA10..+0xA24 as FUN_00435688 leaves them: the gap to the car ahead
     * and behind in ms (x 10 of the centiseconds), their car types and
     * their name ids.  The gaps and types start at messagesystem_init's -1
     * (0x0044305F..); the names have no writer but FUN_00435688 / 0043584A
     *THE PORT'S 0 until then. */
    const sim_ghost_rec *ghost_recs;
    int32_t ghost_name[SIM_GHOSTS];      /* 0x005B60C8                       */
    int32_t ghost_type[SIM_GHOSTS];      /* 0x005B6398                       */
    int32_t ghost_total[SIM_GHOSTS];     /* 0x005B6230, centiseconds         */
    int32_t ghost_scaled[SIM_GHOSTS];    /* 0x005B6500                       */
    int32_t ghost_catitken;              /* [0x005B6668]                     */
    int32_t gap_ahead[SIM_ROSTER_SLOTS];   /* +0xA10                         */
    int32_t gap_behind[SIM_ROSTER_SLOTS];  /* +0xA14                         */
    int32_t type_ahead[SIM_ROSTER_SLOTS];  /* +0xA18                         */
    int32_t type_behind[SIM_ROSTER_SLOTS]; /* +0xA1C                         */
    int32_t name_ahead[SIM_ROSTER_SLOTS];  /* +0xA20                         */
    int32_t name_behind[SIM_ROSTER_SLOTS]; /* +0xA24                         */
    long    ghost_div0;        /* a DIV by a zero cp[numcheckposits] the
                                * original would fault on: skipped, counted */
    /* A DIAGNOSTIC, not the original's: the rival driver's tables for
     * autopilot_q outside gmode 2 (the arcade's own pointer stays NULL
     * there, so no arcade rule sees it). */
    struct sim_arcade *pilot_q;
    long    sounds;            /* FUN_0045AD14 requests (0x50, 0x52), each
                                * also in speech[] since              */
    long    beeps;             /* FUN_0043F355's countdown calls through
                                * FUN_0045ACFD, in speech[] too     */

    /* the co-driver.  `notes` is the caller's (sim_race_init leaves
     * it NULL: FUN_0043FC56 still keeps l p plyd, and speaks nothing).
     * speech[] holds the step's requests until the frame drains them
     * (nspeech = 0); a full ring drops and counts. */
    const sim_pacenotes *notes;
    sim_speech speech[SIM_SPEECH_RING];
    int32_t    nspeech;
    long       speech_lost;
    long       pace_spoken;    /* FUN_0043FD1D: sentences found and played */
    int32_t   *pace_heard;     /* NULL, or notes->count counters: [s]++ each
                                * time sentence s is spoken (pacenote_check) */
    long       pace_invalid;   /* "!!! invalid pace data": a token >= 0x54 */
    /* the feedback records' step half (soundfeed.h), by roster slot
     * as the writers index them, and the camera they are heard from - the
     * caller's (rally keeps it to the last rendered frame; sim_race_init
     * leaves pos NULL and rot the identity). */
    sim_sound_feed snd[8];
    sim_sound_view snd_view;
    /* 0x0045A31E's levels and the thunder flag, from car 0's
     * sim_world.wx after every tick (tick.c) - the audio thread's input. */
    sim_weather weather;
    long       icons_due;      /* FUN_0043FE55: the HUD's arrow queue, the
                                * car's +0xF58. - NOT ported       */
    long    guru;              /* FUN_0043FAC8's "invalid currsegment" guru:
                                * the port returns "not crossed" instead    */
    long    wild;              /* a store the original makes past what the
                                * port holds (checkpt[nextchck] beyond the
                                * record, a route index outside the route) -
                                * dropped, and counted                      */
} sim_race;

/* the stepped car's feedback record (rc->snd[slot & 7]) when a local
 * player drives it - [0x005F4540 + car*4] != -1, which the port reads as
 * slot < numpl (car 0 is player 1's) - else NULL; `view` takes the camera
 * it is heard from (the race's, else the car's own).  The step's impact and
 * scrape writers (0x0045A8B8 ..) test exactly that. */
sim_sound_feed *sim_sound_feed_local(sim_world *w, sim_sound_view *view);

/* Set a race up for one level: everything zeroed, the mode as given,
 * currpos -1 for every slot, then initcheckpoints_q over `route`.  `route`
 * and `geom` are kept by pointer. */
void sim_race_init(sim_race *rc, const sim_route *route,
                   const sim_ground_geom *geom, const sim_start_mode *m);

/* FUN_00435688, gmode 1's position against the ninety ghosts, for
 * the car in `slot` (FUN_0043560F's table first, from rc->ghost_recs).
 * Exposed for the unit gate (ghost_check); the rule step calls it at each
 * checkpoint. */
void sim_ghost_position(sim_race *rc, const car_state *car, uint32_t slot);

/* FUN_004351C9's rule dispatch, after its mass store (sim_derive_mass):
 * modes 0, 1 and 3 run FUN_004354FD (the clock), FUN_00435339 (the
 * checkpoint watermark and the finish), FUN_004353B1 (the end latch) and
 * FUN_004354E0 (out of race); 2 runs FUN_004354FD, FUN_00435534 (the
 * countdown), FUN_0043526C (the checkpoint and its award), FUN_00435484 (out
 * of time), FUN_004353B1 and FUN_004354E0; 4 runs nothing. */
void sim_race_rules_q(car_state *car, sim_race *rc, uint32_t slot);

/* FUN_0043F9E5, the route tracker, from 0x00439E11: the nearest marker
 * (FUN_0043F821), the car's offset from it (FUN_0043FA51, +0x620/+0x624),
 * the off-route and wrong-way counters (FUN_0043F913, +0x270..+0x27C), the
 * last-checkpoint index, and the crossing (FUN_0043FAC8). */
void sim_route_track_q(car_state *car, const sim_world *w, sim_race *rc,
                       const float *sin_table);

/* FUN_0043FA51 alone - +0x620 / +0x624 from the car's pos and nearestb -
 * exposed so trace_check can run it on the game's own records. */
void sim_route_offset_q(car_state *car, sim_race *rc, const float *sin_table);

/* FUN_0043FAC8: 1 where the original returns CARRY SET - the car is within
 * 32 m of the next section's checkpoint marker and on the far side of it -
 * else 0; the caller's JAE skips on 0. */
int  sim_checkpoint_crossed_q(const car_state *car, sim_race *rc);

/* FUN_00439EB6, the false start, from 0x00439EAB at the step's end.  On the
 * hold's expiry it re-places the car on the grid (FUN_00444639). */
int  sim_false_start_q(car_state *car, sim_world *w, sim_race *rc,
                       const sim_place_ctx *pc);

/* FUN_0043D4B3, the car reset: put the car back on the route at currentb,
 * then FUN_0043DFC4 (the nearest marker from there, placed and levelled
 * again, FUN_0043E00B).  0; -1 if a ground query refuses.  In gmode 2 the
 * free-slot search over +-64 markers: slots 1..3's records in
 * rc->rec, a rival within 8 markers of a candidate taking it; none free logs
 * "cant reset car anywhere", takes 100 off +0x1E8 and leaves the car (1). */
int  sim_car_reset_q(car_state *car, sim_world *w, sim_race *rc,
                     const sim_place_ctx *pc);

/* The arcade spawn's placement (0x004360D9..0x004360DE):
 * FUN_004447FB onto route index `idx`, `frac` of the way on, then
 * FUN_0043E00B's leveller and rest.  0, or -1 if a ground query refuses. */
int  sim_place_rest_q(car_state *car, const sim_route *r,
                      const sim_ground_geom *geom, int32_t idx, int32_t frac,
                      const sim_place_ctx *pc, sim_world *w);

/* FUN_004436C1: 1 - CARRY SET - when every local player's car (slots
 * 0 .. numpl-1) has gof != 0; 0 when numpl is 0.  FUN_0044368D stops its
 * batch of steps on it. */
int  sim_race_over_q(const car_state *const *cars, int32_t numpl);

/* ---- the HUD's two step counters (sections 6-7) ------ */

/* messagesystem_init (0x00443011), from raceinit_q: every slot's message
 * set to 202 for 300 steps, no flash, nothing showing; currpos -1.  (The
 * gaps it also clears, +0xA10..+0xA1C, are not modelled.) */
void sim_message_system_init_q(sim_race *rc);

/* FUN_0043F3BA, at 0x00439E1B in the step, after the route tracker: while
 * +0x728 (showing) and msgtimer != 0, msgtimer--; reaching 0 clears +0x728. */
void sim_message_timer_q(sim_race_msg *m);

/* ------------------------------------------------------------------------
 * THE DAMAGE WARNINGS - damage_warn.c.
 * At 0x00439E5E / 0x00439E63, after the accumulators: FUN_004C773F's 82
 * rows over the 35 states car+0x368.., pushing ids 0xCF..0x10B onto
 * FUN_004C90CE's queue at car+0x478..; FUN_004C9154 pops one into msgtopre
 * while nothing is up. */
#define SIM_WARN_ROWS  82
#define SIM_WARN_SLOTS 32

typedef struct sim_warn_row {
    uint16_t state, value;      /* car+ offsets                              */
    uint8_t  from, to;
    uint8_t  thresh;            /* FILD'd from MOV [0x008DCD9C],imm          */
    uint8_t  rising;            /* 1 JC: act at value >= thresh; 0 JNC       */
    uint16_t msg;               /* ESI, 0 = no tail                          */
    uint8_t  level;             /* EBX                                       */
} sim_warn_row;

void sim_damage_warn_q(car_state *car);
/* FUN_004C90CE: ESI id, EAX the damage value's bits, EBX the level. */
void sim_damage_warn_push_q(car_state *car, int32_t id, uint32_t value,
                            uint32_t level);
void sim_damage_warn_pop_q(car_state *car, sim_race_msg *m);
/* FUN_0043E2DF: the 34 states SET from the car's values, no
 * message - raceinit's last call, through the 8-slot loop 0x0043E2B9
 * (0x004445C4), so a stage starts with the carried wear's states. */
void sim_damage_warn_rebuild_q(car_state *car);
/* For warn_vm_check: how often the push took each arm, process-wide. */
void sim_damage_warn_counts_q(long *over, long *drop, long *full,
                              long *masked);
/* For warn_check's census: how often each of the 82 rows fired, process-wide
 * (tests/trim_census.py: which captures take which rows). */
void sim_damage_warn_row_hits_q(long out[SIM_WARN_ROWS]);

/* ------------------------------------------------------------------------
 * THE PARTS LEFT - parts_left.c.  FUN_0043F4F8 at 0x00439E59,
 * between the accumulators and the warnings: grip_half (+0x284) from
 * carpartsleft bits 1..8 (the tyres 1..4, the wheels 5..8), and - with all
 * eight present and +0x26C == 0 - a worn-through wheel and tyre (damage
 * >= 100) that pass the car's LFSR roll come off: the bit cleared, the
 * damage 256.0, a tyre's blowout into `f` (NULL: none).  Returns the bits
 * cleared this step. */
uint32_t sim_parts_left_q(car_state *car, sim_sound_feed *f,
                          const sim_sound_view *v);

/* FUN_0043F355 for one player (FUN_0043F31E, 0x0045E5E7, after the slot
 * loop): unless a false start is being served or the car is not driven,
 * olaydraw = min(alivet_r - 300, 1500) / 100, -1 from 6; a beep on each
 * whole second while a plate shows (counted in rc->beeps). */
void sim_countdown_q(const car_state *car, int32_t *olaydraw, sim_race *rc,
                     uint32_t slot);

/* ---- the co-driver (pacenote.c) ---------------------------------- */

/* Append one request to rc->speech (or count it lost). */
void sim_speech_push(sim_race *rc, int32_t kind, uint32_t slot, int32_t value);

/* FUN_0043FC56, at 0x00439E16 in the step: unless the car is out of the race
 * or finished, the countdown is skipped, it is not driven by a player
 * (driver != 1), held for a false start, all four wheels are off the ground
 * (+0x254) or the race is under 10 s old, the target is currentb plus
 * FISTP(czspd / 8) markers (0 at the route's end), and every route index
 * from l p plyd + 1 to it is spoken (FUN_0043FD1D) - only target + 1 if it
 * is more than 16 ahead; l p plyd (+0x09C) becomes the target. */
void sim_pacenote_trigger_q(car_state *car, sim_race *rc, uint32_t slot);

/* FUN_0043FD1D: the first sentence keyed to `index`, its tokens to the first
 * 0 as SIM_SPEECH_QUEUE requests; a token >= 0x54 ends it ("invalid pace
 * data").  Nothing without rc->notes. */
void sim_pacenote_speak_q(sim_race *rc, uint32_t slot, uint32_t index);

#endif /* RALLY_SIM_PHYSICS_H */
