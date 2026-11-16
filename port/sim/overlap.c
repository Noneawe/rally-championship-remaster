/* overlap.c - FUN_0045D16A, car versus car.  the closing session.
 *
 * is the reading (sections 2-4, from the bytes) and
 * its section 13 the specification this was built from.  The three functions
 * are transcribed in the order of the bytes:
 *
 *   FUN_0045D16A  0x0045D16A..0x0045D257  the loop over the roster
 *   FUN_0045D258  0x0045D258..0x0045D305  one corner: transform, strict test
 *   FUN_0045D306  0x0045D306..0x0045D635  the response, for one inside corner
 *
 * THE PRECISION.  FUN_0044B1AB leaves its three results on the FPU, so D258's
 * divide and D306's car-frame velocity are rounded ONCE, at the FSTP, from a
 * 64-bit significand.  So are the velocity exchange's two product sums.  Both
 * are done in ground.h's sim_x87 model, not in double.  The compose,
 * FUN_00425E1D, is the port's sim_matrix_mul, as everywhere else.
 *
 * WHAT IS NOT MODELLED, AND WHY IT CANNOT MATTER
 *   [0x006868D8]            the hit flag: no absolute reader (section 14)
 *   [0x006869C4..E0]        the hit record: no absolute reader (section 14)
 *   [0x00685DB4..BC]        the corner in the other car's frame.  Every
 *                           later reader writes it first (B453, B56C, B796,
 *                           C90A and CB48 before CCD3; C358 before BCE8's
 *                           response): disasm.tsv, 2026-09-29
 *   [0x006869FC..A04], |.|  the relative velocity and its length: read only
 *                           by the sound's volume, FUN_0045AC49
 *   [0x005D6EA0] = 3.0      FUN_00420010's popped factor, a scratch
 *   FUN_0045A98A            the sound request: counted, collision_car_hits */

#include "collision_int.h"

#include <string.h>

static sim_x87 X(float f) { return sim_x87_from_float(f); }

/* FUN_0044B1AB: v . M + M's row 3, all at extended precision.  The x87
 * stack's grouping, read 2026-09-29: m[a]*x + (m[4+a]*y + m[8+a]*z), then
 * + m[12+a], for a = 0, 1, 2. */
static void vec_by_matrix_x87(const sim_x87 v[3], const float m[16],
                              sim_x87 out[3])
{
    int a;
    for (a = 0; a < 3; a++)
        out[a] = sim_x87_add(
            sim_x87_add(sim_x87_mul(X(m[a]), v[0]),
                        sim_x87_add(sim_x87_mul(X(m[4 + a]), v[1]),
                                    sim_x87_mul(X(m[8 + a]), v[2]))),
            X(m[12 + a]));
}

/* 0x0045D5C6 / 0x0045D5F9: FLD z / FLD y / FLD x / FUN_0044B1AB / FSTP
 * +0x1A4, +0x1A8, +0x1AC - newspd into the car's own frame.  Exported
 * (collision_int.h) so twocar_check can test it against the game's own
 * +0x9CC. */
void sim_car_overlap_cspd_q(car_state *car, const float frame[16])
{
    sim_x87 v[3], r[3];
    v[0] = X(car->newspd[0]);
    v[1] = X(car->newspd[1]);
    v[2] = X(car->newspd[2]);
    vec_by_matrix_x87(v, frame, r);
    car->cxspd = sim_x87_to_float(r[0]);
    car->cyspd_q = sim_x87_to_float(r[1]);
    car->czspd = sim_x87_to_float(r[2]);
}

/* FUN_0045D258: corner `c` through M.  1 if it is inside the other car's
 * box.  FLD [corner] / FMUL 65536.0 (exact), FUN_0044B1AB, FDIV 65536.0,
 * FSTP float; then FLD / FABS / FCOMP [box] / FNSTSW / SHR AH,1 / JAE: C0
 * into CF, and a CLEAR carry leaves.  So |p| == box is OUTSIDE, and an
 * unordered compare (a NaN coordinate) sets C0 and counts as INSIDE. */
static int corner_inside(const sim_consts *k, const float m[16], int c)
{
    sim_x87 scale = X(k->pos_scale), v[3], r[3];
    float p[3];
    int a;

    for (a = 0; a < 3; a++)
        v[a] = sim_x87_mul(X(k->car_corner[a][c]), scale);
    vec_by_matrix_x87(v, m, r);
    for (a = 0; a < 3; a++)
        p[a] = sim_x87_to_float(sim_x87_div(r[a], scale));
    for (a = 0; a < 3; a++) {
        float mag = p[a] < 0.0f ? -p[a] : p[a];     /* FABS, NaN kept    */
        if (mag >= k->car_box[a]) return 0;         /* C0 clear: JAE out */
    }
    return 1;
}

/* 0x0045D4E1: FILD cur.pos / FISUB oth.pos (exact, not wrapped) /
 * FDIV 65536.0 / FMUL 0.5 / FSTP.  Three dimensional: y included.  Then
 * FUN_00420010 scales it by 3.0.  Exported for twocar_check. */
void sim_car_overlap_push_q(const car_state *cur, const car_state *oth,
                            const sim_consts *k, float push[3])
{
    int a;
    for (a = 0; a < 3; a++) {
        sim_x87 d = sim_x87_from_i64((int64_t)cur->pos[a]
                                     - (int64_t)oth->pos[a]);
        push[a] = sim_x87_to_float(sim_x87_mul(sim_x87_div(d, X(k->pos_scale)),
                                               X(k->car_push_half)));
    }
    sim_vec3_scale(push, k->car_push_scale);          /* FUN_00420010, 3.0 */
}

/* FUN_0045D306: the response.  `cur` is the working block (its +0x9CC is
 * w->active_frame, which C100 does NOT restore), `oth` the roster record. */
static void respond(car_state *cur, sim_world *w, car_state *oth,
                    const float *oth_frame, const sim_consts *k)
{
    sim_x87 lo = X(k->car_exchange_lo), hi = X(k->car_exchange_hi);
    float push[3];
    int a;

    /* 0x0045D3A2: extension record 6's FORCE, its only store anywhere: a
     * zero (section 5).  There is no impulse and no force record. */
    w->collision_force[6].f[0] = 0.0f;
    w->collision_force[6].f[1] = 0.0f;
    w->collision_force[6].f[2] = 0.0f;

    if (w->collision_car_hits == 0) {                 /* port diagnostic  */
        memcpy(w->collision_car_in[0], cur->newspd, sizeof(cur->newspd));
        memcpy(w->collision_car_in[1], oth->newspd, sizeof(oth->newspd));
        w->collision_car_first_pass = w->collision_passes;
    }

    /* [0x006CCE98] = 1 and [other+0x234] = 1.  NO timer (+0x1F4), NO
     * +0x224 point, NO +0x230 peak. */
    cur->collision_active = 1;
    w->collision_active = 1;
    oth->collision_active = 1;

    /* 0x0045D41B, per axis x, y, z: both sums from the OLD values, each
     * product pair summed on the FPU and rounded once.  cur*0.6 + oth*0.4
     * is pushed first and goes to the OTHER car; cur*0.4 + oth*0.6 to cur. */
    for (a = 0; a < 3; a++) {
        sim_x87 c = X(cur->newspd[a]), o = X(oth->newspd[a]);
        sim_x87 to_oth = sim_x87_add(sim_x87_mul(c, hi), sim_x87_mul(o, lo));
        sim_x87 to_cur = sim_x87_add(sim_x87_mul(c, lo), sim_x87_mul(o, hi));
        cur->newspd[a] = sim_x87_to_float(to_cur);
        oth->newspd[a] = sim_x87_to_float(to_oth);
    }

    /* cur.pos is AFTER C100's rollback. */
    sim_car_overlap_push_q(cur, oth, k, push);
    for (a = 0; a < 3; a++) {
        cur->newspd[a] = sim_x87_to_float(sim_x87_add(X(cur->newspd[a]),
                                                      X(push[a])));
        oth->newspd[a] = sim_x87_to_float(sim_x87_sub(X(oth->newspd[a]),
                                                      X(push[a])));
    }

    /* The OTHER car first, through its own +0x9CC; then the current car. */
    sim_car_overlap_cspd_q(oth, oth_frame);
    sim_car_overlap_cspd_q(cur, w->active_frame);

    w->collision_car_hits++;                          /* FUN_0045A98A     */
}

/* How many slots the do-while visits: slot 0 at least. */
static uint32_t visited(const sim_roster *r)
{
    return r->numcars == 0 ? 1u : r->numcars;
}

int sim_roster_check_q(const sim_roster *roster)
{
    uint32_t i;

    if (!roster || (roster->gmode != 0 && roster->gmode != 2)) return 0;
    if (roster->numcars > SIM_ROSTER_SLOTS) return -1;
    for (i = 0; i < visited(roster); i++)
        if (i != roster->slot
            && (!roster->car[i] || !roster->active_frame[i]))
            return -1;
    return 0;
}

int sim_car_overlap_q(car_state *car, const sim_consts *k,
                      sim_ground_points *points, sim_world *w,
                      const sim_collision_snapshot *snapshot,
                      const sim_roster *roster)
{
    uint32_t i;

    /* 0x0045D16A: CMP [gmode],2 / JE; CMP [gmode],0 / JE; RET. */
    if (!roster || (roster->gmode != 2 && roster->gmode != 0)) return 0;
    if (!car || !k || !points || !w || !snapshot
        || sim_roster_check_q(roster) != 0)
        return -1;

    i = 0;
    do {
        car_state *oth;
        float t[16], a[16], m[16];
        int c;

        if (i == roster->slot) continue;              /* 0x0045D18D       */
        oth = roster->car[i];                         /* no AND 7         */
        if (oth->driver == 0) continue;               /* +0x25C           */

        /* FUN_00425DBE: T is the .data identity (0x0059ED94, never written)
         * with its translation set to the RAW 16.16 difference - a 32-bit
         * SUB, so it wraps - through FILD / FSTP float.  Not divided by
         * 65536: D258 scales the corner up instead. */
        memset(t, 0, sizeof(t));
        t[0] = t[5] = t[10] = t[15] = 1.0f;
        for (c = 0; c < 3; c++)
            t[12 + c] = (float)(int32_t)((uint32_t)car->pos[c]
                                         - (uint32_t)oth->pos[c]);
        sim_matrix_mul(car->matrix, t, a);            /* -> [0x0059EE54]  */
        sim_matrix_mul(a, roster->active_frame[i], m);/* -> [0x00686964]  */

        /* Built ONCE per other car.  A hit's rollback moves the current car
         * and M is not rebuilt: the remaining corners test a stale M. */
        for (c = 0; c < 8; c++) {
            if (!corner_inside(k, m, c)) continue;
            sim_collision_rollback_q(car, points, w, snapshot);   /* C100 */
            respond(car, w, oth, roster->active_frame[i], k);
        }
    } while (++i < roster->numcars);                  /* JB, unsigned     */
    return 0;
}
