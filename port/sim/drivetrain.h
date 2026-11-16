/* drivetrain.h - the gear ladder, the torque curve, and where `torqu` comes
 * from.
 *
 * SOURCE, AND WHY THIS IS A TRANSLATION RATHER THAN A DERIVATION
 * -------------------------------------------------------------
 * sections 1-8 read the whole chain at the
 * instruction, and tools/drivetrain.py ALREADY reimplements it in Python and
 * is cross-checked against the game's own `initaccels` self-test
 * phase A's oracle gate.  the own text says so in as many words: that
 * Python "is the reference translation for the C port's drivetrain half - do
 * not re-derive it from the disassembly a second time."
 *
 * So this file is a translation OF THE PYTHON, line for line, and the gate
 * that matters is that the two agree bit for bit.  Re-deriving it would have
 * produced a second reading of the disassembly, which is a second chance to be
 * wrong, and no way to tell which of the two was.
 *
 * THE CONSTANTS ARE NOT IN THIS FILE
 * ----------------------------------
 * Twenty-one floats and the nine curve breakpoints are read out of the user's
 * own RAL.EXE at runtime, exactly like the handling table.  They are
 * retail 5.25 addresses: the retail/5.30 delta is NOT constant
 * so this table must be re-measured per
 * build rather than offset, and drivetrain.py carries the same warning.
 *
 * THE ARITHMETIC IS float, DELIBERATELY, AT EVERY STEP
 * ---------------------------------------------------
 * Every floating-point STORE in this image is 32-bit, so the Python
 * rounds to float32 after every operation with its `f32()` helper.  Plain C
 * `float` arithmetic does the same thing on both of this port's targets -
 * x86-64 with SSE2 and aarch64 both round per operation - but NOT on 80-bit
 * x87, which keeps excess precision through a whole expression.  If this is
 * ever built for 32-bit x86, it needs -msse2 -mfpmath=sse or it will disagree
 * with the Python in the low bits while looking correct.  That is the
 * `float_model_probe` question owns for the ARM side (FPCR.FZ).
 *
 * WHAT IS NOT HERE
 *  sim_gearbox_mode2_q (strategy 2, FUN_0043AABF, 678 b).  initaccels
 *     forces autom_1 to 1 (0x00420580), so this model never needs it; the
 *     real step's strategy 2 is gearbox.c's shift_mode2.  Strategies
 *     0 and 1 are here.
 *  The stick path inside sim_gearbox_shift_q.  `initaccels` drives the auto
 *     path, so that is what the oracle can check; the stick path would be
 *     unverifiable code.  (the real step's gearbox, stick
 *     included, is gearbox.c; this file keeps initaccels' auto-only model.)
 *  sim_drivetrain_tick_q's other five countdowns.  Only the torque cut
 *     (car0+0x1D0) is load-bearing here and it is the one implemented.
*/

#ifndef RALLY_SIM_DRIVETRAIN_H
#define RALLY_SIM_DRIVETRAIN_H

#include <stddef.h>
#include <stdint.h>

#include "car_params.h"

#define DT_CURVE_PTS    9
#define DT_RATIO_SLOTS  9      /* ratio[-1..7]; see dt_gears.ratio below */
#define DT_SELFTEST_STEPS 0x1388   /* FUN_0042053A's loop bound, 5,000     */

/* The image constants the chain reads.  One field per drivetrain.py CONSTANTS
 * entry, same names, so a disagreement is greppable in both directions. */
typedef struct dt_consts {
    float dt;            /* 0x005C65C4  0.01                                */
    float mph;           /* 0x0057AF84  m/s -> "game mph", 2.25             */
    float mph_disp;      /* 0x0057AF98  100.0, the centisecond divisor      */
    float k10k;          /* 0x0057B0B8  10000.0                             */
    float throttle_fs;   /* 0x0057B0B0  256.0                               */
    float drag_q;        /* 0x005C6D04  quadratic drag numerator            */
    float roll;          /* 0x005C6D08  rolling resistance                  */
    float revref;        /* 0x005C6BF8  8500.0, final-drive reference revs  */
    float revs_floor;    /* 0x0057AF9C  1000.0  (a FLOOR -)            */
    float ratio_floor;   /* 0x0057AFD4  8.0     (a FLOOR -)            */
    float setg_step;     /* 0x0057B028  0.25, the gearing slider step       */
    float shift_loss;    /* 0x005C6BDC  0.95, revs kept across an upshift   */
    float neutral;       /* 0x005C6D20  0.001, ratio[0]                     */
    float overrev;       /* 0x005C6D1C  1.2, the money-shift ceiling        */
    float down_scale;    /* 0x005C6BC8  0.9, +0xA0 -> the downshift point   */
    float coast_margin;  /* 0x0057B060  0.1, +0xA4/+0xA8 + this are the two
                          * downshift points [0x00580694/98]  */
    float t60;           /* 0x005806C0  26.822428 m/s                       */
    float t100;          /* 0x005806C4  44.704048 m/s                       */
    int32_t cut_steps;   /* 0x005C6CD8  5 - the torque-cut steps            */

    /* FUN_0043F208 reads these three as INTEGERS.  0x005C6C34 holds the bit
     * pattern of 32.0f and FIDIV reads it as 1107296256, which is a type bug
     * in the original -.  A PORT MUST COPY IT, not
     * fix it: repairing the type changes every car's low-speed acceleration.
     * They are int32_t here so nobody can "tidy" them into floats. */
    int32_t class_n_4wd; /* 0x005C6C2C  8                                   */
    int32_t class_n_2wd; /* 0x005C6C30  32                                  */
    int32_t f208_div;    /* 0x005C6C34  1107296256, the bug                 */

    float xt[DT_CURVE_PTS];  /* 0x005C6D28  0.0, 0.125 ... 1.0              */
} dt_consts;

/* The gear ladder.  ratio[0] is NEUTRAL (0.001, doing duty as a flywheel
 * inertia in the free-rev path), ratio[1..ngears] are the gears, and reverse
 * is -first.  The original addresses reverse as [EBX*4 + 0x5C6DF8] with
 * EBX = -1, one slot BELOW the base, so the real table is [-1..7]; it is
 * carried here as a separate field rather than as index 0 of a shifted array,
 * because a negative index in C is how this gets wrong silently.
 *
 * A "ratio" here is METRES PER SECOND OF ROAD SPEED PER UNIT OF revs - which
 * is why sim_drive_torque DIVIDES by it and sim_engine_speed MULTIPLIES.
 * There are no per-gear ratios in the data at all: four numbers and a count
 * produce the whole ladder as an arithmetic progression. */
typedef struct dt_gears {
    int   ngears;                 /* 5..7, clamped on load                  */
    float maxrevs;
    float ratio[8];               /* [0] neutral, [1..ngears] the gears     */
    float reverse;                /* the [-1] slot: -first                  */
    float finaldrive;             /* record +0x78 x 8500 / maxrevs          */
} dt_gears;

/* Load the constants from a PE32 image.  Same return codes as
 * car_params_load(); -1 means the caller should SKIP. */
int dt_consts_load(const char *image_path, dt_consts *out,
                   char *err, size_t errlen);

/* FUN_004206D8.  Runs EVERY CAR, EVERY STEP in the original - the ladder is
 * recomputed from the car record on every tick, which is why a mid-race change
 * to the gearing slider or to maxrevs takes effect at once.  A port may cache
 * it (the function is pure in its inputs) but only if it re-derives on any
 * change to set_g_1, ngears or maxrevs. - this was published backwards
 * once. */
void dt_build_gear_ratios(const car_params *p, const dt_consts *k,
                          int32_t set_g_1, dt_gears *out);

/* FUN_0043B096.  `y` is the curve output already multiplied by engpower; `q`
 * is the (revs/maxrevs) x 0.25 engine-braking term.  BOTH out-of-range
 * branches produce curve[0], not curve[8]. */
void dt_torque_curve(const car_params *p, const dt_consts *k,
                     float revs, float maxrevs, float engpower,
                     float *y, float *q);

/* FUN_0043F208, type bug and all.  Returns the multiplier applied to torqu. */
float dt_torque_scale(const dt_consts *k, int drive_class, int32_t set_g_1);

/* FUN_0043A5CB, sim_engine_speed.  Engine speed is wheel speed - AVERAGED
 * ACROSS BOTH AXLES for drivetrain class 1 - divided by gearratio[gear].
 *
 * EXTRACTED FROM dt_selftest_run's body, deliberately and with
 * the gate re-run to prove the extraction changed nothing.  It was inline
 * because `initaccels` is the only thing that called it;
 * section 5 then recorded that `initaccels` sets fwhlspd = rwhlspd = v every
 * step, so `(v+v)*0.5` is exactly `v` and the two class arms are
 * INDISTINGUISHABLE BY CONSTRUCTION - a measured blind spot.  The tyre force
 * model is the first thing that turns the axles at different speeds, so it is
 * the first caller that can tell them apart, and it has to call the SAME code
 * for that to mean anything. */
float dt_engine_speed(const dt_gears *g, int drive_class,
                      float fwhlspd, float rwhlspd, int gear);

/* [gear*4 + 0x005C6DF8]: the game's table starts at the reverse slot
 * 0x005C6DF4, so gear -1 is a real entry.  dt_gears keeps it in `reverse`,
 * and ratio[-1] would read maxrevs. */
float dt_gear_ratio(const dt_gears *g, int gear);

/* FUN_0043AFEF, sim_drive_torque.  `y` and `q` are dt_torque_curve's outputs.
 * The `+q ... -q` bracket is engine braking and cancels EXACTLY at throttle
 * 256, which is what lets `initaccels` omit it -.
 * Returns torqu, an acceleration in m/s^2, before the torque cut and before
 * dt_torque_scale; *crank_out (if not NULL) gets [0x5C7474], the crank
 * torque FUN_0043CF56 multiplies up later in the same step. */
float dt_drive_torque(const dt_consts *k, const dt_gears *g,
                      float y, float q, int32_t throttle, int gear,
                      float *crank_out);

/* ---- the self-test, which is this half's gate -------------------------- */

typedef struct dt_selftest {
    char  name[CAR_PARAMS_NAME_LEN + 1];
    int   ngears;
    float maxrevs;
    float ratio[8];      /* [1..ngears] populated, as the oracle reports it */
    float finaldrive;
    float top;           /* final speed x mph                              */
    /* t60/t100 are DOUBLE, and deliberately so.  They are the step COUNT
     * divided by 100.0, and tools/drivetrain.py - the reference translation
     * this file is checked against - does that division in double, without
     * its f32() rounding, because they are reporting values rather than
     * simulation state that feeds the next step.
     *
     * Matching the oracle exactly is what lets the gate be a bitwise diff.
     * But whether the ORIGINAL keeps them in float32 is a real question that
     * nobody has read the store width for: `initaccels` computes them and
     * retail never logs them (s6, s10.4),
     * so no shipped output pins it down.  Filed rather than assumed - if it
     * turns out to be a 32-bit store, BOTH sides change together. */
    double t60, t100;    /* step counts / 100.0 - see above                */
    int   gear;
    int   trace_len;     /* every 20th step: speed in "game mph", and gear */
    float trace_v[DT_SELFTEST_STEPS / 20 + 1];
    int   trace_g[DT_SELFTEST_STEPS / 20 + 1];
    float designtop;
} dt_selftest;

/* FUN_0042053A, retail's `initaccels`.  5,000 steps of the drivetrain chain
 * with two drag terms bolted on, no throttle bracket (it cancels exactly at
 * full throttle, which is what lets the self-test omit it - s2).
 *
 * This is the port's drivetrain regression test because the ORIGINAL ships it:
 * it is the game's own statement of what its drivetrain does, and phase
 * A already gates tools/drivetrain.py against it. */
void dt_selftest_run(const car_params *p, const dt_consts *k,
                     int32_t set_g_1, uint32_t gearbits,
                     dt_selftest *out);

#endif /* RALLY_SIM_DRIVETRAIN_H */
