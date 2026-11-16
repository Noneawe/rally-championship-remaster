/* damage_acc.c - the damage model's three ACCUMULATORS, which wear the car:
 * FUN_004C2AF6, sim_damage_wear_q (0x004C33EF) and sim_damage_accumulate_q
 * (0x004C3D27), called in that order by FUN_004C2AE6 at 0x00439E54, late in
 * every car's step (s1).  damage.c is their consumer,
 * which the NEXT step runs first.
 *
 * WHAT THEY DO.  The first turns this step's car into ~50 process globals
 * [0x008DCDC8..0x008DCEBC]: revs, speed and wheel-slip fractions, the brake,
 * the gear and the last shift, the surface, an impact and its side, the
 * suspension's actuators.  The second moves the six temperatures
 * (+0x2A0..+0x2B4) and makes the wear rates.  The third adds every rate into
 * the 44 damage slots (+0x2B8..+0x364), each clamped to 0..100 after its own
 * FSTP, and ends with the summaries.
 *
 * THE BYTES.  Every store is one x87 expression, rounded once at its FSTP
 * (sim_x87, 64-bit significand); the globals are floats, so a global read
 * back is the rounded value.  Every operand order below is the listing's:
 * x87 addition and multiplication are commutative but not associative.
 * The oracle is the original's own instructions, interpreted on the same
 * model over every capture (the damvm, out of the repo): bit for bit.
 *
 * THE NUMBERS are the image's: K(a) reads the float at address `a`
 * from sim_consts.dmg_page, the raw .data page 0x008DCD74.. loaded at start.
 * No store in the binary reaches one of them.
 *
 * NO STATE CROSSES A STEP.  Every global the three read is written earlier
 * in the same call - the four wheel terms [0x008DCDFC..0x008DCE08] included:
 * 0x004C32F9 zeroes each one before its test (the correction of the
 * ticket's census, which missed that indexed store).
 *
 * KEPT, A QUIRK OF THE ORIGINAL: the tyre-wear
 * rate's wet column is read through the dry column's VALUE (0x004C3D02
 * indexes with the EAX that 0x004C3CEA loaded), so seven surface classes
 * blend with the wrong wet rate. */

#include "physics.h"

#include <math.h>
#include <string.h>

static sim_x87 X(float f)                 { return sim_x87_from_float(f); }
static sim_x87 Xi(int32_t i)              { return sim_x87_from_i64(i); }
static sim_x87 Xadd(sim_x87 a, sim_x87 b) { return sim_x87_add(a, b); }
static sim_x87 Xsub(sim_x87 a, sim_x87 b) { return sim_x87_sub(a, b); }
static sim_x87 Xmul(sim_x87 a, sim_x87 b) { return sim_x87_mul(a, b); }
static sim_x87 Xdiv(sim_x87 a, sim_x87 b) { return sim_x87_div(a, b); }
static sim_x87 Xneg(sim_x87 a)            { a.neg ^= 1; return a; } /* FCHS */
static sim_x87 Xabs(sim_x87 a)            { a.neg = 0; return a; }  /* FABS */
static float   Xst(sim_x87 a)             { return sim_x87_to_float(a); }

/* The page.  Out of it reads 0: no capture reaches that (an index from the
 * gear, the compound or the drive class would have to be out of range). */
static uint32_t Kbits(const sim_consts *k, uint32_t a)
{
    uint32_t i = (a - SIM_DMG_PAGE_BASE) / 4u;
    return i < SIM_DMG_PAGE_WORDS ? k->dmg_page[i] : 0u;
}

static float K(const sim_consts *k, uint32_t a)
{
    uint32_t b = Kbits(k, a);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* FLD m32 / FSTP m32 of a float: exact, but a signalling NaN comes back
 * quiet (damage.c's drivetype note). */
static float fld_fstp(float f)
{
    uint32_t b = bits_of(f);
    if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0)
        b |= 0x00400000u;
    memcpy(&f, &b, 4);
    return f;
}

/* x87 a < b: C0 of FCOM, which is also set when unordered. */
static int below(sim_x87 a, sim_x87 b)
{
    sim_x87 d;
    if (a.cls == 2 || b.cls == 2) return 1;
    d = Xsub(a, b);
    if (d.cls == 2) return 0;                    /* inf - inf: equal */
    return !(d.cls == 0 && d.m == 0) && d.neg;
}

/* The ramp over [x0, y0, x1, y1] at `a` (damage.c's ramp(), 23 sites here). */
static float ramp(float v, const sim_consts *k, uint32_t a)
{
    float x0 = K(k, a), y0 = K(k, a + 4), x1 = K(k, a + 8), y1 = K(k, a + 12);
    if (!(v >= x0)) return y0;
    if (!(v < x1)) return y1;
    if (isnan(x0) || isnan(x1) || x1 == x0) return y0;
    return Xst(Xadd(Xmul(Xdiv(Xsub(X(v), X(x0)), Xsub(X(x1), X(x0))),
                         Xsub(X(y1), X(y0))),
                    X(y0)));
}

/* After the FSTP: FLD [0x8DCD80] / FLD x / FCOMPP / JAE (below 0 or NaN ->
 * 0), then FLD [0x8DCD7C] / FLD x / FCOMPP / JB (not below 100 -> 100). */
static void clamp100(float *x, const sim_consts *k)
{
    if (!(*x >= K(k, 0x008DCD80u))) *x = K(k, 0x008DCD80u);
    if (*x >= K(k, 0x008DCD7Cu)) *x = K(k, 0x008DCD7Cu);
}

/* The accumulator's block: FLD ... / FADD slot / FSTP slot / the clamp. */
static void add_clamped(float *slot, sim_x87 term, const sim_consts *k)
{
    *slot = Xst(Xadd(term, X(*slot)));
    clamp100(slot, k);
}

/* The process globals the three write, by address.  Each is written before
 * it is read in the same call. */
typedef struct acc_g {
    float  ce28, ce2c, ce30;         /* set_h, set_s, brake balance / 256  */
    float  cea4[4];                  /* the suspension ramps               */
    float  cd9c;                     /* the scratch                        */
    float  cdc8;                     /* |revs| / maxrevs x 100             */
    float  cdcc;                     /* |speed| / 70 x 100, at most 100    */
    float  cdd0, cdd4, cdd8, cddc;   /* the four slip fractions            */
    float  ce10;                     /* car+0x958                          */
    float  ce24;                     /* 1: the yaw couple is on            */
    float  cde0;                     /* the brake / 256, moving            */
    float  cdf0;                     /* 1: revs above half the limit       */
    int32_t cde8, cdec, ce18, ce1c;  /* shift, gear, compound, surface     */
    float  cde4;                     /* 1: surface class 5                 */
    int32_t cdf4;                    /* the side hit, 1..4, 0 none         */
    float  cdf8;                     /* the impact                         */
    float  cdfc[4], ce0c;            /* the wheels' bumps and their mean   */
    float  ce8c, ce90, ce94;
    float  ce7c, ce78, ce98, ce9c, cea0;
    float  ce80, ce84, ce88, ceb4, ceb8, cebc, ce6c, ce70, ce74;
} acc_g;

/* ---- FUN_004C2AF6: the car into the globals ---------------------------- */

static void damage_feed_q(car_state *car, int32_t surf_class4, float surf_958,
                          const sim_consts *k, sim_damage_globals *g, acc_g *a)
{
    const float k100 = K(k, 0x008DCD7Cu), k70 = K(k, 0x008DCDA8u);
    int i;

    a->ce28 = Xst(Xdiv(Xi(car->set_h_1), X(K(k, 0x008DCD8Cu))));
    a->ce2c = Xst(Xdiv(Xi(car->set_s_1), X(K(k, 0x008DCD8Cu))));
    a->ce30 = Xst(Xdiv(Xi(car->b_bal_1), X(K(k, 0x008DCD8Cu))));

    /* 0x004C2B2C, four times: each wheel's actuators from its suspension
     * damage (dam_susp1..4) - a ramp, the spring take-off clamped, and both
     * overridden when the suspension is gone (FCOMP 100 / AND AH,0x40: C3,
     * so equal or unordered). */
    for (i = 0; i < 4; i++) {
        float d = car->dam[31 + i];
        a->cea4[i] = ramp(d, k, 0x008DCEF8u);
        car->actss[i] = Xst(Xsub(Xmul(X(a->ce2c), X(k100)), X(a->cea4[i])));
        clamp100(&car->actss[i], k);
        car->actsh[i] = Xst(Xmul(X(a->ce28), X(k100)));
        if (isnan(d) || d == k100) {
            car->actss[i] = k100;
            car->actsh[i] = K(k, 0x008DCD80u);
        }
    }
    car->actsh_sum = Xst(Xmul(Xadd(Xadd(Xadd(X(car->actsh[0]), X(car->actsh[1])),
                                        X(car->actsh[2])), X(car->actsh[3])),
                              X(K(k, 0x008DCD94u))));
    car->actss_sum = Xst(Xmul(Xadd(Xadd(Xadd(X(car->actss[0]), X(car->actss[1])),
                                        X(car->actss[2])), X(car->actss[3])),
                              X(K(k, 0x008DCD94u))));

    /* 0x004C2F88: the drive class's bits through the x87. */
    {
        float dc;
        memcpy(&dc, &car->drive_class, 4);
        dc = fld_fstp(dc);
        memcpy(&g->drivetype, &dc, 4);
    }

    /* 0x004C2F94: revs as a share of the limit.  FLD maxrevs / FLD x /
     * FCOMPP / JB: replaced when not below (NaN keeps it). */
    a->cd9c = Xst(Xabs(X(car->revs)));
    if (a->cd9c >= car->maxrevs) a->cd9c = car->maxrevs;
    a->cdc8 = Xst(Xmul(Xdiv(X(a->cd9c), X(car->maxrevs)), X(k100)));

    a->cd9c = Xst(Xabs(X(car->czspd)));
    if (a->cd9c >= k70) a->cd9c = k70;
    a->cdcc = Xst(Xmul(Xdiv(X(a->cd9c), X(k70)), X(k100)));

    a->ce10 = fld_fstp(surf_958);

    a->cd9c = Xst(Xabs(Xsub(X(car->czspd), X(car->fwhlspd))));
    if (a->cd9c >= k70) a->cd9c = k70;
    a->cdd0 = Xst(Xdiv(X(a->cd9c), X(k70)));
    a->cd9c = Xst(Xabs(Xsub(X(car->czspd), X(car->rwhlspd))));
    if (a->cd9c >= k70) a->cd9c = k70;
    a->cdd4 = Xst(Xdiv(X(a->cd9c), X(k70)));
    a->cd9c = Xst(Xabs(Xadd(Xmul(Xsub(X(car->fwhlspd), X(car->czspd)),
                                 X(K(k, 0x008DD124u))), X(car->fwhlspd))));
    if (a->cd9c >= k70) a->cd9c = k70;
    a->cdd8 = Xst(Xdiv(X(a->cd9c), X(k70)));
    a->cd9c = Xst(Xabs(Xadd(Xmul(Xsub(X(car->rwhlspd), X(car->czspd)),
                                 X(K(k, 0x008DD124u))), X(car->rwhlspd))));
    if (a->cd9c >= k70) a->cd9c = k70;
    a->cddc = Xst(Xdiv(X(a->cd9c), X(k70)));

    a->ce24 = car->yaw_couple_gate == 0 ? 1.0f : 0.0f;

    /* 0x004C3181: the brake, when moving at 1 or more (FCOMP / JB: NaN
     * skips) and brake - 16 is not negative (CMP / JS: the sign of the
     * difference). */
    a->cde0 = 0.0f;
    if (!below(Xabs(X(car->czspd)), X(K(k, 0x008DCD78u)))
        && (int32_t)((uint32_t)car->brake - 16u) >= 0)
        a->cde0 = Xst(Xdiv(Xi(car->brake), X(K(k, 0x008DCD8Cu))));

    /* 0x004C31BA: FLD maxrevs / FMUL 0.5 / FCOMP revs / JAE skip. */
    a->cdf0 = 0.0f;
    if (below(Xmul(X(car->maxrevs), X(K(k, 0x008DCDACu))), X(car->revs)))
        a->cdf0 = 1.0f;

    a->cde8 = car->shift_result;
    a->cdec = car->gear;
    a->ce18 = car->tyretype;
    a->ce1c = surf_class4;
    a->cde4 = surf_class4 == 5 ? 1.0f : 0.0f;
    memcpy(&g->wetness, &car->wetns, 4);         /* MOV, 0x004C3221 */

    /* 0x004C322B: the side hit, from the impact's bits; 1, +1 when the
     * scratch x is not below 0, +2 when z is below 0 (or NaN). */
    if (bits_of(car->collision_peak_normal_speed) == 0) {
        a->cdf4 = 0;
    } else {
        a->cdf4 = 1;
        if (!below(X(car->collision_scratch_224[0]), X(K(k, 0x008DCD80u)))) a->cdf4++;
        if (below(X(car->collision_scratch_224[2]), X(K(k, 0x008DCD80u)))) a->cdf4 += 2;
    }
    a->cdf8 = Xst(Xdiv(Xdiv(Xmul(X(car->collision_peak_normal_speed),
                                 X(car->collision_peak_normal_speed)),
                            X(K(k, 0x008DCDC4u))), X(k70)));

    /* 0x004C3288: the bump scale from the vertical speed, 0..8, squared x 4. */
    a->cd9c = Xst(Xsub(Xneg(X(car->cyspd_q)), X(K(k, 0x008DD054u))));
    if (!(a->cd9c >= K(k, 0x008DCD80u))) a->cd9c = K(k, 0x008DCD80u);
    if (a->cd9c >= K(k, 0x008DCD84u)) a->cd9c = K(k, 0x008DCD84u);
    a->cd9c = Xst(Xmul(Xmul(X(a->cd9c), X(a->cd9c)), X(K(k, 0x008DD050u))));

    /* 0x004C32F4: each wheel, zeroed, then its travel x the scale when
     * susp_prev[i] (car+0x610) is 0 and the travel's bits are not: a
     * wheel that has just landed. */
    for (i = 0; i < 4; i++) {
        int32_t flag;
        memcpy(&flag, &car->susp_prev[i], 4);
        a->cdfc[i] = 0.0f;
        if (flag == 0 && bits_of(car->susp[i]) != 0)
            a->cdfc[i] = Xst(Xmul(X(car->susp[i]), X(a->cd9c)));
    }
    /* 0x004C3332: each pulled halfway to the mean. */
    a->ce0c = Xst(Xmul(Xadd(Xadd(Xadd(X(a->cdfc[0]), X(a->cdfc[1])), X(a->cdfc[2])),
                            X(a->cdfc[3])), X(K(k, 0x008DCD94u))));
    a->cd9c = Xst(Xmul(X(a->ce0c), X(K(k, 0x008DCD90u))));
    for (i = 0; i < 4; i++)
        a->cdfc[i] = Xst(Xadd(Xmul(X(a->cdfc[i]), X(K(k, 0x008DCD90u))), X(a->cd9c)));

    /* 0x004C33C8: an impact while +0x460 counts is dropped, and the count
     * restarts (both CMPs are of dword bits). */
    if (bits_of(a->cdf8) != 0 && car->tick_460_q != 0) {
        a->cdf8 = 0.0f;
        car->tick_460_q = 100;
    }
}

/* ---- sim_damage_wear_q, 0x004C33EF: the temperatures and the rates ------ */

/* temp += (rate x input) x 0.01 / temp += -(rate x 0.01), each clamped. */
static void heat(float *t, float rate, float in, const sim_consts *k)
{
    add_clamped(t, Xmul(Xmul(X(rate), X(in)), X(K(k, 0x008DCDC0u))), k);
}
static void cool(float *t, float rate, const sim_consts *k)
{
    add_clamped(t, Xneg(Xmul(X(rate), X(K(k, 0x008DCDC0u)))), k);
}

static void sim_damage_wear_q(car_state *car, const sim_consts *k,
                              sim_damage_globals *g, acc_g *a)
{
    float sq;

    g->framernd = car->rnd;                      /* MOV, 0x004C33F4 */
    a->ce8c = ramp(a->cdc8, k, 0x008DCED8u);

    /* The brakes: heat from the brake, then a constant cooling. */
    heat(&car->temps[3], K(k, 0x008DCFB4u), a->cde0, k);
    heat(&car->temps[4], K(k, 0x008DCFB4u), a->cde0, k);
    cool(&car->temps[3], K(k, 0x008DCFB8u), k);
    cool(&car->temps[4], K(k, 0x008DCFB8u), k);
    /* The tyres: heat from the slip. */
    heat(&car->temps[1], K(k, 0x008DCFBCu), a->cdd8, k);
    heat(&car->temps[2], K(k, 0x008DCFBCu), a->cddc, k);
    cool(&car->temps[1], K(k, 0x008DCFC0u), k);
    cool(&car->temps[2], K(k, 0x008DCFC0u), k);
    /* The brakes' wetness: from the surface, dried, and dried by braking. */
    heat(&car->temps[0], K(k, 0x008DCFC4u), a->cde4, k);
    cool(&car->temps[0], K(k, 0x008DCFC8u), k);
    add_clamped(&car->temps[0],
                Xneg(Xmul(Xmul(X(K(k, 0x008DCFCCu)), X(a->cde0)),
                          X(K(k, 0x008DCDC0u)))), k);

    /* The coolant: revs above what the cooling's damage allows heat it;
     * speed cools it. */
    a->ce90 = ramp(car->dam[3], k, 0x008DCEE8u);
    a->ce94 = Xst(Xsub(X(a->cdc8), X(a->ce90)));
    if (!(a->ce94 >= K(k, 0x008DCD80u))) a->ce94 = K(k, 0x008DCD80u);
    heat(&car->temps[5], K(k, 0x008DCFA8u), a->ce94, k);
    cool(&car->temps[5], K(k, 0x008DCFACu), k);
    add_clamped(&car->temps[5],
                Xneg(Xmul(Xmul(X(K(k, 0x008DCFB0u)), X(a->cdcc)),
                          X(K(k, 0x008DCDC0u)))), k);

    /* The suspension's ramps, each squared (FLD / FMUL ST0 / FSTP). */
    sq = ramp(car->actss_sum, k, 0x008DD090u);
    a->ce7c = Xst(Xmul(X(sq), X(sq)));
    sq = ramp(car->actss_sum, k, 0x008DD070u);
    a->ce78 = Xst(Xmul(X(sq), X(sq)));
    sq = ramp(car->actsh_sum, k, 0x008DD080u);
    a->ce98 = Xst(Xmul(X(sq), X(sq)));
    a->ce9c = ramp(a->cdd0, k, 0x008DCF08u);
    a->cea0 = ramp(a->cdd4, k, 0x008DCF08u);

    /* 0x004C3C85: the body and wheel rates.  ce80 is an FST: ce88 goes on
     * from the unrounded register. */
    a->ce84 = Xst(Xmul(Xmul(Xmul(Xmul(Xmul(X(a->ce10), X(a->cdcc)),
                                      X(K(k, 0x008DD128u))), X(a->ce7c)),
                             X(a->ce98)), X(a->ce24)));
    {
        sim_x87 r = Xmul(Xmul(Xmul(X(a->ce10), X(a->cdcc)), X(K(k, 0x008DD128u))),
                         X(a->ce24));
        a->ce80 = Xst(r);
        a->ce88 = Xst(Xmul(r, X(a->ce78)));
    }

    /* 0x004C3CD9: the tyres' wear rate, by compound (a 16-int row) and
     * surface (the column pair at 0x008DD2F0), blended by the wetness.  The
     * second index is the FIRST lookup's value. */
    {
        uint32_t row = (uint32_t)a->ce18 << 6;
        int32_t  c1 = (int32_t)Kbits(k, 0x008DD2F0u + 8u * ((uint32_t)a->ce1c & 0xFu));
        int32_t  c2 = (int32_t)Kbits(k, 0x008DD2F4u + 8u * (uint32_t)c1);
        sim_x87  dry = Xmul(Xi((int32_t)Kbits(k, 0x008DD16Cu + row + 4u * (uint32_t)c1)),
                            Xsub(X(1.0f), X(g->wetness)));
        sim_x87  wet = Xmul(Xi((int32_t)Kbits(k, 0x008DD16Cu + row + 4u * (uint32_t)c2)),
                            X(g->wetness));
        a->ceb4 = Xst(Xmul(Xadd(dry, wet), X(K(k, 0x008DD2ECu))));
    }
}

/* ---- sim_damage_accumulate_q, 0x004C3D27: the wear into the slots ------- */

/* dam[] slots by name (s2.4). */
enum {
    D_ENGINE = 0, D_EXHAUST = 1, D_TURBO = 2, D_COOLING = 3, D_CLUTCH = 4,
    D_DIFF = 5, D_GEARRVS = 6, D_GEAR0 = 7, D_GEARBOX = 15, D_WHL1 = 16,
    D_BDYWRK = 20, D_BODY = 21, D_ELECT = 22, D_LIGHTS1 = 23, D_LIGHTSF = 27,
    D_LIGHTSB = 28, D_LIGHTS = 29, D_STEER = 30, D_SUSP1 = 31, D_SUSP = 35,
    D_BRAKEF = 36, D_BRAKEB = 37, D_BRAKES = 38, D_TYRE1 = 39, D_TYRES = 43
};

/* The current gear's slot, dam_gear0 + gear; the gear is -1..7, and a gear
 * outside dam[] (never seen) is not written. */
static float *gear_slot(car_state *car, int32_t gear)
{
    int32_t i = D_GEAR0 + gear;
    return (i >= 0 && i < 44) ? &car->dam[i] : NULL;
}

/* (K1 x K2) + slot, the base wear. */
static void base(car_state *car, int d, uint32_t rate, const sim_consts *k)
{
    add_clamped(&car->dam[d], Xmul(X(K(k, rate)), X(K(k, 0x008DCDBCu))), k);
}

/* ((in x K) x 0.01) + slot. */
static void rate(car_state *car, int d, float in, uint32_t r, const sim_consts *k)
{
    add_clamped(&car->dam[d], Xmul(Xmul(X(in), X(K(k, r))), X(K(k, 0x008DCDA0u))), k);
}

/* (impact x K) + slot. */
static void hit(car_state *car, int d, float imp, uint32_t r, const sim_consts *k)
{
    add_clamped(&car->dam[d], Xmul(X(imp), X(K(k, r))), k);
}

static void sim_damage_accumulate_q(car_state *car, const sim_consts *k,
                                    sim_damage_globals *g, acc_g *a)
{
    const sim_x87 h = X(K(k, 0x008DCD90u));
    float *gs;
    int i;

    /* 1. The base wear, every step. */
    base(car, D_ENGINE,  0x008DD000u, k);
    base(car, D_EXHAUST, 0x008DD004u, k);
    base(car, D_COOLING, 0x008DD008u, k);
    base(car, D_CLUTCH,  0x008DD00Cu, k);
    base(car, D_DIFF,    0x008DD010u, k);
    base(car, D_ELECT,   0x008DD014u, k);
    for (i = 0; i < 6; i++) base(car, D_LIGHTS1 + i, 0x008DD018u, k);
    base(car, D_STEER,   0x008DD01Cu, k);
    for (i = 0; i < 4; i++) base(car, D_SUSP1 + i, 0x008DD020u, k);

    /* 2. The engine side. */
    add_clamped(&car->dam[D_TURBO],
                Xmul(Xmul(Xmul(X(K(k, 0x008DCFD4u)), X(a->cdf0)), X(a->cdc8)),
                     X(K(k, 0x008DCDBCu))), k);
    if ((gs = gear_slot(car, a->cdec)) != NULL)
        add_clamped(gs, Xmul(Xmul(Xmul(X(a->cdc8), X(K(k, 0x008DCFD8u))),
                                  X(K(k, 0x008DCDA0u))),
                             X(K(k, 0x008DCFE0u + 4u * (uint32_t)a->cdec))), k);

    /* The brakes, split by the balance. */
    a->ce6c = Xst(Xsub(X(1.0f), X(a->ce30)));
    add_clamped(&car->dam[D_BRAKEF],
                Xmul(Xmul(Xmul(X(K(k, 0x008DD024u)), X(a->ce30)), X(a->cde0)),
                     X(K(k, 0x008DCDBCu))), k);
    add_clamped(&car->dam[D_BRAKEB],
                Xmul(Xmul(Xmul(X(K(k, 0x008DD024u)), X(a->ce6c)), X(a->cde0)),
                     X(K(k, 0x008DCDBCu))), k);

    /* Surface class 5: the electrics and the lights. */
    add_clamped(&car->dam[D_ELECT], Xmul(Xmul(X(K(k, 0x008DD028u)), X(a->cde4)),
                                         X(K(k, 0x008DCDBCu))), k);
    for (i = 0; i < 6; i++)
        add_clamped(&car->dam[D_LIGHTS1 + i],
                    Xmul(Xmul(X(K(k, 0x008DD02Cu)), X(a->cde4)),
                         X(K(k, 0x008DCDBCu))), k);

    /* The tyres: the speed over the rate, by the drive class (its BITS
     * index 0x008DD060 / 0x008DD068). */
    {
        sim_x87 t = Xmul(Xmul(Xdiv(X(a->cdcc),
                                   Xmul(X(a->ceb4), X(K(k, 0x008DCDB0u)))),
                              X(K(k, 0x008DCD7Cu))), X(a->ce24));
        a->ceb8 = Xst(t);
        a->cebc = Xst(t);
        a->ceb8 = Xst(Xmul(X(a->ceb8), X(K(k, 0x008DD060u + 4u * g->drivetype))));
        a->cebc = Xst(Xmul(X(a->cebc), X(K(k, 0x008DD068u + 4u * g->drivetype))));
    }
    for (i = 0; i < 2; i++)
        add_clamped(&car->dam[D_TYRE1 + i],
                    Xmul(Xmul(X(a->ce9c), X(a->ceb8)), X(K(k, 0x008DCDA0u))), k);
    for (i = 2; i < 4; i++)
        add_clamped(&car->dam[D_TYRE1 + i],
                    Xmul(Xmul(X(a->cea0), X(a->cebc)), X(K(k, 0x008DCDA0u))), k);

    /* The engine and the cooling from the revs, and from the heat. */
    rate(car, D_ENGINE,  a->ce8c, 0x008DD034u, k);
    rate(car, D_COOLING, a->ce8c, 0x008DD030u, k);
    if (!below(X(a->ce94), X(K(k, 0x008DCD80u)))) {
        rate(car, D_ENGINE,  car->temps[5], 0x008DD038u, k);
        rate(car, D_COOLING, a->ce94,       0x008DD03Cu, k);
    }

    /* The last shift: the clutch and the gear, NOT clamped. */
    if (a->cde8 == 1)
        car->dam[D_CLUTCH] = Xst(Xadd(X(car->dam[D_CLUTCH]), X(K(k, 0x008DD040u))));
    if (a->cde8 == -1)
        car->dam[D_CLUTCH] = Xst(Xadd(X(car->dam[D_CLUTCH]), X(K(k, 0x008DD044u))));
    if (a->cde8 == 1 && (gs = gear_slot(car, a->cdec)) != NULL)
        *gs = Xst(Xadd(X(*gs), X(K(k, 0x008DD048u))));
    if (a->cde8 == -1 && (gs = gear_slot(car, a->cdec)) != NULL)
        *gs = Xst(Xadd(X(*gs), X(K(k, 0x008DD04Cu))));

    /* 3. The rough: the wheels, the suspension, then the body's rate. */
    for (i = 0; i < 4; i++) rate(car, D_WHL1 + i,  a->ce80, 0x008DD0A0u, k);
    for (i = 0; i < 4; i++) rate(car, D_SUSP1 + i, a->ce88, 0x008DD0A8u, k);
    rate(car, D_BDYWRK,  a->ce84, 0x008DD0ACu, k);
    rate(car, D_STEER,   a->ce84, 0x008DD0B0u, k);
    rate(car, D_ENGINE,  a->ce84, 0x008DD0B4u, k);
    rate(car, D_EXHAUST, a->ce84, 0x008DD0B8u, k);
    rate(car, D_GEARRVS, a->ce84, 0x008DD0BCu, k);
    for (i = 1; i <= 7; i++) rate(car, D_GEAR0 + i, a->ce84, 0x008DD0BCu, k);
    rate(car, D_DIFF,    a->ce84, 0x008DD0C0u, k);

    /* 4. An impact: over 0.005 the body work; over 0.02 the rest, by side. */
    if (!below(X(a->cdf8), X(K(k, 0x008DD05Cu))))
        hit(car, D_BDYWRK, a->cdf8, 0x008DD0DCu, k);
    if (!below(X(a->cdf8), X(K(k, 0x008DD058u)))) {
        const float f = a->cdf8;
        const int32_t s = a->cdf4;
        for (i = 1; i <= 4; i++)
            if (s == i) hit(car, D_LIGHTS1 + i - 1, f, 0x008DD108u, k);
        if (s == 1) hit(car, D_LIGHTSF, f, 0x008DD10Cu, k);
        if (s == 2) hit(car, D_LIGHTSF, f, 0x008DD10Cu, k);
        if (s == 3) hit(car, D_LIGHTSB, f, 0x008DD10Cu, k);
        if (s == 4) hit(car, D_LIGHTSB, f, 0x008DD10Cu, k);
        hit(car, D_STEER,   f, 0x008DD104u, k);
        hit(car, D_ELECT,   f, 0x008DD100u, k);
        hit(car, D_TURBO,   f, 0x008DD0ECu, k);
        hit(car, D_CLUTCH,  f, 0x008DD0F0u, k);
        hit(car, D_DIFF,    f, 0x008DD0F4u, k);
        hit(car, D_GEARRVS, f, 0x008DD0F8u, k);
        for (i = 0; i <= 7; i++) hit(car, D_GEAR0 + i, f, 0x008DD0F8u, k);
        if ((gs = gear_slot(car, a->cdec)) != NULL)
            add_clamped(gs, Xmul(X(f), X(K(k, 0x008DD0FCu))), k);
        for (i = 1; i <= 4; i++)
            if (s == i) hit(car, D_WHL1 + i - 1, f, 0x008DD118u, k);
        for (i = 1; i <= 4; i++)
            if (s == i) hit(car, D_SUSP1 + i - 1, f, 0x008DD120u, k);
        if (s == 1) hit(car, D_COOLING, f, 0x008DD114u, k);
        if (s == 2) hit(car, D_COOLING, f, 0x008DD114u, k);
        if (s == 1) hit(car, D_ENGINE,  f, 0x008DD110u, k);
        if (s == 2) hit(car, D_ENGINE,  f, 0x008DD110u, k);
        if (s == 1) hit(car, D_BRAKEF,  f, 0x008DD11Cu, k);
        if (s == 2) hit(car, D_BRAKEF,  f, 0x008DD11Cu, k);
        if (s == 3) hit(car, D_BRAKEB,  f, 0x008DD11Cu, k);
        if (s == 4) hit(car, D_BRAKEB,  f, 0x008DD11Cu, k);
    }

    /* 0x004C6322: the bumps into the suspension, their mean into the
     * exhaust and the steering. */
    for (i = 0; i < 4; i++) rate(car, D_SUSP1 + i, a->cdfc[i], 0x008DD0E0u, k);
    rate(car, D_EXHAUST, a->ce0c, 0x008DD0E4u, k);
    rate(car, D_STEER,   a->ce0c, 0x008DD0E8u, k);

    /* 5. The summaries (lightsb before lightsf, as the listing adds). */
    car->dam[D_LIGHTS] = Xst(Xdiv(Xadd(Xadd(Xadd(Xadd(Xadd(
        X(car->dam[D_LIGHTS1]), X(car->dam[D_LIGHTS1 + 1])),
        X(car->dam[D_LIGHTS1 + 2])), X(car->dam[D_LIGHTS1 + 3])),
        X(car->dam[D_LIGHTSB])), X(car->dam[D_LIGHTSF])), X(K(k, 0x008DCD74u))));
    car->dam[D_BRAKES] = Xst(Xmul(Xadd(X(car->dam[D_BRAKEF]), X(car->dam[D_BRAKEB])), h));
    a->ce70 = Xst(Xmul(Xadd(X(car->dam[D_TYRE1]), X(car->dam[D_TYRE1 + 1])), h));
    a->ce74 = Xst(Xmul(Xadd(X(car->dam[D_TYRE1 + 2]), X(car->dam[D_TYRE1 + 3])), h));
    g->tyref = a->ce70;
    g->tyrer = a->ce74;
    car->dam[D_TYRES] = Xst(Xmul(Xadd(X(a->ce70), X(a->ce74)), h));
    {
        sim_x87 s = X(car->dam[D_GEARRVS]);
        for (i = 1; i <= 7; i++) s = Xadd(s, X(car->dam[D_GEAR0 + i]));
        car->dam[D_GEARBOX] = Xst(Xmul(Xmul(Xmul(s, h), h), h));
    }
    {
        sim_x87 s = X(car->dam[D_WHL1]);
        for (i = 1; i < 4; i++) s = Xadd(s, X(car->dam[D_WHL1 + i]));
        for (i = 0; i < 6; i++) s = Xadd(s, X(car->dam[D_BDYWRK]));
        car->dam[D_BODY] = Xst(Xmul(s, X(K(k, 0x008DCD98u))));
    }
    car->dam[D_SUSP] = Xst(Xmul(Xadd(Xadd(Xadd(X(car->dam[D_SUSP1]),
                                               X(car->dam[D_SUSP1 + 1])),
                                          X(car->dam[D_SUSP1 + 2])),
                                     X(car->dam[D_SUSP1 + 3])),
                                X(K(k, 0x008DCD94u))));
    car->actss_sum = Xst(Xmul(Xadd(Xadd(Xadd(X(car->actss[0]), X(car->actss[1])),
                                        X(car->actss[2])), X(car->actss[3])),
                              X(K(k, 0x008DCD94u))));
    /* A car of drive class 0 has no turbo (a MOV of dword 0). */
    if (g->drivetype == 0) car->dam[D_TURBO] = 0.0f;
}

void sim_damage_update_q(car_state *car, int32_t surf_class4, float surf_958,
                         const sim_consts *k, sim_damage_globals *g)
{
    acc_g a;
    memset(&a, 0, sizeof(a));
    damage_feed_q(car, surf_class4, surf_958, k, g, &a);
    sim_damage_wear_q(car, k, g, &a);
    sim_damage_accumulate_q(car, k, g, &a);
}
