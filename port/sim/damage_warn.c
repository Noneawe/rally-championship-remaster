/* damage_warn.c - the damage WARNINGS: FUN_004C773F's 82 state machines,
 * FUN_004C90CE's 32-slot queue and FUN_004C9154's pop, called at
 * 0x00439E5E / 0x00439E63, straight after the accumulators (damage_acc.c)
 * in every car's step.
 *
 * WHAT THEY DO.  Each of the 35 states at car+0x368..+0x3F0 follows one
 * temperature or damage field up a ladder of thresholds; a step up pushes a
 * message id (0xCF..0x10B) and the new level onto the queue in the record
 * (+0x478 the count, +0x47C ids, +0x4FC values, +0x57C levels).  The pop
 * moves slot 0's id into msgtopre (+0x71C) when no message is up, for three
 * seconds; the frame draws it (hud.c, FUN_00459163).  Nothing else reads the
 * states or the queue (s6): no physics moves.
 *
 * THE ROWS are the listing's, in its order: each one is
 *   CMP [state],FROM / JNZ / FILD thresh / FLD [value] / FCOMP / FNSTSW /
 *   SHR AH,1 / JC|JNC / MOV [state],TO [/ ESI id, EAX [value], EBX level,
 *   CALL 0x004C90CE]
 * and they run in sequence, so a ladder can climb (or the three
 * temperatures fall) more than one rung in one step.  Decoded by
 * tools/warnstates.py (82 blocks, residue the RET); warn-vm runs the
 * original's own instructions against this table on random cars.
 *
 * THE COMPARE.  C0 is set for value < threshold AND for an unordered value,
 * so a JC row (rising) acts on value >= threshold only, a JNC row (falling)
 * on everything else, NaN included.  The thresholds are FILD'd integers,
 * exact in a float.
 *
 * THE DEDUP KEY IS THE VALUE'S BITS, not its address: the tail's
 * MOV EAX,[value] (0x004C7781) loads the float, and FUN_004C90CE compares
 * EAX with the stored +0x4FC words.  So two rungs of one ladder crossed in
 * the same step (the same bits) collapse into one slot at the higher level,
 * and the same field pushed on a later step at a different value takes a new
 * slot.  (s3 said "the same value address" until.) */

#include "physics.h"

#include <string.h>


/* 0x004C773F.. in address order.  state, value, from, to, threshold, JC,
 * id, level. */
static const sim_warn_row rows[SIM_WARN_ROWS] = {
    /* ctmp, +0x2B4: 1 / 50 / 75, each with a falling edge back */
    { 0x3C0, 0x2B4, 0, 1,   1, 1, 0x0CF, 1 },
    { 0x3C0, 0x2B4, 1, 0,   1, 0, 0,     0 },
    { 0x3C0, 0x2B4, 1, 2,  50, 1, 0x0CF, 2 },
    { 0x3C0, 0x2B4, 2, 1,  50, 0, 0,     0 },
    { 0x3C0, 0x2B4, 2, 3,  75, 1, 0x0D0, 3 },
    { 0x3C0, 0x2B4, 3, 2,  75, 0, 0,     0 },
    /* btmpf +0x2AC, btmpr +0x2B0: 75, with a falling edge */
    { 0x3C4, 0x2AC, 0, 1,  75, 1, 0x0D1, 1 },
    { 0x3C4, 0x2AC, 1, 0,  75, 0, 0,     0 },
    { 0x3C8, 0x2B0, 0, 1,  75, 1, 0x0D1, 1 },
    { 0x3C8, 0x2B0, 1, 0,  75, 0, 0,     0 },
    /* twelve damage fields: 50 / 75 / 90 / 100, one way */
    { 0x368, 0x2B8, 0, 1,  50, 1, 0x0D2, 1 },   /* dam_engine  */
    { 0x368, 0x2B8, 1, 2,  75, 1, 0x0D2, 2 },
    { 0x368, 0x2B8, 2, 3,  90, 1, 0x0D3, 3 },
    { 0x368, 0x2B8, 3, 4, 100, 1, 0x0D4, 4 },
    { 0x36C, 0x2BC, 0, 1,  50, 1, 0x0D5, 1 },   /* dam_exhaust */
    { 0x36C, 0x2BC, 1, 2,  75, 1, 0x0D5, 2 },
    { 0x36C, 0x2BC, 2, 3,  90, 1, 0x0D6, 3 },
    { 0x36C, 0x2BC, 3, 4, 100, 1, 0x0D7, 4 },
    { 0x370, 0x2C0, 0, 1,  50, 1, 0x0D8, 1 },   /* dam_turbo   */
    { 0x370, 0x2C0, 1, 2,  75, 1, 0x0D8, 2 },
    { 0x370, 0x2C0, 2, 3,  90, 1, 0x0D9, 3 },
    { 0x370, 0x2C0, 3, 4, 100, 1, 0x0DA, 4 },
    { 0x374, 0x2C4, 0, 1,  50, 1, 0x0DB, 1 },   /* dam_cooling */
    { 0x374, 0x2C4, 1, 2,  75, 1, 0x0DB, 2 },
    { 0x374, 0x2C4, 2, 3,  90, 1, 0x0DC, 3 },
    { 0x374, 0x2C4, 3, 4, 100, 1, 0x0DD, 4 },
    { 0x378, 0x2C8, 0, 1,  50, 1, 0x0DE, 1 },   /* dam_clutch  */
    { 0x378, 0x2C8, 1, 2,  75, 1, 0x0DE, 2 },
    { 0x378, 0x2C8, 2, 3,  90, 1, 0x0DF, 3 },
    { 0x378, 0x2C8, 3, 4, 100, 1, 0x0E0, 4 },
    { 0x37C, 0x2CC, 0, 1,  50, 1, 0x0E1, 1 },   /* dam_diff    */
    { 0x37C, 0x2CC, 1, 2,  75, 1, 0x0E1, 2 },
    { 0x37C, 0x2CC, 2, 3,  90, 1, 0x0E2, 3 },
    { 0x37C, 0x2CC, 3, 4, 100, 1, 0x0E3, 4 },
    { 0x380, 0x2F4, 0, 1,  50, 1, 0x0E4, 1 },   /* dam_gearbox */
    { 0x380, 0x2F4, 1, 2,  75, 1, 0x0E4, 2 },
    { 0x380, 0x2F4, 2, 3,  90, 1, 0x0E5, 3 },
    { 0x380, 0x2F4, 3, 4, 100, 1, 0x0E6, 4 },
    { 0x384, 0x30C, 0, 1,  50, 1, 0x0E7, 1 },   /* dam_body    */
    { 0x384, 0x30C, 1, 2,  75, 1, 0x0E7, 2 },
    { 0x384, 0x30C, 2, 3,  90, 1, 0x0E8, 3 },
    { 0x384, 0x30C, 3, 4, 100, 1, 0x0E9, 4 },
    { 0x388, 0x310, 0, 1,  50, 1, 0x0EA, 1 },   /* dam_elect   */
    { 0x388, 0x310, 1, 2,  75, 1, 0x0EA, 2 },
    { 0x388, 0x310, 2, 3,  90, 1, 0x0EB, 3 },
    { 0x388, 0x310, 3, 4, 100, 1, 0x0EC, 4 },
    { 0x38C, 0x330, 0, 1,  50, 1, 0x0ED, 1 },   /* dam_steer   */
    { 0x38C, 0x330, 1, 2,  75, 1, 0x0ED, 2 },
    { 0x38C, 0x330, 2, 3,  90, 1, 0x0EE, 3 },
    { 0x38C, 0x330, 3, 4, 100, 1, 0x0EF, 4 },
    { 0x390, 0x350, 0, 1,  50, 1, 0x0F0, 1 },   /* dam_brakes  */
    { 0x390, 0x350, 1, 2,  75, 1, 0x0F0, 2 },
    { 0x390, 0x350, 2, 3,  90, 1, 0x0F1, 3 },
    { 0x390, 0x350, 3, 4, 100, 1, 0x0F2, 4 },
    { 0x39C, 0x344, 0, 1,  50, 1, 0x0F3, 1 },   /* dam_susp    */
    { 0x39C, 0x344, 1, 2,  75, 1, 0x0F3, 2 },
    { 0x39C, 0x344, 2, 3,  90, 1, 0x0F4, 3 },
    { 0x39C, 0x344, 3, 4, 100, 1, 0x0F5, 4 },
    /* dam_lights +0x32C: 50 / 75 / 90, no level 4 */
    { 0x394, 0x32C, 0, 1,  50, 1, 0x0F6, 1 },
    { 0x394, 0x32C, 1, 2,  75, 1, 0x0F6, 2 },
    { 0x394, 0x32C, 2, 3,  90, 1, 0x0F7, 3 },
    /* dam_lights1..4, lightsf, lightsb: 100 */
    { 0x3CC, 0x314, 0, 1, 100, 1, 0x0F8, 1 },
    { 0x3D0, 0x318, 0, 1, 100, 1, 0x0F9, 1 },
    { 0x3D4, 0x31C, 0, 1, 100, 1, 0x0FA, 1 },
    { 0x3D8, 0x320, 0, 1, 100, 1, 0x0FB, 1 },
    { 0x3DC, 0x324, 0, 1, 100, 1, 0x0FC, 1 },
    { 0x3E0, 0x328, 0, 1, 100, 1, 0x0FD, 1 },
    /* dam_whl1..4: 101 - only a detached wheel's 256.0 (FUN_0043F57A,
     * parts_left.c) reaches it; the accumulator clamps to 100 */
    { 0x3E4, 0x2F8, 0, 1, 101, 1, 0x0FE, 1 },
    { 0x3E8, 0x2FC, 0, 1, 101, 1, 0x0FF, 1 },
    { 0x3EC, 0x300, 0, 1, 101, 1, 0x100, 1 },
    { 0x3F0, 0x304, 0, 1, 101, 1, 0x101, 1 },
    /* dam_tyres +0x364: 50 / 75 / 90 */
    { 0x398, 0x364, 0, 1,  50, 1, 0x102, 1 },
    { 0x398, 0x364, 1, 2,  75, 1, 0x102, 2 },
    { 0x398, 0x364, 2, 3,  90, 1, 0x103, 3 },
    /* dam_gearrvs, dam_gear1..7 (no dam_gear0): 100 */
    { 0x3A0, 0x2D0, 0, 1, 100, 1, 0x104, 1 },
    { 0x3A4, 0x2D8, 0, 1, 100, 1, 0x105, 1 },
    { 0x3A8, 0x2DC, 0, 1, 100, 1, 0x106, 1 },
    { 0x3AC, 0x2E0, 0, 1, 100, 1, 0x107, 1 },
    { 0x3B0, 0x2E4, 0, 1, 100, 1, 0x108, 1 },
    { 0x3B4, 0x2E8, 0, 1, 100, 1, 0x109, 1 },
    { 0x3B8, 0x2EC, 0, 1, 100, 1, 0x10A, 1 },
    { 0x3BC, 0x2F0, 0, 1, 100, 1, 0x10B, 1 },
};

/* a DIAGNOSTIC, for warn_vm_check: the push's arms */
static long n_over, n_drop, n_full, n_masked;
static long row_hits[SIM_WARN_ROWS];       /* census: rows that fired    */

static unsigned char *at(car_state *car, unsigned off)
{
    return (unsigned char *)car + off;
}

/* FUN_004C90CE: ESI id, EAX the value's bits, EBX the level. */
void sim_damage_warn_push_q(car_state *car, int32_t id, uint32_t value,
                            uint32_t level)
{
    uint32_t i;
    if (!(car->hud_mask & 4u)) {                   /* TEST [+0x220],4        */
        n_masked++;
        return;
    }
    if (car->warn_count == SIM_WARN_SLOTS) {       /* CMP 0x20 / JZ          */
        n_full++;
        return;
    }
    i = 0;
    if (car->warn_count != 0 && level != 0) {
        /* the scan: a live slot (level != 0) with the same value */
        for (i = 0; i != car->warn_count; i++) {
            if (car->warn_level[i] == 0) continue;
            if (value != car->warn_val[i]) continue;
            if (level <= car->warn_level[i]) {     /* JZ / JC: unsigned  */
                n_drop++;
                return;
            }
            n_over++;
            car->warn_id[i] = id;
            car->warn_level[i] = level;
            return;
        }
    } else {
        i = car->warn_count;   /* 0, or level 0: EDX = count, appended */
    }
    car->warn_id[i] = id;
    car->warn_val[i] = value;
    car->warn_level[i] = level;
    car->warn_count++;
}

/* FUN_004C773F. */
void sim_damage_warn_q(car_state *car)
{
    int r;
    for (r = 0; r < SIM_WARN_ROWS; r++) {
        const sim_warn_row *w = &rows[r];
        int32_t st;
        float v;
        int ge;
        memcpy(&st, at(car, w->state), 4);
        if (st != w->from) continue;
        memcpy(&v, at(car, w->value), 4);
        ge = v >= (float)w->thresh;                /* !C0                    */
        if (w->rising ? !ge : ge) continue;
        st = w->to;
        row_hits[r]++;
        memcpy(at(car, w->state), &st, 4);
        if (w->msg != 0) {
            uint32_t bits;
            memcpy(&bits, at(car, w->value), 4);
            sim_damage_warn_push_q(car, w->msg, bits, w->level);
        }
    }
}

/* FUN_004C9154: one a step, while no message is up. */
void sim_damage_warn_pop_q(car_state *car, sim_race_msg *m)
{
    if (m->w728 != 0 || m->topre != 0) return;
    if (car->warn_count == 0) return;
    m->topre = car->warn_id[0];
    m->flash = 50;                                 /* +0x730 = 0x32          */
    m->timer = 300;                                /* +0x72C = 0x12C, 3 s    */
    car->warn_count--;
    /* three REP MOVSD of 0x1F dwords: all 31, whatever the count */
    memmove(&car->warn_id[0], &car->warn_id[1], 31 * 4);
    memmove(&car->warn_val[0], &car->warn_val[1], 31 * 4);
    memmove(&car->warn_level[0], &car->warn_level[1], 31 * 4);
}

/* FUN_0043E2DF, decoded by tools/warnlevels.py (34 stores, 77 compares;
 * s5): each state is the number of its thresholds the
 * value is strictly ABOVE - FILD threshold / FCOMP [ESI+value] / JNC store,
 * so C0 (threshold < value, or unordered) takes the rung and a NaN climbs
 * the whole ladder.  Not the machines' thresholds: ctmp 50/75/90/100,
 * the wheels 100.  +0x3C8 (btmpr) is not written.  In the listing's order. */
static const struct { uint16_t state, value; uint8_t n, t[4]; } rebuild[34] = {
    { 0x3C0, 0x2B4, 4, { 50, 75, 90, 100 } },   /* ctmp                  */
    { 0x3C4, 0x2AC, 1, { 75 } },                /* btmpf                 */
    { 0x368, 0x2B8, 4, { 50, 75, 90, 100 } },
    { 0x36C, 0x2BC, 4, { 50, 75, 90, 100 } },
    { 0x370, 0x2C0, 4, { 50, 75, 90, 100 } },
    { 0x374, 0x2C4, 4, { 50, 75, 90, 100 } },
    { 0x378, 0x2C8, 4, { 50, 75, 90, 100 } },
    { 0x37C, 0x2CC, 4, { 50, 75, 90, 100 } },
    { 0x380, 0x2F4, 4, { 50, 75, 90, 100 } },
    { 0x384, 0x30C, 4, { 50, 75, 90, 100 } },
    { 0x388, 0x310, 4, { 50, 75, 90, 100 } },
    { 0x38C, 0x330, 4, { 50, 75, 90, 100 } },
    { 0x390, 0x350, 4, { 50, 75, 90, 100 } },
    { 0x39C, 0x344, 4, { 50, 75, 90, 100 } },
    { 0x394, 0x32C, 3, { 50, 75, 90 } },
    { 0x3CC, 0x314, 1, { 100 } },
    { 0x3D0, 0x318, 1, { 100 } },
    { 0x3D4, 0x31C, 1, { 100 } },
    { 0x3D8, 0x320, 1, { 100 } },
    { 0x3DC, 0x324, 1, { 100 } },
    { 0x3E0, 0x328, 1, { 100 } },
    { 0x3E4, 0x2F8, 1, { 100 } },
    { 0x3E8, 0x2FC, 1, { 100 } },
    { 0x3EC, 0x300, 1, { 100 } },
    { 0x3F0, 0x304, 1, { 100 } },
    { 0x398, 0x364, 3, { 50, 75, 90 } },        /* dam_tyres             */
    { 0x3A0, 0x2D0, 1, { 100 } },
    { 0x3A4, 0x2D8, 1, { 100 } },
    { 0x3A8, 0x2DC, 1, { 100 } },
    { 0x3AC, 0x2E0, 1, { 100 } },
    { 0x3B0, 0x2E4, 1, { 100 } },
    { 0x3B4, 0x2E8, 1, { 100 } },
    { 0x3B8, 0x2EC, 1, { 100 } },
    { 0x3BC, 0x2F0, 1, { 100 } }
};

void sim_damage_warn_rebuild_q(car_state *car)
{
    int i, k;
    for (i = 0; i < 34; i++) {
        float v;
        int32_t st = 0;
        memcpy(&v, at(car, rebuild[i].value), 4);
        for (k = 0; k < rebuild[i].n; k++) {
            if (!((float)rebuild[i].t[k] < v || v != v)) break;   /* JNC */
            st = k + 1;
        }
        memcpy(at(car, rebuild[i].state), &st, 4);
    }
}

void sim_damage_warn_row_hits_q(long out[SIM_WARN_ROWS])
{
    memcpy(out, row_hits, sizeof row_hits);
}

void sim_damage_warn_counts_q(long *over, long *drop, long *full,
                              long *masked)
{
    *over = n_over;
    *drop = n_drop;
    *full = n_full;
    *masked = n_masked;
}
