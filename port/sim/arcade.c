/* arcade.c - the arcade's rivals.  arcade.h has the
 * map of what is transcribed from where;
 * the write-up.
 *
 * THE ARITHMETIC.  The integer compares are the bytes' own: CMP / JS is the
 * sign of the WRAPPED difference (js() below), CMP / JB unsigned.  The x87
 * arithmetic is done in long double, as place.c does: the control word is
 * never narrowed, so every intermediate is 64-bit-significand
 * extended and each FSTP rounds once to float; on x86 long double IS that
 * format.  FISTP rounds to nearest-even. */

#include "arcade.h"
#include "physics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* CMP a,b / JS. */
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

/* NEG, wrapping: NEG 0x80000000 is 0x80000000. */
static int32_t wneg(int32_t a)
{
    return (int32_t)(0u - (uint32_t)a);
}

/* FISTP / FIST, round to nearest even; out of range or NaN is the integer
 * indefinite. */
static int32_t fistp(long double v)
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

static int32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (int32_t)v;
}

static uint32_t mulhi(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

/* ---- the image's tables ------------------------------------------------- */

static int rd_i32(const ral_image *im, uint32_t va, int32_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const unsigned char *p = ral_image_at(im, va + 4u * (uint32_t)i, 4);
        if (!p) return -1;
        out[i] = ral_image_i32(p);
    }
    return 0;
}

static int rd_f32(const ral_image *im, uint32_t va, float *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const unsigned char *p = ral_image_at(im, va + 4u * (uint32_t)i, 4);
        if (!p) return -1;
        out[i] = ral_image_f32(p);
    }
    return 0;
}

int sim_arcade_consts_read(const ral_image *im, sim_arcade_consts *k,
                           char *err, size_t n)
{
    int t, rc = 0;
    memset(k, 0, sizeof(*k));
    rc |= rd_i32(im, 0x005B89C8u, k->numarccars, SIM_ARCADE_LEVELS);
    rc |= rd_i32(im, 0x005B89E0u, k->bestpos, SIM_ARCADE_LEVELS);
    rc |= rd_f32(im, 0x005B89F8u, k->lo, SIM_ARCADE_LEVELS);
    rc |= rd_f32(im, 0x005B8A10u, k->hi, SIM_ARCADE_LEVELS);
    rc |= rd_i32(im, 0x005B9540u, k->order, SIM_ARCADE_CARS);
    rc |= rd_f32(im, 0x006CE978u, k->weave, 32);
    for (t = 0; t < 3 && rc == 0; t++) {
        const unsigned char *p = ral_image_at(im, 0x006CE9F8u + 0x1Cu * (uint32_t)t, 28);
        if (!p) rc = -1;
        else memcpy(k->speed[t], p, 28);
    }
    rc |= rd_f32(im, 0x005B921Cu, &k->band, 1);
    rc |= rd_f32(im, 0x005B9220u, &k->push, 1);
    rc |= rd_f32(im, 0x005B9224u, &k->thirty, 1);
    rc |= rd_f32(im, 0x006CEA4Cu, &k->upright, 1);
    rc |= rd_f32(im, 0x006CEA50u, k->up, 3);
    rc |= rd_f32(im, 0x0057AF90u, &k->respawn_speed, 1);
    rc |= rd_f32(im, 0x0057AFBCu, &k->one, 1);
    rc |= rd_f32(im, 0x0057B014u, &k->zero, 1);
    rc |= rd_f32(im, 0x0057B01Cu, &k->fixed, 1);
    rc |= rd_f32(im, 0x0057B0B0u, &k->steer_scale, 1);
    if (rc != 0) {
        if (err && n) snprintf(err, n, "the arcade's tables are not in the image");
        return -1;
    }
    return 0;
}

/* ---- the init: arcade_game_mode_init 0x00434C2B ------------------------- */

void sim_arcade_driver_clear_q(sim_arcade *a)
{
    int32_t c;
    /* 0x00460C7D: ECX = 9 down to 0 (DEC / JNS).  The 16-byte position
     * memories are cleared at ECX*4 + 0..12 - the first 13 dwords of each -
     * and the reverse count [0x006CEB3C] is not cleared at all. */
    for (c = 9; c >= 0; c--) {
        a->mark[c] = 0;
        a->hard[c] = 0;
        a->stuck_t[c] = 0;
        a->rev_t[c] = 0;
        a->worst[c] = 0;
        a->pos_new[c] = a->pos_new[c + 1] = a->pos_new[c + 2]
                      = a->pos_new[c + 3] = 0;
        a->pos_old[c] = a->pos_old[c + 1] = a->pos_old[c + 2]
                      = a->pos_old[c + 3] = 0;
    }
}

int sim_arcade_init_q(sim_arcade *a, const sim_arcade_consts *k,
                      int32_t level, int32_t stage, int32_t entry,
                      int32_t carno1, uint32_t arcrandom, car_state *car0)
{
    uint32_t ecx, esi, ebp;
    int32_t n;

    if ((uint32_t)level >= SIM_ARCADE_LEVELS || (uint32_t)stage >= SIM_ARCADE_LEVELS)
        return -1;                         /* the original indexes on        */
    a->k = k;
    /* 0x00434C6A..0x00434CBD */
    n = k->numarccars[level];
    if (n < 3 || n > SIM_ARCADE_LIST) return -1;
    a->n = n;
    a->lo = k->lo[level];
    a->hi = k->hi[level];
    if (entry == 2 || stage == 0) a->start = n;     /* stage 1 starts last */
    a->bestpos = k->bestpos[stage];
    /* (0x00434D0D..0x00434D71: numpl, numcars, FUN_00434FC3, the rivals'
     * +0x3C and driver, FUN_00435148 - the caller's load.) */

    /* 0x00434D76: [0x005C7984] = arcrandom; the start list.  EBP is the
     * last slot drawn (-1 at first), ESI the next car number, skipping
     * player 1's (and player 2's in a two-player game: not here). */
    a->rnd_fixed = arcrandom;
    ecx = 0;
    esi = 1;
    ebp = 0xFFFFFFFFu;
    for (;;) {
        uint32_t r, edx;
        /* 0x00434D91, where a redraw also comes back to */
        if (esi == (uint32_t)carno1) esi++;
        r = sim_car_rnd_next_q(&a->rnd_fixed);
        r = (r >> (ecx & 31u)) | (r << ((32u - (ecx & 31u)) & 31u));  /* ROR EAX,CL */
        edx = mulhi(r, 3u);
        if (edx == ebp) continue;          /* CMP EDX,EBP / JE 0x00434D91 */
        ebp = edx;
        a->slot[ecx] = (int32_t)(edx + 1u);
        a->carno[ecx] = (int32_t)esi;
        esi++;
        ecx++;
        if (ecx == (uint32_t)n) break;     /* CMP ECX,[N] / JNE           */
    }
    /* 0x00434DD9: the shuffle - the car numbers only, each swapped with one
     * drawn over the whole list.  The slot list is NOT shuffled. */
    for (ecx = 0; ecx != (uint32_t)n; ecx++) {
        uint32_t edx = mulhi(sim_car_rnd_next_q(&a->rnd_fixed), (uint32_t)n);
        int32_t t = a->carno[ecx];
        a->carno[ecx] = a->carno[edx];
        a->carno[edx] = t;
    }
    sim_arcade_driver_clear_q(a);          /* 0x00434E10 */
    /* (0x00434E15..0x00434E3C: FUN_0043595A on all four - the load's.) */
    car0->arc_started = 0;                 /* 0x00434E41 */
    car0->arc_w0 = 0;
    car0->arc_w1 = 0;
    car0->arc_behind = wsub(n, a->start);  /* 0x00434E5F: P = N - S        */
    a->spawns = a->despawns = a->fell_back = a->got_ahead = 0;
    a->resets_refused = 0;
    return 0;
}

/* ---- FUN_00447C3D: the rival car types ---------------------------------- */

/* FUN_00436172: the index of `car` in the order [0x005B9540], or 0. */
static int32_t car_rank(const sim_arcade_consts *k, int32_t car)
{
    int32_t i;
    for (i = 0; i < SIM_ARCADE_CARS; i++)
        if (k->order[i] == car) return i;
    return 0;
}

/* One pick: redrawn while it is type 6 (0x00447DF8), while its rank is 5 or
 * more from the player's (ABS, then CMP 5 / JAE), or while it repeats the
 * player's car or an earlier pick. */
static int32_t pick(const sim_arcade_consts *k, uint32_t *state, int fixed,
                    int32_t rank1, int32_t carnum1, const int32_t *earlier,
                    int nearlier)
{
    for (;;) {
        uint32_t r = fixed ? sim_car_rnd_next_q(state) : sim_rnd_next_q(state);
        int32_t d = (int32_t)mulhi(r, 23u), e;
        int i, again = 0;
        if (d == 6) continue;
        e = wsub(car_rank(k, d), rank1);
        if (js(e, 0)) e = wneg(e);
        if ((uint32_t)e >= 5u) continue;
        if (d == carnum1) continue;
        for (i = 0; i < nearlier; i++)
            if (d == earlier[i]) again = 1;
        if (again) continue;
        return d;
    }
}

void sim_arcade_rival_types_q(sim_arcade *a, const sim_arcade_consts *k,
                              int32_t carnum1, uint32_t arcrandom,
                              uint32_t *rnd)
{
    int32_t rank1 = car_rank(k, carnum1);
    a->rnd_fixed = arcrandom;              /* 0x00447C9E                   */
    a->types[0] = pick(k, &a->rnd_fixed, 1, rank1, carnum1, a->types, 0);
    a->types[1] = pick(k, rnd, 0, rank1, carnum1, a->types, 1);
    a->types[2] = pick(k, rnd, 0, rank1, carnum1, a->types, 2);
}

/* ---- the streamer: FUN_00435A90 ----------------------------------------- */

/* 0x00436044 for one record: carry when it is simulated and its nearestb
 * is within 4 of `m` (SUB / ABS / CMP 4 / JNS). */
static int near_car(const car_state *c, int32_t m)
{
    int32_t d;
    if (c->driver == 0) return 0;
    d = wsub(c->nearestb, m);
    if (js(d, 0)) d = wneg(d);
    return js(d, 4);
}

/* 0x00435FF7: carry = taken.  8 or more from either end of the route, and 4
 * or more markers from every simulated car - the four records at their
 * fixed addresses, whatever numcars is. */
static int marker_taken(car_state *const R[4], int32_t m, int32_t route_n)
{
    int i;
    if (js(m, 8)) return 1;
    if (!js(m, wsub(route_n, 8))) return 1;
    for (i = 0; i < 4; i++)
        if (near_car(R[i], m)) return 1;
    return 0;
}

/* 0x00436065: the rival in `slot` onto pnb + off, its skill from entry
 * a->wi, moving at a->spawn_speed along its own z. */
static int spawn(sim_arcade *a, sim_tick *t, uint32_t slot, int32_t off)
{
    sim_tick_car *tc = &t->car[slot & 7u];
    car_state *c = tc->rec, *c0 = t->car[0].rec;
    sim_race *rc = t->race;
    sim_place_ctx pc;
    long double q, s;
    int32_t at;
    int k;

    if (!tc->params || !tc->ground || !tc->world || !rc->route || !rc->geom
        || (uint32_t)a->wi >= SIM_ARCADE_LIST)
        return -1;
    a->car_number[slot & 7u] = a->carno[a->wi];   /* +0xAA8              */
    c->driver = 2;
    at = wadd(c0->nearestb, off);
    c->nearestb = at;                      /* +0x98, +0x94, +0x9C          */
    c->currentb = at;
    c->l_p_plyd = (uint32_t)at;
    rc->reset_index = at;                  /* [0x005E94A8], [0x005E94B0]: */
    rc->reset_frac = at;                   /* the fraction too, as stored  */
    /* 0x004360A3: lo + (hi - lo) * ((N - 1) - wi) / (N - 1) */
    q = (long double)a->n - (long double)a->k->one;
    q = (q - (long double)a->wi) / q;
    s = q * ((long double)a->hi - (long double)a->lo) + (long double)a->lo;
    a->skill_last = (float)s;
    c->arc_skill = (float)s;
    /* 0x004360D9 FUN_004447FB, 0x004360DE FUN_0043E00B */
    pc.p = tc->params;
    pc.k = t->k;
    pc.sin_table = t->sin_table;
    pc.ground = tc->ground;
    pc.dmg = &t->damage;
    if (sim_place_rest_q(c, rc->route, rc->geom, rc->reset_index,
                         rc->reset_frac, &pc, tc->world) != 0)
        return -1;
    /* 0x004360E3: the car-frame velocity (0, 0, speed), and FUN_0044B1AB
     * with the car's matrix into the world's: m[a]x + (m[4+a]y + m[8+a]z)
     * + m[12+a] */
    c->cxspd = 0.0f;
    c->cyspd_q = 0.0f;
    c->czspd = a->spawn_speed;
    for (k = 0; k < 3; k++)
        c->newspd[k] = (float)(((long double)c->matrix[k] * c->cxspd
                                + ((long double)c->matrix[4 + k] * c->cyspd_q
                                   + (long double)c->matrix[8 + k] * c->czspd))
                               + (long double)c->matrix[12 + k]);
    a->spawns++;
    return 0;
}

/* 0x00436135 */
static void despawn(sim_arcade *a, car_state *c, uint32_t slot)
{
    int32_t zero = 0;
    c->driver = 0;
    c->nearestb = 0;
    a->ab0[slot & 7u] = 0;                 /* +0xAB0                       */
    c->arc_skill = 0.0f;
    c->arc_factor = 0.0f;
    memcpy((uint8_t *)c + 0x44, &zero, 4); /* +0x44 [0x006CCCA8]           */
    a->despawns++;
}

/* 0x00435DF6 / 0x00435E12: a gap + 1 into the smaller of the two unsigned
 * minima - and EDX left holding it, stored or not. */
static uint32_t gap_ahead(car_state *c0, const car_state *w)
{
    uint32_t e = (uint32_t)wsub(w->nearestb, c0->nearestb) + 1u;
    if (e < c0->arc_gap_ahead) c0->arc_gap_ahead = e;
    return e;
}

static uint32_t gap_behind(car_state *c0, const car_state *w)
{
    uint32_t e = (uint32_t)wsub(c0->nearestb, w->nearestb) + 1u;
    if (e < c0->arc_gap_behind) c0->arc_gap_behind = e;
    return e;
}

/* CMP / JNS lo, then CMP / JS hi: v clamped to [lo, hi] by the sign of the
 * wrapped differences. */
static int32_t clamp_js(int32_t v, int32_t lo, int32_t hi)
{
    if (js(v, lo)) v = lo;
    if (!js(v, hi)) v = hi;
    return v;
}

/* 0x00435EA6..0x00435F04: x 330, snapped when more than 1,000 away. */
static void smooth_snap(int32_t *ms, uint32_t gap)
{
    int32_t want = (int32_t)(gap * 0x14Au), d = wsub(want, *ms);
    if (js(d, 0)) d = wneg(d);
    if ((uint32_t)d > 1000u) *ms = want;           /* JBE: unsigned       */
}

/* 0x00435F04..0x00435F32: then 1/64 of the way, SAR 6. */
static void smooth_close(int32_t *ms, uint32_t gap)
{
    int32_t want = (int32_t)(gap * 0x14Au);
    *ms = wadd(*ms, wsub(want, *ms) >> 6);
}

int sim_arcade_stream_q(sim_arcade *a, sim_tick *t)
{
    car_state *R[4], *c0, *w;
    int32_t n, P, pnb, e, tt, pos;
    uint32_t edx;
    int i;

    if (!a || !a->k || !t || !t->race || !t->race->route || t->numcars < 4)
        return -1;
    for (i = 0; i < 4; i++) R[i] = t->car[i].rec;
    c0 = R[0];
    n = a->n;

    if (c0->arc_started == 0) {
        /* 0x00435A9D: W0 = list[S - 2] 80 markers ahead, W1 = list[S - 1]
         * 40 ahead, both at rest.  The second index goes to Wi0 again
         * (0x00435B0F): Wi1 is not set here. */
        int32_t i0 = wsub(a->start, 2), i1 = wadd(i0, 1);
        if ((uint32_t)i0 >= SIM_ARCADE_LIST || (uint32_t)i1 >= SIM_ARCADE_LIST)
            return -1;
        c0->arc_wi0 = i0;
        a->wi = i0;
        c0->arc_w0 = a->slot[i0];
        a->spawn_speed = 0.0f;
        if (spawn(a, t, (uint32_t)c0->arc_w0, 0x50) != 0) return -1;
        c0->arc_wi0 = i1;
        a->wi = i1;
        c0->arc_w1 = a->slot[i1];
        a->spawn_speed = 0.0f;
        if (spawn(a, t, (uint32_t)c0->arc_w1, 0x28) != 0) return -1;
        c0->arc_started = 1;
    }

    /* 0x00435B55: the window's spacing, from the shuffled car number at the
     * player's place */
    e = clamp_js(c0->arcadeposition, 0, n);
    e = clamp_js(wsub(e, 1), 0, n);
    if ((uint32_t)e >= SIM_ARCADE_LIST) return -1;
    tt = a->carno[e] & 0xF;
    a->d1 = 0x2C + tt;
    a->d2 = 0x28 + tt;
    a->dsum = a->d2 + a->d1;
    pnb = c0->nearestb;

    /* 0x00435BBB: fell back - the rival ahead is D1 or more ahead */
    if (c0->arc_behind != 0) {
        w = R[c0->arc_w0 & 3];
        if (!js(wsub(w->nearestb, pnb), a->d1)) {
            if (marker_taken(R, wsub(pnb, a->d2), t->race->route->n))
                goto position;             /* JB 0x00435D5E: past both arms */
            despawn(a, w, (uint32_t)c0->arc_w0);
            c0->arc_w0 = c0->arc_w1;
            c0->arc_wi0 = c0->arc_wi1;
            c0->arc_wi1 = wsub(n, c0->arc_behind);
            a->wi = c0->arc_wi1;
            if ((uint32_t)a->wi >= SIM_ARCADE_LIST) return -1;
            c0->arc_w1 = a->slot[a->wi];
            a->spawn_speed = a->k->respawn_speed;
            if (spawn(a, t, (uint32_t)c0->arc_w1, wneg(a->d2)) != 0) return -1;
            c0->arc_behind = wsub(c0->arc_behind, 1);
            a->fell_back++;
        }
    }

    /* 0x00435C7E: got ahead - the rival behind is D1 or more behind, and
     * the ceiling not reached: P == N - 3, or P >= N - 2 - bestpos (JAE) */
    P = c0->arc_behind;
    if (P == wsub(n, 3)) goto position;
    if ((uint32_t)P >= (uint32_t)wsub(wsub(n, 2), a->bestpos)) goto position;
    w = R[c0->arc_w1 & 3];
    if (js(wsub(pnb, w->nearestb), a->d1)) goto position;
    if (marker_taken(R, wadd(pnb, a->d2), t->race->route->n)) goto position;
    despawn(a, w, (uint32_t)c0->arc_w1);
    c0->arc_wi1 = c0->arc_wi0;
    c0->arc_w1 = c0->arc_w0;
    c0->arc_wi0 = wsub(wsub(n, 3), c0->arc_behind);
    a->wi = c0->arc_wi0;
    if ((uint32_t)a->wi >= SIM_ARCADE_LIST) return -1;
    c0->arc_w0 = a->slot[a->wi];
    a->spawn_speed = a->k->respawn_speed;
    if (spawn(a, t, (uint32_t)c0->arc_w0, a->d2) != 0) return -1;
    c0->arc_behind = wadd(c0->arc_behind, 1);
    a->got_ahead++;

position:
    /* 0x00435D5E: the position and the gaps, frozen at the finish */
    if (c0->rfn == 0) {
        int32_t wslot[2];
        c0->arc_gap_ahead = 0xFFFFFFFFu;
        c0->arc_gap_behind = 0xFFFFFFFFu;
        pos = wsub(n, c0->arc_behind);
        pnb = c0->nearestb;
        edx = (uint32_t)c0->bloffstz;      /* EDX: slot 0's bloffstz ...    */
        wslot[0] = c0->arc_w1;
        wslot[1] = c0->arc_w0;
        for (i = 0; i < 2; i++) {
            w = R[wslot[i] & 3];
            if (js(pnb, w->nearestb)
                || (pnb == w->nearestb && js((int32_t)edx, w->bloffstz))) {
                edx = gap_ahead(c0, w);    /* ... until a helper runs */
            } else {
                pos = wsub(pos, 1);
                edx = gap_behind(c0, w);
            }
        }
        c0->arcadeposition = pos;
        /* 0x00435E2E: a synthetic gap where no rival is */
        if (c0->arc_gap_behind == 0xFFFFFFFFu) {
            if (c0->arc_behind == 0) goto smoothing;
            c0->arc_gap_behind = (uint32_t)clamp_js(
                wsub(a->dsum, (int32_t)c0->arc_gap_ahead), a->d2, a->dsum);
        }
        if (c0->arc_gap_ahead == 0xFFFFFFFFu && c0->arcadeposition != 1)
            c0->arc_gap_ahead = (uint32_t)clamp_js(
                wsub(a->dsum, (int32_t)c0->arc_gap_behind), a->d2, a->dsum);
smoothing:
        smooth_snap(&c0->arc_gap_behind_ms, c0->arc_gap_behind);
        smooth_snap(&c0->arc_gap_ahead_ms, c0->arc_gap_ahead);
        smooth_close(&c0->arc_gap_behind_ms, c0->arc_gap_behind);
        smooth_close(&c0->arc_gap_ahead_ms, c0->arc_gap_ahead);
    }

    /* 0x00435F38: the spacing push, every record's cleared first */
    for (i = 0; i < 4; i++) R[i]->arc_push = 0.0f;
    {
        car_state *w0 = R[c0->arc_w0 & 3], *w1 = R[c0->arc_w1 & 3];
        int32_t d = wsub(w0->nearestb, w1->nearestb), v;
        c0->arc_spacing_d = d;
        if (!js(d, 8) || js(d, -8) || d == 0) return 0;
        v = js(d, 0) ? wsub(-8, d) : wsub(8, d);
        w0->arc_push = (float)((long double)v * (long double)a->k->push);
        w1->arc_push = (float)((long double)wneg(v) * (long double)a->k->push);
    }
    return 0;
}

int sim_arcade_reset_taken_q(const car_state *rival, int32_t cand,
                             int32_t route_n)
{
    int32_t d;
    /* 0x0043D5A3 */
    if (rival->driver != 2) return 0;
    if (js(cand, 8)) return 1;
    if (!js(cand, wsub(route_n, 8))) return 1;
    d = wsub(cand, rival->currentb);
    if (!js(d, 8)) return 0;
    if (js(d, -8)) return 0;
    return 1;
}

/* ---- the rubber band, FUN_0043618F -------------------------------------- */

static void rubber_band(sim_arcade *a, car_state *car, const car_state *car0)
{
    int32_t lead = wsub(car->nearestb, car0->nearestb);
    long double f;
    if (js(lead, 0)) lead = 0;
    if (!js(lead, 100)) lead = 100;
    car->arc_band = (float)((long double)lead * (long double)a->k->band);
    f = ((long double)car->arc_skill + (long double)car->arc_band)
        + (long double)car->arc_push;
    car->arc_factor = (float)f;
    /* FCOMPP / SHR AH,1: C0 - below, or unordered */
    if (!(car->arc_factor >= a->k->zero)) car->arc_factor = a->k->zero;
    if (!(car->arc_factor < a->k->one)) car->arc_factor = a->k->one;
}

/* ---- the rival driver, FUN_004601BA ------------------------------------- */

/* The route lookups' clamp: LEA [nb*4 + base + 4k] / SUB base / SHR 2, then
 * JB n or n - 1. */
static uint32_t at(int32_t nb, int32_t k, int32_t n)
{
    uint32_t i = ((uint32_t)nb * 4u + 4u * (uint32_t)k) >> 2;
    if (i >= (uint32_t)n) i = (uint32_t)n - 1u;
    return i;
}

/* [0x0098DC08 + 4i] is angle[i + 1]. */
static int32_t heading(const sim_route *r, int32_t nb, int32_t k)
{
    return r->angle[at(nb, k, r->n) + 1u];
}

/* The route marker's point (x, 0, z): the polygon [0x00983B38 + 4i], its
 * +0x04, the point 12 bytes a record. */
static void marker_xz(const sim_route *r, const sim_ground_geom *g,
                      uint32_t i, int32_t v[3])
{
    const uint8_t *pol = g->pols + (size_t)r->pol[i] * 0x40u;
    const uint8_t *p = g->pnts + (size_t)rd32(pol + 4) * 12u;
    v[0] = rd32(p);
    v[1] = 0;
    v[2] = rd32(p + 8);
}

/* FUN_0041E9C6 on a float triple: |v| at extended precision; zero (or
 * unordered, C3) gives zero; else each component / |v|, and -0.0 to 0. */
static void normalize(float v[3])
{
    long double l = sqrtl(((long double)v[0] * v[0])
                          + (((long double)v[1] * v[1])
                             + ((long double)v[2] * v[2])));
    int i;
    if (!(l != 0.0L)) {
        v[0] = v[1] = v[2] = 0.0f;
        return;
    }
    for (i = 0; i < 3; i++) {
        v[i] = (float)((long double)v[i] / l);
        if (v[i] == 0.0f) v[i] = 0.0f;             /* 0x80000000 -> 0      */
    }
}

/* FUN_0041FD06: out = b x c as the FPU orders it - ECX is c, EBX b. */
static void cross(const float b[3], const float c[3], float out[3])
{
    out[0] = (float)((long double)c[2] * b[1] - (long double)b[2] * c[1]);
    out[1] = (float)((long double)c[0] * b[2] - (long double)b[0] * c[2]);
    out[2] = (float)((long double)c[1] * b[0] - (long double)b[1] * c[0]);
}

/* 0x00460C3F then 0x00460C57: an integer difference, FILD / FSTP each. */
static void diff_f(const int32_t to[3], const int32_t from[3], float out[3])
{
    out[0] = (float)wsub(to[0], from[0]);
    out[1] = 0.0f;
    out[2] = (float)wsub(to[2], from[2]);
}

static void driver(sim_arcade *a, car_state *car, uint32_t slot,
                   const sim_route *r, const sim_ground_geom *g)
{
    const sim_arcade_consts *k = a->k;
    int32_t nb, A, B, c, e, t, ti, m = 0, v0[3], v1[3];
    uint32_t speed;
    const uint8_t *tab;
    float D[3], S[3], V[3], F[3], X3[3];
    int32_t *pnew, *pold;
    long double sv;

    memset(a->out, 0, sizeof(a->out));     /* 0x004601BB..0x004601EA      */
    a->ac = 0;
    a->stuck4 = 0;
    a->sharp = 0;
    a->narrow = 0;
    m = 0;                                 /* 0x004601EF: [0x006CEBA8] = 0,
                                            * the controller's slot */
    if ((uint32_t)car->alivet_r < 0x320u) return;   /* JB: the countdown  */
    if (!(car->matrix[5] >= k->upright)) return;    /* +0xC8, upright    */
    if (!r || r->n < 1 || !g) return;
    pnew = &a->pos_new[m * 4];
    pold = &a->pos_old[m * 4];
    nb = car->nearestb;

    /* 0x00460245: the polygon two markers on, its bytes +0x0A and +0x0B */
    {
        const uint8_t *pol = g->pols + (size_t)r->pol[at(nb, 2, r->n)] * 0x40u;
        a->poly_a = pol[0x0A];
        a->poly_b = pol[0x0B];
        if (js(a->poly_a + a->poly_b, 0x12)) {
            a->narrow = 1;
            a->ac = 1;
        }
        a->ac = 1;                         /* 0x004602AC, both arms */
    }
    /* 0x004602B6: speed = 2 x |(+0x198, +0x1A0)| */
    sv = sqrtl((long double)car->newspd[0] * car->newspd[0]
               + (long double)car->newspd[2] * car->newspd[2]);
    speed = (uint32_t)fistp(sv) << 1;
    a->speed = (int32_t)speed;

    /* 0x004602E2: the corner ahead */
    if (a->ac != 1) {
        if (speed >= 0x6Eu) { A = heading(r, nb, 15); B = heading(r, nb, 16); c = 15; }
        else                { A = heading(r, nb, 4);  B = heading(r, nb, 5);  c = 4; }
    } else if ((uint32_t)a->cls920[slot & 7u] >= 3u) {
        if (speed >= 0x46u) { A = heading(r, nb, 15); B = heading(r, nb, 16); c = 15; }
        else                { A = heading(r, nb, 3);  B = heading(r, nb, 4);  c = 3; }
    } else {
        if (speed >= 0x5Au) { A = heading(r, nb, 14); B = heading(r, nb, 15); c = 14; }
        else                { A = heading(r, nb, 4);  B = heading(r, nb, 5);  c = 4; }
    }
    e = wsub(B, A) >> 16;                  /* SAR 0x10, then ABS           */
    if (js(e, 0)) e = wneg(e);
    a->turn = e;
    if (!js(e, a->ac == 1 ? 0x780 : 0x980)) {
        if (!js(e, 0xC00)) a->hard[m] = 1;
        if (!js(e, a->worst[m])) {          /* EDX: the slot, 0     */
            a->worst[m] = e;
            a->mark[m] = wadd(nb, c);
        }
    }
    /* 0x00460594: passed the remembered corner */
    if (!js(nb, a->mark[m])) {
        a->mark[m] = 0;
        a->worst[m] = 0;
    }
    /* 0x004605BE: the target speed */
    e = wsub(heading(r, nb, 4), heading(r, nb, 0)) >> 25;
    if (js(e, 0)) e = wneg(e);
    e = wadd(e, 10);
    if ((uint32_t)e >= 0x18u) e = 0x18;
    if ((uint32_t)e >= 0x10u) a->sharp = 1;
    tab = k->speed[0];
    if (a->ac == 1) tab = k->speed[1];
    if ((uint32_t)a->cls920[slot & 7u] >= 3u) tab = k->speed[2];
    t = tab[e];
    if (a->mark[m] != 0) {
        if (a->hard[m] != 0) t = 0x19;
        else t = js((int32_t)speed, 0x28) ? 0x32 : 0x28;
    } else {
        a->hard[m] = 0;
    }
    a->target = t;
    if ((uint32_t)t >= 0x32u)
        t = fistp(((long double)t - (long double)k->thirty) * (long double)a->factor
                  + (long double)k->thirty);
    /* 0x004606D5: the pedals, unsigned */
    if (speed < (uint32_t)t) {
        a->out[1] = 0x100;
    } else if (speed >= (uint32_t)t + 5u) {
        if (speed >= (uint32_t)t + 15u) a->out[3] = 1;
        a->out[2] = 0xFF;
    }

    /* 0x00460712: the steering target, weaved sideways */
    a->weave_q = k->weave[(uint32_t)wadd(nb, m) & 31u];
    marker_xz(r, g, at(nb, 0, r->n), v0);
    if (a->hard[m] != 0) {
        if (speed >= 0x46u)      ti = (int32_t)at(nb, 5, r->n);
        else if (speed >= 0x28u) ti = (int32_t)at(nb, 4, r->n);
        else                     ti = (int32_t)at(nb, 2, r->n);
    } else {
        ti = (int32_t)at(nb, 2, r->n);
        if (a->sharp == 1 && speed >= 0x46u) ti = (int32_t)at(nb, 4, r->n);
        if (speed >= 0x5Au)      ti = (int32_t)at(nb, 5, r->n);
        else if (speed >= 0x3Cu) ti = (int32_t)at(nb, 3, r->n);   
    }
    marker_xz(r, g, (uint32_t)ti, v1);
    diff_f(v1, v0, D);
    normalize(D);
    cross(D, k->up, S);
    normalize(S);
    v1[0] = wadd(v1[0], fistp((long double)S[0] * a->weave_q * k->fixed));
    v1[2] = wadd(v1[2], fistp((long double)S[2] * a->weave_q * k->fixed));
    /* 0x00460A3F: the car's own x, z into the position memory */
    pnew[0] = car->pos[0];
    pnew[1] = 0;
    pnew[2] = car->pos[2];
    diff_f(v1, pnew, V);
    normalize(V);
    F[0] = -car->matrix[2];                /* FCHS of +0xBC                */
    F[1] = 0.0f;
    F[2] = car->matrix[10];                /* +0xDC                        */
    cross(F, V, X3);
    {
        long double s = (long double)X3[1] * k->steer_scale;
        int32_t i = fistp(s), eax = js(i, 0) ? wneg(i) : i;
        a->steer = i;
        if (!(s >= 0.0L)) eax = wneg(eax); /* C0: below, or unordered      */
        if (car->gear == -1) eax = wneg(eax);
        a->out[0] = eax;
    }
    /* 0x00460B35: reversing out */
    if (a->rev_t[m] != 0) {
        a->rev_t[m]--;
        if (car->gear != -1) {
            a->out[5] = 1;
            a->out[2] = 0x100;
        }
    } else if (js(car->gear, 1)) {
        a->out[4] = 1;
    }
    /* 0x00460B81: every 200 steps, has it moved half a metre? */
    a->stuck_t[m] = wadd(a->stuck_t[m], 1);
    if (js(a->stuck_t[m], 200)) return;
    a->stuck_t[m] = 0;
    {
        int32_t dx = wsub(pnew[0], pold[0]), dz = wsub(pnew[2], pold[2]);
        pold[0] = pnew[0];
        pold[2] = pnew[2];
        if (js(dx, 0)) dx = wneg(dx);
        if (js(dz, 0)) dz = wneg(dz);
        if (js(dx, dz)) dz = (int32_t)((uint32_t)dz >> 1);
        else            dx = (int32_t)((uint32_t)dx >> 1);
        a->moved = wadd(dx, dz);
    }
    if (!js(a->moved, 0x8000)) return;
    if (car->gear != -1) {
        a->rev_t[m] = 0xFA;
    } else {
        int32_t cnt = wadd(a->rev_n[m], 1);
        a->rev_t[m] = 0;
        if ((uint32_t)cnt >= 4u) {
            cnt = 0;
            a->stuck4 = 1;
        }
        a->rev_n[m] = cnt;
    }
}

void sim_arcade_drive_q(sim_arcade *a, car_state *car, uint32_t slot,
                        const car_state *car0, const sim_route *r,
                        const sim_ground_geom *geom, sim_input_globals *g)
{
    if (!a || !a->k || !car0) return;
    /* 0x00438A75: [0x006CEBA8] = the slot; FUN_0043618F; the factor */
    rubber_band(a, car, car0);
    a->factor = car->arc_factor;
    driver(a, car, slot, r, geom);
    g->inputst = a->out[0];                /* [0x005C65C8]                 */
    g->pedal_a = a->out[1];                /* [0x005C65CC]                 */
    g->pedal_b = a->out[2];                /* [0x005C65D0]                 */
    g->hbrake = a->out[3];                 /* [0x005C65D4]                 */
    g->gear_up = a->out[4];                /* [0x005C65D8]                 */
    g->gear_down = a->out[5];              /* [0x005C65DC]                 */
}

void sim_arcade_autopilot_q(sim_arcade *a, car_state *car, uint32_t slot,
                            const sim_route *r, const sim_ground_geom *geom,
                            sim_input_globals *g)
{
    if (!a || !a->k) return;
    /* 0x00438ADE: [0x006CEBA8] = the slot, FLD1 into [0x006CEA80], the
     * driver - no rubber band - and the same six copies */
    a->factor = a->k->one;
    driver(a, car, slot, r, geom);
    g->inputst = a->out[0];
    g->pedal_a = a->out[1];
    g->pedal_b = a->out[2];
    g->hbrake = a->out[3];
    g->gear_up = a->out[4];
    g->gear_down = a->out[5];
}
