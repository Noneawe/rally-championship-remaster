/* damage.c - the damage model's consumer, FUN_004C66EA (sim_damage_efficiency
 * in the reading), and the four process globals it reads,
 * has the reading (ss1, 3, 6);
 * s30 the measurement.
 *
 * WHERE IT RUNS.  FUN_00439B37's fifth call, 0x00439B4B, is FUN_0043D033,
 * whose first instruction calls FUN_00439F90: driver (car+0x25C) == 2 calls
 * FUN_0043E2D3, anything else 0x004C2AE0 -> FUN_004C66EA.  So the six
 * efficiencies are made at the START of the step, from the damage and
 * temperatures the PREVIOUS step's accumulators (FUN_004C2AE6, at 0x00439E54)
 * left, and the tyre model reads them in the same step.
 *
 * THE MEASUREMENT.  Applied to record N-1 of the single-car capture, stage 1
 * gives record N's +0x3F4..+0x408 on 36,753 of 36,753 steps, all six slots
 * The
 * game's brakeefff moves on 2,761 of those steps, so carrying the previous
 * record's value - what the port did before - is exact on 33,992.
 *
 * THE ORDER OF THE BYTES.  Every sum is one x87 expression and is stored
 * once, at its FSTP: sim_x87 below.  Every compare is FLD m32 / FCOMP m32 of
 * two floats, which C's float compare reproduces exactly; the NaN cases are
 * written out where the branch sense makes them differ.
 *
 * Stage 2, the 18 rolls (0x004C6B15..0x004C766D): the chances are
 * ramps of the damage block into globals 0x008DCE34..0x008DCECC, and each
 * roll compares a rotation of framernd ([0x008DCED0], made by
 * sim_damage_wear_q) with chance x 65536.  The globals are not kept: their
 * only other readers (0x0042D434..) are display code.
 *
 * WHAT IS NOT HERE
 *   The three accumulators that wear the car are damage_acc.c;
 *   FUN_0043E2D3 (driver == 2), the AI car's reset, is below. */

#include "physics.h"

#include <math.h>
#include <string.h>

static sim_x87 X(float f)                 { return sim_x87_from_float(f); }
static sim_x87 Xadd(sim_x87 a, sim_x87 b) { return sim_x87_add(a, b); }
static sim_x87 Xsub(sim_x87 a, sim_x87 b) { return sim_x87_sub(a, b); }
static sim_x87 Xmul(sim_x87 a, sim_x87 b) { return sim_x87_mul(a, b); }
static sim_x87 Xdiv(sim_x87 a, sim_x87 b) { return sim_x87_div(a, b); }
static float   Xst(sim_x87 a)             { return sim_x87_to_float(a); }

/* The ramp idiom, 23 sites in the function (s3), over the
 * record [x0, y0, x1, y1] at ESI.  Returns what the FSTP after it stores.
 *   FLD v / FCOMP [esi]    / JB  -> y0     below x0, or unordered
 *   FLD v / FCOMP [esi+8]  / JB  -> lerp   below x1
 *                                -> y1
 *   FLD x1 / FSUB x0 / FCOMP 0.0 / AND AH,0x40 / JNE -> y0   (C3: equal OR
 *                                                            unordered)
 *   ((v - x0) / (x1 - x0)) x (y1 - y0) + y0, in the register */
static float ramp(float v, const float t[4])
{
    if (!(v >= t[0])) return t[1];
    if (!(v < t[2])) return t[3];
    if (isnan(t[0]) || isnan(t[2]) || t[2] == t[0]) return t[1];
    return Xst(Xadd(Xmul(Xdiv(Xsub(X(v), X(t[0])), Xsub(X(t[2]), X(t[0]))),
                         Xsub(X(t[3]), X(t[1]))),
                    X(t[1])));
}

/* The clamp, after the FSTP: FLD 0.0 / FLD X / FCOMPP / SHR AH,1 / JAE skip
 * (C0: X below 0 OR unordered -> 0.0), then FLD 1.0 / FLD X / FCOMPP / JB
 * skip (X >= 1.0 -> 1.0).  Three of the six slots have it. */
static float clamp_eff(float x, const sim_consts *k)
{
    if (!(x >= k->dmg_eff_lo)) x = k->dmg_eff_lo;
    if (x >= k->dmg_eff_hi) x = k->dmg_eff_hi;
    return x;
}

/* term = input x 0.01 x weight: FLD input / FMUL [0x008DCDB8] / FMUL w. */
static sim_x87 term(float input, float w, const sim_consts *k)
{
    return Xmul(Xmul(X(input), X(k->dmg_pct)), X(w));
}

/* brakeefff / brakeeffr: 0x004C6781 / 0x004C681D. */
static float brake_eff(float btmp, float dam_brake, const car_state *car,
                       const sim_consts *k, const sim_damage_globals *g)
{
    sim_x87 e = X(1.0f);                                       /* FLD1      */
    e = Xsub(e, term(btmp,         k->dmg_w[3], k));            /* x 1.0     */
    e = Xsub(e, term(dam_brake,    k->dmg_w[2], k));            /* x 0.3     */
    e = Xsub(e, term(car->temps[0], k->dmg_w[4], k));           /* bwet, 0.4 */
    e = Xsub(e, term(g->wetness,   k->dmg_w[5], k));            /* x 0.2     */
    return clamp_eff(Xst(e), k);
}

/* Stage 2's ramp records live in the damage page (sim_consts.dmg_page). */
static float ramp_at(float v, const sim_consts *k, uint32_t rec)
{
    float t[4];
    int j;
    for (j = 0; j < 4; j++) {
        uint32_t i = (rec - SIM_DMG_PAGE_BASE) / 4u + (uint32_t)j;
        uint32_t b = i < SIM_DMG_PAGE_WORDS ? k->dmg_page[i] : 0u;
        memcpy(&t[j], &b, sizeof(b));
    }
    return ramp(v, t);
}

/* One roll, eight instructions: FLD chance / FMUL 65536 / FISTP [0x008DCD9C]
 * / MOV EAX,framernd / ROR EAX,r / AND EAX,0xFFFF / CMP EAX,[0x008DCD9C] /
 * JNS skip.  JNS reads SF alone: the roll hits when draw - threshold is
 * negative as a dword (so a NaN chance, FISTP's 0x80000000, always hits). */
static int roll(float chance, unsigned r, const sim_consts *k,
                const sim_damage_globals *g)
{
    uint32_t thr = (uint32_t)sim_x87_fistp(Xmul(X(chance), X(k->dmg_roll)));
    uint32_t w = g->framernd;
    uint32_t draw = ((w >> r) | (w << ((32u - r) & 31u))) & 0xFFFFu;
    return (int32_t)(draw - thr) < 0;
}

/* 0x004C6B15..0x004C766D.  The chances first, each a ramp stored to its
 * global (s5): stikgear's two, dropgear's, the handbrake's,
 * then elecoff and the six lights, the fail chances (record 0x008DCF88) and
 * the recover chances (record 0x008DCF98, but elecoff's recover chance is
 * made from 0x008DCF88, the fail record: kept as read).  Then the rolls,
 * each a rotation of ONE word: the seven on/off flags latch (a flag that is
 * set rolls to clear, one that is clear rolls to set); stikgear and
 * dropgear are cleared and rolled afresh every step. */
static void stage2(car_state *car, const sim_consts *k,
                   const sim_damage_globals *g)
{
    float stik_clutch = ramp_at(car->dam[4],  k, 0x008DCF48u);  /* 0x8DCEC0 */
    float stik_box    = ramp_at(car->dam[15], k, 0x008DCF58u);  /* 0x8DCEC4 */
    float drop        = ramp_at(car->dam[15], k, 0x008DCF68u);  /* 0x8DCEC8 */
    float hbrake      = ramp_at(car->dam[38], k, 0x008DCF78u);  /* 0x8DCECC */
    float fail[7], rec[7];
    int i;

    /* 0x8DCE34..0x8DCE4C: dam_elect (22), dam_light1..4, f, b (23..28) */
    for (i = 0; i < 7; i++) fail[i] = ramp_at(car->dam[22 + i], k, 0x008DCF88u);
    /* 0x8DCE50..0x8DCE68 */
    rec[0] = ramp_at(car->dam[22], k, 0x008DCF88u);
    for (i = 1; i < 7; i++) rec[i] = ramp_at(car->dam[22 + i], k, 0x008DCF98u);

    /* 0x004C7289; the CMP / JZ to the next instruction at 0x004C72C3 is
     * dead (s5). */
    if (car->handbrake == 0 && roll(hbrake, 0xD, k, g)) car->handbrake = 1;

    if (car->elecoff != 0) { if (roll(rec[0], 4, k, g)) car->elecoff = 0; }
    else if (roll(fail[0], 4, k, g)) car->elecoff = 1;

    for (i = 0; i < 6; i++) {           /* ROR 5..0xA */
        if (car->lightoff[i] != 0) {
            if (roll(rec[1 + i], 5u + (unsigned)i, k, g)) car->lightoff[i] = 0;
        } else if (roll(fail[1 + i], 5u + (unsigned)i, k, g)) {
            car->lightoff[i] = 1;
        }
    }

    car->stikgear = 0;                                          /* 0x004C75C7 */
    if (roll(stik_clutch, 0xB, k, g)) car->stikgear = 1;
    if (roll(stik_box, 0x19, k, g)) car->stikgear = 1;
    car->dropgear = 0;                                          /* 0x004C7633 */
    if (roll(drop, 0x12, k, g)) car->dropgear = 1;
}

void sim_damage_efficiency_q(car_state *car, const sim_consts *k,
                             const sim_damage_globals *g)
{
    /* dam[] indices: car+0x2B8 + 4i.  0 engine, 1 exhaust, 2 turbo,
     * 4 clutch, 5 diff, 6..14 gearrvs gear0 gear1..gear7, 15 gearbox,
     * 22 elect, 30 steer, 36 brakef, 37 brakeb, 39..42 tyre1..tyre4. */
    float sp, wear;
    uint32_t bits = 0;
    int i;

    /* ---- stage 1: the six efficiencies ------------------------------- */

    /* engpower, 0x004C66EA: FLD1, the two terms, then x 0.6 when drivetype
     * (CMP dword - the laundered BITS, not a float test) is non-zero and
     * dam_turbo is not below 100 (FCOMP / SHR AH,1 / JB: NaN skips). */
    {
        sim_x87 e = X(1.0f);
        e = Xsub(e, term(car->dam[0], k->dmg_w[0], k));
        e = Xsub(e, term(car->dam[1], k->dmg_w[1], k));
        if (g->drivetype != 0 && car->dam[2] >= k->dmg_full)
            e = Xmul(e, X(k->dmg_turbo));
        car->engpower = clamp_eff(Xst(e), k);
    }

    car->brakeefff = brake_eff(car->temps[3], car->dam[36], car, k, g);
    car->brakeeffr = brake_eff(car->temps[4], car->dam[37], car, k, g);

    /* tyregripf / tyregripr, 0x004C68B9..0x004C6A97: the temperature ramp
     * is STORED into the slot, the wear ramp into [0x008DCE6C], and then
     * slot = wear x 0.01 x 0.01 x slot in the register.  No clamp. */
    car->tyregripf = ramp(car->temps[1], k->dmg_ramp_ttmp);
    car->tyregripr = ramp(car->temps[2], k->dmg_ramp_ttmp);
    wear = ramp(g->tyref, k->dmg_ramp_tyre);
    car->tyregripf = Xst(Xmul(Xmul(Xmul(X(wear), X(k->dmg_pct)), X(k->dmg_pct)),
                              X(car->tyregripf)));
    wear = ramp(g->tyrer, k->dmg_ramp_tyre);
    car->tyregripr = Xst(Xmul(Xmul(Xmul(X(wear), X(k->dmg_pct)), X(k->dmg_pct)),
                              X(car->tyregripr)));

    /* steerpull, 0x004C6A9D: the ramp stored, then FMUL ST(0), stored. */
    sp = ramp(car->dam[30], k->dmg_ramp_steer);
    car->steerpull = Xst(Xmul(X(sp), X(sp)));

    /* ---- stage 2, 0x004C6B15: the 18 rolls. */
    stage2(car, k, g);

    /* ---- stage 3, 0x004C766E: gear0 = 0 (a MOV of dword 0), then bit i
     * for each of the nine slots from gearrvs that is EQUAL to 100 -
     * FCOMP / AND AH,0x40: C3, so an unordered slot sets its bit too. */
    car->dam[7] = 0.0f;
    for (i = 0; i < 9; i++) {
        float d = car->dam[6 + i];
        if (isnan(d) || d == k->dmg_full) bits |= 1u << i;
    }
    car->gearbits = bits;

    /* ---- stage 4, 0x004C76A6: the terminal flag.  Each test is FCOMP 100
     * / SHR AH,1 / JAE: taken when NOT below, so NaN does not count. */
    if (car->dam[0] >= k->dmg_full || car->dam[4] >= k->dmg_full
        || car->dam[5] >= k->dmg_full || car->dam[15] >= k->dmg_full
        || car->dam[22] >= k->dmg_full) {
        car->damage_accum = 1;
        if (car->dam[22] >= k->dmg_full) car->engpower = 0.0f;
    } else {
        car->damage_accum = 0;
    }
}

void sim_damage_globals_from_car_q(const car_state *car, const sim_consts *k,
                                   sim_damage_globals *g)
{
    uint32_t bits;

    /* FUN_004C2AF6, 0x004C2F88: FLD [car+0x40] / FSTP [0x008DCE14].  An
     * integer through the x87 as a float: 0 and 1 (a denormal) survive, and
     * the only bit pattern the round trip changes is a signalling NaN, which
     * comes back quiet. */
    memcpy(&bits, &car->drive_class, sizeof(bits));
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
        bits |= 0x00400000u;
    g->drivetype = bits;

    /* FUN_004C2AF6, 0x004C3221: MOV EAX,[car+0x280] / MOV [0x008DCE20],EAX. */
    memcpy(&g->wetness, &car->wetns, sizeof(g->wetness));

    /* sim_damage_accumulate_q's tail, 0x004C65B6 / 0x004C65CE: FLD / FADD /
     * FMUL 0.5 / FSTP. */
    g->tyref = Xst(Xmul(Xadd(X(car->dam[39]), X(car->dam[40])), X(k->dmg_half)));
    g->tyrer = Xst(Xmul(Xadd(X(car->dam[41]), X(car->dam[42])), X(k->dmg_half)));

    /* sim_damage_wear_q, 0x004C33F4: MOV [0x008DCED0], car+0x00C - and
     * nothing moves the rnd after it in the step, so the record's is the
     * copy (MEASURED: damage-diff's stage-2 flags, 1,082 flips, exact
     * with the last stepped car's record).  Left out until 2026-10-23, the
     * live rolls ran on 0: every roll hit. */
    g->framernd = car->rnd;
}

/* FUN_0043E2D3 -> FUN_0043EEDD: driver 2's step starts here instead
 * of the consumer.  Transcribed from the image (Ghidra, 0x0043EEDD..F036):
 *   REP STOSD 0 over +0x29C..+0x367: the flag, the temperatures, dam[]
 *   FLD [0x0057AFBC] / FST x4 / FSTP: the five efficiencies +0x3F4..+0x404
 *   FILD set_s / FDIV [0x0057B0B0] / FMUL [0x0057AF98] / FST: actss_sum,
 *     actss[0..3]; the same from set_h: actsh_sum, actsh[0..3]
 *   MOV 0: steerpull, handbrake +0x428, stikgear, dropgear, gearbits,
 *     elecoff, the six lightoffs, the eight octant dents +0x064..+0x080
 *   MOV -1: carpartsleft +0x298
 * So an AI car carries one step's wear, never more. */
void sim_damage_reset_ai_q(car_state *car, const sim_consts *k)
{
    sim_x87 v;
    float f;
    int i;

    memset(&car->damage_accum, 0, 0x368 - 0x29C);
    car->engpower = car->brakeefff = car->brakeeffr = k->dmg_ai_one;
    car->tyregripf = car->tyregripr = k->dmg_ai_one;
    v = Xmul(Xdiv(sim_x87_from_i64(car->set_s_1), X(k->dmg_ai_div)), X(k->dmg_ai_mul));
    f = Xst(v);
    car->actss_sum = f;
    for (i = 0; i < 4; i++) car->actss[i] = f;
    v = Xmul(Xdiv(sim_x87_from_i64(car->set_h_1), X(k->dmg_ai_div)), X(k->dmg_ai_mul));
    f = Xst(v);
    car->actsh_sum = f;
    for (i = 0; i < 4; i++) car->actsh[i] = f;
    memset(&car->steerpull, 0, sizeof(car->steerpull));
    car->handbrake = 0;
    car->stikgear = 0;
    car->dropgear = 0;
    car->gearbits = 0;
    car->elecoff = 0;
    memset(car->lightoff, 0, sizeof(car->lightoff));
    memset(car->caroctdamage, 0, sizeof(car->caroctdamage));
    car->carpartsleft = -1;
}

/* FUN_0043F422, from 0x00439E9C - transcribed from the image
 * (ralimg.py disasm 0x43f422 0xd6).  Every compare is FLD m32 / FCOMP m32,
 * C0 to the carry: so `x < 0` is "below OR unordered".
 *   FLD peak / FCOMP 4.0 / JB ret                  peak below 4, or NaN
 *   FLD p[i] / FCOMP 0.0 / JAE / ADD EDX,4|2|1     x, y, z below 0 (or NaN)
 *   FLD peak / FDIV 64.0 / FSTP t
 *   FLD 0.0 / FLD t / FCOMPP / JAE                 t below 0 or NaN -> 0
 *   FLD 1.0 / FLD t / FCOMPP / JB                  t not below 1    -> 1
 *   FLD1 / FSUB oct / FMUL t / FADD oct / FSTP oct */
void sim_car_octant_damage_q(car_state *car, const sim_consts *k)
{
    float peak = car->collision_peak_normal_speed, t;
    int   i = 0;

    if (!(peak >= k->oct_min)) return;
    if (!(car->collision_scratch_224[0] >= k->oct_lo)) i += 4;
    if (!(car->collision_scratch_224[1] >= k->oct_lo)) i += 2;
    if (!(car->collision_scratch_224[2] >= k->oct_lo)) i += 1;
    t = Xst(Xdiv(X(peak), X(k->oct_div)));
    if (!(t >= k->oct_lo)) t = k->oct_lo;
    if (!(t < k->oct_hi)) t = k->oct_hi;
    car->caroctdamage[i] = Xst(Xadd(Xmul(Xsub(X(1.0f), X(car->caroctdamage[i])),
                                         X(t)),
                                    X(car->caroctdamage[i])));
}
