/* place.c - putting a car on the start line.
 *
 * Read whole from retail's bytes this session (tools/ralimg.py disasm);
 * has the transcription, and
 * sections 1-2a the route passes it reads.
 *
 * raceinit_q (0x00444445) runs, in this order, for the race about to start:
 *
 *   setbestline_q       0x004438FE  the route: order, headings, ordinals
 *   initcarpositions_q  0x004445EF  per car: FUN_00444639 (here), then two
 *                                   clears ABOVE the 1,668-byte record
 *   initobjectsoncourse 0x0045C8C5  objects back where they stood
 *   gamemodestuff_init  0x00434B2D  mode tables - no car field (gmode 0)
 *   messagesystem_init  0x00443011  car +0x71C.. and +0xA0C.. (above)
 *   carparts_init       0x0045FB76  [0x006CE5A4 + 0x78 k]: not the car
 *   resettimers         0x00442FB0  sim_reset_timers_q
 *   singlecarinit       0x00444569  FIFTY settle steps, FUN_0045E3FC
 *                                   (sim_tick.settle)
 *   carparts_init again, resetalldamagemessages (0x0043E2B9: message slots)
 *
 * So what lands in the replayed record is FUN_00444639's, resettimers', and
 * the settle's.  This file is the first two; tick.c runs the third.
 *
 * WHERE THE START IS.  Not stored: it is DERIVED from the route.  FUN_0044440C
 * takes the first route marker after the first whose +0x26 tag is set - the
 * start line - and FUN_004447FB puts the car on that marker's point, facing
 * the next segment's heading, 0.7 m up (car_model_y_q).  FUN_00444639 then
 * moves it 2 m back along the heading, and in a two-player or network game
 * 1.25 m to the side by slot parity (slot 0 left).  The leveller drops it.
 * Run 18 (LEVEL33) and run 20 (LEVEL61, two cars) both put their frame 0
 * within centimetres of that. */

#include "physics.h"
#include "camera.h"
#include "arcade.h"

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

static int32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return (int32_t)v;
}

/* A field the record has no name for yet, by its offset. */
static void put32(car_state *car, unsigned off, uint32_t v)
{
    memcpy((uint8_t *)car + off, &v, sizeof(v));
}

/* CMP a,b / JS - the sign of the WRAPPED difference (input.c has the why). */
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

static int32_t wneg_i(int32_t a)
{
    return (int32_t)(0u - (uint32_t)a);
}

/* FISTP, round to nearest even; out of range or NaN is the integer
 * indefinite.  objects.c's rule, here on a long double. */
static int32_t fistp_ld(long double v)
{
    long double f, d, r;
    if (!(v > -2147483648.5L && v < 2147483647.5L)) return INT32_MIN;
    f = floorl(v);
    d = v - f;
    if (d > 0.5L)      r = f + 1.0L;
    else if (d < 0.5L) r = f;
    else               r = (fmodl(f, 2.0L) == 0.0L) ? f : f + 1.0L;
    return (int32_t)r;
}

static const uint8_t *marker(const uint8_t *pols, int32_t pol)
{
    return pols + (size_t)pol * 0x40u;
}

/* ---- the route: FUN_004438D6, FUN_00443A33, FUN_00443B6B, FUN_00443C37 -- */

int sim_route_build(sim_route *r, const uint8_t *pols, int32_t npols,
                    const uint8_t *pnts, int32_t npnts, int32_t variant)
{
    int32_t count = 0, n, i, first;

    memset(r, 0, sizeof(*r));
    /* FUN_004438D6: [0x006CE08C] 0 -> 0x3E, 1 -> 0x3D, else 0. */
    r->tex = variant == 0 ? 0x3E : variant == 1 ? 0x3D : 0;
    r->pol = (int32_t *)malloc(sizeof(int32_t) * (size_t)(npols > 0 ? npols : 1));
    if (!r->pol) return -2;

    /* FUN_00443A33: every class-0xFF record whose texture BYTE is the
     * selected one (CMP [esi+1],AL), in pols order. */
    for (i = 0; i < npols; i++) {
        const uint8_t *m = marker(pols, i);
        if (m[0] == 0xFF && m[1] == (uint8_t)r->tex) r->pol[count++] = i;
    }
    n = 0;
    if (count >= 2) {                              /* JB 0x00443B56          */
        /* The first with tag BYTE +0x26 == 1 goes to the front; none leaves
         * the count 0. */
        for (first = 0; first < count; first++)
            if (marker(pols, r->pol[first])[0x26] == 1) break;
        if (first < count) {
            int32_t t = r->pol[first];
            r->pol[first] = r->pol[0];
            r->pol[0] = t;
            n = 1;
            /* Each next: the previous marker's +0x0C link, searched over the
             * WHOLE collected list from 0 (0x00443AFC), swapped into place. */
            while (n != count) {
                int32_t link = rd32(marker(pols, r->pol[n - 1]) + 0x0C), e;
                for (e = 0; e < count; e++)
                    if (r->pol[e] == link) break;
                if (e == count) break;
                t = r->pol[n];
                r->pol[n] = r->pol[e];
                r->pol[e] = t;
                n++;
            }
        }
    }
    r->n = n;

    /* angle[-1 .. n], and the ordinals. */
    r->angle_mem = (int32_t *)calloc((size_t)n + 3u, sizeof(int32_t));
    r->ord = (int32_t *)calloc((size_t)n + 1u, sizeof(int32_t));
    if (!r->angle_mem || !r->ord) {
        sim_route_free(r);
        return -2;
    }
    r->angle = r->angle_mem + 1;
    for (i = 0; i < n; i++) {
        int32_t pi = rd32(marker(pols, r->pol[i]) + 0x04);
        if (pi < 0 || pi >= npnts) {
            sim_route_free(r);
            return -1;
        }
    }

    /* FUN_00443B6B: for i = 1 .. n-1, FILD dx, FILD dz, FPATAN (ST1 = dx,
     * ST0 = dz: atan2(dx, dz)), FLDPI / FDIVP, x 0.5, x 65536, x 65536,
     * FISTP.  THE PORT'S ARITHMETIC: atan2l in the host's long double - the
     * x87's own FPATAN on x86 hosts with an 80-bit long double, a wider or
     * narrower one elsewhere.  The result is a 32-bit binary angle, so a
     * difference needs the quotient within 2^-32 of a half-integer; and only
     * its top 16 bits reach the car's matrix (getmatcrash_q, SHR 0x10). */
    if (n >= 2) {
        for (i = 1; i < n; i++) {
            long double dx, dz, a;
            {
                const uint8_t *P = pnts + (size_t)rd32(marker(pols, r->pol[i - 1]) + 0x04) * 12u;
                const uint8_t *Q = pnts + (size_t)rd32(marker(pols, r->pol[i]) + 0x04) * 12u;
                dx = (long double)wsub(rd32(Q + 0), rd32(P + 0));
                dz = (long double)wsub(rd32(Q + 8), rd32(P + 8));
            }
            a = atan2l(dx, dz) / 3.14159265358979323846264338327950288L;
            a = a * 0.5L * 65536.0L * 65536.0L;
            r->angle[i] = fistp_ld(a);
        }
        r->angle[n] = r->angle[n - 1];             /* 0x00443C19             */
    }
    r->angle[0] = r->angle[1];                     /* 0x00443C27: also runs  */
    r->angle[-1] = r->angle[1];                    /* when n < 2             */

    /* FUN_00443C37: ord increments BEFORE the store on a tag neither 0 nor 1. */
    if (n >= 2) {
        int32_t ord = 0;
        for (i = 0; i < n; i++) {
            uint8_t tag = marker(pols, r->pol[i])[0x26];
            if (tag != 0 && tag != 1) ord++;
            r->ord[i] = ord;
        }
    }
    return 0;
}

void sim_route_free(sim_route *r)
{
    free(r->pol);
    free(r->angle_mem);
    free(r->ord);
    memset(r, 0, sizeof(*r));
}

static const int32_t *route_point(const sim_route *r, const uint8_t *pols,
                                  const uint8_t *pnts, int32_t i)
{
    return (const int32_t *)(const void *)
           (pnts + (size_t)rd32(marker(pols, r->pol[i]) + 0x04) * 12u);
}

/* ---- the helpers FUN_00444639 calls ------------------------------------- */

/* FUN_0044440C: the first route index from 1 whose tag byte is non-zero -
 * the start line - or 0.  A do-while against [0x005D6510] = n - 1: with
 * n == 2 the original tests index 1, then runs past the list; the port stops
 * at the list's end and returns 0. */
static int32_t start_marker(const sim_route *r, const uint8_t *pols)
{
    int32_t e;
    if (js(r->n, 2)) return 0;                     /* CMP [..],2 / JS        */
    e = 1;
    do {
        if (e >= r->n) return 0;                   /* the port's bound       */
        if (marker(pols, r->pol[e])[0x26] != 0) return e;
        e++;
    } while (e != r->n - 1);
    return 0;
}

/* 0x0041EA8B -> 0x0041EC71: FILD each of x, z, FSTP float, then
 *   idx = (angle + 0x80000) >>> 20,  s = sin[idx],  c = sin[idx + 1024]
 *   x' = x c + z s,  z' = z c - x s      (each one FSTP, read 2026-10-06)
 * then FLD / FISTP each back.  EBX (y) goes through unchanged.  EAX = x,
 * ECX = z, EDX = the angle at both callers read (0x0044473D, 0x0043FAA2). */
void sim_table_rotate_q(const float *sin_table, uint32_t angle,
                        int32_t *x, int32_t *z)
{
    uint32_t i = (angle + 0x80000u) >> 20;
    sim_x87 s = sim_x87_from_float(sin_table[i]);
    sim_x87 c = sim_x87_from_float(sin_table[i + SIM_SIN_QUARTER]);
    sim_x87 xx = sim_x87_from_float((float)*x);
    sim_x87 zz = sim_x87_from_float((float)*z);
    float x1 = sim_x87_to_float(sim_x87_add(sim_x87_mul(xx, c), sim_x87_mul(zz, s)));
    float z1 = sim_x87_to_float(sim_x87_sub(sim_x87_mul(zz, c), sim_x87_mul(xx, s)));
    *x = fistp_ld((long double)x1);
    *z = fistp_ld((long double)z1);
}

/* IMUL [frac] / SHRD EAX,EDX,0x10 / ADD base. */
static int32_t lerp16(int32_t a, int32_t b, int32_t frac)
{
    int64_t prod = (int64_t)wsub(b, a) * (int64_t)frac;
    return wadd((int32_t)(uint32_t)((uint64_t)prod >> 16), a);
}

/* FUN_004447FB: the car onto route index `idx`, `frac` (16.16) of the way to
 * the next.  Returns [0x00581AA8], the heading. */
static int32_t place_on_route(car_state *car, const sim_route *r,
                              const uint8_t *pols, const uint8_t *pnts,
                              const sim_consts *k, int32_t idx, int32_t frac,
                              sim_world *w)
{
    uint32_t ecx = (uint32_t)idx, edx;
    const int32_t *A, *B;
    int32_t heading;

    if (ecx >= (uint32_t)r->n) ecx = (uint32_t)r->n - 1u;    /* JB: unsigned */
    edx = ecx + 1u;
    if (edx >= (uint32_t)r->n) edx = (uint32_t)r->n - 1u;
    A = route_point(r, pols, pnts, (int32_t)ecx);
    B = route_point(r, pols, pnts, (int32_t)edx);
    car->pos[0] = lerp16(A[0], B[0], frac);
    /* the y also gets [0x005C6CF8], 0.7 m */
    car->pos[1] = wadd(lerp16(A[1], B[1], frac), k->car_model_y_q);
    car->pos[2] = lerp16(A[2], B[2], frac);
    /* 0x0098DC08 + 4i is angle[i + 1]: the segment LEAVING the marker. */
    heading = lerp16(r->angle[ecx + 1u], r->angle[edx + 1u], frac);
    w->binary_angle[0] = heading;                  /* +0x9C0                 */
    return heading;
}

/* FUN_0043F87C: the nearest marker in x/z over [lo, hi).  Each distance is
 * FUN_0043F8A1's FILD dx / FMUL ST0 / FILD dz / FMUL ST0 / FADDP / FSQRT /
 * FISTP - or 0xFFFFFFFF for an index outside [0, n) (CMP ECX,0 / JS and
 * CMP ECX,n / JNS) - and a distance <= the best so far (JA skips only a
 * LARGER one, unsigned) takes over - so a tie goes to the LATER index.  EBP
 * starts at 0, EDI at 0xFFFFFFFF; the loop is a do-while on INC ECX / CMP
 * ECX,EDX / JB.  The squares and their sum are exact in 64 bits; the port
 * takes the square root in the host's long double. */
void sim_route_nearest_q(const sim_route *r, const sim_ground_geom *geom,
                         int32_t x, int32_t z, int32_t lo, int32_t hi,
                         int32_t *at_out, uint32_t *dist_out)
{
    uint32_t best = 0xFFFFFFFFu, e = (uint32_t)lo;
    int32_t at = 0;
    do {
        uint32_t d = 0xFFFFFFFFu;
        if (!js((int32_t)e, 0) && js((int32_t)e, r->n)) {
            const int32_t *P = route_point(r, geom->pols, geom->pnts, (int32_t)e);
            int64_t dx = wsub(P[0], x), dz = wsub(P[2], z);
            uint64_t sq = (uint64_t)(dx * dx) + (uint64_t)(dz * dz);
            d = (uint32_t)fistp_ld(sqrtl((long double)sq));
        }
        if (!(d > best)) {
            best = d;
            at = (int32_t)e;
        }
        e++;
    } while (e < (uint32_t)hi);
    if (at_out) *at_out = at;
    if (dist_out) *dist_out = best;
}

/* FUN_0043F7D3: FUN_0043F87C over [0, n), or 0 when there is no route.  It
 * also stores the winning distance / 65536 at [0x005C7998] (FILD EDI: a
 * SIGNED load), which `nearest_dist` receives when given. */
static int32_t nearest_marker(const sim_route *r, const sim_ground_geom *geom,
                              int32_t x, int32_t z, float *nearest_dist)
{
    int32_t at;
    uint32_t d;
    if (r->n == 0) return 0;
    sim_route_nearest_q(r, geom, x, z, 0, r->n, &at, &d);
    if (nearest_dist) *nearest_dist = (float)((double)(int32_t)d / 65536.0);
    return at;
}

/* ---- the leveller: FUN_0043E0F6 ----------------------------------------- */

/* getmatcrash_q, FUN_0043B919: the top 16 bits of +0x9C4 (pitch), +0x9C0
 * (heading), +0x9C8 (roll) through FUN_00425BF1 (about x), FUN_00425C3A
 * (about y) and FUN_00425C82 (about z) - each writes four slots of its own
 * scratch matrix, whose other twelve are .data's identity - then
 *   car matrix = (roll x pitch) x heading            (FUN_00425E1D twice) */
static void matrix_from_angles(car_state *car, const sim_world *w,
                               const float *sin_table)
{
    float rx[16], ry[16], rz[16], t[16];
    uint32_t ip = (((uint32_t)w->binary_angle[1] >> 16) & 0xFFFFu) >> 4;
    uint32_t ih = (((uint32_t)w->binary_angle[0] >> 16) & 0xFFFFu) >> 4;
    uint32_t ir = (((uint32_t)w->binary_angle[2] >> 16) & 0xFFFFu) >> 4;
    float s, c;

    memset(rx, 0, sizeof(rx));
    memset(ry, 0, sizeof(ry));
    memset(rz, 0, sizeof(rz));
    rx[0] = rx[15] = 1.0f;                         /* 0x0059ECD4              */
    ry[5] = ry[15] = 1.0f;                         /* 0x0059ED14              */
    rz[10] = rz[15] = 1.0f;                        /* 0x0059ED54              */
    s = sin_table[ip]; c = sin_table[ip + SIM_SIN_QUARTER];
    rx[5] = c; rx[6] = s; rx[9] = -s; rx[10] = c;  /* XOR 0x80000000: -s     */
    s = sin_table[ih]; c = sin_table[ih + SIM_SIN_QUARTER];
    ry[0] = c; ry[2] = -s; ry[8] = s; ry[10] = c;
    s = sin_table[ir]; c = sin_table[ir + SIM_SIN_QUARTER];
    rz[0] = c; rz[1] = s; rz[4] = -s; rz[5] = c;
    sim_matrix_mul(rz, rx, t);                     /* ESI 0x59ED54, EDI 0x59ECD4 */
    sim_matrix_mul(t, ry, car->matrix);            /* ESI 0x59EE94, EDI 0x59ED14 */
}

/* FUN_0043D033 then the query driver FUN_0045AE5D. */
static int leveller_query(car_state *car, const sim_ground_geom *geom,
                          const sim_ground *ground, sim_ground_inputs *in,
                          const sim_world *w, const float *sin_table)
{
    matrix_from_angles(car, w, sin_table);
    return sim_ground_query_contacts(geom, car, in, ground->state);
}

static int level_car(car_state *car, const sim_ground_geom *geom,
                     const car_params *p, const sim_consts *k,
                     const float *sin_table, const sim_ground *ground,
                     const sim_damage_globals *dmg, sim_world *w)
{
    sim_ground_inputs in = ground->in;
    sim_ground_points *gp = ground->state;
    uint32_t step_a = 0x20000000u, step_b = 0x20000000u;   /* 1/8 turn   */
    int32_t m;
    int pass, i;

    /* FUN_0043D033: the damage consumer (FUN_00439F90 - FUN_0043E2D3 for
     * driver 2, unported, as in the step), then the setup scalars, of which
     * the query reads susph and suspt. */
    if (car->driver != 2) sim_damage_efficiency_q(car, k, dmg);
    sim_setup_values(car, p, k, w);
    in.susph = w->susph;
    in.suspt = w->suspt;

    w->binary_angle[1] = 0;                        /* +0x9C4  [0x006CD628]   */
    w->binary_angle[2] = 0;                        /* +0x9C8  [0x006CD62C]   */
    /* [0x005D31A0] = 0x80: the driver's own mask (SIM_GROUND_MASK_CONTACT). */
    for (pass = 0; pass < 8; pass++) {
        if (leveller_query(car, geom, ground, &in, w, sin_table) != 0) return -1;
        /* left (0 + 2) against right (1 + 3) drives the roll, +0x9C8 */
        if (js(wadd(gp->clear[0], gp->clear[2]), wadd(gp->clear[1], gp->clear[3])))
            w->binary_angle[2] = wsub(w->binary_angle[2], (int32_t)step_b);
        else
            w->binary_angle[2] = wadd(w->binary_angle[2], (int32_t)step_b);
        step_b >>= 1;                              /* SHR: logical           */
        /* front (0 + 1) against rear (2 + 3) drives the pitch, +0x9C4 */
        if (js(wadd(gp->clear[0], gp->clear[1]), wadd(gp->clear[2], gp->clear[3])))
            w->binary_angle[1] = wsub(w->binary_angle[1], (int32_t)step_a);
        else
            w->binary_angle[1] = wadd(w->binary_angle[1], (int32_t)step_a);
        step_a >>= 1;
    }
    if (leveller_query(car, geom, ground, &in, w, sin_table) != 0) return -1;
    /* The drop: the lowest of 0x00640000 (100 m) and the four wheels'
     * clearances (CMP / JNS), off the car's y and the query's buffers. */
    m = 0x00640000;
    for (i = 0; i < 4; i++)
        if (js(gp->clear[i], m)) m = gp->clear[i];
    car->pos[1] = wsub(car->pos[1], m);
    for (i = 0; i < 4; i++) {
        gp->clear[i] = wsub(gp->clear[i], m);
        gp->iy[i] = wsub(gp->iy[i], m);
    }
    return 0;
}

/* ---- FUN_0043E00B -------------------------------------------------------- */

/* Level, refresh the active frame, and come to rest.  Four callers
 * FUN_00444639 at the start, FUN_0043DFC4 on a
 * reset, the arcade rival spawn and a debug key. */
static int car_rest(car_state *car, const sim_ground_geom *geom,
                    const car_params *p, const sim_consts *k,
                    const float *sin_table, const sim_ground *ground,
                    const sim_damage_globals *dmg, sim_world *w)
{
    if (level_car(car, geom, p, k, sin_table, ground, dmg, w) != 0) return -1;
    sim_refresh_active_frame(w, car, k);           /* FUN_0043D630           */
    car->newspd[0] = car->newspd[1] = car->newspd[2] = 0.0f;
    car->cxspd = 1.0f;                             /* 0x0043E028: as read    */
    car->cyspd_q = 0.0f;
    car->czspd = 0.0f;
    car->rotspd[0] = car->rotspd[1] = car->rotspd[2] = 0.0f;
    /* +0x6B0..+0x6B8 and [0x005BB80C..14] take the angles: above the record. */
    car->freerev_clear_1e8_q = 0;                  /* +0x1E8                 */
    car->fwhlspd = car->rwhlspd = 0.0f;            /* +0x1B8, +0x1BC         */
    car->gear = 0;
    car->yrotspd = car->wantyrotspd = 0.0f;        /* +0x200, +0x204         */
    car->speed_len = 0.0f;                         /* +0x1C4                 */
    car->collision_response_timer = 0;             /* +0x1F4                 */
    /* +0x6B0..+0x6B8 and +0x68C's hand-over: the camera's half. */
    if (w && w->camera) sim_camera_rest_q(w->camera, w->binary_angle);
    return 0;
}

int sim_place_rest_q(car_state *car, const sim_route *r,
                     const sim_ground_geom *geom, int32_t idx, int32_t frac,
                     const sim_place_ctx *pc, sim_world *w)
{
    if (!r || r->n < 1 || !pc->ground || !pc->ground->state) return -1;
    place_on_route(car, r, geom->pols, geom->pnts, pc->k, idx, frac, w);
    return car_rest(car, geom, pc->p, pc->k, pc->sin_table, pc->ground,
                    pc->dmg, w);
}

/* ---- FUN_00444639 -------------------------------------------------------- */

int sim_place_car_q(car_state *car, uint32_t slot, const sim_start_mode *m,
                    const sim_route *route, const sim_ground_geom *geom,
                    const car_params *p, const sim_consts *k,
                    const float *sin_table, const sim_ground *ground,
                    const sim_damage_globals *dmg, sim_world *w,
                    int32_t *slot_angle_out)
{
    const uint8_t *pols = geom->pols, *pnts = geom->pnts;
    int32_t heading = 0, idx, seg;

    if (!ground || !ground->state) return -1;
    /* 0x00444639..0x004446BB.  +0x694 and +0xAD4/+0xAD8 (0x80000000) are
     * above the record. */
    put32(car, 0x478, 0);
    put32(car, 0x1E4, 0);
    car->revs = 0.0f;                              /* +0x1D8                 */
    car->gear = 0;                                 /* +0x1D4                 */
    car->throttle = car->brake = car->handbrake_ramp = 0;   /* +0x638..640  */
    car->stang = car->stangv = 0;                  /* +0x644, +0x648         */
    car->usteerangle = car->usteerangle_v_q = 0;   /* +0x64C, +0x650         */

    if (m->gmode != 4) {
        if (route->n < 1) return -1;               /* reads before the array */
        idx = start_marker(route, pols);
        if (m->gmode != 1 && m->gmode != 3)        /* two cars a marker      */
            idx = wsub(idx, (int32_t)(slot >> 1));
        if (m->no_route_q) idx = 0;                /* [0x5E94A8] = 0         */
        heading = place_on_route(car, route, pols, pnts, k, idx, 0, w);
        if (!m->no_route_q) {
            int32_t x = 0, z = (int32_t)0xFFFE0000u;            /* 2 m back */
            sim_table_rotate_q(sin_table, (uint32_t)heading, &x, &z);
            car->pos[0] = wadd(car->pos[0], x);
            car->pos[2] = wadd(car->pos[2], z);
            if (m->network
                || (m->gmode == 0 && m->numpl != 1)) {
                x = (int32_t)(((slot & 1u) * 2u - 1u) * 0x14000u); /* 1.25 m */
                z = 0;
                sim_table_rotate_q(sin_table, (uint32_t)heading, &x, &z);
                car->pos[0] = wadd(car->pos[0], x);
                car->pos[2] = wadd(car->pos[2], z);
            }
        }
    }

    /* 0x00444798: FUN_0043F7D3, the nearest marker, into +0x98, +0x94,
     * +0x9C; its ordinal less one, clamped to 0..100, into +0x90 and +0xAC. */
    idx = nearest_marker(route, geom, car->pos[0], car->pos[2], NULL);
    car->nearestb = idx;
    car->currentb = idx;
    car->l_p_plyd = (uint32_t)idx;
    seg = route->n ? route->ord[idx] - 1 : -1;
    if (js(seg, 0)) seg = 0;
    if (!js(seg, 0x64)) seg = 0x64;
    car->currsegment = seg;
    car->nextchck = seg;

    /* 0x004447D7: FUN_0043E00B. */
    if (car_rest(car, geom, p, k, sin_table, ground, dmg, w) != 0) return -1;

    /* 0x004447DC: +0x264, +0x268, +0x26C. */
    car->rescue_hold_q = 0;
    car->tctime = 0;
    car->tick_26c_q = 0;
    if (slot_angle_out) *slot_angle_out = heading;
    return 0;
}

/* ---- FUN_0043D4B3 and FUN_0043DFC4: the car reset ---------------- */

/* FUN_0043DFC4, read 2026-10-06: from wherever the car now stands, the
 * nearest marker (FUN_0043F7D3) into nearestb, currentb, +0x9C and BOTH of
 * [0x005E94A8] / [0x005E94B0] - the fraction too, as read, so the car goes
 * idx/65536 of the way on to the next marker - then FUN_004447FB and
 * FUN_0043E00B.  +0xFB4 = 8 is rc->fb4_q; +0xFB0 = 8 is above the record and not modelled;
 * +0xFAC = 10. */
static int respawn(car_state *car, sim_world *w, sim_race *rc,
                   const sim_place_ctx *pc, uint32_t slot)
{
    const sim_route *r = rc->route;
    int32_t idx = nearest_marker(r, rc->geom, car->pos[0], car->pos[2],
                                 &rc->nearest_dist);
    car->nearestb = idx;                           /* +0x098                 */
    car->currentb = idx;                           /* +0x094                 */
    car->l_p_plyd = (uint32_t)idx;                 /* +0x09C                 */
    rc->reset_index = idx;
    rc->reset_frac = idx;
    place_on_route(car, r, rc->geom->pols, rc->geom->pnts, pc->k,
                   rc->reset_index, rc->reset_frac, w);
    if (car_rest(car, rc->geom, pc->p, pc->k, pc->sin_table, pc->ground,
                 pc->dmg, w) != 0)
        return -1;
    rc->fac_q[slot] = 10;
    rc->fb4_q[slot] = 8;                          /* +0xFB4, 0x0043DFF6     */
    return 0;
}

/* FUN_0043D4B3, read 2026-10-06:
 *   [0x005E94A8] = currentb ; [0x005E94B0] = 0
 *   gmode == 2: a search of +/-64 route indices for one no other car's
 *               driver-2 record is near (FUN_0043D5A3) - its outcome with
 *               no rivals only (below)
 *   FUN_004447FB          the car onto currentb's marker
 *   FUN_0043DFC4          and again onto the marker nearest THERE
 *   +0xFAC = 10
 * The log string beside the arcade search, "cant reset car anywhere -
 * waiting a second", names the routine. */
int sim_car_reset_q(car_state *car, sim_world *w, sim_race *rc,
                    const sim_place_ctx *pc)
{
    uint32_t slot = w->race_slot & 7u;

    if (!rc || !rc->route || rc->route->n < 1 || !pc->ground
        || !pc->ground->state)
        return -1;                     /* the original reads before the array */
    rc->reset_index = car->currentb;
    rc->reset_frac = 0;
    /* gmode 2 (0x0043D4C7, read, ported): offsets k = 0, -0,
     * 1, -1 .. 63, -63 from currentb until slots 1..3 all clear
     * FUN_0043D5A3 - CF (taken) only for a RIVAL (driver 2) when the
     * candidate is within 8 of a route end or within -8..7 markers of its
     * currentb.  The records are the ARRAY's: a rival resetting itself
     * finds its own record there.  None free: the log line, +0x1E8 -= 100
     * (a second later) and no reset - not even +0xFAC. */
    if (rc->mode.gmode == 2) {
        int32_t k = 0;
        for (;;) {
            int32_t cand = wadd(rc->reset_index, k), s, taken = 0;
            for (s = 1; s <= 3 && !taken; s++)
                if (rc->rec[s])
                    taken = sim_arcade_reset_taken_q(rc->rec[s], cand,
                                                     rc->route->n);
            if (!taken) {
                rc->reset_index = cand;            /* 0x0043D584         */
                break;
            }
            k = wneg_i(k);                         /* NEG                */
            cand = wadd(rc->reset_index, k);
            taken = 0;
            for (s = 1; s <= 3 && !taken; s++)
                if (rc->rec[s])
                    taken = sim_arcade_reset_taken_q(rc->rec[s], cand,
                                                     rc->route->n);
            if (!taken) {
                rc->reset_index = cand;
                break;
            }
            k = wadd(wneg_i(k), 1);                /* NEG / INC          */
            if (k == 0x40) {
                /* "cant reset car anywhere - waiting a second" */
                car->freerev_clear_1e8_q = wsub(car->freerev_clear_1e8_q, 100);
                if (rc->arcade) rc->arcade->resets_refused++;
                return 1;
            }
        }
    }
    place_on_route(car, rc->route, rc->geom->pols, rc->geom->pnts, pc->k,
                   rc->reset_index, rc->reset_frac, w);
    if (respawn(car, w, rc, pc, slot) != 0) return -1;
    rc->fac_q[slot] = 10;
    w->d4b3_ran++;
    return 0;
}

/* ---- resettimers_q and FUN_00442FEA -------------------------------------- */

void sim_reset_timers_q(car_state *car, const sim_start_mode *m, uint32_t seed)
{
    int32_t t = m->countdown_skip ? 0x320 : 0;
    car->alivet_r = t;                             /* +0x014                 */
    car->alivet = t;                               /* +0x008                 */
    car->started = 0;                              /* +0x018                 */
    car->fs_count = 0;                             /* +0x01C                 */
    if (seed == 0) seed = 1;                       /* JNE / INC EAX          */
    car->flags_010_q = seed;                       /* +0x010                 */
    car->rnd = seed;                               /* +0x00C                 */
}
