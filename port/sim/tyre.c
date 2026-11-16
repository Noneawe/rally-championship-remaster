/* tyre.c - sim_tyre_forces_q, sim_yaw_model_q and sim_car_step.
 * See physics.h.
 *
 * SOURCE
 * ------
 * sections 3 and 4, which read FUN_0043C48C
 * (650 instructions) and FUN_0043B254 (391) in full from disasm.tsv, with
 * Ghidra instruction coverage of 100% on both.  The section numbers in the
 * comments below are that document's own numbered steps, so the two can be
 * read side by side.
 *
 * WHERE THE ROUNDING IS WRITTEN DOWN AND WHERE IT IS NOT - READ THIS
 * -----------------------------------------------------------------
 * physics.c can say exactly where each x87 store falls, because every function
 * in it was disassembled instruction by instruction this session.  THIS FILE
 * CANNOT. records these two functions as ALGEBRA - it
 * says `GRIPZF = -(G[0]*Ny0 + G[1]*Ny1) * dt * mass * TYREGRIPF`, not which of
 * those five operators is followed by an FSTP.  A 650-instruction function has
 * intermediate stores and this port does not know where they are.
 *
 * So the rule here is the opposite of physics.c's: plain C `float` arithmetic,
 * rounding after every operator, which is the ASSUMPTION THAT EVERY VALUE IS
 * STORED.  It is stated rather than hidden because it is the most likely place
 * for this port to be wrong in the low bits, and because the fix is known and
 * cheap - re-read the two functions and mark the stores.  Filed in
 * against.
 *
 * ONE expression here does have its rounding measured rather than assumed: the
 * yaw model's steering term.  Against the state-dump capture, the
 * single-rounding form is bit-identical to the game on 25,806 of 36,753 real
 * frames and the per-operator form on 25,207 - but the number that settles it
 * is not either of those.  The per-operator form produces 335 frames whose
 * values are SMALLER than the game's own, and the friction circle downstream
 * can only ever make a value smaller still, so that form is refuted rather
 * than merely outscored.
 *
 * That is what a measurement of a store point looks like, and it is why the
 * rest of this file is flagged rather than quietly guessed.
*/

#include "physics.h"
#include "camera.h"
#include "collision_int.h"

#include <math.h>
#include <string.h>

static float st(double v) { return (float)v; }

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


/* The tyre model's wheel arithmetic is one x87 expression per FSTP (session
 * 81: steps 2, 4, 5, 6 and 9 were written in float, rounding at every
 * operation, and were one ULP off the capture on thousands of steps).  These
 * shorten the transcriptions; ground.h's model is the same bits on both
 * targets. */
static sim_x87 X(float f)                 { return sim_x87_from_float(f); }
static sim_x87 Xi(int32_t i)              { return sim_x87_from_i64(i); }
static sim_x87 Xadd(sim_x87 a, sim_x87 b) { return sim_x87_add(a, b); }
static sim_x87 Xsub(sim_x87 a, sim_x87 b) { return sim_x87_sub(a, b); }
static sim_x87 Xmul(sim_x87 a, sim_x87 b) { return sim_x87_mul(a, b); }
static sim_x87 Xdiv(sim_x87 a, sim_x87 b) { return sim_x87_div(a, b); }
static sim_x87 Xchs(sim_x87 a)            { a.neg = !a.neg; return a; }
static float   Xst(sim_x87 a)             { return sim_x87_to_float(a); }

/* FUN_0043A5CB, sim_engine_speed, through drivetrain.c's one copy. */
static void engine_speed(car_state *car, const dt_gears *g)
{
    if (car->gear != 0)
        car->revs = dt_engine_speed(g, car->drive_class,
                                    car->fwhlspd, car->rwhlspd, car->gear);
}

/* FUN_0043A626, sim_wheel_speed_from_revs: in gear, f = revs * ratio, and
 * class 1 copies it to r. */
static void wheel_speed_from_revs(car_state *car, const dt_gears *g)
{
    if (car->gear == 0) return;
    car->fwhlspd = Xst(Xmul(X(car->revs), X(dt_gear_ratio(g, car->gear))));
    if (car->drive_class == 1) car->rwhlspd = car->fwhlspd;
}

/* Move v toward zero by a, never through it - AND LEAVE AN EXACT ZERO ALONE.
 * Steps 5, 6 and 10 of the tyre model are all this shape in the original, and
 * each begins with the same test (0x0043C7D9, 0x0043C880, 0x0043C921,
 * 0x0043CBF1):
 *
 *     FLDZ / FLD v / FCOMPP / FNSTSW / AND AH,0x40 / JNZ skip     ; v == 0
 *     SHR AH,1 / JNC pos                                          ; v <  0 ?
 *        v += a ; STORE ; if (!(v < 0)) v = 0                     ; JC keeps
 *     pos:
 *        v -= a ; STORE ; if (v < 0)    v = 0                     ; JNC keeps
 *
 * The port used to write this as `v - signf(v) * a` clamped on `nv * v < 0`,
 * which agrees everywhere EXCEPT v == 0: signf(0) is +1, so a wheel at rest
 * was braked to -a and a car with no lateral speed got a full -gripx kick.
 * found it because the ground query is the first thing in the port that
 * reads the car's POSITION: physics_selftest's car left its tiled level in
 * one step, sliding sideways at the 80 m/s guard -.
 * An unordered compare (NaN) sets C3 too, so NaN is also left alone. */
static float toward_zero(float v, float a)
{
    float n;
    if (v == 0.0f || v != v) return v;
    if (v < 0.0f) {
        n = v + a;
        return (n < 0.0f) ? n : 0.0f;
    }
    n = v - a;
    return (n < 0.0f) ? 0.0f : n;
}

/* FUN_0043F2A5, sim_traction_limit.  Read from the image this session:
 *     FLD 800.0 / FLD czspd / FABS / FADD 20.0 / FDIVP / FSUB 20.0 / FSTP
 * so the whole expression is ONE rounding, then a compare against
 * [0x0057B014] = 0.0 and a clamp to zero.  A wheelspin allowance of 20 m/s at
 * rest, falling to zero at 20 m/s. */
static float sim_traction_limit(const sim_consts *k, float czspd)
{
    float t = st((double)k->trac_num / (fabs((double)czspd) + k->trac_add)
                 - k->trac_sub);
    return (t < k->zero) ? k->zero : t;
}

/* ------------------------------------------------------------------------ */
/* FUN_0043B254 - the yaw model.             */
/* ------------------------------------------------------------------------ */

void sim_yaw_model_q(car_state *car, const car_params *p, const sim_consts *k,
                     const sim_surface *surf, const car_geometry *geom,
                     const float *sin_table, sim_world *w)
{
    const int fourwd = (car->drive_class == 1);
    float sinsteer = sim_steer_sin(sin_table, car->usteerangle);
    float steer_raw, grip_t;

    /* The yaw model's first act.  The timer is the value after
     * FUN_0043B990's decrement. */
    w->yaw_pivot_fade = sim_yaw_pivot_fade_q(car->collision_response_timer, k);

    /* ---- term 1: steering, capped by the friction circle ----------------
     *
     * MEASURED, not assumed: this is  sin(delta) * v / L  with ONE rounding.
     * `steer_curve` in is the global sine table
     * at 0x009C3EE4 (physics.h explains how that was pinned down), so the
     * "bicycle model" reading is an identity rather than an analogy. */
    steer_raw = st((double)sinsteer * car->czspd / k->wheelbase);
    car->yaw_steer_term = steer_raw;

    /* The friction circle:  a_lat = omega x v <= mu.g  =>  omega <= mu.g / v.
     * `grip t` is the game's own label for the bound.  accel_grip[4] is the
     * FIFTH contact point, the one that is not a wheel - the yaw ceiling is
     * the only thing in the binary that reads it (s1).
     *
     * ~~A zero czspd would divide by zero here.  The original has no guard~~ -
     * IT DOES.  0x0043B315 is FLD czspd / FCOMP 0.0 /
     * AND AH,0x40 / JNZ past the whole clamp, so a zero speed skips it.
     *
     * AND THE BOUND IS ONE x87 EXPRESSION WITH ONE FSTP (0x0043B32E..5F):
     *     FLD accel[4] / FMUL gravity / FMUL 2.2|2.0 / FMUL grip_t /
     *     FDIV czspd / FABS / FSTP [0x005C6F94]
     * This port used to write it in float, rounding four times.  On the
     * capture that is exactly one ULP wrong on a fraction of clamped frames -
     * which had read as "blended contact points", and 734 of them were
     * pure gravel.  It is computed here with
     * ground.h's x87 model, so it is the same bits on both targets. */
    /* AND AH,0x40 tests C3, which FCOMP sets for EQUAL and for UNORDERED: so
     * a NaN skips too, and the skip is to 0x0043B492 - past the stopped-wheel
     * zero and the STEERANGLE scale below as well as the clamp. */
    if (car->czspd < 0.0f || car->czspd > 0.0f) {
        sim_x87 g = sim_x87_div(
            sim_x87_mul(sim_x87_mul(sim_x87_mul(sim_x87_from_float(surf->accel[4]),
                                                sim_x87_from_float(k->gravity)),
                                    sim_x87_from_float(fourwd ? 2.2f : 2.0f)),
                        sim_x87_from_float(p->grip_t)),
            sim_x87_from_float(car->czspd));
        g.neg = 0;                                             /* FABS       */
        grip_t = sim_x87_to_float(g);
        car->yaw_steer_term = clampf(car->yaw_steer_term, -grip_t, grip_t);

        /* ~~A stopped car cannot yaw.~~: the other way round.
         * 0x0043B3D1 is FLD czspd / FABS / FCOMP 1.0 / JB past the store - so
         * |czspd| < 1 (or NaN) SKIPS it - then FLD fwhlspd / FABS / FCOMP 1.0
         * / JAE past it.  The term is zeroed when the car MOVES (|czspd| >= 1)
         * on front wheels that do not turn (|fwhlspd| < 1): locked wheels do
         * not steer.  Every one of the 1,097 +0x210 misses was this: the game
         * -0.0, the port +0.0. */
        if (!(fabsf(car->czspd) < 1.0f) && fabsf(car->fwhlspd) < 1.0f)
            car->yaw_steer_term = 0.0f;

        /* The digital-steering scale, 0x0043B407..0x0043B492, when dannyebx
         * bit 0x1000 (a steering KEY) is set.  ~~`prev` ... IT IS AN
         * INTERPRETATION~~ - read: the divisor is [0x005D6EA0], which
         * 0x0043B365 copied from +0x210 BEFORE the clamp, i.e. steer_raw.
         *   CMP dword [0x005D6EA0],0 / JZ -> STEERANGLE = 0   (the BITS: -0.0
         *                                                       does not)
         *   r = FSTP float(+0x210 / raw)
         *   JB keeps r when r < 1.0 OR unordered, so a NaN survives the
         *   first test; JAE keeps r only when r >= 0.0, so it dies at the
         *   second:   if (r >= 1.0) r = 1.0;  if !(r >= 0.0) r = 0.0
         *   STEERANGLE = FISTP(r FIMUL STEERANGLE)            an INTEGER
         * So the keyboard's steer is cut back to what the friction circle
         * allows, and stays cut: +0x644 is the ramp's own accumulator. */
        if ((car->dannyebx & 0x1000u) != 0u) {
            uint32_t raw_bits;
            memcpy(&raw_bits, &steer_raw, 4);
            if (raw_bits == 0u) {
                car->stang = 0;
            } else {
                float r = sim_x87_to_float(sim_x87_div(
                    sim_x87_from_float(car->yaw_steer_term),
                    sim_x87_from_float(steer_raw)));
                if (r >= 1.0f) r = 1.0f;
                if (!(r >= 0.0f)) r = 0.0f;
                car->stang = sim_x87_fistp(sim_x87_mul(
                    sim_x87_from_float(r), sim_x87_from_i64(car->stang)));
            }
        }
    }

    /* ---- term 2: ROTSPDHBRK, two-wheel drive only ---------------------- */
    car->rotspdhbrk = 0.0f;
    if (!fourwd) {
        float s = sqrtf(car->cxspd * car->cxspd + car->czspd * car->czspd);
        if (s > 2.0f) s = 2.0f;
        car->rotspdhbrk = (2.0f - s) / 2.0f * sinsteer
                        * ((float)car->handbrake_ramp / 256.0f)
                        * car->fwhlspd * 0.25f
                        * ((float)car->throttle / 256.0f);
    }

    /* ---- term 3: ROTSPDPOWR -------------------------------------------- */
    {
        int32_t n;
        if (fourwd)
            n = (car->brake - car->throttle) / 2 + 0x80
              + (int32_t)(w->bbal * (float)car->brake * 1.5f) + car->handbrake_ramp;
        else
            n = 0x100 - car->throttle
              + (int32_t)(w->bbal * (float)car->brake * 2.0f) + car->handbrake_ramp;

        car->rotspdpowr = -((float)n / 256.0f) * car->yaw_powr_in * 0.05f
                        * p->powr_scale;

        /* Opposing terms are halved. */
        if (car->rotspdpowr * car->yaw_steer_term < 0.0f)
            car->rotspdpowr *= 0.5f;
        if (car->grip_half == -1)
            car->rotspdpowr *= fourwd ? 2.4f : 1.6f;
    }

    /* ---- term 4: ROTSPDSPIN, four-wheel drive only ---------------------
     * Zero above 5 m/s and proportional to longitudinal slip: power-oversteer,
     * and only the 4WD class gets it. */
    car->rotspdspin = 0.0f;
    if (fourwd) {
        float sp = fabsf(car->czspd);
        float a, b;
        if (sp > 5.0f) sp = 5.0f;
        a = (5.0f - sp) / 5.0f;
        /* The original flips the sign of [0x005C6BC0] in place to get the
         * lower bound and flips it back forty instructions later - physics.h
         * says why this port does not reproduce that. */
        b = clampf(car->zspddif, -k->clamp_zspddif, k->clamp_zspddif)
            / k->clamp_zspddif;
        car->rotspdspin = sinsteer / k->wheelbase
                        * ((float)car->throttle / 256.0f) * b * a * 7.5f;
    }

    /* ---- sum, slew, and emit a couple ---------------------------------- */
    car->wantyrotspd = car->yaw_steer_term + car->rotspdpowr
                     + car->rotspdspin + car->rotspdhbrk;

    w->yaw_couple[0] = w->yaw_couple[1] = 0.0f;

    if (car->yaw_couple_gate == 0) {
        float old = car->rotspd[1];      /* [car0+0x188] - the y component   */
        float d   = old - car->wantyrotspd;
        float f;

        car->yrotspd = old;

        /* Move the error toward zero by at most 0.1 - a rate limiter on yaw. */
        if (d >  0.1f) d -= 0.1f;
        else if (d < -0.1f) d += 0.1f;
        else d = 0.0f;

        car->yrotspd = d + car->wantyrotspd;

        /* Angular acceleration back into a force, then applied at +/- the two
         * lever arms: records 19 and 20 carry an x force at +pz and -pz, which
         * is a pure couple and therefore a yaw moment and nothing else. */
        f = (car->yrotspd - old) / k->dt * 0.5f / 0.75f;

        memset(&w->force[SIM_REC_YAW_FIRST], 0, 2 * sizeof(sim_force_rec));
        w->force[SIM_REC_YAW_FIRST    ].f[0] = f / geom->wheel_z[0];
        w->force[SIM_REC_YAW_FIRST    ].p[2] = geom->wheel_z[0];
        w->force[SIM_REC_YAW_FIRST + 1].f[0] = f / geom->wheel_z[3];
        w->force[SIM_REC_YAW_FIRST + 1].p[2] = geom->wheel_z[3];

        w->yaw_couple[0] = w->force[SIM_REC_YAW_FIRST    ].f[0];
        w->yaw_couple[1] = w->force[SIM_REC_YAW_FIRST + 1].f[0];
    }
}

/* ------------------------------------------------------------------------ */
/* FUN_0043C2BA - the yaw pivot shift.     */
/* ------------------------------------------------------------------------ */

float sim_yaw_pivot_fade_q(int32_t timer, const sim_consts *k)
{
    /* 0x0043B254..0x0043B2BE:
     *     FILD +0x1F4 / FILD 50 / FDIVP / FSUB 1.0 / FCHS / FSTP [0x005C749C]
     * then FCOMPP/JNC against 0.0 (below or unordered -> 0.0) and FCOMPP/JC
     * against 1.0 (not below -> 1.0). */
    sim_x87 f = sim_x87_sub(sim_x87_div(sim_x87_from_i64(timer),
                                        sim_x87_from_i64(50)),
                            sim_x87_from_float(k->pivot_one));
    float fade;
    f.neg = !f.neg;                                            /* FCHS       */
    fade = sim_x87_to_float(f);
    if (!(fade >= k->zero)) fade = k->zero;
    if (fade >= k->pivot_one) fade = k->pivot_one;
    return fade;
}

void sim_yaw_pivot_shift_q(car_state *car, const car_geometry *geom,
                           const sim_consts *k, const sim_world *w)
{
    /* The rear point first (car+0xCFC), then the front (car+0xCF0):
     * 0x0043C38E and 0x0043C3AB, each followed by FUN_0043C3CF. */
    const float lever_z[2] = { geom->wheel_z[3], geom->wheel_z[0] };
    sim_x87 a, b, tot;
    float v, wgt[2];
    int i;

    /* 0x0043C2BA zeroes [0x005C6E1C]/[0x005C6E20] (the two FISTP results,
     * scratch read only here), then CMP [0x005D45B4],1.  That global gates
     * the whole step too (simtick_q runs the step only when it is not 0) and
     * its writer is unfound: no absolute or [reg + disp] reference stores it
     * (ralimg.py refs / disp / find).  The capture's race has it
     * at 1 - the shift is there on 36,476 of 36,531 grounded steps - so the
     * port treats it as 1 whenever the step runs.  Then car+0x254. */
    if (car->yaw_couple_gate != 0) return;

    /* |+0x210| + |+0x21C| (steering, handbrake) and |+0x214| + |+0x218|
     * (power, spin), FADDP each; FADD ST0,ST1 makes the total, and FCOM
     * against 0.0 with AND AH,0x40 returns on zero OR unordered. */
    a = sim_x87_add(sim_x87_from_float(fabsf(car->yaw_steer_term)),
                    sim_x87_from_float(fabsf(car->rotspdhbrk)));
    b = sim_x87_add(sim_x87_from_float(fabsf(car->rotspdpowr)),
                    sim_x87_from_float(fabsf(car->rotspdspin)));
    tot = sim_x87_add(b, a);
    if (tot.cls == 2 || (tot.cls == 0 && tot.m == 0)) return;

    /* FDIVP / FSTP [0x005C7974], clamped as the fade is. */
    v = sim_x87_to_float(sim_x87_div(a, tot));
    if (!(v >= k->zero)) v = k->zero;
    if (v >= k->pivot_one) v = k->pivot_one;
    wgt[0] = v;
    /* FLD1 / FSUB [0x005C7978] / FSTP */
    wgt[1] = sim_x87_to_float(sim_x87_sub(sim_x87_from_float(1.0f),
                                          sim_x87_from_float(v)));

    for (i = 0; i < 2; i++) {
        /* FUN_0043C3CF.  [0x005C7878] = (0, 0, lever z). */
        const float lever[3] = { 0.0f, 0.0f, lever_z[i] };
        float wl[3], d[3], dw[3];

        sim_vec3_by_matrix(car->rotspd, w->active_frame, wl); /* D5DC      */
        sim_vec3_cross(lever, wl, d);                          /* 41FD06    */
        sim_vec3_by_matrix(d, car->matrix, dw);                /* D612      */
        sim_vec3_scale(dw, wgt[i]);                            /* 420010    */
        sim_vec3_scale(dw, w->yaw_pivot_fade);                 /* 420010    */
        /* FLD / FMUL 65536.0 / FMUL dt / FISTP / ADD, x then z. */
        car->pos[0] += sim_x87_fistp(sim_x87_mul(
            sim_x87_mul(sim_x87_from_float(dw[0]),
                        sim_x87_from_float(k->pos_scale)),
            sim_x87_from_float(k->dt)));
        car->pos[2] += sim_x87_fistp(sim_x87_mul(
            sim_x87_mul(sim_x87_from_float(dw[2]),
                        sim_x87_from_float(k->pos_scale)),
            sim_x87_from_float(k->dt)));
    }
}

/* FUN_0043B990, sim_drivetrain_tick_q, at 0x00439C0C: the step's countdowns
 * (unported until now, so the shift cut's last step still cut
 * the torque: 117 of the capture's 127 such steps missed, by 0.011-0.30 m/s).
 * Read whole:
 *     +0xFB4, +0xFB0: DEC if != 0                  above the replay cut, NOT
 *                                                  modelled
 *     +0x268: zeroed unless alivet_r >= 1000 (JB) and OOR, OOT, RFN are 0;
 *             then, only in gmode 1, ++ in neutral with CPLEFT & 0x1E !=
 *             0x1E, else zeroed; any other gmode leaves it
 *     +0x26C: ++ unless 0, -1 or 1
 *     +0x1F4 (collision response timer), +0x1D0 (shift cut): DEC if != 0
 *     +0x5FC = +0xF08 unless +0xF08 < +0x5FC        +0xF08 above the cut,
 *                                                  NOT modelled
 *     +0x460: DEC if != 0 */
static void sim_drivetrain_tick_q(car_state *car, sim_world *w, int32_t gmode)
{
    if (w->race && w->race->fb4_q[w->race_slot & 7u] != 0)
        w->race->fb4_q[w->race_slot & 7u]--;                /* +0xFB4 */
    if ((uint32_t)car->alivet_r < 1000u || car->oor != 0 || car->oot != 0 || car->rfn != 0)
        car->tctime = 0;
    else if (gmode == 1) {
        if ((car->carpartsleft & 0x1E) != 0x1E && car->gear == 0) car->tctime++;
        else car->tctime = 0;
    }
    if (car->tick_26c_q != 0 && car->tick_26c_q != -1 && car->tick_26c_q != 1)
        car->tick_26c_q++;
    if (car->collision_response_timer != 0) car->collision_response_timer--;
    if (car->shift_torque_cut != 0) car->shift_torque_cut--;
    if (car->tick_460_q != 0) car->tick_460_q--;
}

/* ------------------------------------------------------------------------ */
/* 0x00439CAF..0x00439D0B - the engine and the wheels after the tyre model.  */
/* The port had stopped at the      */
/* tyre model; every in-gear step then rounds the wheel through the revs.    */
/* ------------------------------------------------------------------------ */

/* FUN_0043CF56, the engine with no load.  It calls
 * the torque CURVE (0x0043B096), not the limiter, so it writes q; the crank
 * torque it multiplies up is the one sim_drive_torque left in this step.
 *     torqu = crank * finaldrive / ratio[0] / 10000 * FILD throttle / 256
 *     revs  = torqu * dt / ratio[0] + revs
 *     decay = revs * 0.005 + 5.0
 *     revs  = toward_zero(revs, decay)                  ; 0x0043CFBD.. */
static void sim_engine_freerev_q(car_state *car, const car_params *p,
                                 const sim_consts *k, const dt_consts *dk,
                                 const dt_gears *g, sim_world *w, float *rq)
{
    float y, q, decay;
    dt_torque_curve(p, dk, car->revs, g->maxrevs, car->engpower, &y, &q);
    *rq = q;
    w->torqu = Xst(Xdiv(Xmul(Xdiv(Xdiv(Xmul(X(w->crank), X(g->finaldrive)),
                                       X(g->ratio[0])), X(dk->k10k)),
                             Xi(car->throttle)), X(dk->throttle_fs)));
    car->revs = Xst(Xadd(Xdiv(Xmul(X(w->torqu), X(k->dt)), X(g->ratio[0])),
                         X(car->revs)));
    decay = Xst(Xadd(Xmul(X(car->revs), X(k->free_decay_scale)),
                     X(k->free_decay_base)));
    car->revs = toward_zero(car->revs, decay);
}

int sim_engine_after_tyres_q(car_state *car, const car_params *p,
                             const sim_consts *k, const dt_consts *dk,
                             const dt_gears *g, sim_world *w, float *rq)
{
    float idle, cap;

    if (!car || !p || !k || !dk || !g || !w || !rq) return -1;

    /* FUN_0043A471 (0x00439CAF): a wheel off the ground spins down, 0.01 per
     * step, 1.0 with the brake on - the front only in neutral, the rear in
     * any gear - then, in gear, the revs follow the wheels. */
    {
        float a = k->wheel_spin_down[car->brake != 0 ? 1 : 0];
        if (car->front_airborne != 0 && car->gear == 0)
            car->fwhlspd = toward_zero(car->fwhlspd, a);
        if (car->drag_gate_q != 0)                    /* rear axle airborne */
            car->rwhlspd = toward_zero(car->rwhlspd, a);
        engine_speed(car, g);
    }

    /* FUN_0043CF2A / FUN_0043CF39: in neutral, or in gear with every driven
     * wheel airborne, the engine free-revs; in the second case the wheels are
     * then slaved to it. */
    if (car->gear == 0) {
        sim_engine_freerev_q(car, p, k, dk, g, w, rq);
    } else if (car->freerev != 0) {
        sim_engine_freerev_q(car, p, k, dk, g, w, rq);
        wheel_speed_from_revs(car, g);
    }

    /* 0x00439CBE: off the brake the revs do not fall below idle,
     * [0x5C6E30] = [0x5C6CEC + class*4], FCOMPP / JAE.  0x00439CE9: nor rise
     * past [0x5C6E3C] = maxrevs * 1.2, FCOMPP / JB. */
    idle = k->idle_revs[car->drive_class != 0 ? 1 : 0];
    if (car->brake == 0 && car->revs < idle) car->revs = idle;
    cap = Xst(Xmul(X(car->maxrevs), X(k->overrev_scale)));
    if (!(car->revs < cap)) car->revs = cap;

    /* 0x00439D0B: and in gear the wheels are re-derived from the revs - every
     * step, so a wheel speed always passes through revs and back. */
    wheel_speed_from_revs(car, g);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* FUN_0043C48C - the tyre force model.      */
/* ------------------------------------------------------------------------ */

void sim_tyre_forces_q(car_state *car, const car_params *p,
                       const sim_consts *k, const dt_consts *dk,
                       const dt_gears *gears, const sim_surface *surf,
                       sim_world *w)
{
    const int   fourwd = (car->drive_class == 1);
    const float K      = k->grip_k;          /* [0x005C78DC] = 4.0 */
    const float dt     = k->dt;
    int i;

    /* ---- 0. two attitude scalars from the orientation matrix ------------
     * car0+0x0B8 and car0+0x0D8 are matrix[1] and matrix[9] - the struct's
     * matrix starts at 0x0B4. */
    w->tiltx  = clampf(1.0f - fabsf(car->matrix[1]), 0.1f, 1.0f);
    w->tiltz  = clampf(1.0f - fabsf(car->matrix[9]), 0.1f, 1.0f);
    w->tiltz2 = clampf(2.0f * w->tiltz - 1.0f, 0.0f, 1.0f);  /* [0x5C6DF0]; F12D's */

    if (car->freerev != 0) goto tail;
    /* Not a skip: sim_engine_freerev_slip_q (0x0043CF39) sees the same flag
     * later in the same step and runs the engine with no load, then re-slaves
     * FWHLSPD/RWHLSPD to the revs.  A SUBSTITUTION -. */

    /* ---- 1. drive torque ------------------------------------------------ */
    if (car->gear == 0) w->torqu = 0.0f;
    {
        w->tracspd = sim_traction_limit(k, car->czspd);
        if (car->fwhlspd > car->czspd + w->tracspd)
            car->fwhlspd = car->czspd + w->tracspd;
    }

    /* ---- 2. longitudinal grip, per axle --------------------------------
     * sum over the axle of mu x N, times dt (making it an impulse), times
     * mass, times the axle's DAMAGE EFFICIENCY.  N is force record i's +0x04,
     * the vertical load sim_apply_weight put there. */
    /* 0x0043C5E4..0x0043C646: one FSTP each.  The selector is car0+0x63C,
     * the brake input (it is the FILD of step 5). */
    {
        const float *g = (car->brake != 0) ? surf->brake : surf->accel;
        w->gripzf = Xst(Xmul(Xmul(Xmul(Xchs(Xadd(Xmul(X(g[0]), X(w->force[0].f[1])),
                                                 Xmul(X(g[1]), X(w->force[1].f[1])))),
                                       X(dt)), X(w->mass)), X(car->tyregripf)));
        w->gripzb = Xst(Xmul(Xmul(Xmul(Xchs(Xadd(Xmul(X(g[2]), X(w->force[2].f[1])),
                                                 Xmul(X(g[3]), X(w->force[3].f[1])))),
                                       X(dt)), X(w->mass)), X(car->tyregripr)));
    }

    /* ---- 3. lateral grip: two estimates, larger magnitude wins ---------- */
    w->gripx1 = -(surf->slide[0] + surf->slide[1] + surf->slide[2] + surf->slide[3])
                * w->force[0].f[1] * dt * w->mass;
    w->gripx2 = (((float)car->throttle + 256.0f) / 256.0f)
                * fabsf(car->cxspd * surf->mean_slide) * w->mass;
    if (w->gripx2 > w->gripx1 * 2.5f) w->gripx2 = w->gripx1 * 2.5f;
    w->gripx = (fabsf(w->gripx2) >= fabsf(w->gripx1)) ? w->gripx2 : w->gripx1;

    /* ---- 4. torque onto the wheel speeds --------------------------------
     * THE DRIVETRAIN CLASS IS SETTLED HERE AND NOWHERE ELSE: class 1 splits
     * drive 50/50 between the axles, class 0 sends all of it to the front.
     * That is 4WD against FWD from the code alone (then
     * corroborates it from the 24 named cars). */
    /* 0x0043C729..0x0043C792: torqu * [0.5 *] dt * K + wheel, one FSTP. */
    if (car->gear != 0 && fourwd) {
        car->fwhlspd = Xst(Xadd(Xmul(Xmul(Xmul(X(w->torqu), X(0.5f)), X(dt)), X(K)),
                                X(car->fwhlspd)));
        car->rwhlspd = Xst(Xadd(Xmul(Xmul(Xmul(X(w->torqu), X(0.5f)), X(dt)), X(K)),
                                X(car->rwhlspd)));
    }
    if (!fourwd) {
        car->fwhlspd = Xst(Xadd(Xmul(Xmul(X(w->torqu), X(dt)), X(K)),
                                X(car->fwhlspd)));
    }
    engine_speed(car, gears);                         /* 0x0043C798 */

    /* ---- 5. brakes, split front/rear by the brake balance ---------------
     * p->brake_decel is the car record's +0x3C already multiplied by gravity
     * on load, so it is a deceleration in m/s^2. */
    /* 0x0043C79D and 0x0043C846: (1 - bbal) * decel * FILD brake / 256 * dt
     * * eff * K, one FSTP each.  The 256 is [0x0057B0B0]. */
    {
        float bf = Xst(Xmul(Xmul(Xmul(Xdiv(Xmul(Xmul(Xsub(X(1.0f), X(w->bbal)),
                                                     X(p->brake_decel)), Xi(car->brake)),
                                           X(256.0f)), X(dt)), X(car->brakeefff)), X(K)));
        float br = Xst(Xmul(Xmul(Xmul(Xdiv(Xmul(Xmul(X(w->bbal), X(p->brake_decel)),
                                                Xi(car->brake)),
                                           X(256.0f)), X(dt)), X(car->brakeeffr)), X(K)));
        /* Clamped so braking can never push a wheel through zero and spin it
         * backwards - and a wheel already at zero is not touched. */
        car->fwhlspd = toward_zero(car->fwhlspd, bf);
        car->rwhlspd = toward_zero(car->rwhlspd, br);
    }

    /* ---- 6. handbrake, rear only ---------------------------------------- */
    {
        /* 0x0043C8ED: decel * FILD handbrake / 256 * dt * K * mass. */
        float hb = Xst(Xmul(Xmul(Xmul(Xdiv(Xmul(X(p->handbrake_decel), Xi(car->handbrake_ramp)),
                                           X(256.0f)), X(dt)), X(K)), X(w->mass)));
        car->rwhlspd = toward_zero(car->rwhlspd, hb);
        if (car->handbrake_ramp != 0
            && fabsf(car->fwhlspd + car->rwhlspd) < 1.0f)
            car->fwhlspd = car->rwhlspd = 0.0f;
    }
    engine_speed(car, gears);                         /* 0x0043C9C7 */

    /* ---- 7. slip, per axle ---------------------------------------------- */
    w->wgripzb = fabsf(car->rwhlspd - car->czspd);
    w->wgripzf = fabsf(car->fwhlspd - car->czspd);

    /* FUN_0043F12D (called at 0x0043C9F4): grip that grows with slip.
     *     t = [0x5C6C24 | 0x5C6C28 by class] * (slide[0]+..+slide[3])  ; FSTP
     *     gripzf += wgripzf * t * tiltz2                               ; FSTP
     *     gripzb += wgripzb * t * tiltz2                               ; FSTP
     * [0x005C6DF0] is tiltz2, so step 0's third scalar has this reader.
     * left both helpers out as "likely skid audio"; read
     * them as grip arithmetic and the port was never told. */
    {
        sim_x87 sum = sim_x87_add(sim_x87_add(sim_x87_add(
                          sim_x87_from_float(surf->slide[0]),
                          sim_x87_from_float(surf->slide[1])),
                          sim_x87_from_float(surf->slide[2])),
                          sim_x87_from_float(surf->slide[3]));
        float t = sim_x87_to_float(sim_x87_mul(
                      sim_x87_from_float(k->slip_grip_scale[fourwd ? 1 : 0]), sum));
        sim_x87 tz = sim_x87_from_float(w->tiltz2);
        w->gripzf = sim_x87_to_float(sim_x87_add(sim_x87_mul(sim_x87_mul(
                        sim_x87_from_float(w->wgripzf), sim_x87_from_float(t)), tz),
                        sim_x87_from_float(w->gripzf)));
        w->gripzb = sim_x87_to_float(sim_x87_add(sim_x87_mul(sim_x87_mul(
                        sim_x87_from_float(w->wgripzb), sim_x87_from_float(t)), tz),
                        sim_x87_from_float(w->gripzb)));
    }

    /* FUN_0043F1A5 (0x0043C9F9): below 20 m/s both grips are scaled by
     *     f = (|czspd| / 20 + 1) * 0.5                                 ; FSTP
     * from 0.5 at rest to 1 at 20 m/s.  FCOMP / SHR AH,1 / JAE skips it on
     * C0 clear, so an unordered (NaN) speed is scaled too. */
    {
        float a = fabsf(car->czspd);
        if (!(a >= k->lowspd_grip_ref)) {
            float f = sim_x87_to_float(sim_x87_mul(sim_x87_add(sim_x87_div(
                          sim_x87_from_float(a), sim_x87_from_float(k->lowspd_grip_ref)),
                          sim_x87_from_float(k->lowspd_grip_add)),
                          sim_x87_from_float(k->lowspd_grip_half)));
            w->gripzf = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(w->gripzf),
                                                     sim_x87_from_float(f)));
            w->gripzb = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(w->gripzb),
                                                     sim_x87_from_float(f)));
        }
    }

    /* ---- 8. attitude scaling -------------------------------------------- */
    w->gripx  *= w->tiltx;
    w->gripzf *= w->tiltz;
    w->gripzb *= w->tiltz;
    if (car->grip_half != 0) { w->gripzf *= 0.5f; w->gripzb *= 0.5f; }

    /* ---- 9. the friction-limited slip solve, per axle --------------------
     * The grip AVAILABLE is capped by the slip PRESENT.  That is what makes
     * this a friction model rather than a spring: a wheel turning at exactly
     * the car's speed generates no longitudinal force at all. */
    /* 0x0043CA61..0x0043CC..: per axle,
     *     avail = |(wheel - czspd) / K|                 ; FSTP
     *     u     = avail < grip ? avail : grip           ; FCOMPP / JB
     *     t     = (wheel - czspd) / K                   ; FSTP
     *     u     = |u|, negated when t < 0               ; FABS / FCOMP 0 / FCHS
     *     wheel = wheel - (K - 1) * u                   ; one FSTP
     *     rec   = u / dt * 0.5                          ; FST + FSTP
     * The FABS matters only for a negative grip, which the min passes on. */
    {
        float avail = fabsf(Xst(Xdiv(Xsub(X(car->fwhlspd), X(car->czspd)), X(K))));
        float t     = Xst(Xdiv(Xsub(X(car->fwhlspd), X(car->czspd)), X(K)));
        float f;
        w->ugrpzf = fabsf((avail < w->gripzf) ? avail : w->gripzf);
        if (!(t >= 0.0f)) w->ugrpzf = -w->ugrpzf;
        car->fwhlspd = Xst(Xsub(X(car->fwhlspd),
                                Xmul(Xsub(X(K), X(1.0f)), X(w->ugrpzf))));
        f = Xst(Xmul(Xdiv(X(w->ugrpzf), X(dt)), X(0.5f)));
        w->force[SIM_REC_LONG_FRONT    ].f[2] = f;
        w->force[SIM_REC_LONG_FRONT + 1].f[2] = f;

        avail = fabsf(Xst(Xdiv(Xsub(X(car->rwhlspd), X(car->czspd)), X(K))));
        t     = Xst(Xdiv(Xsub(X(car->rwhlspd), X(car->czspd)), X(K)));
        w->ugrpzb = fabsf((avail < w->gripzb) ? avail : w->gripzb);
        if (!(t >= 0.0f)) w->ugrpzb = -w->ugrpzb;
        car->rwhlspd = Xst(Xsub(X(car->rwhlspd),
                                Xmul(Xsub(X(K), X(1.0f)), X(w->ugrpzb))));
        f = Xst(Xmul(Xdiv(X(w->ugrpzb), X(dt)), X(0.5f)));
        w->force[SIM_REC_LONG_REAR    ].f[2] = f;
        w->force[SIM_REC_LONG_REAR + 1].f[2] = f;
    }

    /* ---- 10. the same, laterally ---------------------------------------- */
    {
        float t = toward_zero(car->cxspd, w->gripx);  /* cannot cross zero */
        w->usegripx = t - car->cxspd;              /* the lateral velocity killed */
        {
            float f = w->usegripx / dt * 0.25f;
            for (i = SIM_REC_LAT_FIRST; i < SIM_REC_LAT_FIRST + 4; i++)
                w->force[i].f[0] = f;              /* FORCE FG and FORCE RG */
        }
    }

    /* ---- 11. centre-of-gravity height ----------------------------------- */
    w->maxugrpx = w->gripx1;
    w->usecghx  = w->set_cgh;
    if (fabsf(w->usegripx) > w->maxugrpx) {
        float x = (fabsf(w->usegripx) - w->maxugrpx) * 0.5f + w->maxugrpx;
        w->usecghx = fabsf(x / w->usegripx) * w->set_cgh;
    }
    w->usecghz = w->set_cgh * 0.5f;

    /* ---- 12. drag -------------------------------------------------------
     * No vertical component, and it opposes the horizontal velocity. */
    w->force[SIM_REC_DRAG].f[0] = 0.0f;
    w->force[SIM_REC_DRAG].f[2] = 0.0f;
    {
        float v = sqrtf(car->cxspd * car->cxspd + car->czspd * car->czspd);
        if (v > surf->drag_vmin) {
            w->force[SIM_REC_DRAG].f[0] = -surf->drag_k / v * car->cxspd;
            w->force[SIM_REC_DRAG].f[2] = -surf->drag_k / v * car->czspd;
        }
    }

    /* ---- 13. hold the wheels before the start ---------------------------
     * 0x320 = 800 ticks = 8 s at the 10 ms quantum. */
    if (w->shaungmode != 1 && car->alivet_r < 0x320)
        car->fwhlspd = car->rwhlspd = 0.0f;

    /* ---- 14. fold in the yaw couple and consume it ---------------------- */
    w->force[SIM_REC_LAT_FIRST    ].f[0] += w->yaw_couple[0] * 0.5f;
    w->force[SIM_REC_LAT_FIRST + 1].f[0] += w->yaw_couple[0] * 0.5f;
    w->force[SIM_REC_LAT_FIRST + 2].f[0] += w->yaw_couple[1] * 0.5f;
    w->force[SIM_REC_LAT_FIRST + 3].f[0] += w->yaw_couple[1] * 0.5f;
    w->yaw_couple[0] = w->yaw_couple[1] = 0.0f;

tail:
    /* ---- TAIL, also reached when car->freerev != 0 ---------------------- */
    car->zspddif = car->fwhlspd - car->czspd;
    {
        float a = fabsf(car->zspddif) - 0.1f;
        float b = fabsf(car->cxspd)   - 1.5f;
        if (a < 0.0f) a = 0.0f;
        if (b < 0.0f) b = 0.0f;
        car->skidamt = sqrtf(a * a + b * b);
    }
    /* car0+0xF40 and car0+0xF44 also get a per-axle slip magnitude here.  They
     * are ABOVE THE REPLAY CUT (0x684) and so are not car_state members - see
     * car.h.  Not written, and named so the omission is deliberate. */
    (void)dk;
}

/* ------------------------------------------------------------------------ */
/* FUN_0043DA08 -> FUN_0045D636 and FUN_0043DB58 -> FUN_0045D6D7.    */
/* ------------------------------------------------------------------------ */

/* Read this session by asking what writes newspd -'s
 * "0043da08 0043db58  unread" row was the ONLY producer of the quantity both
 * integrators consume, and it had never been opened.
 *
 * THE SEVEN-RECORD EXTENSION AT 0x005C7784 IS SEPARATE from the 23-record
 * accumulator. FUN_0043DA19/0043DB69 integrate it inside the collision loop:
 * linear effects run on each pass; angular effects run only on pass zero.
 * owns that path.
 *
 * THREE GENUINELY DEAD FUNCTIONS, FOUND WHILE SCOPING THIS.  The image also
 * contains 0x0043DA3E and 0x0043DABA (right where the ticket's own scoping
 * snippet assumed the real arithmetic was, reading linearly past two `RET`s
 * it had not checked for) and 0x0043DB99 (a byte-for-byte duplicate of
 * FUN_0045D6D7's tail).  `ralimg.py calls` finds ZERO rel32 sites for any of
 * the three, anywhere in the image, and `ralimg.py refs` finds no pointer to
 * them either - not called indirectly, not called at all.  Not ported. */

void sim_force_to_velocity_q(car_state *car, const sim_consts *k, sim_world *w)
{
    int i;
    /* FUN_0045D636, called once per force record with ESI = that record.
     * Per record: impulse = force * dt, taken as CAR-LOCAL (every input the
     * tyre force model builds it from - fwhlspd, cxspd, czspd - already is);
     * FUN_0043D612 rotates it into world space with car->matrix (folded in
     * here rather than given its own entry point - its whole body is that
     * one rotation and it has no other caller); the result adds onto
     * newspd, which sim_integrate_position adds straight to world position
     * with no further rotation, so newspd must already be world-frame. */
    for (i = 0; i < SIM_FORCE_RECORDS; i++) {
        float imp[3];
        float world[3];
        imp[0] = w->force[i].f[0];
        imp[1] = w->force[i].f[1];
        imp[2] = w->force[i].f[2];
        sim_vec3_scale(imp, k->dt);
        sim_vec3_by_matrix(imp, car->matrix, world);
        car->newspd[0] += world[0];
        car->newspd[1] += world[1];
        car->newspd[2] += world[2];
    }

    /* cxspd/cyspd_q/czspd, derived ONCE after the loop rather than once per
     * record as the original does - see physics.h's comment on why that is
     * the bit-identical shortcut and not a guess: the transform is a pure
     * function of the CURRENT newspd and active_frame, both of which are
     * unaffected by how many times it has been evaluated before, so only
     * the last of the 23 evaluations the original makes is ever kept. */
    {
        float local[3];
        sim_vec3_by_matrix(car->newspd, w->active_frame, local);
        car->cxspd   = local[0];
        car->cyspd_q = local[1];
        car->czspd   = local[2];
    }
}

void sim_force_to_angular_q(car_state *car, const sim_consts *k, sim_world *w)
{
    int i;
    /* FUN_0045D6D7.  Per record, and it is a genuine 23-pass round trip -
     * see physics.h's comment on why this one cannot be collapsed the way
     * the linear twin above is: car->rotspd is both read and written on
     * every iteration. */
    for (i = 0; i < SIM_FORCE_RECORDS; i++) {
        float local[3];
        float torque[3];

        sim_vec3_by_matrix(car->rotspd, w->active_frame, local);   /* world -> car frame */
        sim_vec3_cross(w->force[i].f, w->force[i].p, torque);       /* r x F, FUN_0041FD06 */

        /* ONE FSTP over "-(torque * dt * inv_inertia) + local", per axis -
         * the x87 run is FMUL/FMUL/FCHS/FADD/FSTP with no intermediate
         * store. */
        local[0] = st((double)local[0]
                       - (double)torque[0] * k->dt * k->inv_inertia[0]);
        local[1] = st((double)local[1]
                       - (double)torque[1] * k->dt * k->inv_inertia[1]);
        local[2] = st((double)local[2]
                       - (double)torque[2] * k->dt * k->inv_inertia[2]);

        sim_vec3_by_matrix(local, car->matrix, car->rotspd);        /* car frame -> world, STORE */
    }
}

void sim_collision_force_to_velocity_q(car_state *car, const sim_consts *k,
                                       sim_world *w)
{
    int i;
    for (i = 0; i < SIM_COLLISION_FORCE_RECORDS; i++) {
        float impulse[3], world[3];
        impulse[0] = w->collision_force[i].f[0];
        impulse[1] = w->collision_force[i].f[1];
        impulse[2] = w->collision_force[i].f[2];
        sim_vec3_scale(impulse, k->dt);
        sim_vec3_by_matrix(impulse, car->matrix, world);
        car->newspd[0] += world[0];
        car->newspd[1] += world[1];
        car->newspd[2] += world[2];
    }
    {
        float local[3];
        sim_vec3_by_matrix(car->newspd, w->active_frame, local);
        car->cxspd = local[0];
        car->cyspd_q = local[1];
        car->czspd = local[2];
    }
}

void sim_collision_force_to_angular_q(car_state *car, const sim_consts *k,
                                      sim_world *w)
{
    int i;
    if (w->collision_passes != 0) return;
    for (i = 0; i < SIM_COLLISION_FORCE_RECORDS; i++) {
        float local[3], torque[3];
        sim_vec3_by_matrix(car->rotspd, w->active_frame, local);
        sim_vec3_cross(w->collision_force[i].f,
                       w->collision_force[i].p, torque);
        local[0] = st((double)local[0]
                       - (double)torque[0] * k->dt * k->inv_inertia[0]);
        local[1] = st((double)local[1]
                       - (double)torque[1] * k->dt * k->inv_inertia[1]);
        local[2] = st((double)local[2]
                       - (double)torque[2] * k->dt * k->inv_inertia[2]);
        sim_vec3_by_matrix(local, car->matrix, car->rotspd);
    }
}

static float sim_producer_store(double value)
{
    return (float)value;
}

static float sim_setup_lerp(int32_t value, const sim_consts *k,
                            float low, float high)
{
    double slider = (double)value / (double)k->bump_scale;
    return sim_producer_store(slider * ((double)high - (double)low)
                              + (double)low);
}

void sim_setup_steer_q(const car_state *car, const sim_consts *k,
                       sim_world *w)
{
    w->ste_k = sim_setup_lerp(car->ste_s_1, k, k->ste_k_low, k->ste_k_high);
    w->ste_j = sim_setup_lerp(car->ste_s_1, k, k->ste_j_low, k->ste_j_high);
}

void sim_setup_values(const car_state *car, const car_params *p,
                      const sim_consts *k, sim_world *w)
{
    sim_setup_steer_q(car, k, w);
    w->bbal = sim_setup_lerp(car->b_bal_1, k, k->bbal_low, k->bbal_high);
    w->susph = sim_setup_lerp(car->set_h_1, k, k->susph_low, k->susph_high);
    w->set_cgh = sim_setup_lerp(car->set_h_1, k,
                                k->set_cgh_low[p->drive_class],
                                k->set_cgh_high[p->drive_class]);
    w->susp_factor = sim_setup_lerp(car->set_s_1, k,
                                    k->susp_factor_low, k->susp_factor_high);
    w->suspt = sim_setup_lerp(car->set_s_1, k, k->suspt_low, k->suspt_high);
    w->suspd = sim_setup_lerp(car->set_s_1, k, k->suspd_low, k->suspd_high);
    w->bumpf = sim_setup_lerp(car->set_s_1, k, k->bumpf_low, k->bumpf_high);
}

/* FUN_0043D198's store at 0x0043D2E6: car+0x910 + 4i, the height of
 * wheel i's centre in the car's own frame, 16.16 - what the original's wheel
 * matrices (game/wheels.c) put the tyre at.  y6 is the model's point 6
 * height, [0x006C2D18], the same float the ground query starts a wheel's
 * contact from.  susph/suspt come from the car's own set h 1 / set s 1, as
 * sim_setup_values derives them for the step. */
void sim_wheel_heights(const car_state *car, const sim_consts *k, float y6,
                       int32_t out[4])
{
    float susph = sim_setup_lerp(car->set_h_1, k, k->susph_low, k->susph_high);
    float suspt = sim_setup_lerp(car->set_s_1, k, k->suspt_low, k->suspt_high);
    int i;

    for (i = 0; i < 4; i++) {
        sim_x87 y = sim_x87_add(sim_x87_sub(sim_x87_from_float(y6),
                                            sim_x87_from_float(susph)),
                                sim_x87_from_float(k->wheel_ride));
        sim_x87 t = sim_x87_mul(sim_x87_sub(sim_x87_from_float(car->susp[i]),
                                            sim_x87_from_float(k->susp_half)),
                                sim_x87_from_float(suspt));
        out[i] = sim_x87_fistp(sim_x87_mul(sim_x87_add(y, t),
                                    sim_x87_from_float(k->pos_scale)));
    }
}

void sim_derive_mass(const car_state *car, const sim_consts *k, sim_world *w)
{
    w->mass = car->rfn == 0 ? 1.0f : k->mass_nonzero_scale;
}

void sim_apply_weight(const sim_consts *k, sim_world *w)
{
    /* [0x685D94] is NOT a bump sample: its one writer is FUN_0045B233
     * (0x0045B27C: FILD [0x685D8C] / FDIV 65536.0 / FSTP), the lowest body
     * point's clearance in metres, left by D66C's call earlier in the step.
     * So the weight grows from 1x to 2x only as the body rises 1..5 m off
     * the road.  The port read points->bump[5], a 0..255 map sample, and ran
     * at 2x on the ground. */
    float bump = sim_producer_store((double)w->body_contact_clearance
                                    / (double)k->pos_scale);
    float factor;
    float world_force[3];
    int i;

    w->weight_clearance = bump;
    if (bump < k->load_bump_low) bump = k->load_bump_low;
    if (bump > k->load_bump_high) bump = k->load_bump_high;
    factor = sim_producer_store(((double)bump - (double)k->load_bump_low)
                                * (double)k->load_bump_step
                                + (double)k->load_bump_low);
    world_force[0] = 0.0f;
    world_force[1] = sim_producer_store(-((double)factor * (double)k->gravity
                                          * (double)w->mass
                                          * (double)k->load_bump_step));
    world_force[2] = 0.0f;

    for (i = 0; i < 4; i++)
        sim_vec3_by_matrix(world_force, w->active_frame, w->force[i].f);

    /* 0x0043D96B..0x0043D9C6: the four application points are the corners
     * (+-1, 0, +-1) - FLD1 into rec1 x, rec3 x, rec0 z, rec1 z, FCHS into
     * rec0 x, rec2 x, rec2 z, rec3 z; y stays D859's zero - each rotated
     * through car+0x9CC by D5DC like the forces. */
    {
        static const float corner[4][3] = {
            { -1.0f, 0.0f,  1.0f }, {  1.0f, 0.0f,  1.0f },
            { -1.0f, 0.0f, -1.0f }, {  1.0f, 0.0f, -1.0f } };
        for (i = 0; i < 4; i++)
            sim_vec3_by_matrix(corner[i], w->active_frame, w->force[i].p);
    }
}

void sim_apply_downforce(const car_state *car, const car_params *p,
                         const sim_consts *k, sim_world *w)
{
    float speed = car->czspd;
    float drag;

    if (car->drag_gate_q != 0) return;
    drag = sim_producer_store((double)speed * (double)speed
                              * (double)k->drag_speed_scale
                              + (double)p->drag);
    w->force[4].f[1] = sim_producer_store(-((double)drag * (double)k->gravity));
    w->force[4].p[2] = -1.0f;
}

void sim_apply_body_drag_q(const car_state *car, const sim_consts *k,
                           sim_world *w)
{
    /* 0x0043D79E..0x0043D858, three identical blocks on car+0x1A4/1A8/1AC:
     * FLD v / FMUL ST0 / FMUL [5.0] / FDIV [10000.0] / FSTP rec5.f[a];
     * FLD rec5.f[a] / FABS / FLD v / FCOMP 0.0 / JC (v < 0) skips FCHS /
     * FSTP rec5.f[a].  Opposes the velocity; v == 0 is not < 0, so -|f|. */
    const float v[3] = { car->cxspd, car->cyspd_q, car->czspd };
    int a;

    for (a = 0; a < 3; a++) {
        float f = sim_producer_store((double)v[a] * (double)v[a]
                                     * (double)k->body_drag_num
                                     / (double)k->body_drag_den);
        f = fabsf(f);
        w->force[5].f[a] = v[a] < k->zero ? f : -f;
    }
}

void sim_suspension_update(car_state *car, const sim_consts *k,
                           const sim_ground *ground, sim_world *w)
{
    const sim_collision_points *points = ground->in.pts;
    int i;

    for (i = 0; i < 4; i++) {
        int32_t neg_clear = ground->state->clear[i] == INT32_MIN
                          ? INT32_MIN : -ground->state->clear[i];
        float previous = car->susp[i];
        float state = sim_producer_store((double)neg_clear / (double)k->pos_scale
                                         / (double)w->suspt);
        float spring, damping;

        /* 0x0043D19D: FLD susp[i] / FST [0x005D6E4C] / FSTP car+0x610[i]. */
        car->susp_prev[i] = previous;
        if (state < k->zero) state = k->zero;
        if (state >= k->load_bump_low) state = k->load_bump_low;
        car->susp[i] = state;

        spring = sim_producer_store(-((double)state
                                      * (double)k->suspension_force_scale
                                      * (double)w->force[i].f[1]));
        damping = sim_producer_store(((double)state - (double)previous)
                                     * (double)w->suspd);
        if (damping < k->zero)
            damping = sim_producer_store((double)damping
                                         * (double)w->susp_factor);
        /* 0x0043D27F: FCOMP 0.0 / AND AH,0x40 / JNZ skips the add when the
         * state IS zero - the damping is added to every compressed wheel.
         * The port had the test inverted. */
        if (state != k->zero)
            spring = sim_producer_store((double)spring + (double)damping);
        w->force[6 + i].f[1] = spring;

        w->force[6 + i].p[0] = points->x[i];
        w->force[6 + i].p[1] = points->y[i];
        w->force[6 + i].p[2] = points->z[i];
        w->force[11 + i].p[0] = points->x[i];
        w->force[11 + i].p[1] = -k->lateral_point_y;
        w->force[11 + i].p[2] = points->z[i];
        w->force[15 + i].p[0] = points->x[i];
        w->force[15 + i].p[1] = -k->longitudinal_point_y;
        w->force[15 + i].p[2] = points->z[i];
    }
    w->force[10].p[1] = k->zero;
}

/* ------------------------------------------------------------------------ */
/* FUN_00439B37 - the per-car step.'s order.        */
/* ------------------------------------------------------------------------ */

/* FUN_0043A2D3, the spiral search for a free position (0x00439DDD: after
 * the guard, when all passes retried). is the
 * reading, 0x0043A2D3..0x0043A3DD whole.
 *
 *   [0x5C7958] = 0x5A     two absolute references, both writes: not modelled
 *   newspd *= 0.9         FUN_00420010, three roundings
 *   C100                  back to the step's snapshot
 *   B233                  the lowest body point; logged, no other reader
 *   ebp = (car+0x08 & 0xF) + 0x70   the tick counter as the step found it
 *   do { [0x5C746C] = 0
 *        x += spiral_x[ebp & 0xF] << ((ebp >> 4) + 1); z likewise
 *        45AE5D, B28F, B30E, D16A, BCE8      - no CB48, no D89A
 *        [0x5C746C] == 0 -> placed
 *   } while (++ebp != 0x100)
 *   FUN_0043D4B3; RET                        - counted; sim_car_step runs
 *                                              the reset after
 *
 * A probe accumulates onto the position unless a response rolls it back
 * through C100, which every response that sets [0x5C746C] does, so a
 * placement is the snapshot plus ONE probe's offset: run 21 shows exactly
 * that on all nine of its searches (s12).  The
 * current car's velocity is thrown away afterwards by A28D's restore; D16A's
 * exchanges with the OTHER car, one per overlapping probe corner, are not.
 * The offsets are IMUL, the low 32 bits, then a wrapping ADD. */
int sim_spiral_search_q(car_state *car, const sim_consts *k,
                        sim_ground *g, const float *sin_table, sim_world *w,
                        const sim_collision_snapshot *snapshot,
                        const sim_roster *roster)
{
    uint32_t ebp, first;
    int32_t hits_before = w->collision_car_hits;
    int rc;

    sim_vec3_scale(car->newspd, k->spiral_speed_scale);
    sim_collision_rollback_q(car, g->state, w, snapshot);
    rc = sim_body_contact_candidate_q(car, g, w);
    if (rc != 0) return rc;
    first = ((uint32_t)car->alivet & 0xFu) + 0x70u;
    ebp = first;
    memset(w->collision_spiral_why, 0, sizeof(w->collision_spiral_why));
    do {
        uint32_t idx = ebp & 0xFu, scale = 1u << ((ebp >> 4) + 1u);
        uint8_t *why = &w->collision_spiral_why[ebp - first];
        int32_t before;
        w->collision_retry_flag = 0;
        car->pos[0] = (int32_t)((uint32_t)car->pos[0]
                                + (uint32_t)k->spiral_x[idx] * scale);
        car->pos[2] = (int32_t)((uint32_t)car->pos[2]
                                + (uint32_t)k->spiral_z[idx] * scale);
        /* FUN_0045AE5D: the query alone, not D66C. */
        rc = sim_ground_query_contacts(g->geom, car, &g->in, g->state);
        if (rc != 0) return rc;
        /* B28F and B30E end in D66C when they respond (the loop's order). */
        before = w->collision_retry_flag;
        rc = sim_missed_point_collision_q(car, k, g, sin_table, w, snapshot);
        if (rc < 0) return rc;
        if (rc > 0) {
            rc = sim_collision_refresh_ground_q(car, g, w);
            if (rc != 0) return rc;
        }
        if (w->collision_retry_flag != before) *why |= 1u;
        before = w->collision_retry_flag;
        rc = sim_polygon_transition_collision_q(car, k, g, sin_table, w,
                                                snapshot);
        if (rc < 0) return rc;
        if (rc > 0) {
            rc = sim_collision_refresh_ground_q(car, g, w);
            if (rc != 0) return rc;
        }
        if (w->collision_retry_flag != before) *why |= 2u;
        before = w->collision_retry_flag;
        rc = sim_car_overlap_q(car, k, g->state, w, snapshot, roster);
        if (rc < 0) return rc;
        w->collision_spiral_car_hits = w->collision_car_hits - hits_before;
        if (w->collision_retry_flag != before) *why |= 4u;
        before = w->collision_retry_flag;
        rc = sim_object_collision_q(car, k, g, sin_table, w, snapshot);
        if (rc < 0) return rc;
        if (w->collision_retry_flag != before) *why |= 8u;
        if (w->collision_retry_flag == 0) {
            /* 0x0043A381: B233 again; a body point below the road lifts
             * the car by it and 0x800 more (CMP [min],0 / JNS). */
            rc = sim_body_contact_candidate_q(car, g, w);
            if (rc != 0) return rc;
            if (w->body_contact_clearance < 0)
                car->pos[1] = (int32_t)((uint32_t)car->pos[1]
                                        - (uint32_t)w->body_contact_clearance
                                        + 0x800u);
            /* "porky" ebp, [0x5C6E50], [0x685D8C]: logged, suppressed in a
             * live race.  Then [0x5C7470]++ and FUN_0043DC70 once more. */
            w->collision_spiral_probe = (int32_t)ebp;
            ++w->collision_spiral_found;
            sim_divergence_guard(car, k, w->speed_cap);   /* 0x0043A3D8 */
            return 0;
        }
    } while (++ebp != 0x100u);
    ++w->collision_spiral_lost;                 /* FUN_0043D4B3: the caller */
    return 0;
}

int sim_spiral_probe_of_move_q(const car_state *before, const car_state *after,
                               const sim_consts *k)
{
    int32_t dx = (int32_t)((uint32_t)after->pos[0] - (uint32_t)before->pos[0]);
    int32_t dy = (int32_t)((uint32_t)after->pos[1] - (uint32_t)before->pos[1]);
    int32_t dz = (int32_t)((uint32_t)after->pos[2] - (uint32_t)before->pos[2]);
    uint32_t e = ((uint32_t)before->alivet & 0xFu) + 0x70u;

    if (dy != 0) return -1;
    for (; e != 0x100u; e++) {
        uint32_t sc = 1u << ((e >> 4) + 1u);
        if ((int32_t)((uint32_t)k->spiral_x[e & 0xFu] * sc) == dx
            && (int32_t)((uint32_t)k->spiral_z[e & 0xFu] * sc) == dz)
            return (int)e;
    }
    return -1;
}

int sim_car_step(car_state *car, const car_params *p, const sim_consts *k,
                 const dt_consts *dk, const sim_ground *ground,
                 const car_geometry *geom, const float *sin_table,
                 sim_world *w, const sim_roster *roster)
{
    dt_gears gears;
    const sim_surface *surf = &w->surf;
    sim_ground step_ground = *ground;
    sim_collision_snapshot collision_snapshot;
    sim_damage_globals *dmg = (roster && roster->damage) ? roster->damage
                                                         : &w->damage;
    int rc;

    /* A roster FUN_0045D16A would refuse is refused before the car is
     * touched, not in the middle of the collision loop. */
    if (sim_roster_check_q(roster) != 0) return -1;

    /* FUN_00439B37's FIRST instruction is CALL 0x0043F3E0: the car's own
     * LFSR, car+0x0C (36,753 of 36,753 on the capture). */
    (void)sim_car_rnd_next_q(&car->rnd);

    /* FUN_0043D644, car+0x6F0 = (view is 2 or 3).  FUN_0043D66C
     * calls it first, at 0x00439BF8 before any reader of +0x6F0, and the
     * view does not change inside the step before the camera runs. */
    if (w->camera)
        w->camera_view_2_3 = (w->camera->cam.view == 2
                              || w->camera->cam.view == 3);

    w->collision_b796_seen = 0;
    w->collision_peak_normal_speed = 0.0f;
    w->collision_car_hits = 0;
    w->collision_car_first_pass = -1;
    w->collision_spiral_probe = -1;
    w->collision_spiral_car_hits = 0;
    w->collision_active = 0;
    car->collision_peak_normal_speed = 0.0f;
    car->collision_active = 0;

    /* D859 clears all six floats of all 23 records (D86A, stride 0x20).
     * Records 0..3's points are D8AB's to write again, and record 5
     * is D79E's: nothing survives the clear. */
    memset(w->force, 0, sizeof(w->force));

    /* FUN_004206D8 - the ladder is REBUILT FROM THE CAR RECORD EVERY STEP, so
     * a mid-race change to the gearing slider takes effect at once. */
    dt_build_gear_ratios(p, dk, car->set_g_1, &gears);

    /* FUN_0043A22F at 0x00439B46: car+0x1C4 = |newspd|, the value C535's
     * class 1/9 speed test reads inside the collision loop. */
    sim_speed_len_q(car);

    /* SAVE newyspd, for the vertical-velocity delta at the end of the step. */
    {
        float saved_newyspd = car->newspd[1];

        /* FUN_0043D033's first instruction, CALL FUN_00439F90: the damage
         * model's consumer, on the damage the previous step left (session
         * 83, damage.c).  driver == 2 calls FUN_0043E2D3 instead: the AI
         * car's damage reset. */
        if (car->driver != 2) {
            sim_damage_efficiency_q(car, k, dmg);
        } else {
            sim_damage_reset_ai_q(car, k);
            w->damage_e2d3_due = 1;
        }

        /* FUN_0043D033 runs before the query and supplies its three ground
         * inputs as well as the values consumed by the force model. */
        sim_setup_values(car, p, k, w);
        step_ground.in.susph = w->susph;
        step_ground.in.suspt = w->suspt;

        /* 0x00439B50: FUN_004351C9 - the mass store FIRST, on rfn as the
         * step found it, then the race rules (race.c).  The mass was
         * derived further down until; nothing between here and there
         * writes rfn but the rule step, which is why it did not show - and
         * why it would on the finish step, where the rule step sets rfn. */
        sim_derive_mass(car, k, w);
        if (w->race) sim_race_rules_q(car, w->race, w->race_slot);

        sim_refresh_active_frame(w, car, k);

        /* FUN_0043D66C refreshes ground points and airborne gates before the
         * step's snapshotting FUN_0045B76A query. */
        {
            rc = sim_collision_refresh_ground_q(car, &step_ground, w);
            if (rc != 0) return rc;
        }

        /* FUN_0045B76A, the wheel-to-ground query -.  The original
         * calls it at 0x00439C02, two calls before the solver; nothing
         * between them (FUN_004533C8, unread, and the drivetrain tick) is
         * known to touch what it reads - matrix, pos, car+0x060, CPLEFT -
         * so it runs here, against the car as the step found it.  It reads
         * car->bump_q as the PREVIOUS step's solver left it. */
        {
            sim_contact contact[SIM_CONTACT_POINTS];
            rc = sim_ground_contacts(step_ground.geom, car, &step_ground.in,
                                     step_ground.state);
            if (rc != 0) return rc;
            sim_ground_to_contacts(step_ground.state, contact);

            /* 0x00439C0C, between the query and the solver. */
            sim_drivetrain_tick_q(car, w, w->shaungmode);

            /* FUN_0043BA75, the surface solver -.
             * section 4 puts it after the drivetrain tick and BEFORE
             * FUN_0043A16A, so it reads the wetness the PREVIOUS step left in
             * car0+0x280 (the port does not run FUN_0043A16A at all;
             * car->wetns is whatever the caller or the replay holds).  The
             * force clear above it is harmless ordering - the solver writes
             * none of the 23 records. */
            rc = sim_surface_solve_q(step_ground.table, step_ground.level, contact, p,
                                     w->bumpf, car, &w->surf);
            if (rc != 0) return rc;
        }
        /* FUN_0043A16A, after the solver -
         * wetns and the weather words, sampled where the camera is.  The
         * camera's own step comes later (0x00439E83), so this reads the
         * position the previous step left. */
        if (w->weather && w->camera)
            sim_weather_sample_q(w->weather, w->camera->cam.pos, car, &w->wx);
        rc = sim_collision_snapshot_save(car, &step_ground,
                                         &collision_snapshot);
        if (rc != 0) return rc;
        /* FUN_0043C0B0 reloads the saved car state and clears the retry flag. */
        w->collision_retry_flag = 0;

        /* FUN_004351C9 (the mass, ~~here~~ at 0x00439B50 above since),
         * D8AB and D9CC produce records 0..4. */
        sim_apply_weight(k, w);
        sim_apply_downforce(car, p, k, w);

        /* 0x00439C2F: the gearbox, FUN_0043A65E (it had never
         * been called, so all 129 of the capture's gear changes missed).
         */
        sim_gearbox_update_q(car, p, k, dk, &gears, w);

        /* 0x00439C34: the drive torque. */
        {
            float y, q;
            float *rq = (roster && roster->rev_q) ? roster->rev_q : &w->rev_q;
            /* FUN_0043B058, the rev limiter, sim_drive_torque's first call
             * (the port had gone straight to the curve, whose
             * x >= 1 clamp then returned the IDLE torque):
             *     FLD revs / FCOMP maxrevs / JB -> counter
             *     revs >= maxrevs: [car+0x1F0] = 5, Y = 0
             *     else counter != 0: DEC, Y = 0
             *     else: the curve, which writes Y and q
             * Y = 0 leaves q as the last curve call wrote it. */
            if (car->revs >= car->maxrevs) {
                car->rev_limit_cut = 5;                       /* 0x0043B06C */
                y = 0.0f;
                q = *rq;
            } else if (car->rev_limit_cut != 0) {
                car->rev_limit_cut--;
                y = 0.0f;
                q = *rq;
            } else {
                dt_torque_curve(p, dk, car->revs, gears.maxrevs, car->engpower, &y, &q);
                *rq = q;
            }
            w->rev_q = *rq;
            w->torqu = dt_drive_torque(dk, &gears, y, q, car->throttle, car->gear,
                                       &w->crank);
            if (car->shift_torque_cut != 0) w->torqu = 0.0f;
            w->torqu *= dt_torque_scale(dk, car->drive_class, car->set_g_1);
        }

        /* 0x00439C39..0x00439C9F: yaw term 3's input is THIS step's cxspd
         * (car+0x1A4), clamped to +-[0x5C6C08] - the in-place negation again.
         * FCOMPP / JB keeps v < 15; FCOMPP / JAE keeps v >= -15, so a NaN
         * lands on -15.  The port had never written car+0x1C8. */
        {
            float v = car->cxspd;
            if (!(v < k->clamp_rotspd)) v = k->clamp_rotspd;
            if (!(v >= -k->clamp_rotspd)) v = -k->clamp_rotspd;
            car->yaw_powr_in = v;
        }

        sim_yaw_model_q(car, p, k, surf, geom, sin_table, w);
        sim_tyre_forces_q(car, p, k, dk, &gears, surf, w);
        rc = sim_engine_after_tyres_q(car, p, k, dk, &gears, w,
                                      (roster && roster->rev_q) ? roster->rev_q
                                                                : &w->rev_q);
        if (rc != 0) return rc;

        /* FUN_0043D79E at 0x00439D10: record 5, the body's quadratic drag
         * (it had never been read). */
        sim_apply_body_drag_q(car, k, w);

        /* FUN_0043D198 consumes the just-computed clear[] and writes the
         * replay-visible suspension state and records 6..18's points. */
        sim_suspension_update(car, k, &step_ground, w);

        /* ~~FUN_0043D66C refreshes suspension-derived airborne gates and body
         * clearance before FUN_0045C90A writes force record 21~~ - session
         * 84: retail has NO D66C here.  Its eight call sites are the step
         * start (0x00439BF8), the loop (0x00439D62), the step end
         * (0x00439DFD) and five inside the collision responses; C90A calls
         * only B233, which sim_body_contact_q runs itself.  So car+0x248..
         * +0x254 keep the STEP-START values through C90A and C2BA, and the
         * capture agrees: C2BA's gate is the previous record's +0x254 (11 of
         * 11 airborne-transition steps; s4). */
        rc = sim_body_contact_q(car, k, &step_ground, w);
        if (rc < 0) return rc;

        sim_force_to_velocity_q(car, k, w);
        sim_force_to_angular_q(car, k, w);

        sim_integrate_position(car, k);
        sim_integrate_orientation(car, k);
        /* FUN_0043C2BA at 0x00439D33: the yaw pivot shift, which reads the
         * step-start active frame - the refresh comes after it. */
        sim_yaw_pivot_shift_q(car, geom, k, w);
        sim_refresh_active_frame(w, car, k);
        /* FUN_0043A3DE at 0x00439D3D: the binary angles, whose heading BCE8
         * rotates by. */
        sim_binary_angles_q(car, k, w);

        /* SAVE |velocity|, the speed the rest of the step may not exceed:
         * FUN_0041FFFB / FSTP [0x005C6E38] at 0x00439D42.  Until step 4b
         * this summed (x+y)+z; retail adds the last two squares first. */
        w->speed_cap = sim_vec3_len_q(car->newspd);
        w->collision_passes = 0;

        /* ==== the contact/collision iteration, 0x00439D5D..0x00439DE8 ======
         *
         * Retail's only exit test is [0x005C746C] != 0; every response that
         * wants another pass increments it (C100) - FUN_0045D16A's included,
         * once per inside corner (overlap.c). */
        {
            float saved_velocity[3] = { 0.0f, 0.0f, 0.0f };
            float saved_angular[3] = { 0.0f, 0.0f, 0.0f };
            uint8_t why_overflow = 0, *why;
            int32_t before;
        memset(w->collision_pass_why, 0, sizeof(w->collision_pass_why));
        for (;;) {
            /* FUN_0043D89A clears the seven extension records. */
            memset(w->collision_force, 0, sizeof(w->collision_force));
            w->collision_missed_point = 0;
            w->collision_missed_point_retry = 0;
            w->collision_missed_point_normal_speed = 0.0f;
            rc = sim_collision_refresh_ground_q(car, &step_ground, w);
            if (rc != 0) return rc;
            if (w->collision_passes == 0) {
                memcpy(w->collision_first_pos, car->pos, sizeof(car->pos));
                w->collision_first_clearance = w->body_contact_clearance;
                w->collision_first_index = w->body_contact_index;
                memcpy(w->collision_first_vel, car->newspd, sizeof(car->newspd));
            }
            if (w->collision_passes < 16) {
                w->collision_pass_clear[w->collision_passes] = w->body_contact_clearance;
                w->collision_pass_vy[w->collision_passes] = car->newspd[1];
            }
            why = w->collision_passes < 16 ? &w->collision_pass_why[w->collision_passes] : &why_overflow;
            before = w->collision_retry_flag;
            rc = sim_missed_point_collision_q(car, k, &step_ground, sin_table, w, &collision_snapshot);
            if (rc < 0) return rc;
            if (rc > 0) {
                rc = sim_collision_refresh_ground_q(car, &step_ground, w);
                if (rc != 0) return rc;
            }
            if (w->collision_retry_flag != before) *why |= 1u;
            before = w->collision_retry_flag;
            rc = sim_polygon_transition_collision_q(car, k, &step_ground,
                                                    sin_table, w,
                                                    &collision_snapshot);
            if (rc < 0) return rc;
            if (rc > 0) {
                rc = sim_collision_refresh_ground_q(car, &step_ground, w);
                if (rc != 0) return rc;
            }
            if (w->collision_retry_flag != before) *why |= 2u;
            before = w->collision_retry_flag;
            rc = sim_body_contact_loop_q(car, k, &step_ground, w,
                                         &collision_snapshot);
            if (rc < 0) return rc;
            if (w->collision_body_contact_retry) {
                rc = sim_collision_refresh_ground_q(car, &step_ground, w);
                if (rc != 0) return rc;
            }
            if (w->collision_retry_flag != before) *why |= 16u;
            before = w->collision_retry_flag;
            /* FUN_0045D16A, car versus car (0x00439D76) -.
             * Changes nothing without a second driven car in the roster. */
            rc = sim_car_overlap_q(car, k, step_ground.state, w,
                                   &collision_snapshot, roster);
            if (rc < 0) return rc;
            if (w->collision_retry_flag != before) *why |= 4u;
            before = w->collision_retry_flag;
            /* FUN_0045BCE8, object collision, extension record 0 -
             * step 4b.  Skipped when the caller gives no objects. */
            rc = sim_object_collision_q(car, k, &step_ground, sin_table, w,
                                        &collision_snapshot);
            if (rc < 0) return rc;
            if (w->collision_retry_flag != before) *why |= 8u;
            /* FUN_0043F037 saves the car-frame velocity before DA19/DB69. */
            sim_collision_save_cspd_q(car, w);
            sim_collision_force_to_velocity_q(car, k, w);
            sim_collision_force_to_angular_q(car, k, w);
            if (w->collision_retry_flag == 0) break;

            /* FUN_0043F058, then FUN_0043A243 (pass zero only), then the
             * SAME integrator pair, FUN_0043C2BA (0x00439DAC) after the
             * orientation, then the frame and FUN_0043A3DE. */
            sim_collision_impact_q(car, k, w);
            if (w->collision_passes == 0) {
                memcpy(saved_velocity, car->newspd, sizeof(saved_velocity));
                memcpy(saved_angular, car->rotspd, sizeof(saved_angular));
            }
            sim_integrate_position(car, k);
            sim_integrate_orientation(car, k);
            sim_yaw_pivot_shift_q(car, geom, k, w);
            sim_refresh_active_frame(w, car, k);
            sim_binary_angles_q(car, k, w);          /* 0x00439DB6 */
            w->collision_retry_flag = 0;
            ++w->collision_passes;
            /* JB: unsigned compare against [0x005C6E84]. */
            if ((uint32_t)w->collision_passes >= (uint32_t)k->passes) {
                /* Exhausted: FUN_0043DC70, then FUN_0043A2D3's spiral search
                 * for a free position (above; s17), then
                 * [0x005C6F04]++. */
                sim_divergence_guard(car, k, w->speed_cap);
                {
                    int32_t lost = w->collision_spiral_lost;
                    rc = sim_spiral_search_q(car, k, &step_ground, sin_table, w,
                                             &collision_snapshot, roster);
                    if (rc < 0) return rc;
                    /* All 144 probes overlapped: FUN_0043A2D3's last act is
                     * CALL FUN_0043D4B3, so the reset runs here. */
                    if (w->collision_spiral_lost != lost) {
                        w->d4b3_due = 1;
                        if (w->race) {
                            sim_place_ctx pc = { p, k, sin_table, &step_ground, dmg };
                            if (sim_car_reset_q(car, w, w->race, &pc) < 0) return -1;
                        }
                    }
                }
                ++w->collision_exhausted;
                break;
            }
        }
        /* FUN_0043A28D restores A243's snapshot and FUN_0043C04C re-derives
         * the car-frame velocity; the guard runs only on this path. */
        if (w->collision_passes != 0) {
            float local[3];
            memcpy(car->newspd, saved_velocity, sizeof(saved_velocity));
            memcpy(car->rotspd, saved_angular, sizeof(saved_angular));
            sim_vec3_by_matrix(car->newspd, w->active_frame, local);
            car->cxspd = local[0];
            car->cyspd_q = local[1];
            car->czspd = local[2];
            sim_divergence_guard(car, k, w->speed_cap);
        }
        }
        /* 0x00439DFD: FUN_0043D66C after the loop, on every path. */
        rc = sim_collision_refresh_ground_q(car, &step_ground, w);
        if (rc != 0) return rc;

        /* 0x00439E02: FUN_0043D47E, the counter D66C clears on the ground:
         *     driver (car+0x25C) == 2 or 0: car+0x1E8 = 0
         *     else: ++car+0x1E8, and at >= 700 (JB, unsigned) FUN_0043D4B3
         * ~~D4B3 (three callers) is NOT ported~~: place.c's
         * sim_car_reset_q, run when the step has a race context; without one
         * the step still records that it was due. */
        if (car->driver == 2 || car->driver == 0) {
            car->freerev_clear_1e8_q = 0;
        } else {
            car->freerev_clear_1e8_q++;
            if ((uint32_t)car->freerev_clear_1e8_q >= 700u) {
                w->d4b3_due = 1;
                if (w->race) {
                    sim_place_ctx pc = { p, k, sin_table, &step_ground, dmg };
                    if (sim_car_reset_q(car, w, w->race, &pc) < 0) return -1;
                }
            }
        }

        /* 0x00439E07 FUN_0043C14D (soundfeed.c): the sound revs
         * car+0x1E4, the speed car+0x294 and 0x0045A626's car+0x28C/+0x290
         * on every step; with a race, the car's feedback record (record
         * [0x006CE084] & 7) and the shift triggers.  The camera it is heard
         * from is the race's (the player's, numpl 1), else the car's own.
         * 0x00439E0C FUN_0043B1F5, below: the two wheel spin angles
         * (the rest of what follows it, 0x0043B254.., is another
         * function's).
         * 0x00439E11: FUN_0043F9E5, the route tracker and the checkpoint
         * crossing (race.c). */
        /* FUN_0043B1F5: angle += FISTP(wheelspd / radius * dt * 65536.0 / pi),
         * the front pair from car+0x1B8 and the rear from +0x1BC, the sum
         * left to wrap (the sin table takes the low 16 bits). */
        car->wheel_spin_f += (int32_t)sim_x87_fistp(sim_x87_mul(sim_x87_mul(
            sim_x87_mul(sim_x87_div(sim_x87_from_float(car->fwhlspd),
                                    sim_x87_from_float(k->wheel_radius)),
                        sim_x87_from_float(k->dt)),
            sim_x87_from_float(k->pos_scale)), sim_x87_from_float(k->inv_pi)));
        car->wheel_spin_r += (int32_t)sim_x87_fistp(sim_x87_mul(sim_x87_mul(
            sim_x87_mul(sim_x87_div(sim_x87_from_float(car->rwhlspd),
                                    sim_x87_from_float(k->wheel_radius)),
                        sim_x87_from_float(k->dt)),
            sim_x87_from_float(k->pos_scale)), sim_x87_from_float(k->inv_pi)));
        {
            sim_sound_feed *sf = NULL;
            sim_sound_view sv;
            if (w->race) {
                sf = &w->race->snd[w->race_slot & 7u];
                sv = w->race->snd_view;
                if (!sv.pos && w->camera) sv.pos = w->camera->cam.pos;
            }
            sim_sound_step_q(car, k->idle_revs[car->drive_class != 0 ? 1 : 0],
                             w->surf.cls_a[4], w->camera_view_2_3, sf,
                             sf ? &sv : NULL);
        }
        if (w->race) sim_route_track_q(car, w, w->race, sin_table);

        /* 0x00439E16 FUN_0043FC56 (pacenote.c): the co-driver's
         * trigger.  It keeps l p plyd (+0x09C, in the record) and queues
         * sound requests; it moves nothing else.  0x00439E1B: FUN_0043F3BA,
         * msgtimer (race.c): above the record, so it moves no
         * replayed word. */
        if (w->race) sim_pacenote_trigger_q(car, w->race, w->race_slot);
        if (w->race) sim_message_timer_q(&w->race->msg[w->race_slot & 7u]);

        /* 0x00439E20: FUN_00447FA6, not ported.  0x00439E25: car+0x1C0, the
         * mean wheel speed - after the feed has read last step's. */
        sim_sound_wheel_mean_q(car);

        /* THE STEP'S VERTICAL VELOCITY DELTA, [0x006CD570] (car+0x90C) - above
         * the replay cut, so it is not a car_state member: the
         * camera's shake reads it (FUN_0044DEE4, views 1-3). */
        float yvel_delta = (float)((double)car->newspd[1] - saved_newyspd);

        /* FUN_0043A22F again, at 0x00439E4F. */
        sim_speed_len_q(car);

        /* 0x00439E54: FUN_004C2AE6, the three damage accumulators (damage_acc.c): the car wears.  car+0x930 / +0x958 are the surface
         * solver's point 4.  0x00439E59: FUN_0043F4F8, the parts-left
         * evaluator (parts_left.c): a tyre's blowout goes into the
         * car's feedback record, heard from the car's OWN camera.
         * 0x00439E5E / 0x00439E63:
         * FUN_004C773F and FUN_004C9154, the damage warnings and their pop
         * into msgtopre (damage_warn.c).  0x00439E68: FUN_0043E235,
         * the stall to neutral after a collision.  0x00439E6D:
         * FUN_0043E290 tests car+0x234 and returns either way. */
        sim_damage_update_q(car, w->surf.cls_a[4], w->surf.f2c[4], k, dmg);
        {
            sim_sound_view bv;
            sim_sound_feed *bf = NULL;
            if (w->race) {
                bf = &w->race->snd[w->race_slot & 7u];
                bv = w->race->snd_view;
                bv.pos = w->camera ? w->camera->cam.pos : NULL;
            }
            w->parts_off |= sim_parts_left_q(car, bf, bf ? &bv : NULL);
        }
        sim_damage_warn_q(car);
        if (w->race) sim_damage_warn_pop_q(car, &w->race->msg[w->race_slot & 7u]);
        sim_gearbox_stall_q(car, k, dk);

        /* 0x00439E83: FUN_0044DEE4, the camera (camera.c), when
         * [0x005F4540 + slot*4] != -1 - the caller gives a camera only to
         * such a car.  Its queries set [0x005D31A0] = 0x40. */
        if (w->camera) {
            rc = sim_camera_step_q(w->camera, car, w->binary_angle,
                                   yvel_delta,
                                   step_ground.object_state
                                       ? &step_ground.object_state->rnd
                                       : NULL,
                                   step_ground.geom, sin_table);
            if (rc != 0) return rc;
        }

        /* 0x00439E92: FUN_0044CD28, the car's ground light (ground.h).
         * [0x005D31A0] is what the step's last query left: the camera's
         * 0x40 when it ran, else the contacts' 0x80.  The hint [0x005819C0]
         * is car+0xE00, the origin point's polygon this step. */
        sim_car_ground_light_q(car, step_ground.geom, step_ground.level,
                               step_ground.light,
                               step_ground.state
                                   ? step_ground.state->prev_poly[4] : -1,
                               w->camera ? SIM_GROUND_MASK_CAMERA
                                         : SIM_GROUND_MASK_CONTACT,
                               w->race ? w->race->fb4_q[w->race_slot & 7u] : 0);

        /* 0x00439E9C: FUN_0043F422, the octant damage - what the
         * engine's crush pass dents the car and cracks its windows from. */
        sim_car_octant_damage_q(car, k);

        /* 0x00439E72..0x00439EA6: nine calls; the camera's, 0x00439E92's
         * and 0x00439E9C's the only ones ported (above).  0x00439EAB:
         * FUN_00439EB6, the false start (race.c).  0x00439EB0:
         * FUN_00459101, message 206 on +0x274 - not ported. */
        if (w->race) {
            sim_place_ctx pc = { p, k, sin_table, &step_ground, dmg };
            if (sim_false_start_q(car, w, w->race, &pc) != 0) return -1;
        }
    }
    return 0;
}
