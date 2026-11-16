/* parts_left.c - the parts-left evaluator: FUN_0043F4F8, called at
 * 0x00439E59, between the accumulators (damage_acc.c) and the warnings
 * (damage_warn.c) in every car's step (s4).
 *
 * WHAT IT DOES.  carpartsleft (car+0x298) bits 1..4 are the four TYRES and
 * bits 5..8 the four WHEELS (the readers say so: FUN_0043F691 tests bit n
 * against dam_tyre n, FUN_0043F57A bit n+4 against dam_whl n, and the ground
 * solver drops wheel i unless both bits i+1 and i+5 are set - ground.c).
 *
 *   grip_half (+0x284) = 1    ; 0x0043F4F8
 *   bits 5, 6, 1, 2 all set?  else return
 *   grip_half = -1
 *   bits 7, 8, 3, 4 all set?  else return
 *   grip_half = 0
 *   tick_26c_q (+0x26C) == 0  ->  FUN_0043F57A ; FUN_0043F691
 *
 * so the grip is halved when a part of wheels 1-2 is gone (and the yaw term
 * scaled, tyre.c), halved only when a part of wheels 3-4 is.  THE DETACHES
 * RUN ONLY WITH ALL EIGHT BITS PRESENT: once one wheel or tyre is gone,
 * nothing else of the eight ever comes off.
 *
 * FUN_0043F57A, the wheels, and FUN_0043F691, the tyres, are one shape each:
 * for n = 1..4, if FUN_0043F7C3(CL = 5 / 9 / 14 / 25) - rnd (+0x00C) ROL CL,
 * AND 0xFFFF, CMP 1: CF, so sixteen zero bits - and the damage is not below
 * [0x0057AF98] (100.0: FCOMP / FNSTSW / SHR AH,1 / JB, so NaN is "below"),
 * then: the part's bit set? detach it, store [0x0057B0B0] (256.0) into the
 * damage, and RETURN; bit clear? return.  Neither arm goes on to the next
 * wheel, so at most one wheel and one tyre in a step.  A roll that fails
 * goes on to the next n.
 *
 *   wheel n: 0x0045FBE0(ECX = n+4) - XOR the bit out, then 0x0045FC0C, the
 *            flying piece (an 8-slot pool at 0x006CE5A4, type 0x14 x
 *            [0x006CE084] + 0x50 + n+4).  NOT PORTED: the pool has no port
 * Reported in the return value.
 *   tyre n:  0x0043F7A8(ECX = n) - XOR the bit out, then 0x0045A7B4, the
 *            BLOWOUT sound's trigger in the car's feedback record (any
 *            slot's: it does not test for a local player) - soundfeed.c.
 *
 * The 256 is what the wheel warnings (0xFE..0x101, damage_warn.c) wait for:
 * nothing else stores a damage field above 101. */

#include "physics.h"
#include "soundfeed.h"

static int bit(const car_state *car, unsigned n)
{
    return ((uint32_t)car->carpartsleft >> n) & 1u;            /* BT        */
}

/* FUN_0043F7C3: MOV EAX,[rnd] / ROL EAX,CL / AND EAX,0xFFFF / CMP EAX,1 -
 * CF is EAX < 1. */
static int roll(const car_state *car, unsigned cl)
{
    uint32_t r = car->rnd;
    r = (r << cl) | (r >> (32u - cl));
    return (r & 0xFFFFu) == 0;
}

/* the damage FCOMP against 100.0: JB on C0, which is set for "below" and
 * for unordered */
static int worn_through(float dam)
{
    return !(dam < 100.0f) && dam == dam;
}

static const unsigned rolls[4] = { 5, 9, 14, 25 };

uint32_t sim_parts_left_q(car_state *car, sim_sound_feed *f,
                          const sim_sound_view *v)
{
    uint32_t off = 0;
    unsigned n;

    car->grip_half = 1;
    if (!bit(car, 5) || !bit(car, 6) || !bit(car, 1) || !bit(car, 2))
        return 0;
    car->grip_half = -1;
    if (!bit(car, 7) || !bit(car, 8) || !bit(car, 3) || !bit(car, 4))
        return 0;
    car->grip_half = 0;
    if (car->tick_26c_q != 0) return 0;

    /* FUN_0043F57A: dam_whl1..4, car+0x2F8.. = dam[16..19] */
    for (n = 0; n < 4; n++) {
        if (!roll(car, rolls[n]) || !worn_through(car->dam[16 + n])) continue;
        if (bit(car, n + 5)) {
            car->carpartsleft ^= (int32_t)(1u << (n + 5));    /* 0x0045FBEF */
            off |= 1u << (n + 5);
            car->dam[16 + n] = 256.0f;                         /* [0x0057B0B0] */
        }
        break;
    }
    /* FUN_0043F691: dam_tyre1..4, car+0x354.. = dam[39..42] */
    for (n = 0; n < 4; n++) {
        if (!roll(car, rolls[n]) || !worn_through(car->dam[39 + n])) continue;
        if (bit(car, n + 1)) {
            car->carpartsleft ^= (int32_t)(1u << (n + 1));    /* 0x0043F7B7 */
            off |= 1u << (n + 1);
            sim_sound_blowout_q(car, f, v);                    /* 0x0045A7B4 */
            car->dam[39 + n] = 256.0f;
        }
        break;
    }
    return off;
}
