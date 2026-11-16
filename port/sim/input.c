/* input.c - the tick's input stage: the five calls before the physics step.
 * Read whole from retail's bytes this session
 * (tools/ralimg.py disasm); has the
 * transcription, and sections 2, 8, 11 the earlier readings it corrects.
 *
 * WHERE IT RUNS.  FUN_0045E525's slot loop, 0x0045E590..0x0045E5A4, on the
 * WORKING car, after the copy-in and before FUN_00439B37:
 *
 *   FUN_0043895C   the device's four words into car+0x464..0x470, the
 *                  rising edges into +0x474, and the words into the
 *                  staging struct at 0x005C65C8 (sim_input_globals)
 *   FUN_00438208   the reset gates on that struct, and the gear
 *   FUN_004383C3   the steering: +0x644/+0x648, then +0x208/+0x64C/+0x650
 *   FUN_00438616   the pedals: +0x638/+0x63C/+0x640
 *   FUN_004386E5   the rescue key: +0x264, +0x088, FUN_0043D4B3
 *
 * FUN_00438880, which fills [0x005C65FC..0x5C6608] from the device, runs
 * before the copy-in and is not here: the caller supplies its four words.
 * That is the seam a live keyboard or a replayed capture plugs into.
 *
 * THE PROCESS GLOBALS.  The staging struct, and ste k / ste j, are not car
 * fields.  sim_input_globals holds them across ticks, as sim_tick holds
 * rev_q: ste k and ste j are written by FUN_0043D033 INSIDE the physics step
 * of whichever car stepped last, so the ramp reads the previous step's.
 *
 * WHAT IS NOT PORTED, AND IS FLAGGED IN sim_world:
 *  FUN_0043895C's rival-controller override ([0x006CEF7C] != 0, or
 *     gmode 2 with driver 2): FUN_0043618F / FUN_004601BA rewrite the staging
 *     struct.  w->input_rival_due.
 *  ~~edge bit 5's FUN_00439024 (the camera view): w->input_439024_due.~~
 *     run on w->camera when there is one (rally's); the flag is
 *     still set, and a caller with no camera (trace_check) only sees it.
 *  ~~FUN_004386E5's message slots and its call to FUN_0043D4B3~~:
 *     the slots go to the race context (w->race) and the tick runs the
 *     reset (sim_car_reset_q) when the hold reaches 100; w->d4b3_due is
 *     still set. */

#include "physics.h"
#include "camera.h"
#include "arcade.h"

#include <string.h>

/* CMP a,b / JS: the sign of the WRAPPED difference, which is what every
 * clamp in this stage tests.  It is not a signed compare: CMP x,0xE0000000
 * with x = 0x7FFFFFFF sets the sign, so a large enough positive steer clamps
 * to FULL LEFT.  No keyboard reaches it (+-256 gives +-0x28000000 at most). */
static int js(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b) < 0;
}

static int32_t wadd(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static int32_t wsub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

/* ---- FUN_0043895C, 0x0043895C..0x00438A74 ------------------------------ */
static void copy_in(car_state *car, const uint32_t words[4], int human,
                    int32_t gmode, sim_input_globals *g, sim_world *w)
{
    uint32_t old = car->dannyebx;              /* [0x0057BBCC]              */
    uint32_t edge;

    car->dannyeax = words[0];
    car->dannyebx = words[1];
    car->dannyecx = words[2];
    car->dannyedx = words[3];
    edge = ~old & car->dannyebx;
    car->dannyebx_edge = edge;

    g->pedal_a   = (int32_t)car->dannyeax;     /* 0x004389BE                */
    g->inputst   = (int32_t)car->dannyecx;     /* 0x004389C8                */
    g->pedal_b   = (int32_t)car->dannyedx;     /* 0x004389D2                */
    g->hbrake    = (int32_t)((car->dannyebx & 4u) * 0x40u);   /* 0 or 0x100 */
    g->gear_up   = (int32_t)(((edge & 1u) * 0x100u) >> 8);
    g->gear_down = (int32_t)(((edge & 2u) * 0x80u) >> 8);

    /* 0x00438A18: [0x006CE084] < [0x005F0A54], JAE - a local player's slot. */
    if (human) {
        if (edge & 8u)
            car->latch_84_q = (int32_t)(((uint32_t)car->latch_84_q + 1u) & 1u);
        if (edge & 0x20u) {
            w->input_439024_due = 1;
            /* binding slot 9 is "cycle camera views" (the REDEFINE
             * KEYBOARD screen's item 10): FUN_00439024, here, before the
             * step - on the car's own camera when the caller has one.  No
             * view name: that message is F1's (FUN_00438C42), not this
             * call's. */
            if (w->camera) (void)sim_camera_next_view_q(w->camera, car->alivet);
        }
    }

    /* 0x00438A52: [0x006CEF7C] (not modelled: 0) or gmode 2 with driver 2
     * hands the struct to the rival controller: FUN_0043618F and
     * FUN_004601BA, their six outputs over the staging struct (arcade.c) - with the race's arcade state, else only flagged. */
    if (gmode == 2 && car->driver == 2) {
        sim_race *rc = w->race;
        w->input_rival_due = 1;
        if (rc && rc->arcade && rc->rec[0])
            sim_arcade_drive_q(rc->arcade, car, w->race_slot, rc->rec[0],
                               rc->route, rc->geom, g);
    } else if (w->race && w->race->autopilot_q
               && (w->race->arcade || w->race->pilot_q)
               && (w->race_slot & 7u) == 0) {
        /* the DIAGNOSTIC autopilot (physics.h), not the original's */
        sim_arcade_autopilot_q(w->race->arcade ? w->race->arcade
                                               : w->race->pilot_q,
                               car, 0, w->race->route, w->race->geom, g);
    }
}

/* ---- FUN_00438208, 0x00438208..0x004383C2 ------------------------------ */
static void reset_gates(car_state *car, int32_t gmode, sim_input_globals *g)
{
    /* Gate 1: +0x26C neither 0 nor -1 - everything off, neutral, RETURN. */
    if (car->tick_26c_q != 0 && car->tick_26c_q != -1) {
        g->gear_down = g->gear_up = 0;
        g->pedal_a = g->pedal_b = 0;
        g->hbrake = 0x1F4;
        car->gear = 0;
        return;
    }
    /* The automatic's pedal arbitration, autom_1 == 2 (0x0043825B). */
    if (car->autom_1 == 2) {
        if (car->gear != 0) {
            if (car->gear < 0) {                       /* JNS skips at > 0  */
                int32_t t = g->pedal_a;                /* reverse: swap     */
                g->pedal_a = g->pedal_b;
                g->pedal_b = t;
            }
        } else {
            int32_t v = g->pedal_a;                    /* neutral: the      */
            g->sign_q = 1;                             /* larger, signed    */
            if (js(g->pedal_a, g->pedal_b)) {          /* (CMP / JNS)       */
                g->sign_q = -1;
                v = g->pedal_b;
            }
            g->pedal_a = v;
            g->pedal_b = 0;
        }
    }
    /* Gate 3: the pre-start hold, alivet_r < 0x320 (CMP / JNS). */
    if (js(car->alivet_r, 0x320)) {
        g->hbrake = 0x1F4;
        if (gmode != 1) {
            g->pedal_b = 0;                            /* NOT the throttle  */
            g->inputst = 0;
            g->gear_up = g->gear_down = 0;
        }
    }
    /* Gates 4 and 5: OUTOFTIM (+0xA4), OUTOFRAC (+0xA0). */
    if (car->oot != 0) {
        g->gear_down = g->gear_up = 0;
        g->pedal_a = g->pedal_b = 0;
        g->hbrake = 0x1F4;
        car->gear = 0;
    }
    if (car->oor != 0) {
        g->gear_down = g->gear_up = 0;
        g->pedal_a = g->pedal_b = 0;
        g->hbrake = 0x1F4;
        car->gear = 0;
    }
    /* Gate 6: RACE FIN (+0xA8) - the pedals and the gear, not the flags. */
    if (car->rfn != 0) {
        g->pedal_a = g->pedal_b = 0;
        g->hbrake = 0x1F4;
        car->gear = 0;
    }
}

/* ---- FUN_004383C3, 0x004383C3..0x00438615 ------------------------------ */

/* One accumulator's ramp, 0x004383EB..0x00438484 (and 0x00438486..0x0043851F
 * for the second, identical). */
static void ramp(int32_t *acc, int32_t inputst, float ste_k)
{
    /* MOV [scratch],0xA00000 / FILD / FMUL ste_k / FISTP */
    int32_t step = sim_x87_fistp(sim_x87_mul(sim_x87_from_i64(0xA00000),
                                             sim_x87_from_float(ste_k)));
    if (inputst != 0) {
        if (inputst < 0) {                             /* JNS: right        */
            *acc = wsub(*acc, step);
            if (js(*acc, (int32_t)0xE0000000u)) *acc = (int32_t)0xE0000000u;
        } else {
            *acc = wadd(*acc, step);
            if (!js(*acc, 0x20000000)) *acc = 0x20000000;
        }
        return;
    }
    /* Centred: back to zero at DOUBLE the rate, ADD EAX,EAX (0x00438453). */
    step = wadd(step, step);
    if (*acc == 0) return;
    if (*acc < 0) {
        *acc = wadd(*acc, step);
        if (*acc >= 0) *acc = 0;                       /* JS keeps          */
    } else {
        *acc = wsub(*acc, step);
        if (*acc < 0) *acc = 0;                        /* JNS keeps         */
    }
}

static void steering(car_state *car, const sim_input_globals *g)
{
    int32_t pul;

    if (g->inputst == 0
        || (car->driver != 2 && (car->dannyebx & 0x1000u) != 0u)) {
        /* The RAMP: a key, or nothing pressed.  +0x648 first. */
        ramp(&car->stangv, g->inputst, g->ste_k);
        ramp(&car->stang,  g->inputst, g->ste_k);
    } else {
        /* The DIRECT path, 0x00438523: an axis (bit 0x1000 clear), or a
         * driver-2 car.  IMUL 0x20000000 / SHRD EAX,EDX,8 is inputst << 21
         * in 32 bits; FILD / FMUL ste_j / FISTP; then the two clamps. */
        int32_t v = (int32_t)(uint32_t)((uint64_t)((int64_t)g->inputst
                                        * (int64_t)0x20000000) >> 8);
        car->stang = 0;
        v = sim_x87_fistp(sim_x87_mul(sim_x87_from_i64(v),
                                      sim_x87_from_float(g->ste_j)));
        if (js(v, (int32_t)0xE0000000u)) v = (int32_t)0xE0000000u;
        if (!js(v, 0x20000000)) v = 0x20000000;
        car->stangv = v;
        car->stang  = v;
    }

    /* The pull, 0x0043858F: FILD 0x20000000 / FMUL steerpull / FISTP. */
    pul = sim_x87_fistp(sim_x87_mul(sim_x87_from_i64(0x20000000),
                                    sim_x87_from_float(car->steerpull)));
    if (car->flags_010_q & 0x10u) pul = wsub(0, pul);  /* NEG             */
    car->steeringpul = pul;
    car->usteerangle = wadd(car->stang, pul);
    car->usteerangle_v_q = wadd(wadd(wadd(wadd(car->stangv, pul), pul), pul),
                                pul);

    /* FLD dam_steer (car+0x330, dam[30]) / FCOMP 100.0 / JB keeps: < 100, or
     * unordered.  So a NaN keeps the steering. */
    if (car->dam[30] >= 100.0f) {
        car->usteerangle = 0;
        car->usteerangle_v_q = 0;
    }
}

/* ---- FUN_00438616, 0x00438616..0x004386E4 ------------------------------ */

/* Bit SET ramps, bit CLEAR copies (TEST / JZ to the copy).  input-and-
 * had it the other way round. */
static void pedal(int32_t *out, int32_t staged, uint32_t ebx, uint32_t bit)
{
    if ((ebx & bit) == 0u) { *out = staged; return; }
    if (staged != 0) {
        *out = wadd(*out, 0x10);
        if (!js(*out, 0x100)) *out = 0x100;
    } else {
        *out = wsub(*out, 0x10);
        if (*out < 0) *out = 0;
    }
}

static void pedals(car_state *car, const sim_input_globals *g)
{
    pedal(&car->throttle, g->pedal_a, car->dannyebx, 0x4000u);
    pedal(&car->brake,    g->pedal_b, car->dannyebx, 0x2000u);
    /* +0x640: a COPY, not a ramp.  The reset value 0x1F4 is above 0x100
     * (JA, unsigned), so the handbrake field cannot zero it. */
    car->handbrake_ramp = g->hbrake;
    if ((uint32_t)g->hbrake <= 0x100u && car->handbrake != 0)
        car->handbrake_ramp = 0;
}

/* ---- FUN_004386E5, 0x004386E5..0x004387CA ------------------------------ */
static void rescue(car_state *car, int32_t gmode, sim_world *w)
{
    /* Held (bit 0x80), not arcade, and slow: FLD +0x1C4 / FCOMP 5.0 / JAE
     * leaves - so a NaN speed counts as slow. */
    sim_race_msg *msg = w->race ? &w->race->msg[w->race_slot & 7u] : NULL;
    if ((car->dannyebx & 0x80u) != 0u && gmode != 2
        && !(car->speed_len >= 5.0f)) {
        /* +0x264 == 0: the "rescue" message - above the cut, so it lands in
         * the race context's message slots when there is one. */
        if (car->rescue_hold_q == 0u && msg) {
            msg->topre = 0xCD;                         /* 205               */
            msg->flash = 0x19;
            msg->timer = 300;
        }
        car->rescue_hold_q++;
        if (car->rescue_hold_q != 100u) return;        /* JB / JNE          */
        car->catitken = wadd(car->catitken,
                             wsub(0x2BC, car->freerev_clear_1e8_q));
        if (msg) msg->w724 = msg->topre = msg->timer = msg->w728 = 0;
        /* CALL FUN_0043D4B3, the method's last act: the tick runs it, since
         * it needs the level (tick.c). */
        w->d4b3_due = 1;
        return;
    }
    if (car->rescue_hold_q != 0u) {
        if (msg) msg->w724 = msg->topre = msg->timer = msg->w728 = 0;
        car->rescue_hold_q = 0;
    }
}

void sim_input_stage_q(car_state *car, const uint32_t words[4], int human,
                       int32_t gmode, sim_input_globals *g, sim_world *w)
{
    copy_in(car, words, human, gmode, g, w);
    reset_gates(car, gmode, g);
    steering(car, g);
    pedals(car, g);
    rescue(car, gmode, w);
    /* The gearbox reads [0x005C65D8]/[0x005C65DC] inside the step. */
    w->gear_up_in   = g->gear_up;
    w->gear_down_in = g->gear_down;
    w->gear_sign_in = g->sign_q;
}

/* FUN_0045E3FC's two calls: the staging dwords [0x005C65FC..0x5C6608]
 * are zeroed at 0x0045E40C, so FUN_0043895C copies four zero words in, and
 * FUN_00438208 gates them.  FUN_004383C3, FUN_00438616 and FUN_004386E5 do
 * not run: +0x638..+0x650 and +0x264 keep what the car had. */
void sim_input_settle_q(car_state *car, int human, int32_t gmode,
                        sim_input_globals *g, sim_world *w)
{
    static const uint32_t zero[4] = { 0, 0, 0, 0 };
    copy_in(car, zero, human, gmode, g, w);
    reset_gates(car, gmode, g);
    w->gear_up_in   = g->gear_up;
    w->gear_down_in = g->gear_down;
    w->gear_sign_in = g->sign_q;
}

void sim_input_ste_q(const car_state *car, const sim_consts *k,
                     sim_input_globals *g)
{
    sim_world tmp;
    memset(&tmp, 0, sizeof(tmp));
    sim_setup_steer_q(car, k, &tmp);
    g->ste_k = tmp.ste_k;
    g->ste_j = tmp.ste_j;
}
