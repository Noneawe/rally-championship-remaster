/* race.c - the race rules: the stage clock, sections and checkpoints, the
 * finish and the false start.
 *
 * is the reading; every function here was
 * transcribed again from retail's bytes this session (tools/ralimg.py disasm),
 * and where the bytes said more than the document, the document is corrected
 *
 *
 * WHERE EACH PIECE RUNS, inside FUN_00439B37 (the step) unless noted:
 *
 *   0x00439B50  FUN_004351C9   mass (sim_derive_mass), then the rule dispatch
 *                              on [0x005D6750]: sim_race_rules_q
 *   0x00439E11  FUN_0043F9E5   the route tracker and the crossing test:
 *                              sim_route_track_q, sim_checkpoint_crossed_q
 *   0x00439E1B  FUN_0043F3BA   msgtimer: sim_message_timer_q
 *   0x00439EAB  FUN_00439EB6   the false start: sim_false_start_q
 *   0x0045E5E7  FUN_0043F31E   after the slot loop, the countdown plate and
 *                              its beep for players 1 and 2:
 *                              sim_countdown_q, from tick.c
 *   0x004436AD  FUN_004436C1   after each step of FUN_0044368D's batch: is
 *                              the race over?  sim_race_over_q
 *   at load     FUN_00458715   initcheckpoints_q, in sim_race_init
 *   raceinit_q  0x00443011     messagesystem_init: sim_message_system_init_q
 *
 * THE ORDER IS VISIBLE IN THE CAPTURE.  The crossing test runs late in step n
 * and moves currsegment; the rule step runs early in step n + 1 and moves
 * nextchck.  Run 18 shows exactly that one-step lag at all nine of its
 * sections.
 *
 * WHAT IS NOT HERE, AND IS COUNTED: ~~gmode 2's arms (arcade,
 * section 7 - out of)~~ (the arcade's clock: the countdown, the
 * checkpoint's award, out of time, the chimes - its rivals and position are
 * not), gmode 1 / 3's live position (section 8), and every sound request.
 * sim_race says how many times each was due. */

#include "physics.h"
#include "camera.h"

#include <math.h>
#include <string.h>

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

static int32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return (int32_t)v;
}

/* FISTP, round to nearest even; out of range or NaN is the integer
 * indefinite.  place.c's rule. */
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

/* A route index's marker point, or NULL where the original would read
 * outside the route or the level. */
static const int32_t *route_point(const sim_race *rc, int32_t i)
{
    const sim_route *r = rc->route;
    int32_t pol, pnt;
    if (i < 0 || i >= r->n) return NULL;
    pol = r->pol[i];
    if (pol < 0 || pol >= rc->geom->npols) return NULL;
    pnt = rd32(rc->geom->pols + (size_t)pol * 0x40u + 0x04);
    if (pnt < 0 || pnt >= rc->geom->npnts) return NULL;
    return (const int32_t *)(const void *)(rc->geom->pnts + (size_t)pnt * 12u);
}

/* ---- initcheckpoints_q, FUN_00458715 ------------------------------------ */

sim_sound_feed *sim_sound_feed_local(sim_world *w, sim_sound_view *view)
{
    sim_race *rc = w ? w->race : NULL;
    uint32_t numpl;
    if (!rc) return NULL;
    numpl = rc->mode.numpl > 0 ? (uint32_t)rc->mode.numpl : 1u;
    if ((w->race_slot & 7u) >= numpl) return NULL;
    *view = rc->snd_view;
    if (!view->pos && w->camera) view->pos = w->camera->cam.pos;
    return &rc->snd[w->race_slot & 7u];
}

static void ghost_table(sim_race *rc);

void sim_race_init(sim_race *rc, const sim_route *route,
                   const sim_ground_geom *geom, const sim_start_mode *m)
{
    int32_t e, found = 0, i;

    memset(rc, 0, sizeof(*rc));
    rc->route = route;
    rc->geom = geom;
    rc->mode = *m;
    for (i = 0; i < SIM_ROSTER_SLOTS; i++) {
        rc->currpos[i] = -1;
        /* as messagesystem_init leaves them (sim_message_system_init_q) */
        rc->gap_ahead[i] = rc->gap_behind[i] = -1;
        rc->type_ahead[i] = rc->type_behind[i] = -1;
    }
    /* 0x00434A34: gamemode_q builds the ghosts' table at the start, the
     * defaults until the caller hands records (rc->ghost_recs) */
    ghost_table(rc);
    /* No countdown plate until FUN_0043F355 writes one - THE PORT'S
     * initial value: what the HUD block holds before the first write is
     * not read. */
    rc->olaydraw[0] = rc->olaydraw[1] = -1;
    /* the feedback records as 0x0040D213 leaves them; no camera
     * until the caller gives one. */
    for (i = 0; i < 8; i++) sim_sound_feed_init(&rc->snd[i]);
    rc->snd_view.rot[0] = rc->snd_view.rot[5] = rc->snd_view.rot[10]
        = rc->snd_view.rot[15] = 1.0f;

    /*   numcheckposits = 0
     *   if ([0x5D650C] < 2) return                    CMP / JS
     *   for (edx = 1; ; ) {                           a do-while
     *       if (pols[route[edx]].+0x26 != 0) list[ebp++] = edx
     *       if (++edx == [0x5D6510]) break            n - 1 (place.c)
     *   }
     *   numcheckposits = ebp - 1                      the LAST INDEX
     * With n == 2 the original runs past the route; the port stops at its
     * end. */
    rc->numcheckposits = 0;
    if (!route || !geom || js(route->n, 2)) return;
    e = 1;
    do {
        int32_t pol = route->pol[e];
        if (pol >= 0 && pol < geom->npols
            && geom->pols[(size_t)pol * 0x40u + 0x26] != 0) {
            if (found < SIM_RACE_CHECKPOINTS) rc->cp[found] = e;
            else rc->cp_overflow++;
            found++;
        }
        e++;
    } while (e != route->n - 1 && e < route->n);
    rc->numcheckposits = found - 1;
}

/* ---- the rule step: FUN_004351C9's dispatch ----------------------------- */

/* FUN_004354FD, the stage clock - the ONLY thing that advances catitken. */
static void stage_clock(car_state *car)
{
    if (car->tick_26c_q != -1 && car->tick_26c_q != 0) return;
    if (car->fs_count != 0) return;                /* serving a false start  */
    if (js(car->alivet_r, 0x320)) return;          /* SUB EAX,0x320 / JS     */
    if (car->rfn != 0) return;                     /* already finished       */
    car->catitken = wadd(car->catitken, 1);        /* 0x0043552D             */
}

/* FUN_004355D2, currpos on [0x005D6750]: 2 copies car 1's arcadepo
 * ([0x006C2764], the ARRAY record, not the working block); 1 and 3 call the
 * ghost models; 0 writes nothing. */
static void live_position(sim_race *rc, const car_state *car, uint32_t slot)
{
    if (rc->mode.gmode == 1) sim_ghost_position(rc, car, slot);
    else if (rc->mode.gmode == 3) rc->position_due++;
}

/* FUN_0043560F, the ninety ghosts' table: the defaults (total 12000 + i x
 * 1000, type i mod 23, name 1445 + i), then the records at [0x005D6770]
 * over them, all three fields, when there are any. */
static void ghost_table(sim_race *rc)
{
    int32_t i;
    for (i = 0; i < SIM_GHOSTS; i++) {
        rc->ghost_total[i] = i * 0x3E8 + 0x2EE0;
        rc->ghost_type[i] = (int32_t)((uint32_t)i % 0x17u);
        rc->ghost_name[i] = i + 0x5A5;
    }
    if (!rc->ghost_recs) return;                   /* CMP ESI,0 / JE         */
    for (i = 0; i < SIM_GHOSTS; i++) {
        rc->ghost_name[i] = rc->ghost_recs[i].name;    /* [ESI]              */
        rc->ghost_type[i] = rc->ghost_recs[i].type;    /* [ESI+8]            */
        rc->ghost_total[i] = rc->ghost_recs[i].total;  /* [ESI+0xC]          */
    }
}

/* FUN_00435688.  Writes the working block (+0xA0C..+0xA24), the car being
 * stepped: here the slot's. */
void sim_ghost_position(sim_race *rc, const car_state *car, uint32_t slot)
{
    uint32_t frac, den, ci;
    int32_t i, pos, t;

    slot &= 7u;
    ghost_table(rc);                               /* 0x00435688 CALL        */
    rc->currpos[slot] = -1;
    rc->gap_behind[slot] = rc->gap_ahead[slot] = -1;
    rc->type_behind[slot] = rc->type_ahead[slot] = -1;
    /* the four name dwords to spaces: no text here (the labels' draw is
     * not ported) */

    /* 0x004356E7: frac = cp[nextchck + 1] x 1000 / cp[numcheckposits] -
     * route-entry indices, MUL / DIV unsigned (EDX:EAX by EBX) */
    ci = (uint32_t)car->nextchck + 1u;
    den = ((uint32_t)rc->numcheckposits < SIM_RACE_CHECKPOINTS)
              ? (uint32_t)rc->cp[rc->numcheckposits] : 0u;
    if (den == 0u) { rc->ghost_div0++; return; }
    frac = (uint32_t)(((uint64_t)(ci < SIM_RACE_CHECKPOINTS ? (uint32_t)rc->cp[ci] : 0u)
                       * 1000u) / den);
    /* 0x00435712: scaled[i] = total[i] x frac / 1000, the same MUL / DIV */
    for (i = 0; i < SIM_GHOSTS; i++)
        rc->ghost_scaled[i] = (int32_t)(uint32_t)
            (((uint64_t)(uint32_t)rc->ghost_total[i] * frac) / 1000u);
    /* 0x0043572F..0x0043574B: i mod 10 tested, its JNE to the next
     * instruction - nothing. */
    /* 0x0043574D: the first i in 0..0x58 with scaled[i] - catitken >= 0
     * (CMP / JNS: the sign of the wrapped difference); none: 0x59.  Then
     * INC: currpos 1..90 - scaled[89] is never compared. */
    for (i = 0; i < 0x59; i++)
        if ((int32_t)((uint32_t)rc->ghost_scaled[i] - (uint32_t)car->catitken) >= 0)
            break;
    pos = i + 1;
    rc->currpos[slot] = pos;
    rc->ghost_catitken = car->catitken;
    if (pos != 1) {                                /* 0x0043577D: ahead      */
        t = (int32_t)((uint32_t)car->catitken - (uint32_t)rc->ghost_scaled[pos - 2]);
        rc->gap_ahead[slot] = (int32_t)((uint32_t)t * 10u);
        rc->type_ahead[slot] = rc->ghost_type[pos - 2];
        rc->name_ahead[slot] = rc->ghost_name[pos - 2];
    }
    if (pos != 0x5A) {                             /* 0x004357E5: behind     */
        t = (int32_t)((uint32_t)rc->ghost_scaled[pos - 1] - (uint32_t)car->catitken);
        rc->gap_behind[slot] = (int32_t)((uint32_t)t * 10u);
        rc->type_behind[slot] = rc->ghost_type[pos - 1];
        rc->name_behind[slot] = rc->ghost_name[pos - 1];
    }
}

/* FUN_00435339, modes 0 / 1 / 3.  NOT "FUN_0043526C with the award removed":
 * it records no split (FUN_00435576) and plays no sound (FUN_0045ACA9) -
 */
static void checkpoint_step(car_state *car, sim_race *rc, uint32_t slot)
{
    sim_race_msg *msg = &rc->msg[slot];
    size_t off;

    if (car->rfn != 0) return;
    if ((uint32_t)car->nextchck >= (uint32_t)car->currsegment) return;  /* JAE */
    live_position(rc, car, slot);
    /* MOV [EBX*4 + 0x006CCDA4],EAX: checkpt[nextchck] = currpos. */
    off = offsetof(car_state, checkpt) + (size_t)(uint32_t)car->nextchck * 4u;
    if ((uint32_t)car->nextchck < 0x10000000u && off + 4u <= sizeof(car_state))
        memcpy((uint8_t *)car + off, &rc->currpos[slot], 4);
    else
        rc->wild++;
    car->nextchck = car->currsegment;
    if (car->nextchck == rc->numcheckposits) {     /* the finish line        */
        car->rfn = 1;
        car->oor = 0;
        msg->topre = 0xC8;                         /* 200, the finish        */
        msg->timer = -1;
        msg->flash = 0;
    }
}

/* One arm of FUN_004353B1: 1 when the arm latches (and the function
 * returns).  FLD +0x1C4 / FCOMP 1.0 / FNSTSW / SHR AH,1 / JAE: C0 into CF,
 * so the count advances below 1.0 m/s AND on an unordered speed. */
static int end_latch_arm(int32_t *x, float speed)
{
    if (*x == 0) return 0;
    if (*x == 0x12C) return 1;
    if (speed >= 1.0f) return 0;                   /* still moving           */
    *x = wadd(*x, 1);
    return (uint32_t)*x >= 0x12Cu;                 /* JB                     */
}

/* FUN_004353B1: three seconds stopped after the race ended, then gof. */
static void end_latch(car_state *car)
{
    if (end_latch_arm(&car->oor, car->speed_len)) { car->gof = -1; return; }
    if (end_latch_arm(&car->oot, car->speed_len)) { car->gof = -1; return; }
    if (end_latch_arm(&car->rfn, car->speed_len)) car->gof = 2;
}

/* FUN_004354E0: a non-zero damage accumulator head retires the car. */
static void out_of_race(car_state *car)
{
    if (car->oor == 0 && car->damage_accum != 0) car->oor = 1;
}

/* ---- gmode 2, the arcade (sections 4.1, 7) -------- */

/* 0x0045ACA9 / 0x0045ACC5 / 0x0045ACE1: a chime flag, set only when the car
 * is a local player's ([0x005F4540 + car*4] != -1: slot < numpl, as
 * sim_sound_feed_local reads it). */
static void chime(sim_race *rc, uint32_t slot, int k)
{
    if ((int32_t)slot < rc->mode.numpl) rc->chime[k] = 1;
}

/* FUN_00435534, the countdown.  0x00435558: XOR EDX,EDX / DIV 100 / CMP
 * EAX,8 / JA; CMP EDX,0 / JNE - unsigned, so it also chimes at 0. */
static void arcade_countdown(car_state *car, sim_race *rc, uint32_t slot)
{
    uint32_t left;
    if (car->rfn != 0) return;
    if (js(car->alivet_r, 0x320)) return;          /* SUB EAX,0x320 / JS     */
    if (car->catileft == 0) return;
    car->catileft = wsub(car->catileft, 1);
    left = (uint32_t)car->catileft;
    if (left / 100u <= 8u && left % 100u == 0u) chime(rc, slot, 1);
}

/* FUN_00435576: the split - catitken at clamp(currsegment - 1, 0,
 * numcheckposits) (DEC / JNS; CMP / JS: signed). */
static void arcade_split(const car_state *car, sim_race *rc)
{
    int32_t i = wsub(car->currsegment, 1);
    if (i < 0) i = 0;
    if (!js(i, rc->numcheckposits)) i = rc->numcheckposits;
    if (i >= 0 && i < SIM_RACE_SPLITS) rc->split[i] = car->catitken;
    else rc->wild++;
}

/* FUN_0043526C, the arcade checkpoint.  Guarded to car index 0 ([0x006CE084]),
 * NOT on rfn (FUN_00435339's guard); it does not clear oor at the finish. */
static void arcade_checkpoint(car_state *car, sim_race *rc, uint32_t slot)
{
    sim_race_msg *msg = &rc->msg[slot];
    size_t off;

    if (slot != 0) return;
    if ((uint32_t)car->nextchck >= (uint32_t)car->currsegment) return;  /* JAE */
    arcade_split(car, rc);                         /* 0x0043528C             */
    chime(rc, slot, 0);                            /* 0x00435292 CHECKPT     */
    /* 0x00435297 FUN_004355D2, gmode 2: currpos = [0x006C2764], slot 0's
     * ARRAY record's arcadepo - the car itself in a one-car race */
    rc->currpos[slot] = car->arcadeposition;
    off = offsetof(car_state, checkpt) + (size_t)(uint32_t)car->nextchck * 4u;
    if ((uint32_t)car->nextchck < 0x10000000u && off + 4u <= sizeof(car_state))
        memcpy((uint8_t *)car + off, &rc->currpos[slot], 4);
    else
        rc->wild++;
    if ((uint32_t)car->currsegment < (uint32_t)rc->numcheckposits) {    /* JAE */
        msg->topre = 0xC9;                         /* 201                    */
        msg->flash = 0x32;
        msg->timer = 0x12C;
        /* 0x004352D9: catileft += [nextchck*4 + 0x5B5798] * 100 */
        if (rc->allowance
            && (uint32_t)car->nextchck + 1u < SIM_RACE_ALLOWANCE_N)
            car->catileft = wadd(car->catileft,
                (int32_t)((uint32_t)rc->allowance[car->nextchck + 1] * 100u));
        else
            rc->wild++;
    }
    car->oot = 0;                                  /* 0x004352EF             */
    car->nextchck = car->currsegment;
    if (car->nextchck == rc->numcheckposits) {     /* the finish line        */
        car->rfn = 1;
        msg->topre = 0xC8;                         /* 200                    */
        msg->timer = -1;
        msg->flash = 0;
    }
}

/* FUN_00435484, out of time - the only writer of oot. */
static void arcade_out_of_time(car_state *car, sim_race *rc, uint32_t slot)
{
    sim_race_msg *msg = &rc->msg[slot];
    if (car->driver != 1) return;                  /* a human's car          */
    if (car->catileft != 0) return;
    if (car->oot != 0) return;
    if (rc->dontrunoutoftime != 0) return;         /* [0x005B9228]           */
    if (rc->cheat_word & 0x10000u) return;         /* BT [0x70AA78],0x10     */
    chime(rc, slot, 2);                            /* 0x004354B2 CHECKOT     */
    car->oot = 1;
    msg->topre = 0xC7;                             /* 199                    */
    msg->flash = 0x32;
    msg->timer = -1;
}

void sim_race_rules_q(car_state *car, sim_race *rc, uint32_t slot)
{
    slot &= 7u;
    switch (rc->mode.gmode) {
    case 0: case 1: case 3:
        stage_clock(car);
        checkpoint_step(car, rc, slot);
        end_latch(car);
        out_of_race(car);
        break;
    case 2:                                        /* 0x0043520D             */
        if (!rc->allowance) rc->arcade_due++;      /* nothing to pay with    */
        stage_clock(car);
        arcade_countdown(car, rc, slot);
        arcade_checkpoint(car, rc, slot);
        arcade_out_of_time(car, rc, slot);
        end_latch(car);
        out_of_race(car);
        break;
    default:
        break;                                     /* 4: falls to the RET    */
    }
}

/* ---- the route tracker: FUN_0043F9E5 ------------------------------------ */

/* FUN_0043FAC8.  Read whole, 0x0043FAC8..0x0043FC55:
 *
 *   [0x005FE9D8] != 0          -> RET, CF from CMP [x],0 = 0: not crossed
 *   ecx = list[currsegment + 1]                      (0x005B5394 = list + 1)
 *   ebx = route[ecx]; ebx >= npols -> the guru "!!! invalid
 *                                      [car0.currsegment] ... [numcheckposits]"
 *   d = sqrt(dx*dx + dz*dz) * (1/65536), FSTP float  ([0x005C7A34])
 *   d >= 32.0 ([0x005C79A0]) -> CLC
 *   a = FPATAN(car.x - p.x, car.z - p.z) / pi * 0.5 * 65536, FISTP,
 *       AND 0xFFFF, SHL 16                           ([0x005C7A2C])
 *   b = angle[ecx + 1] ([0x005C7A30]) - 0x40000000
 *   CMP a,b / JS -> CLC, else STC
 *
 * So: crossed when the car is within 32 m of the marker and its bearing from
 * the marker is within a quarter turn of the heading of the segment leaving
 * it - the far side of the line.  THE PORT'S ARITHMETIC: the square root and
 * FPATAN in the host's long double (place.c's rule); both only matter at a
 * rounding boundary of a 32 m test or a quarter-turn test. */
int sim_checkpoint_crossed_q(const car_state *car, sim_race *rc)
{
    const sim_route *r = rc->route;
    const int32_t *P;
    int32_t s = car->currsegment, ci, a, b;
    int64_t dx, dz;
    uint64_t sq;
    float d;
    long double ang;

    if (rc->mode.no_route_q != 0) return 0;
    if (s < -1 || s + 1 >= SIM_RACE_CHECKPOINTS) { rc->wild++; return 0; }
    ci = rc->cp[s + 1];
    if (ci < 0 || ci >= r->n || r->pol[ci] < 0
        || r->pol[ci] >= rc->geom->npols) {
        rc->guru++;                                /* JMP 0x004D509A         */
        return 0;
    }
    P = route_point(rc, ci);
    if (!P) { rc->wild++; return 0; }
    dx = wsub(P[0], car->pos[0]);
    dz = wsub(P[2], car->pos[2]);
    sq = (uint64_t)(dx * dx) + (uint64_t)(dz * dz);
    d = (float)(sqrtl((long double)sq) * (1.0L / 65536.0L));
    if (d >= 32.0f) return 0;                      /* JAE -> CLC             */
    ang = atan2l((long double)wsub(car->pos[0], P[0]),
                 (long double)wsub(car->pos[2], P[2]));
    ang = ang / 3.14159265358979323846264338327950288L * 0.5L * 65536.0L;
    a = (int32_t)(((uint32_t)fistp_ld(ang) & 0xFFFFu) << 16);
    b = wsub(r->angle[ci + 1], 0x40000000);
    return !js(a, b);                              /* JS -> CLC; else STC    */
}

/* FUN_0043FA51: the car's x/z offset from the nearest marker, rotated by
 * MINUS the heading of the segment leaving it, into +0x620 / +0x624; zero on
 * the route's last marker (INC / CMP n / JNS). */
void sim_route_offset_q(car_state *car, sim_race *rc, const float *sin_table)
{
    const sim_route *r = rc->route;
    int32_t nb = car->nearestb, x, z;
    const int32_t *P;

    if (!js(wadd(nb, 1), r->n)) {
        car->bloffstx = 0;
        car->bloffstz = 0;
        return;
    }
    P = route_point(rc, nb);
    if (!P) { rc->wild++; return; }
    x = wsub(car->pos[0], P[0]);                   /* [0x6CCD98] - [ESI]     */
    z = wsub(car->pos[2], P[2]);                   /* [0x6CCDA0] - [ESI+8]   */
    sim_table_rotate_q(sin_table, (uint32_t)0u - (uint32_t)r->angle[nb + 1],
                       &x, &z);                    /* NEG EDX                */
    car->bloffstx = x;
    car->bloffstz = z;
}

/* |a - b| as the original takes it: SUB, CMP 0 / JNS, NEG - so the
 * difference that wraps to 0x80000000 stays there. */
static uint32_t wabs(int32_t a, int32_t b)
{
    uint32_t d = (uint32_t)a - (uint32_t)b;
    if ((int32_t)d < 0) d = 0u - d;
    return d;
}

/* FUN_0043F913: two step counters and their sound thresholds.
 *   off route: the nearest marker 32 m or more away ([0x005C799C]), or more
 *              than three markers from currentb      -> +0x270, +0x278
 *   wrong way: the car's heading (+0x9C0) a JB 0x60000000 or more from the
 *              heading of the segment leaving the nearest marker - 135
 *              degrees                                -> +0x274, +0x27C
 * Back on the route, a counter zeroes and its threshold returns to 300.
 * Unless [0x00574558] is set, reaching a threshold requests a sound
 * (FUN_0045AD14: 0x50 wrong way, 0x52 off route) and raises it by 3,000.
 * the tokens are the co-driver's - 0x50 *turn around*, 0x52 *wrong
 * way* - pushed to the FRONT of the speech queue (0x0040EB0A). */
static void route_counters(car_state *car, const sim_world *w, sim_race *rc)
{
    const sim_route *r = rc->route;
    int32_t nb = car->nearestb;
    int near = !(rc->nearest_dist >= 32.0f);       /* C0: less, or unordered */

    if (near && js((int32_t)wabs(nb, car->currentb), 4)) {
        car->offroute_q = 0;
        car->offroute_next_q = 300;
    } else {
        car->offroute_q = wadd(car->offroute_q, 1);
    }
    if (nb < 0 || nb >= r->n) {
        rc->wild++;
    } else if (wabs(w->binary_angle[0], r->angle[nb + 1]) < 0x60000000u) {
        car->wrongway_q = 0;
        car->wrongway_next_q = 300;
    } else {
        car->wrongway_q = wadd(car->wrongway_q, 1);
    }
    if (rc->quiet_q != 0) return;
    if (!js(car->wrongway_q, car->wrongway_next_q)) {
        car->wrongway_next_q = wadd(car->wrongway_next_q, 3000);
        rc->sounds++;                              /* FUN_0045AD14(0x50)     */
        sim_speech_push(rc, SIM_SPEECH_FRONT, w->race_slot, 0x50);
    }
    if (!js(car->offroute_q, car->offroute_next_q)) {
        car->offroute_next_q = wadd(car->offroute_next_q, 3000);
        rc->sounds++;                              /* FUN_0045AD14(0x52)     */
        sim_speech_push(rc, SIM_SPEECH_FRONT, w->race_slot, 0x52);
    }
}

/* FUN_0043F9E5, read whole:
 *
 *   nearestb = [0x5E94A8] = FUN_0043F821()
 *       the nearest of markers nearestb-1 and nearestb (FUN_0043F87C), its
 *       distance / 65536 into [0x5C7998]; over 10 m (0xA0000, JA) the whole
 *       route instead (FUN_0043F7D3); no route: 0
 *   FUN_0043FA51 ; FUN_0043F913
 *   if (ord[nearestb] - 1 == currsegment) currentb = nearestb
 *   if (currsegment >= numcheckposits) return            JAE: unsigned
 *   if (!FUN_0043FAC8()) return                          JAE: CF = 0
 *   currentb = nearestb = list[currsegment + 1] ; currsegment++ */
void sim_route_track_q(car_state *car, const sim_world *w, sim_race *rc,
                       const float *sin_table)
{
    const sim_route *r = rc->route;
    int32_t nb = 0;

    if (!r || !rc->geom) return;
    if (r->n != 0) {
        int32_t lo = wsub(car->nearestb, 1), at;
        uint32_t d;
        sim_route_nearest_q(r, rc->geom, car->pos[0], car->pos[2], lo,
                            wadd(lo, 2), &at, &d);
        rc->nearest_dist = (float)((double)(int32_t)d / 65536.0);
        if (d > 0xA0000u) {                        /* CMP EDI,0xA0000 / JA   */
            sim_route_nearest_q(r, rc->geom, car->pos[0], car->pos[2], 0, r->n,
                                &at, &d);
            rc->nearest_dist = (float)((double)(int32_t)d / 65536.0);
        }
        nb = at;
    }
    car->nearestb = nb;
    rc->reset_index = nb;
    if (r->n == 0) return;                         /* no route: the port stops */

    sim_route_offset_q(car, rc, sin_table);
    route_counters(car, w, rc);

    if (nb >= 0 && nb < r->n && wsub(r->ord[nb], 1) == car->currsegment)
        car->currentb = nb;
    if ((uint32_t)car->currsegment >= (uint32_t)rc->numcheckposits) return;
    if (!sim_checkpoint_crossed_q(car, rc)) return;
    car->currentb = rc->cp[car->currsegment + 1];
    car->nearestb = car->currentb;
    car->currsegment = wadd(car->currsegment, 1);
}

/* ---- the false start: FUN_00439EB6 -------------------------------------- */

/* Read whole, 0x00439EB6..0x00439F8F:
 *
 *   started == 0 && gear != 0                -> started = 1
 *   started && fs count == 0 && alivet r < 0x320 (CMP / JNS)
 *       -> fs count = 500 ; msgtopre 0xCC, msgflash 0, msgtimer -1
 *   fs count != 0 -> fs count-- ; on reaching 0:
 *       catitken += 1000 ; alivet r = 0 ; started = 0
 *       msgflash 0, msgtimer 300, +0x724 0, msgtopre 0xCA, +0x720 0, +0x728 0
 *       FUN_00444639 - THE CAR GOES BACK ON THE GRID
 *
 * had the penalty and the countdown restart, not
 * the `started` clear, the message or the re-placement (section 15). */
int sim_false_start_q(car_state *car, sim_world *w, sim_race *rc,
                      const sim_place_ctx *pc)
{
    uint32_t slot = w->race_slot & 7u;
    sim_race_msg *msg = &rc->msg[slot];

    if (car->started == 0 && car->gear != 0) car->started = 1;
    if (car->started != 0 && car->fs_count == 0 && js(car->alivet_r, 0x320)) {
        car->fs_count = 0x1F4;                     /* 5.00 s                 */
        msg->topre = 0xCC;                         /* 204, the false start   */
        msg->flash = 0;
        msg->timer = -1;
    }
    if (car->fs_count == 0) return 0;
    car->fs_count = wsub(car->fs_count, 1);
    if (car->fs_count != 0) return 0;
    car->catitken = wadd(car->catitken, 0x3E8);    /* +10.00 s               */
    car->alivet_r = 0;                             /* the countdown restarts */
    car->started = 0;
    msg->flash = 0;
    msg->timer = 300;
    msg->w724 = 0;
    msg->topre = 0xCA;                             /* 202, the race start    */
    msg->w720 = 0;
    msg->w728 = 0;
    return sim_place_car_q(car, slot, &rc->mode, rc->route, rc->geom, pc->p,
                           pc->k, pc->sin_table, pc->ground, pc->dmg, w, NULL);
}

/* ---- FUN_004436C1 -------------------------------------------------------- */

int sim_race_over_q(const car_state *const *cars, int32_t numpl)
{
    int32_t i;
    if (numpl == 0) return 0;                      /* CMP / JE -> CLC        */
    i = 0;
    do {
        if (!cars[i] || cars[i]->gof == 0) return 0;
        i++;
    } while (i != numpl && i < SIM_ROSTER_SLOTS);
    return 1;                                      /* STC                    */
}

/* ---- the HUD's step counters ------------------------------------ */

/* 0x00443011..0x0044308F, a loop over all eight slots:
 *   +0x730 = 0 ; +0x72C = 300 ; +0x724 = 0 ; +0x71C = 0xCA ; +0x720 = 0
 *   +0x728 = 0 ; +0xA0C = +0xA14 = +0xA10 = +0xA1C = +0xA18 = -1 */
void sim_message_system_init_q(sim_race *rc)
{
    int i;
    for (i = 0; i < SIM_ROSTER_SLOTS; i++) {
        sim_race_msg *m = &rc->msg[i];
        m->flash = 0;
        m->timer = 300;
        m->w724 = 0;
        m->topre = 0xCA;                           /* 202, the race start    */
        m->w720 = 0;
        m->w728 = 0;
        rc->currpos[i] = -1;
        /* 0x00443063..0x00443081: the gaps and types too */
        rc->gap_behind[i] = rc->gap_ahead[i] = -1;
        rc->type_behind[i] = rc->type_ahead[i] = -1;
    }
}

/* 0x0043F3BA:
 *   CMP [+0x728],0 / JE ret ; EAX = msgtimer ; CMP EAX,0 / JE ret
 *   DEC [msgtimer] / JNZ ret ; [+0x728] = 0 */
void sim_message_timer_q(sim_race_msg *m)
{
    if (m->w728 == 0 || m->timer == 0) return;
    m->timer = wsub(m->timer, 1);
    if (m->timer == 0) m->w728 = 0;
}

/* 0x0043F355, ESI = the car, EDI = the HUD block's olaydraw:
 *   CMP [ESI+0x1C],0 / JNZ ret              fs count
 *   CMP [ESI+0x25C],0 / JE ret              c1driver
 *   EAX = alivet r - [0x00684568] / JS ret  300, static
 *   clamp 0..0x5DC (CMP / JS) ; EDX:EAX / 100, unsigned DIV
 *   EAX >= 6 (JB) -> -1 ; [EDI] = EAX
 *   EDX == 0 and EAX != -1 -> [0x005C6E4C] = [0x0068456C + EAX*4] ; CALL
 *   0x0045ACFD, the sound
 * the sound is the co-driver's count, played at once (0x0040EABF),
 * `slot` the car FUN_0043F31E set in [0x006CE084]. */
void sim_countdown_q(const car_state *car, int32_t *olaydraw, sim_race *rc,
                     uint32_t slot)
{
    /* [0x0068456C]: speech\m\count\ 5 4 3 2 1 go (the male set's ids; the
     * player's +0/+1 and the voice are the audio side's) */
    static const int32_t count_id[6] = { 0xC9, 0xC7, 0xC5, 0xC3, 0xC1, 0xCB };
    int32_t u;
    uint32_t d, f;
    if (car->fs_count != 0 || car->driver == 0) return;
    u = wsub(car->alivet_r, 300);
    if (u < 0) return;
    if (js(u, 0x5DC) == 0) u = 0x5DC;
    d = (uint32_t)u / 100u;
    f = (uint32_t)u % 100u;
    *olaydraw = d < 6u ? (int32_t)d : -1;
    if (f == 0 && *olaydraw != -1) {
        rc->beeps++;
        sim_speech_push(rc, SIM_SPEECH_SAMPLE, slot, count_id[*olaydraw]);
    }
}
