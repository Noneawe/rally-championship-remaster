/* car.h - the per-car simulation record, byte-exact over the replayed prefix.
 *
 *
 * WHAT THIS IS
 * ------------
 * The original keeps eight cars as 4,064-byte (0xFE0) records reached through
 * a pointer table at 0x006CE044, and copies ONE of them, whole, into a fixed
 * working block at 0x006CCC64 around each car's turn in the 100 Hz tick
 * (0x0045E636 in, 0x0045E653 out -).  Nothing in the
 * physics addresses a car by index: it reads and writes that one block of
 * globals, which is why FUN_00439B37 takes no arguments and why 73 consecutive
 * calls with no register traffic between them makes sense.
 *
 * This struct IS that block.  Every offset below is cited to
 *'s field table, which merged three of the game's own
 * naming facilities; the SMALL-CAPS-in-comments names are the game's, not ours.
 *
 * WHY IT STOPS AT 0x684 AND NOT AT 0xFE0
 * --------------------------------------
 * The replay restores a CONTIGUOUS 1,668-byte PREFIX, car+0x0000..car+0x0683,
 * with one REP MOVSD per car (section 2).  That prefix is:
 *
 *  what diffs, `memcmp(orig.car[c], port.car[c], 1668)`;
 *  the part the replay format guarantees is authoritative, because a seek
 *     reconstructs it exactly and the rest is whatever the last step left.
 *
 * The 2,396 bytes above it are and are NOT struct members here.  Some are
 * known to be load-derived rather than simulation state -
 * section 3 traced car+0xCF0's writer to FUN_00425AAF, reached only from the
 * .ro3 model loader - and some (`wind 1..6`, `anim 00..16`) are named and
 * otherwise unread.  Mixing them in would put open questions inside the one
 * structure the harness treats as settled.  Two of them the physics genuinely
 * needs are passed in separately, as car_geometry below.
 *
 * THE OFFSET CONVENTION, AND THE TRAP IN IT
 * -----------------------------------------
 * Offsets here are from the RECORD BASE (car1 = 0x006C2110), which is how
 * and all cite
 * them.'s own table is relative to the `shauncarnum1`
 * FIELD, one level further in, and is therefore 0x24 SMALLER throughout.  The
 * conversion is already applied below; do not apply it twice.
 *
 * Working-block addresses (`car0+N`, which is what,
 * and quote) are this struct's offsets plus
 * 0x006CCC64.  car0 - car1 = 0xAB54.
 *
 * WHY EXPLICIT PADDING AND STATIC ASSERTS
 * ---------------------------------------
 * The record is mostly unnamed: 167 offsets are identified out of 1,016 dwords.
 * A struct written as "field, field, field" would encode our reading of the
 * GAPS as if it were evidence, and one wrong gap silently shifts every field
 * after it - which would report as a physics divergence at frame 1.  So
 * every named field is placed at its cited offset over an explicit `_padNNN`,
 * and car_layout_assert() checks every one of them at compile time.  A wrong
 * offset is a build failure here, not a mystery in the harness.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *  The 32-record handling table.  It is copyrighted image data and it is
 *     loaded at runtime - car_params.h, the own text.
 *  Anything above 0x684.  See above.
 *  Types for fields whose type nobody has read.  Those are `uint32_t raw`
 *     with the game's name in the comment, not a float we guessed at.
*/

#ifndef RALLY_SIM_CAR_H
#define RALLY_SIM_CAR_H

#include <stddef.h>
#include <stdint.h>

/* The record, the array and the working block -. */
#define CAR_RECORD_SIZE      0x0FE0u   /* 4,064 - the whole record          */
#define CAR_REPLAY_PREFIX    0x0684u   /* 1,668 - what a replay restores    */
#define CAR_RECORD_VA        0x006C2110u  /* car1, the array base           */
#define CAR_WORKING_VA       0x006CCC64u  /* car0, the live simulation car  */
#define CAR_ARRAY_SLOTS      8u

/* ------------------------------------------------------------------------ */

typedef struct car_state {
    /* --- 2.1 identity, setup and race state ----------------------------- */
    uint8_t  _pad000[0x008];
    int32_t  alivet;            /* 0x008  tick counter, ++ every step        */
    uint32_t rnd;               /* 0x00C  the car's own LFSR, stepped ONCE per
                                 *        sim_car_step at its first
                                 *        instruction (0x00439B37 calls
                                 *        0x0043F3E0): FUN_0043F400, rnd_next's
                                 *        step with EBX = 0x87654321.  Measured
                                 *        36,753 of 36,753 on the capture
                                 * Readers:
                                 *        0x0043F7C3 (a 1-in-65536-shaped
                                 *        chance test) and 0x004C33EF.       */
    uint32_t flags_010_q;       /* 0x010  bit 0x10 negates the steering pull
                                 *        (FUN_004383C3, 0x004385AB).
                                 *        Otherwise uninterpreted.  Set on
                                 *        run 20's car 0, clear on run 18. */
    int32_t  alivet_r;         /* 0x014  ALIVETR - reset by a false start.
                                 * step 13 and
                                 * both gate on
                                 *        it being >= 0x320 (800 ticks = 8 s). */
    int32_t  started;           /* 0x018  latches when the car is in gear    */
    int32_t  fs_count;          /* 0x01C  false-start hold, counts from 500  */
    int32_t  b_bal_1;           /* 0x020  BRAKE BALANCE, 0..256              */
    int32_t  carnum1;           /* 0x024  SHAUNCARNUM1 - indexes the handling
                                 *        table, masked to 5 bits.           */
    int32_t  set_g_1;           /* 0x028  the GEARING SLIDER, 0..4.  It
                                 *        interpolates the ladder; it is NOT a
                                 *        step counter (s4).    */
    int32_t  tyretype;          /* 0x02C  SHAUNTYRETYPE1 - the compound       */
    int32_t  set_h_1;           /* 0x030  0..256, expansion of `h` unknown    */
    int32_t  set_s_1;           /* 0x034  0..256                              */
    int32_t  autom_1;           /* 0x038  AUTOM 1 / MA-SE-FU - the gearbox
                                 *        strategy (s5).        */
    int32_t  ste_s_1;           /* 0x03C  steering ramp rate, 0..256          */
    int32_t  drive_class;       /* 0x040  0 = front-wheel drive, 1 = 4WD.
                                 *        Measured three ways - see
                                 */
    uint8_t  _pad044[0x01C];
    float    bump_q;            /* 0x060  min over the five contact points of
                                 *        surface record +0x28, x bumpf.  The
                                 *        surface solver's (s5a); what it DOES downstream is
                                 *        unread - one reader, 0x0045B0D8, in
                                 *        the block.                      */
    float    caroctdamage[8];   /* 0x064  eight octant damage values          */
    int32_t  latch_84_q;        /* 0x084  a one-bit latch: FUN_0043895C adds
                                 *        one and ANDs 1 on edge bit 3 of
                                 *        dannyebx, human slots only */
    int32_t  catitken;          /* 0x088  CATITKEN - the time taken, in
                                 *        centiseconds; the rescue adds its
                                 *        penalty here */
    int32_t  catileft;          /* 0x08C  CATILEFT - the arcade's time left,
                                 *        centiseconds: the load seeds it
                                 *        (FUN_00434FC3), gmode 2's countdown
                                 *        and checkpoints move it (race.c;
                                 */
    int32_t  currsegment;       /* 0x090  CUR SEG                             */
    int32_t  currentb;          /* 0x094  CUR BLP                             */
    int32_t  nearestb;          /* 0x098  NEAR BLP                            */
    uint32_t l_p_plyd;          /* 0x09C  named by the overlay, uninterpreted */
    int32_t  oor;               /* 0x0A0  OUTOFRAC - a counter to 300, NOT a
                                 *        boolean (standing-rules house rule) */
    int32_t  oot;               /* 0x0A4  OUTOFTIM                            */
    int32_t  rfn;               /* 0x0A8  RACE FIN - blocks all shifting      */
    int32_t  nextchck;          /* 0x0AC  section watermark already credited  */
    int32_t  gof;               /* 0x0B0  game-over flag, -1 / 2              */

    /* The orientation matrix.  Integrated by FUN_0043C22F and RE-ORTHONORMAL-
     * ISED EVERY STEP -.  A port that omits the
     * re-orthonormalisation diverges for a reason that has nothing to do with
     * arithmetic.  0x0B8 and 0x0D8 are the two elements sim_tyre_forces_q
     * reads for its attitude scalars (tilt x / tilt z). */
    float    matrix[16];        /* 0x0B4 .. 0x0F0                             */

    uint8_t  _pad0f4[0x040];
    int32_t  pos[3];            /* 0x134 x, 0x138 y, 0x13C z - int32 16.16
                                 * FIXED POINT, not float. s3. */
    int32_t  checkpt[16];       /* 0x140 .. 0x17C  CHECKPT0..CHECKPTF, the
                                 * per-section snapshot of currpos.  These are
                                 * INSIDE the replayed prefix -
                                 * filed them in "the tail" until.       */
    float    zspddif;           /* 0x180  longitudinal slip (written in TAIL)  */
    float    rotspd[3];         /* 0x184/188/18C angular velocity, VECY.
                                 * Clamped to +/- 4.0 by the divergence guard. */
    uint8_t  _pad190[0x004];
    float    rotspd_len;        /* 0x194  |rotspd| as FUN_0043C22F stored it
                                 * (0x0043C23A; its only writer, three refs) -
                                 * the angular speed BEFORE the collision
                                 * loop, which re-rounds rotspd */
    float    newspd[3];         /* 0x198/19C/1A0 NEWXSPD/NEWYSPD/NEWZSPD -
                                 * linear velocity.  Clamped to +/- 80.0.      */
    float    cxspd;             /* 0x1A4  CXSPD - lateral speed, car frame     */
    float    cyspd_q;           /* 0x1A8  the triple's middle component - the
                                 * same writer as cxspd/czspd (FUN_0045D636,
                                 * but no reader has been found yet, so
                                 * the name is a position in the triple, not a
                                 * confirmed meaning.                          */
    float    czspd;             /* 0x1AC  CZSPD - forward speed, car frame     */
    uint8_t  _pad1b0[0x008];
    float    fwhlspd;           /* 0x1B8  FWHLSPD - front wheel speed          */
    float    rwhlspd;           /* 0x1BC  RWHLSPD - rear wheel speed           */
    float    wheelspd_mean;     /* 0x1C0  (fwhlspd + rwhlspd) x 0.5, stored at
                                 * 0x00439E25 AFTER FUN_0043C14D's feed has
                                 * read the previous step's: the engine
                                 * whine's input.    */
    float    speed_len;         /* 0x1C4  |newspd|, FUN_0043A22F at step start
                                 * and end (s11.2; 36,754
                                 * of 36,754 captured frames).  Was named
                                 * `end_latch` after a READER: the end-latch
                                 * compares it against 1.0 (s1,
                                 * s5), 0x00438704 against 5.0, C535 against
                                 * 8.0.                                        */
    float    yaw_powr_in;       /* 0x1C8  scales ROTSPDPOWR - yaw model term 3 */
    int32_t  shift_cooldown;    /* 0x1CC  20 steps (200 ms) after a shift      */
    int32_t  shift_torque_cut;  /* 0x1D0  5 steps (50 ms) of ZERO drive torque
                                 * on every gear change. s2.    */
    int32_t  gear;              /* 0x1D4  the gear.  Indexes ratio[-1..7].     */
    float    revs;              /* 0x1D8                                        */
    float    maxrevs;           /* 0x1DC  from the handling record +0x74,
                                 * floored at 1000 on load.             */

    uint8_t  _pad1e0[0x004];
    int32_t  sound_revs;        /* 0x1E4  FUN_0043C14D: the revs, floored at
                                 * the class's idle, FISTP'd, eased 50/256 a
                                 * step and clamped 2000..12000 - the engine
                                 * sound's input, not the tacho.                        */
    int32_t  freerev_clear_1e8_q; /* 0x1E8  a step counter: FUN_0043D66C
                                 * zeroes it whenever freerev (+0x250) is 0
                                 * (0x0043D711); FUN_0043D47E, at the step's
                                 * end, zeroes it for driver 0 or 2, else adds
                                 * one and calls FUN_0043D4B3 at 700.  Readers
                                 * 0x00438757, 0x0043E088 not followed.       */
    uint8_t  _pad1ec[0x004];
    int32_t  rev_limit_cut;     /* 0x1F0  FUN_0043B058, the rev limiter: set
                                 * to 5 at revs >= maxrevs, counted down, the
                                 * torque curve zeroed while either holds.
                                 * Its only absolute references are those
                                 * three.                        */
    int32_t  collision_response_timer; /* 0x1F4  B28F/B30E set 0x32        */
    int32_t  wheel_spin_f;      /* 0x1F8  the front wheels' spin angle, a 16-bit
                                 * binary angle that is never wrapped (FUN_0043B1F5,
                                 * 0x0043B1F5; 6,030 of 6,030 steps of run 41)      */
    int32_t  wheel_spin_r;      /* 0x1FC  the rear pair's                        */
    float    yrotspd;           /* 0x200  YROTSPD - actual yaw rate            */
    float    wantyrotspd;       /* 0x204  WANTYROTSPD - demanded yaw rate      */
    int32_t  steeringpul;       /* 0x208  round(0x20000000 * steerpull), the
                                 * steering pull in the steer's own fixed
                                 * point: FUN_004383C3 FISTPs it (0x004385A5).
                                 * Typed float until - nothing read it. */
    uint8_t  _pad20c[0x004];
    float    yaw_steer_term;    /* 0x210  the bicycle-model steering term,
                                 * capped by the friction circle.               */
    float    rotspdpowr;        /* 0x214  yaw term 3                           */
    float    rotspdspin;        /* 0x218  yaw term 4, four-wheel drive only    */
    float    rotspdhbrk;        /* 0x21C  yaw term 2, two-wheel drive only     */
    uint32_t hud_mask;          /* 0x220  the HUD's element mask, from the
                                 * menus (car_load.c; 0xF7 by default); bit 4
                                 * lets a damage warning into the queue
                                 * (FUN_004C90CE)                       */
    float    collision_scratch_224[3]; /* 0x224: B30E model point or CXSPD */
    float    collision_peak_normal_speed; /* 0x230 reset by step; max CB48  */
    int32_t  collision_active;      /* 0x234  reset by step; B28F/B30E    */
    int32_t  shift_result;      /* 0x238  ~~upshift_loss~~: the
                                 * gearbox's outcome this step - 1 after ANY
                                 * completed change (up, down, neutral,
                                 * reverse, from neutral, the stall), -1 after
                                 * a refused downshift, 0 otherwise.  == 1
                                 * multiplies revs by 0.95 in the gearbox
                                 * tail, so a downshift loses it too.
                                 */
    int32_t  gear_changed_q;    /* 0x23C  set to 1 with every write of 1 or -1
                                 * above; NOTHING in the gearbox clears it, and
                                 * its clearer is unfound (s6).     */
    /* 0x0044CD28 (from the step at 0x00439E92), the car's GROUND LIGHT:
     * the draw's base colour [0x005E0964] (0x00453019) and the shade index
     * it is looked up from.  sim_car_ground_light_q, ground.h.          */
    uint32_t ground_col;        /* 0x240  0xAARRGGBB                       */
    int32_t  ground_shade;      /* 0x244  index into [0x005E7FDC]          */
    int32_t  front_airborne;    /* 0x248  FUN_0043D66C: (susp[0] | susp[1]) ==
                                 * 0, bitwise.  FUN_0043A471 reads it: an
                                 * airborne front wheel in NEUTRAL spins down
                                 * (the port had never written it). */
    int32_t  drag_gate_q;       /* 0x24C  D9CC skips record 4 when nonzero.
                                 * FUN_0043D66C writes it: the REAR axle is
                                 * airborne (susp 3|4 bits == 0).  Name kept
                                 * so the port's users do not move; see
                                 */
    int32_t  freerev;           /* 0x250  != 0 SUBSTITUTES the free-revving
                                 * engine for the whole tyre force model.
                                 * Writer FUN_0043D66C: every DRIVEN wheel is
                                 * airborne (front pair for class 0, all four
                                 * for class 1)..     */
    int32_t  yaw_couple_gate;   /* 0x254  == 0 is what lets the yaw model emit
                                 * its couple into records 19/20.  Writer
                                 * FUN_0043D66C: ALL FOUR wheels airborne.
                                 * Also gates body contact.*/
    int32_t  road_wheels_q;     /* 0x258  how many of the four wheels stand on
                                 *        class 1, 2, 3 or 12 - the surface
                                 *        solver's tail.  0..4 in the
                                 *        capture, all five values seen.      */
    int32_t  driver;            /* 0x25C  THE SIMULATION GATE.  == 2 is the
                                 * arcade-rival shift path. s1.  */
    int32_t  numcollpts;        /* 0x260  how many collision points FUN_0045AE5D
                                 * walks.  Its one writer is FUN_00425AAF at
                                 * model load (`numcollpts` in its own log
                                 * line): the model's points + 2.     */
    uint32_t rescue_hold_q;     /* 0x264  ticks the rescue key has been held
                                 * while slow: FUN_004386E5 calls the car
                                 * reset at exactly 100 */
    uint32_t tctime;            /* 0x268  a counter: FUN_0043B990 adds one per
                                 * step in gmode 1, in neutral, from alivet_r
                                 * 1000 on, while OOR/OOT/RFN are clear and
                                 * CPLEFT & 0x1E != 0x1E; else zeroes it.     */
    int32_t  tick_26c_q;        /* 0x26C  FUN_0043B990 adds one unless it is
                                 * 0, -1 or 1                    */
    /* FUN_0043F913, in the route tracker (race.c).  Names are the
     * reading of their one writer: run 18 never moves them off 0 / 300. */
    int32_t  offroute_q;        /* 0x270  steps > 32 m from the nearest marker
                                 *        or > 3 markers from currentb        */
    int32_t  wrongway_q;        /* 0x274  steps heading >= 135 deg from the
                                 *        route; FUN_00459101's message 206
                                 *        reads it against 300               */
    int32_t  offroute_next_q;   /* 0x278  300; the sound threshold, +3000   */
    int32_t  wrongway_next_q;   /* 0x27C  300; the sound threshold, +3000   */
    float    wetns;            /* 0x280  WETNS - 0..1, the surface curve's
                                 * second axis.  Resolved weather_q.      */
    int32_t  grip_half;         /* 0x284  != 0 halves both longitudinal grips;
                                 * == -1 also scales ROTSPDPOWR.  Its one
                                 * writer, FUN_0043F4F8 (parts_left.c):
                                 * 1 = a part of wheel 1 or 2 gone, -1 = of
                                 * wheel 3 or 4, 0 = all eight present.    */
    float    skidamt;           /* 0x288  written in the tyre model's TAIL      */
    /* 0x0045A626's (the sound feed) and FUN_0043C14D's, every step;
     * all three 0 while all four wheels are airborne (car+0x254).          */
    float    skid_snd;          /* 0x28C  skidamt / [0x005C6C20] (1.0)          */
    float    roll_snd;          /* 0x290  1.0: the rolling sound's gate         */
    float    speed_snd;         /* 0x294  |newspd| x 2.25 (FUN_0043C14D)        */
    int32_t  carpartsleft;      /* 0x298  CPLEFT - bits 1..4 the tyres, 5..8
                                 * the wheels (parts_left.c); 9, 12..17
                                 * the bodywork (car_mask1, rally.c)          */
    uint32_t damage_accum;      /* 0x29C  head of the damage accumulator;
                                 * nonzero retires the car. s1.  */
    float    temps[6];          /* 0x2A0..0x2B4  BWET TTMPF TTMPR BTMPF BTMPR
                                 * CTMP - written only by sim_damage_wear_q
                                 * (unported); [0] wetness of the brakes, [1..2]
                                 * tyre and [3..4] brake temperatures, read by
                                 * FUN_004C66EA (damage.c).        */

    /* --- 2.4 the damage block: 44 consecutive slots, 0x2B8..0x364 --------
     * The single longest unbroken run in the record, fully named on both the
     * log-macro and overlay sides with zero disagreement.  Indices follow
     *'s listing exactly; dam[7] is `dam_gear0 _q`, a one-slot
     * hole nobody has read at the address itself. */
    float    dam[44];           /* 0x2B8 .. 0x364                              */

    /* --- the 35 damage-warning states, 0x368..0x3F0 -------------
     * FUN_004C773F's hysteresis machines, one per temperature or damage
     * field; nothing else reads them (s6). */
    int32_t  warn_state[35];    /* 0x368 .. 0x3F0                              */

    /* --- 2.5 the damage EFFICIENCY slots, 0x3F4.. ------------------------
     * FUN_004C66EA's output: efficiency = 1 - sum(damage% * 0.01 * weight).
     * Four of the six feed the tyre force model directly. */
    float    engpower;          /* 0x3F4  multiplies the torque curve          */
    float    brakeefff;         /* 0x3F8  front brake efficiency               */
    float    brakeeffr;         /* 0x3FC  rear brake efficiency                */
    float    tyregripf;         /* 0x400  front longitudinal grip efficiency   */
    float    tyregripr;         /* 0x404  rear longitudinal grip efficiency    */
    float    steerpull;         /* 0x408                                        */
    uint32_t elecoff;           /* 0x40C                                        */
    uint32_t lightoff[6];       /* 0x410..0x424 light1off..light4off,
                                 * lightfoff, lightboff                        */
    int32_t  handbrake;         /* 0x428  0..256.  NOTE: this shares the offset
                                 * lists for `handbrake`; the
                                 * lightoff run above is 0x410..0x424.          */
    int32_t  stikgear;          /* 0x42C                                        */
    int32_t  dropgear;          /* 0x430                                        */
    uint32_t gearbits;          /* 0x434  GEARBITS - the gear-failure mask.
                                 * sim_gear_is_broken is BT gearbits, gear+1,
                                 * so bit g+1 disables gear g.  This is the
                                 * consumer the damage model was missing.       */
    float    actsh[4];          /* 0x438..0x444                                 */
    float    actss[4];          /* 0x448..0x454                                 */
    float    actss_sum;         /* 0x458                                        */
    float    actsh_sum;         /* 0x45C                                        */
    int32_t  tick_460_q;        /* 0x460  FUN_0043B990 counts it down to 0     */

    /* The per-tick digital control input package. s8,
     *'s subsection.  The overlay's `dannyE**`
     * labels are register dumps that name nothing - the FIELDS are real. */
    uint32_t dannyeax;          /* 0x464                                        */
    uint32_t dannyebx;          /* 0x468  flags.  bit 0x1000 = digital steering
                                 * active; (v & 0xF00) != 0xE00 diverts the
                                 * gearbox before the mode switch.              */
    uint32_t dannyecx;          /* 0x46C  the one signed, 10-bit-wide field     */
    uint32_t dannyedx;          /* 0x470                                        */
    uint32_t dannyebx_edge;     /* 0x474  the newly pressed bits, ~old & new
                                 * dannyebx - FUN_0043895C.  Bit 0 is the
                                 * gear-up flag, bit 1 gear-down (s4).  36,753 / 36,753 on the capture.      */

    /* --- the warnings' queue, 0x478..0x5FB (FUN_004C90CE) --------
     * 32 slots in three parallel arrays; the pop (FUN_004C9154) shifts all
     * 31 down. */
    uint32_t warn_count;        /* 0x478                                        */
    int32_t  warn_id[32];       /* 0x47C  message ids 0xCF..0x10B               */
    uint32_t warn_val[32];      /* 0x4FC  the damage value's BITS - the key      */
    uint32_t warn_level[32];    /* 0x57C  1..4                                  */
    uint8_t  _pad5fc[0x004];

    /* --- 2.6 the high offsets that are STILL INSIDE the prefix ------------
     * The replay cut is 0x684.  Everything from here to 0x654 is restored on a
     * seek; said otherwise until. */
    float    susp[4];           /* 0x600..0x60C suspension travel, per wheel    */
    float    susp_prev[4];      /* 0x610..0x61C the travel before this step's:
                                 * FUN_0043D198's FSTP at 0x0043D1AA, read by
                                 * the damage feed's landing test at
                                 * 0x004C3304 (its only reader)             */
    /* ~~float, uninterpreted~~: INTEGERS, 16.16.  Their one writer,
     * FUN_0043FA51 in the route tracker, stores FUN_0041EA8B's two FISTP
     * results: the car's x/z offset from the nearest route marker, rotated
     * into the frame of the segment leaving it (z along, x across). */
    int32_t  bloffstx;          /* 0x620  BLOFFSTX - across the route       */
    int32_t  bloffstz;          /* 0x624  BLOFFSTZ - along the route        */
    /* the arcade streamer's gaps, slot 0's record only (FUN_00435A90,
     * 0x00435D6B..0x00435F32).  Route markers + 1
     * to the nearest rival ahead / behind, unsigned minima from 0xFFFFFFFF,
     * then each x 330 smoothed 1/64 a tick for the HUD (FUN_00458A66). */
    uint32_t arc_gap_ahead;     /* 0x628  [0x006C2738]                          */
    uint32_t arc_gap_behind;    /* 0x62C  [0x006C273C]                          */
    int32_t  arc_gap_ahead_ms;  /* 0x630  [0x006C2740]                          */
    int32_t  arc_gap_behind_ms; /* 0x634  [0x006C2744]                          */
    int32_t  throttle;          /* 0x638  0..256 full scale.  Confirmed twice:
                                 * the +q/-q bracket cancels exactly at 256
                                 * (s2) and the overlay's torque
                                 * plot sets exactly 0x100.                      */
    int32_t  brake;             /* 0x63C  0..256; != 0 also selects brake grip
                                 * over accel grip in the tyre model.            */
    int32_t  handbrake_ramp;    /* 0x640  THE handbrake the physics reads: the
                                 * tyre model's step 6 (0x0043C8F5, 0x0043C990)
                                 * and the yaw model's terms 2 and 3
                                 * (0x0043B541/5A4/5FC).  FUN_00438616 ramps it,
                                 * pre-step.  500 on the capture's 800
                                 * pre-start frames and its last 464, 0
                                 * elsewhere.  The port read +0x428 instead,
                                 * which is 0 on all 36,754.      */
    /* The two steer accumulators are INTEGERS, +-0x20000000 full lock: the
     * ramp SUBs and CMPs them as dwords and the yaw model FIMULs/FISTPs
     * +0x644 (0x0043B47A).  Typed float until. */
    int32_t  stang;             /* 0x644  STEERANGLE                            */
    int32_t  stangv;            /* 0x648  STEERANGLEV                           */
    int32_t  usteerangle;       /* 0x64C  USTEERANGLE.  The yaw model indexes
                                 * the steer curve with `usteerangle >> 20`.     */
    int32_t  usteerangle_v_q;   /* 0x650  stangv + 4 * steeringpul, zeroed
                                 * with usteerangle at full steering damage
                                 * (FUN_004383C3).  Its one other reader is
                                 * 0x0045ECC5, outside the step.            */
    int32_t  arcadeposition;    /* 0x654  ARCADEPO.  The highest NAMED offset
                                 * below the replay cut - corrected this
                                 * from 0x64C in four documents.                 */
    /* the rest of the replay prefix.  0x658..0x670 are the arcade
     * streamer's state, in SLOT 0's record (the streamer addresses
     * [0x006C2768..0x006C2780] absolutely); 0x674..0x680 are every rival's
     * own, the rubber band's (FUN_0043618F) -. */
    int32_t  arc_behind;        /* 0x658  numcarsbehind P                       */
    int32_t  arc_w0;            /* 0x65C  the rival ahead, a slot               */
    int32_t  arc_w1;            /* 0x660  the rival beside or behind            */
    int32_t  arc_wi0;           /* 0x664  W0's start-list index                 */
    int32_t  arc_wi1;           /* 0x668  W1's                                  */
    int32_t  arc_started;       /* 0x66C  the streamer's first call is done     */
    int32_t  arc_spacing_d;     /* 0x670  W0.nb - W1.nb, the push's scratch     */
    float    arc_push;          /* 0x674  the spacing push, +-0.05 a marker     */
    float    arc_skill;         /* 0x678  the rival's skill, 0.60..1.00         */
    float    arc_band;          /* 0x67C  -0.02 a marker of lead                */
    float    arc_factor;        /* 0x680  skill + band + push, clamped 0..1     */
} car_state;                    /* 0x684 total                                  */

/* ------------------------------------------------------------------------ */
/* Load-time geometry, kept OUT of car_state on purpose.
 *
 * car+0xCF0..0xCFC (`fl/fr/bl/br z pos`) sit in the unreplayed tail, and
 * traced their only writer to FUN_00425AAF - three
 * parallel x/y/z arrays at +0xAF0/+0xBF0/+0xCF0, one triple per wheel, each an
 * int 16.16 -> float /65536.0 conversion from a 12-byte-stride source table,
 * reached ONLY from the .ro3 model loader.  They are the car model's collision
 * points, baked once when the model loads; the replay does not store them
 * because reloading the same .ro3 reproduces them exactly.
 *
 * The yaw model needs two of them as lever arms for its couple (records 19 and
 * 20), so they are an INPUT to the step rather than state inside it.  That
 * distinction is the whole of what is about for this field, and it is the
 * one part of that is answered. */
typedef struct car_geometry {
    float wheel_z[4];           /* car+0xCF0, 0xCF4, 0xCF8, 0xCFC - fl fr bl br */
} car_geometry;

/* ------------------------------------------------------------------------ */
/* Compile-time layout check.
 *
 * The record is mostly unnamed - 167 offsets identified out of 1,016 dwords -
 * so a struct written as "field, field, field" would encode our reading of the
 * GAPS as if it were evidence, and one wrong gap silently shifts every field
 * after it. would report that as a physics divergence at frame 1, in a
 * place that has nothing to do with physics.  Every offset below is therefore
 * checked HERE, where a mistake is a build failure.
 *
 * File-scope typedefs, not a function: C99 has no _Static_assert, and a static
 * function nobody calls trips -Wunused-function, which this tree treats as
 * something to fix rather than to silence. */

#define CAR_ASSERT_OFF(field, want)     typedef char car_off_##field[(offsetof(car_state, field) == (want)) ? 1 : -1]

CAR_ASSERT_OFF(alivet,          0x008);
CAR_ASSERT_OFF(rnd,             0x00C);
CAR_ASSERT_OFF(alivet_r,        0x014);
CAR_ASSERT_OFF(b_bal_1,         0x020);
CAR_ASSERT_OFF(carnum1,         0x024);
CAR_ASSERT_OFF(set_g_1,         0x028);
CAR_ASSERT_OFF(tyretype,        0x02C);
CAR_ASSERT_OFF(autom_1,         0x038);
CAR_ASSERT_OFF(drive_class,     0x040);
CAR_ASSERT_OFF(bump_q,          0x060);
CAR_ASSERT_OFF(caroctdamage,    0x064);
CAR_ASSERT_OFF(currsegment,     0x090);
CAR_ASSERT_OFF(oor,             0x0A0);
CAR_ASSERT_OFF(rfn,             0x0A8);
CAR_ASSERT_OFF(gof,             0x0B0);
CAR_ASSERT_OFF(matrix,          0x0B4);
CAR_ASSERT_OFF(drag_gate_q,      0x24C);
CAR_ASSERT_OFF(pos,             0x134);
CAR_ASSERT_OFF(checkpt,         0x140);
CAR_ASSERT_OFF(zspddif,         0x180);
CAR_ASSERT_OFF(rotspd,          0x184);
CAR_ASSERT_OFF(rotspd_len,      0x194);
CAR_ASSERT_OFF(newspd,          0x198);
CAR_ASSERT_OFF(hud_mask,        0x220);
CAR_ASSERT_OFF(warn_state,      0x368);
CAR_ASSERT_OFF(warn_count,      0x478);
CAR_ASSERT_OFF(warn_id,         0x47C);
CAR_ASSERT_OFF(warn_val,        0x4FC);
CAR_ASSERT_OFF(warn_level,      0x57C);
CAR_ASSERT_OFF(collision_scratch_224, 0x224);
CAR_ASSERT_OFF(collision_peak_normal_speed, 0x230);
CAR_ASSERT_OFF(collision_active, 0x234);
CAR_ASSERT_OFF(cxspd,           0x1A4);
CAR_ASSERT_OFF(cyspd_q,         0x1A8);
CAR_ASSERT_OFF(czspd,           0x1AC);
CAR_ASSERT_OFF(fwhlspd,         0x1B8);
CAR_ASSERT_OFF(rwhlspd,         0x1BC);
CAR_ASSERT_OFF(speed_len,       0x1C4);
CAR_ASSERT_OFF(yaw_powr_in,     0x1C8);
CAR_ASSERT_OFF(shift_cooldown,  0x1CC);
CAR_ASSERT_OFF(shift_torque_cut, 0x1D0);
CAR_ASSERT_OFF(gear,            0x1D4);
CAR_ASSERT_OFF(revs,            0x1D8);
CAR_ASSERT_OFF(maxrevs,         0x1DC);
CAR_ASSERT_OFF(yrotspd,         0x200);
CAR_ASSERT_OFF(wantyrotspd,     0x204);
CAR_ASSERT_OFF(yaw_steer_term,  0x210);
CAR_ASSERT_OFF(rotspdpowr,      0x214);
CAR_ASSERT_OFF(rotspdspin,      0x218);
CAR_ASSERT_OFF(rotspdhbrk,      0x21C);
CAR_ASSERT_OFF(shift_result,    0x238);
CAR_ASSERT_OFF(gear_changed_q,  0x23C);
CAR_ASSERT_OFF(ground_col,      0x240);
CAR_ASSERT_OFF(ground_shade,    0x244);
CAR_ASSERT_OFF(freerev,         0x250);
CAR_ASSERT_OFF(yaw_couple_gate, 0x254);
CAR_ASSERT_OFF(collision_response_timer, 0x1F4);
CAR_ASSERT_OFF(freerev_clear_1e8_q, 0x1E8);
CAR_ASSERT_OFF(rev_limit_cut,   0x1F0);
CAR_ASSERT_OFF(wheel_spin_f,    0x1F8);
CAR_ASSERT_OFF(wheel_spin_r,    0x1FC);
CAR_ASSERT_OFF(front_airborne,  0x248);
CAR_ASSERT_OFF(road_wheels_q,   0x258);
CAR_ASSERT_OFF(driver,          0x25C);
CAR_ASSERT_OFF(numcollpts,      0x260);
CAR_ASSERT_OFF(wetns,           0x280);
CAR_ASSERT_OFF(grip_half,       0x284);
CAR_ASSERT_OFF(skidamt,         0x288);
CAR_ASSERT_OFF(skid_snd,        0x28C);
CAR_ASSERT_OFF(roll_snd,        0x290);
CAR_ASSERT_OFF(speed_snd,       0x294);
CAR_ASSERT_OFF(wheelspd_mean,   0x1C0);
CAR_ASSERT_OFF(sound_revs,      0x1E4);
CAR_ASSERT_OFF(tick_26c_q,      0x26C);
CAR_ASSERT_OFF(tick_460_q,      0x460);
CAR_ASSERT_OFF(carpartsleft,    0x298);
CAR_ASSERT_OFF(temps,           0x2A0);
CAR_ASSERT_OFF(dam,             0x2B8);
CAR_ASSERT_OFF(engpower,        0x3F4);
CAR_ASSERT_OFF(brakeefff,       0x3F8);
CAR_ASSERT_OFF(brakeeffr,       0x3FC);
CAR_ASSERT_OFF(tyregripf,       0x400);
CAR_ASSERT_OFF(tyregripr,       0x404);
CAR_ASSERT_OFF(handbrake,       0x428);
CAR_ASSERT_OFF(gearbits,        0x434);
CAR_ASSERT_OFF(dannyeax,        0x464);
CAR_ASSERT_OFF(dannyebx,        0x468);
CAR_ASSERT_OFF(dannyecx,        0x46C);
CAR_ASSERT_OFF(dannyedx,        0x470);
CAR_ASSERT_OFF(dannyebx_edge,   0x474);
CAR_ASSERT_OFF(susp,            0x600);
CAR_ASSERT_OFF(susp_prev,       0x610);
CAR_ASSERT_OFF(throttle,        0x638);
CAR_ASSERT_OFF(brake,           0x63C);
CAR_ASSERT_OFF(handbrake_ramp,  0x640);
CAR_ASSERT_OFF(stang,           0x644);
CAR_ASSERT_OFF(stangv,          0x648);
CAR_ASSERT_OFF(usteerangle_v_q, 0x650);
CAR_ASSERT_OFF(flags_010_q,     0x010);
CAR_ASSERT_OFF(latch_84_q,      0x084);
CAR_ASSERT_OFF(catitken,        0x088);
CAR_ASSERT_OFF(catileft,        0x08C);
CAR_ASSERT_OFF(steeringpul,     0x208);
CAR_ASSERT_OFF(rescue_hold_q,   0x264);
CAR_ASSERT_OFF(usteerangle,     0x64C);
CAR_ASSERT_OFF(arcadeposition,  0x654);
CAR_ASSERT_OFF(arc_gap_ahead,   0x628);
CAR_ASSERT_OFF(arc_gap_behind_ms, 0x634);
CAR_ASSERT_OFF(arc_behind,      0x658);
CAR_ASSERT_OFF(arc_spacing_d,   0x670);
CAR_ASSERT_OFF(arc_push,        0x674);
CAR_ASSERT_OFF(arc_factor,      0x680);

/* And the size itself: exactly the prefix memcmps, no more. */
typedef char car_size_is_replay_prefix[
    (sizeof(car_state) == CAR_REPLAY_PREFIX) ? 1 : -1];

#endif /* RALLY_SIM_CAR_H */
