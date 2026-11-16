/* gearbox.c - the gearbox, FUN_0043A65E and everything it reaches; the stall
 * to neutral at the step's end, FUN_0043E235.  The gear
 * half of the tick's input stage, FUN_0043895C / FUN_00438208, was here too;
 * moved it into the whole stage, input.c.
 * has the reading; the summary.
 *
 * THE ORDER OF THE BYTES, NOT A RE-DESIGN
 * Every path below ends in its own RET in the original, so each label is a
 * function here and each function ends where the RET is.  The x87 notes say
 * where a value is stored (FSTP: rounded to float) and where it stays in the
 * register (an FCOMP on an unrounded product: sim_x87).
 *
 * Strategy 2, FUN_0043AABF (autom_1 == 2, the menus' fully automatic), is
 * here since, checked against run 23 (s3.5, s11).
 *
 * WHAT IS NOT HERE
 *  FUN_0043AABF's block at 0x0043AB97, which nothing references.
 *  FUN_0043F118, FUN_0043AD87's first call: it writes two process globals
 *     ([0x005C6E28] = alivet, [0x005C6E24] = 0), a 0-60 timer's start that
 *     FUN_0043F0EA reads.  Nothing in the car record.
 *  The scratch globals [0x005D6EA0], [0x005C6E74/78/7C] and [0x005C7970]:
 *     locals here. */

#include "physics.h"

#include <math.h>

/* CMP r/m32,0x320 / JS: the sign of the WRAPPED difference, not a signed
 * less-than (they differ only within 0x320 of INT32_MIN). */
static int js_0x320(int32_t v)
{
    return (int32_t)((uint32_t)v - 0x320u) < 0;
}

/* FCOMP m32 / FNSTSW / SHR AH,1: CF is C0, which is set for less-than AND
 * for unordered.  So JB is taken when below(a, b) and JAE when it is not.
 * The sign of the x87 difference is the sign of the exact difference (a
 * rounding never crosses zero, and the difference is zero only when equal). */
static int below(sim_x87 a, float b)
{
    sim_x87 d;
    if (a.cls == 2 || isnan(b)) return 1;
    d = sim_x87_sub(a, sim_x87_from_float(b));
    if (d.cls == 2) return 1;                 /* inf - inf: unordered too    */
    if (d.cls == 0 && d.m == 0) return 0;
    return d.neg;
}

static float ratio_of(const dt_gears *g, int32_t gear)
{
    return dt_gear_ratio(g, gear);
}

/* FUN_0043AAB4: MOV EAX,ECX / INC EAX / BT [gearbits],EAX.  CF = the bit,
 * so bit g+1 disables gear g (bit 0 is reverse). */
static int broken(const car_state *car, int32_t g)
{
    return (int)((car->gearbits >> (uint32_t)(g + 1)) & 1u);
}

/* FUN_0043AA8B: ECX = gear - 1 downward, skipping broken gears; reaching 0
 * is STC ("none"). */
static int next_down(const car_state *car, int32_t g, int32_t *out)
{
    int32_t c = g;
    for (;;) {
        c--;
        if (c == 0) return 0;
        if (!broken(car, c)) { *out = c; return 1; }
    }
}

/* FUN_0043AA9D: ECX = gear + 1 upward while ECX <= ngears (JBE, unsigned),
 * the first candidate tested before the bound. */
static int next_up(const car_state *car, const dt_gears *gr, int32_t g,
                   int32_t *out)
{
    uint32_t c = (uint32_t)g + 1u;
    for (;;) {
        if (!broken(car, (int32_t)c)) { *out = (int32_t)c; return 1; }
        c++;
        if (!(c <= (uint32_t)gr->ngears)) return 0;
    }
}

/* FUN_0043A5CB, sim_engine_speed. */
static void revs_from_wheels(car_state *car, const dt_gears *gr)
{
    if (car->gear == 0) return;
    car->revs = dt_engine_speed(gr, car->drive_class, car->fwhlspd,
                                car->rwhlspd, car->gear);
}

/* FUN_0043A603: revs = czspd / ratio[gear], FLD / FDIV / FSTP. */
static void revs_from_body(car_state *car, const dt_gears *gr)
{
    if (car->gear == 0) return;
    car->revs = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->czspd),
                    sim_x87_from_float(ratio_of(gr, car->gear))));
}

/* FUN_0043A626, sim_wheel_speed_from_revs. */
static void wheels_from_revs(car_state *car, const dt_gears *gr)
{
    if (car->gear == 0) return;
    car->fwhlspd = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(car->revs),
                       sim_x87_from_float(ratio_of(gr, car->gear))));
    if (car->drive_class == 1) car->rwhlspd = car->fwhlspd;
}

typedef struct gb {
    car_state        *car;
    const car_params *p;
    const sim_consts *k;
    const dt_consts  *dk;
    const dt_gears   *g;
    sim_world        *w;
} gb;

/* 0x0043A8CA: the completed shift's epilogue.  The same five stores close
 * the neutral, reverse and from-neutral paths, and FUN_0043E235. */
static void shift_done(car_state *car, const dt_consts *dk)
{
    car->shift_torque_cut = dk->cut_steps;            /* [0x005C6CD8], 5 */
    car->shift_cooldown   = 0x14;
    car->shift_result     = 1;
    car->gear_changed_q   = 1;
}

/* 0x0043A810: a refused downshift - the cooldown, NO torque cut, -1. */
static void shift_refused(car_state *car)
{
    car->shift_cooldown = 0x14;
    car->shift_result   = -1;
    car->gear_changed_q = 1;
}

/* 0x0043A8F5 */
static void to_neutral(gb *b)
{
    b->car->gear = 0;
    shift_done(b->car, b->dk);
}

/* 0x0043A930: reverse, unless its bit is set; the revs quartered. */
static void to_reverse(gb *b)
{
    car_state *car = b->car;
    if (broken(car, -1)) return;
    car->gear = -1;
    car->revs = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(car->revs),
                    sim_x87_from_float(b->k->gear_reverse_revs)));
    wheels_from_revs(car, b->g);
    shift_done(car, b->dk);
}

/* FUN_0043AD87, from neutral: the lowest unbroken gear 1..6 whose revs at
 * this body speed fall under the upshift point.  The search STORES the
 * fraction into car->revs ([0x006CCE3C]); if no gear qualifies it returns
 * with that fraction still there and the last gear tried - kept, it is
 * what the bytes do (s3.3).  The bound is 7, not ngears. */
static void select_lowest(gb *b)
{
    car_state *car = b->car;
    float saved = car->revs;                           /* [0x005C6E7C] */
    int32_t g;

    revs_from_body(car, b->g);                         /* gear 0: no-op */
    for (g = 1; g != 7; g++) {
        if (broken(car, g)) continue;
        car->gear = g;
        revs_from_body(car, b->g);
        car->revs = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->revs),
                        sim_x87_from_float(car->maxrevs)));
        if (below(sim_x87_from_float(car->revs), b->p->upshift)) break;
    }
    if (g == 7) return;

    car->gear = g;
    revs_from_body(car, b->g);
    /* FLD saved / FSUB revs / FMUL 0.99 / FADD revs / FSTP: one expression */
    car->revs = sim_x87_to_float(sim_x87_add(sim_x87_mul(
                    sim_x87_sub(sim_x87_from_float(saved),
                                sim_x87_from_float(car->revs)),
                    sim_x87_from_float(b->k->gear_select_blend)),
                    sim_x87_from_float(car->revs)));
    wheels_from_revs(car, b->g);
    shift_done(car, b->dk);
}

/* 0x0043A83D */
static void shift_up(gb *b)
{
    car_state *car = b->car;
    int32_t g = car->gear, to;

    if (g == -1) { to_neutral(b); return; }
    if (g == 0)  { select_lowest(b); return; }
    if (g == b->g->ngears) return;
    wheels_from_revs(car, b->g);
    if (!next_up(car, b->g, g, &to)) return;
    car->gear = to;
    revs_from_wheels(car, b->g);
    shift_done(car, b->dk);
}

/* 0x0043A876 */
static void shift_down(gb *b)
{
    car_state *car = b->car;
    int32_t g = car->gear, to;
    float cap;

    if (g == 1)  { to_neutral(b); return; }
    if (g == 0)  { to_reverse(b); return; }
    if (g == -1) return;
    wheels_from_revs(car, b->g);
    if (!next_down(car, g, &to)) { to_neutral(b); return; }
    car->gear = to;
    revs_from_wheels(car, b->g);
    /* FLD revs / FCOMP [0x005C6E3C] / JB done.  [0x005C6E3C] is
     * f32(maxrevs x 1.2), 0x004202A9 - the post-tyre block's cap. */
    cap = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(car->maxrevs),
                                       sim_x87_from_float(b->k->overrev_scale)));
    if (below(sim_x87_from_float(car->revs), cap)) {
        shift_done(car, b->dk);
        return;
    }
    car->gear++;                                       /* put back up */
    revs_from_wheels(car, b->g);
    shift_refused(car);
}

/* 0x0043A98C, the automatic.  The upshift test predicts from the BODY speed
 * (FUN_0043A603) and restores the revs; the downshift tests from the
 * engine's own revs, both against the record's three thresholds. */
static void shift_auto(gb *b)
{
    car_state *car = b->car;
    const sim_world *w = b->w;
    int32_t g = car->gear, to;
    float rf, downfrac;

    if (w->gear_down_in != 0) {
        if (g == 0) { to_reverse(b); return; }
        if (g >= 0) { to_neutral(b); return; }        /* JNS */
    }
    if (w->gear_up_in != 0) {
        if (g == 0) { select_lowest(b); return; }
        if (g < 0)  { to_neutral(b); return; }        /* JS */
    }
    if (car->yaw_couple_gate != 0) return;             /* all four airborne */

    if (g >= 1) {
        float saved = car->revs;                       /* PUSH */
        revs_from_body(car, b->g);
        rf = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->revs),
                 sim_x87_from_float(car->maxrevs)));   /* [0x005D6EA0] */
        car->revs = saved;                             /* POP */
        if (!below(sim_x87_from_float(rf), b->p->upshift)) { shift_up(b); return; }
    }
    if (g < 2) return;

    rf = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->revs),
             sim_x87_from_float(car->maxrevs)));
    if (!next_down(car, g, &to)) return;
    /* [0x0058069C] = f32(+0xA0 x 0.9), 0x0042025E.  The product and quotient
     * stay in st(0) for the FCOMP. */
    downfrac = b->p->upshift * b->dk->down_scale;
    if (!below(sim_x87_div(sim_x87_mul(sim_x87_from_float(rf),
                   sim_x87_from_float(ratio_of(b->g, g))),
                   sim_x87_from_float(ratio_of(b->g, to))), downfrac))
        return;
    /* [0x00580698] / [0x00580694] = f32(+0xA8 / +0xA4 + 0.1), 0x0042027C /
     * 0x0042026A - NOT the raw record values (the raw values
     * missed every one of the capture's downshifts). */
    if (car->brake != 0) {                             /* [0x006CD2A0] */
        if (!below(sim_x87_from_float(rf),
                   b->p->coast_on_brake + b->dk->coast_margin)) return;
    } else {
        if (!below(sim_x87_from_float(rf),
                   b->p->coast_off_brake + b->dk->coast_margin)) return;
    }
    shift_down(b);
}

/* FUN_0043A7AB, strategies 0 and 1.  An arcade rival goes straight to the
 * automatic, past the broken-gear, dropgear and cooldown tests. */
static void shift(gb *b)
{
    car_state *car = b->car;

    car->shift_result = 0;
    if (car->driver == 2) { shift_auto(b); return; }
    if (broken(car, car->gear)) { to_neutral(b); return; }
    if (car->dropgear != 0) { to_neutral(b); return; }
    if (car->shift_cooldown != 0) { car->shift_cooldown--; return; }
    if (car->stikgear != 0) return;
    if (car->autom_1 != 0) { shift_auto(b); return; }
    /* the stick */
    if (b->w->gear_up_in != 0)   { shift_up(b); return; }
    if (b->w->gear_down_in != 0) { shift_down(b); return; }
}

/* ---- FUN_0043AABF, strategy 2: the fully automatic -------------
 * s3.5.  Its own labels, not strategy 1's: its reverse does not
 * quarter the revs, its up does not skip broken gears, and a downshift the
 * overrev cap refuses goes to NEUTRAL, not to REFUSED. */

/* 0x0043AC4B: reverse, unless its bit is set.  No revs x 0.25. */
static void m2_reverse(gb *b)
{
    car_state *car = b->car;
    if (broken(car, -1)) return;
    car->gear = -1;
    wheels_from_revs(car, b->g);
    shift_done(car, b->dk);
}

/* 0x0043AB6A: up one gear.  INC, not FUN_0043AA9D: no broken-gear walk. */
static void m2_up(gb *b)
{
    car_state *car = b->car;
    int32_t g = car->gear;

    if (g == b->g->ngears) return;                     /* [0x005C7944]       */
    if (g == -1) { select_lowest(b); return; }         /* 0x0043AC45         */
    wheels_from_revs(car, b->g);
    car->gear++;
    revs_from_wheels(car, b->g);
    shift_done(car, b->dk);                            /* 0x0043ABE5         */
}

/* 0x0043ABB4, entered from 0x0043AD7C: down one gear (DEC, no walk); over
 * the cap ([0x005C6E3C], JB) the gear goes back up and then to NEUTRAL. */
static void m2_down(gb *b)
{
    car_state *car = b->car;
    float cap;

    wheels_from_revs(car, b->g);
    car->gear--;
    revs_from_wheels(car, b->g);
    cap = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(car->maxrevs),
                                       sim_x87_from_float(b->k->overrev_scale)));
    if (below(sim_x87_from_float(car->revs), cap)) {
        shift_done(car, b->dk);
        return;
    }
    car->gear++;
    revs_from_wheels(car, b->g);
    to_neutral(b);                                     /* JMP 0x0043AC10     */
}

/* 0x0043AC95, the shift tests.  EBX holds the gear from the top. */
static void m2_auto(gb *b)
{
    car_state *car = b->car;
    int32_t g = car->gear;
    float saved = car->revs, rf;                       /* PUSH revs          */

    revs_from_body(car, b->g);                         /* FUN_0043A603       */
    rf = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->revs),
             sim_x87_from_float(car->maxrevs)));       /* [0x005D6EA0]       */
    car->revs = saved;                                 /* POP                */
    /* CMP EBX,1 / JS; +0x254 (all four airborne) skips the test only. */
    if (g >= 1 && car->yaw_couple_gate == 0
        && !below(sim_x87_from_float(rf), b->p->upshift)) {
        m2_up(b);                                      /* JAE 0x0043AB6A     */
        return;
    }
    /* In reverse with the brake word set - the THROTTLE key, after
     * FUN_00438208's swap - and rolling back no faster than 1 m/s. */
    if (g == -1 && car->brake != 0
        && !below(sim_x87_from_float(car->czspd), b->k->mode2_back_speed)) {
        select_lowest(b);                              /* JAE 0x0043AC45     */
        return;
    }
    /* The ENGINE's revs; the downshift is the coast-down test alone - no
     * lower-gear projection against [0x0058069C] as in strategy 1. */
    rf = sim_x87_to_float(sim_x87_div(sim_x87_from_float(car->revs),
             sim_x87_from_float(car->maxrevs)));
    if (g >= 2) {                                      /* CMP EBX,2 / JS     */
        if (car->brake != 0) {                         /* [0x006CD2A0]       */
            if (below(sim_x87_from_float(rf),
                      b->p->coast_on_brake + b->dk->coast_margin)) {
                m2_down(b);                            /* [0x00580698]       */
                return;
            }
        } else if (below(sim_x87_from_float(rf),
                         b->p->coast_off_brake + b->dk->coast_margin)) {
            m2_down(b);                                /* [0x00580694]       */
            return;
        }
    }
    /* 1st, the brake on, below 1 m/s forward: straight into reverse. */
    if (g == 1 && car->brake != 0
        && below(sim_x87_from_float(car->czspd), b->k->mode2_fwd_speed))
        m2_reverse(b);                                 /* JB 0x0043AC4B      */
}

/* FUN_0043AABF.  The idle test and the out-of-neutral test come BEFORE
 * dropgear and the cooldown, so neither waits for the cooldown.  The block
 * at 0x0043AB97 (gear 1 -> reverse, autom_1 != 0 -> neutral, then the
 * downshift) has no reference in the image (no rel32, rel8 or absolute
 * site) and is not ported. */
static void shift_mode2(gb *b)
{
    car_state *car = b->car;
    /* [0x005C6E30], the class's idle revs (3000 / 2000), set at load. */
    float idle = b->k->idle_revs[car->drive_class != 0 ? 1 : 0];

    car->shift_result = 0;
    if (broken(car, car->gear)) { to_neutral(b); return; }
    /* In gear below idle: NEUTRAL.  FLD revs / FCOMP / JAE. */
    if (car->gear != 0 && below(sim_x87_from_float(car->revs), idle)) {
        to_neutral(b);
        return;
    }
    if (car->gear == 0) {
        /* FLD maxrevs / FMUL 0.95 / FCOMP revs: the product unrounded.
         * Revs above 0.95 x maxrevs leave neutral, the way the arbitration's
         * sign says: -1 (the brake won) reverse, else FUN_0043AD87. */
        if (!below(sim_x87_mul(sim_x87_from_float(car->maxrevs),
                               sim_x87_from_float(b->k->mode2_select_frac)),
                   car->revs))
            return;
        if (b->w->gear_sign_in == -1) m2_reverse(b);
        else select_lowest(b);
        return;
    }
    if (car->dropgear != 0) { to_neutral(b); return; }
    if (car->shift_cooldown != 0) { car->shift_cooldown--; return; }
    if (car->stikgear != 0) return;
    m2_auto(b);
}

/* FUN_0043A6E5 / FUN_0043A70A: the gear named by dannyebx bits 8..11, as a
 * signed nibble (SHL 0x14 / SAR 0x1C).  No torque cut, no cooldown. */
static void select_direct(gb *b)
{
    car_state *car = b->car;
    int32_t target = (int32_t)((car->dannyebx & 0xF00u) << 20) >> 28;

    car->shift_result = 0;
    if (car->gear == target) return;
    if (car->gear == 0) {
        car->gear = target;
        wheels_from_revs(car, b->g);
    } else if (target == 0 || broken(car, target)) {
        car->gear = 0;
    } else {
        wheels_from_revs(car, b->g);
        car->gear = target;
        revs_from_wheels(car, b->g);
    }
    car->shift_result   = 1;
    car->gear_changed_q = 1;
}

void sim_gearbox_update_q(car_state *car, const car_params *p,
                          const sim_consts *k, const dt_consts *dk,
                          const dt_gears *gears, sim_world *w)
{
    gb b;
    b.car = car; b.p = p; b.k = k; b.dk = dk; b.g = gears; b.w = w;

    /* 0x0043A65E..0x0043A67A: RACE FIN, then the pre-start hold, skip to the
     * tail.  CMP alivet_r,0x320 / JS: signed. */
    if (car->rfn == 0
        && !(w->shaungmode != 1 && js_0x320(car->alivet_r))) {
        if (car->driver == 2) {
            shift(&b);
        } else if ((car->dannyebx & 0xF00u) != 0xE00u) {
            select_direct(&b);
            return;                                    /* 0x0043A69B: no tail */
        } else if (car->autom_1 == 2) {
            shift_mode2(&b);                           /* FUN_0043AABF       */
        } else {
            shift(&b);                                 /* 1, and via 0x0043A7A5 */
        }
    }
    /* 0x0043A6C4 */
    if (car->shift_result == 1)
        car->revs = sim_x87_to_float(sim_x87_mul(sim_x87_from_float(car->revs),
                        sim_x87_from_float(dk->shift_loss)));
    wheels_from_revs(car, gears);
}

void sim_gearbox_stall_q(car_state *car, const sim_consts *k,
                         const dt_consts *dk)
{
    /* [0x005C6E34] = FLD idle / FADD 100.0 / FSTP, 0x004204BC: the idle
     * table indexed by the drivetrain class itself. */
    float idle = k->idle_revs[car->drive_class != 0 ? 1 : 0];
    float stall = sim_x87_to_float(sim_x87_add(sim_x87_from_float(idle),
                      sim_x87_from_float(k->stall_margin)));

    if (car->collision_active == 0) return;
    if (car->gear == 0) return;
    if (!below(sim_x87_from_float(car->revs), stall)) return;
    car->gear = 0;
    shift_done(car, dk);
}
