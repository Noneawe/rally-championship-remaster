/* setup.c - see setup.h.
 *
 * Every function names the original it transcribes.  The state lives in the
 * VM's data copy at the original's own addresses (setup.h).  `setup_check`
 * runs the originals in the VM against these, byte for byte. */
#include "setup.h"

#include <math.h>
#include <string.h>

/* ---- the original's addresses ------------------------------------------- */

enum {
    A_PLAYERS   = 0x00708DE8,   /* pointer table, [0x7082F8] picks the entry   */
    A_PIDX      = 0x007082F8,
    A_CARRECS   = 0x00708E0C,   /* per car: [+0x74] -> the repair record       */
    A_BUDGET    = 0x007082A4,
    A_SPENT     = 0x0070CBF4,
    A_CURSOR    = 0x00707E91,
    A_FLAG      = 0x0070BE7A,   /* touched: item * 4                           */
    A_LOCK      = 0x0070BECE,   /* overtime lock: item * 4                     */
    A_COSTP     = 0x0070BF22,   /* item * 4 -> pointer to its cost dword       */
    A_EDIT      = 0x0070CA08,   /* a picker is being edited                    */
    A_PANEL     = 0x0070CA0C,   /* which: 1 gearing 2 tyres 3 steer 4 brake
                                   5 height 6 stiffness                        */
    A_RES       = 0x0070CA10,   /* the charge's verdict                        */
    A_COST      = 0x0070CAC8,   /* part n's service time, n = item - 2         */
    A_DIV99     = 0x0070CAC0,   /* 99.0f                                       */
    A_DIV100    = 0x0070CAC4,   /* 100.0f                                      */
    A_HI        = 0x0070CB08,   /* part n's longest repair                     */
    A_THR       = 0x0070CB40,   /* part n's failure threshold                  */
    A_SHARE     = 0x0070CB78,   /* part n's share of the inspection            */
    A_PEN       = 0x0070CBB0,   /* part n's penalty                            */
    A_STATUS    = 0x0070CBE8,   /* 100 - the penalties: the status bar         */
    A_TOTAL     = 0x0070CBEC,   /* the sum of the longest repairs              */
    A_GEAR      = 0x0070CBF8,   /* gearing: working / original / edit start    */
    A_GEAR0     = 0x0070CBFC,
    A_GEARS     = 0x0070CC00,
    A_TYRE      = 0x0070CC04,   /* tyres                                       */
    A_TYRE0     = 0x0070CC08,
    A_TYRES     = 0x0070CC0C,
    A_H         = 0x0070CC10,   /* suspension height                           */
    A_H0        = 0x0070CC14,
    A_HS        = 0x0070CC18,
    A_S         = 0x0070CC1C,   /* suspension stiffness                        */
    A_S0        = 0x0070CC20,
    A_SS        = 0x0070CC24,
    A_HAS1      = 0x0070CC2C,   /* never written: 0                            */
    A_HAS5      = 0x0070CC30,
    A_WORK      = 0x00708358,   /* the routine's scratch dword                 */
    A_ONCE      = 0x0070CA48    /* 1 once the screen has run its first frame   */
};

#define R(a)    vm_rd32(v, (uint32_t)(a))
#define W(a, x) vm_wr32(v, (uint32_t)(a), (uint32_t)(x))
#define NPARTS  14
#define LAST    20              /* flags run 0..19; item 20 is the -1       */

/* part n's record offsets: the shortest repair; the longest is +4 */
static const unsigned lo_off[NPARTS] = {
    0x00, 0x38, 0x08, 0x40, 0x10, 0x48, 0x18, 0x50,
    0x20, 0x58, 0x28, 0x60, 0x30, 0x68
};

uint32_t setup_player(vm *v)
{
    return R(A_PLAYERS + 4u * R(A_PIDX));
}

static float rdf(vm *v, uint32_t a)
{
    uint32_t w = R(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static int32_t S(uint32_t x)
{
    return (int32_t)x;
}

/* ---- the costs: FUN_004A3047 -------------------------------------------- */

void setup_costs(vm *v)
{
    uint32_t P = setup_player(v);
    uint32_t car = R(P + 8);
    uint32_t rec = R(R(A_CARRECS + 4u * car) + 0x74);
    long double d99 = (long double)rdf(v, A_DIV99), d100 = (long double)rdf(v, A_DIV100);
    int n;
    int32_t total = 0;

    for (n = 0; n < NPARTS; n++) {
        int32_t level = S(R(P + 0xD4 + 4u * (uint32_t)n));
        int32_t lo = S(R(rec + lo_off[n])), hi = S(R(rec + lo_off[n] + 4));
        int32_t cost, diff;
        if (level == 0) {
            cost = 0;
        } else {
            level -= 1;
            diff = hi - lo;
            if (diff == 0) {
                cost = lo;
            } else {
                long double x = ((long double)diff / d99) * (long double)level;
                cost = lo + (int32_t)nearbyintl(x);
            }
        }
        W(A_COST + 4u * (uint32_t)n, cost);
        W(A_HI + 4u * (uint32_t)n, hi);
        W(A_THR + 4u * (uint32_t)n, R(rec + 0x78 + 4u * (uint32_t)n));
        if (n == 1 && (R(P + 0x278) & 0x1E0u) != 0x1E0u) W(A_THR + 4, 10);
        if (n == 13 && (R(P + 0x278) & 0x1Eu) != 0x1Eu) W(A_THR + 4u * 13, 10);
    }
    /* 0x004A3918: the sum of the longest repairs - every part but n = 4 */
    for (n = 0; n < NPARTS; n++)
        if (n != 4) total += S(R(A_HI + 4u * (uint32_t)n));
    W(A_TOTAL, total);

    /* 0x004A398D: the setup costs - the car's own, over the loop's n = 12 / 13 */
    W(0x0070CB00, R(rec + 0x70));
    W(0x0070CB04, R(rec + 0x74));
    W(A_COST + 4u * 12, R(rec + 0x30));
    W(A_COST + 4u * 13, R(rec + 0x68));
    W(A_H0, R(P + 0x114)); W(A_H, R(P + 0x114));
    W(A_S0, R(P + 0x118)); W(A_S, R(P + 0x118));

    /* 0x004A39DD..: the inspection shares and the penalties.  A share is the
     * part's longest repair over the total, per cent (round to nearest); a
     * part at or over its threshold costs level/100 of its share. */
    for (n = 0; n < NPARTS; n++) {
        int32_t thr, level, share;
        if (n == 4) continue;                                  /* turbo      */
        share = (int32_t)nearbyintl(((long double)S(R(A_HI + 4u * (uint32_t)n)) /
                                     (long double)total) * d100);
        W(A_SHARE + 4u * (uint32_t)n, share);
        thr = S(R(A_THR + 4u * (uint32_t)n));
        if (n == 1 && R(A_HAS1) != 1) thr = 10;                /* 0x004A3A8C */
        if (n == 5 && R(A_HAS5) == 1) thr = 10;                /* 0x004A3C46 */
        level = S(R(P + 0xD4 + 4u * (uint32_t)n));
        if (level >= thr) {
            long double x = ((long double)level / d100) * (long double)share;
            W(A_PEN + 4u * (uint32_t)n, (int32_t)nearbyintl(x));
        }
    }
}

/* FUN_0046D371 (reached through the screen's entry, not a CALL): the
 * pickers' working values from the player record, when the screen has not
 * run yet.  Its own slip is kept: it loads +0x114 into EAX and then
 * overwrites EAX from [0x70CC14], so the edit value is the original's, not
 * the record's - the same number at entry. */
void setup_panels_from_player(vm *v)
{
    uint32_t P = setup_player(v);
    if (R(A_ONCE) == 1) return;
    W(A_TYRE0, R(P + 0x10C)); W(A_TYRE, R(P + 0x10C));
    W(A_GEAR0, R(P + 0x120)); W(A_GEAR, R(P + 0x120));
    W(A_S, R(P + 0x118));     W(A_S0, R(P + 0x118));
    W(A_H, R(A_H0));
}

void setup_begin(vm *v, uint32_t budget)
{
    uint32_t i;
    W(A_BUDGET, budget);
    W(0x00708294, 1);
    W(A_SPENT, 0);
    W(0x0070CAA0, 0);
    W(A_EDIT, 0);
    W(A_PANEL, 0);
    W(A_RES, 0);
    for (i = 0; i < LAST; i++) { W(A_FLAG + 4 * i, 0); W(A_LOCK + 4 * i, 0); }
    W(A_ONCE, 0);
}

/* ---- the charge ledger -------------------------------------------------- */

static uint32_t flag(vm *v, int c) { return R(A_FLAG + 4u * (uint32_t)c); }
static void setflag(vm *v, int c, uint32_t x) { W(A_FLAG + 4u * (uint32_t)c, x); }

/* the sum 0x00468C39..0x00468C57 makes: every touched item's cost */
static int32_t spent_sum(vm *v)
{
    int32_t sum = 0;
    int i;
    for (i = 0; i < LAST; i++)
        if (flag(v, i)) {
            uint32_t p = R(A_COSTP + 4u * (uint32_t)i);
            sum += S(R(p));
        }
    return sum;
}

/* 0x00468675 */
static void clear_locks(vm *v)
{
    int i;
    for (i = 0; i < LAST; i++) W(A_LOCK + 4u * (uint32_t)i, 0);
}

/* 0x0046868C: the sum, and the locks off when 900 s or more is left of the
 * budget. */
static void settle(vm *v)
{
    int32_t sp = spent_sum(v);
    W(A_SPENT, sp);
    if (S(R(A_BUDGET)) - sp >= 0x384) clear_locks(v);
}

/* 0x00468C19: touch the cursor's item and see what it costs.  The verdict:
 * 3 = the budget (less the held-back 900 s) still covers it; 1 = it dips
 * into the held-back time; 2 = no: the item is left untouched. */
static void charge(vm *v)
{
    int c = S(R(A_CURSOR));
    int32_t left;
    W(A_RES, 0);
    setflag(v, c, flag(v, c) ^ 1u);
    W(A_SPENT, spent_sum(v));
    left = S(R(A_BUDGET)) - S(R(A_SPENT));
    if (left >= 0x384) {
        setflag(v, c, flag(v, c) ^ 1u);
        W(A_RES, 3);
    } else if (left <= 0) {
        setflag(v, c, 0);
        W(A_RES, 2);
    } else {
        setflag(v, c, flag(v, c) ^ 1u);
        W(A_RES, 1);
    }
}

/* 0x00468D71 / 0x00468D98 */
static void touch_and_lock(vm *v)
{
    int c = S(R(A_CURSOR));
    setflag(v, c, 1);
    W(A_LOCK + 4u * (uint32_t)c, 1);
}
static void touch(vm *v) { setflag(v, S(R(A_CURSOR)), 1); }

/* 0x00468640..0x0046866E, shared by every picker's tail */
static void after_charge_ok(vm *v)
{
    if (R(A_RES) == 1) {
        touch_and_lock(v);
        W(A_EDIT, 0);
    } else {
        W(A_EDIT, 0);
        clear_locks(v);
        touch(v);
    }
}

/* ---- the select handlers ------------------------------------------------ */

/* 0x00468420: a component row. */
static void select_row(vm *v)
{
    int c = S(R(A_CURSOR));
    int32_t left;
    int i;
    W(A_RES, 0);
    W(A_EDIT, 0);
    setflag(v, c, flag(v, c) ^ 1u);
    W(A_SPENT, spent_sum(v));
    left = S(R(A_BUDGET)) - S(R(A_SPENT));
    if (left >= 0x384) {
        for (i = 0; i < LAST; i++) W(A_LOCK + 4u * (uint32_t)i, 0);
    } else if (left < 0) {
        setflag(v, c, flag(v, c) ^ 1u);
        W(A_RES, 1);
    } else {
        W(A_LOCK + 4u * (uint32_t)c, 1);
    }
    W(A_SPENT, spent_sum(v));
}

/* the picker selects: 0x004682D4 .. 0x004683D7 */
static void select_picker(vm *v, int item)
{
    int c = S(R(A_CURSOR));
    uint32_t cost_p = R(A_COSTP + 4u * (uint32_t)item);
    int free_item = item == 16 || item == 17;                  /* steer, brake */
    if (!free_item && flag(v, c) != 1 &&
        S(R(A_BUDGET)) - S(R(A_SPENT)) - S(R(cost_p)) < 0)
        return;                                                /* over budget */
    W(A_EDIT, R(A_EDIT) ^ 1u);
    switch (item) {
    case 14: W(A_PANEL, 1); W(A_GEARS, R(A_GEAR)); break;      /* 0x004682D4 */
    case 15: W(A_PANEL, 2); W(A_TYRES, R(A_TYRE)); break;      /* 0x0046831D */
    case 16: W(A_PANEL, 3); break;                             /* 0x00468366 */
    case 17: W(A_PANEL, 4); break;                             /* 0x0046837A */
    case 18: W(A_PANEL, 5); W(A_HS, R(A_H)); break;            /* 0x0046838E */
    case 19: W(A_PANEL, 6); W(A_SS, R(A_S)); break;            /* 0x004683D7 */
    }
}

int setup_select(vm *v, int item)
{
    if (item == 1) return 1;                                   /* 0x0046E300 */
    if (item >= 2 && item <= 13) { select_row(v); return 0; }
    if (item >= 14 && item <= 19) { select_picker(v, item); return 0; }
    return -1;
}

/* ---- the panels' per-frame handlers ------------------------------------- */

static void step_clamped(vm *v, uint32_t a, int dir, int32_t max)
{
    int32_t x = S(R(a));
    if (dir < 0) x = x == 0 ? 0 : x - 1;
    else         x = x >= max ? max : x + 1;
    W(a, x);
}

/* a picker with a working value, an original and an edit-start value,
 * ending in the charge: 0x0046857A (gearing), 0x004689A4 (tyres) */
static void picker_a_confirm(vm *v, uint32_t cur, uint32_t orig,
                             uint32_t start, void (*toggle)(vm *))
{
    int c;
    c = S(R(A_CURSOR));
    if (R(start) != R(cur)) {                                  /* changed    */
        setflag(v, c, 0);
        charge(v);
        if (R(A_RES) == 2) { W(cur, R(orig)); W(A_EDIT, 0); settle(v); return; }
        after_charge_ok(v);
    } else {                                                   /* unchanged  */
        W(A_EDIT, 0);
        toggle(v);
        if (flag(v, c) == 1) {
            setflag(v, c, 0);
            charge(v);
            if (R(A_RES) == 2) { W(cur, R(orig)); W(A_EDIT, 0); settle(v); return; }
            after_charge_ok(v);
        }
    }
    settle(v);
}

static void picker_a(vm *v, unsigned key, uint32_t cur, uint32_t orig,
                     uint32_t start, void (*toggle)(vm *), int32_t max)
{
    int confirm = 0;
    if (key) {
        W(0x0070802C, 0);                                      /* the clock  */
        if (key == SU_KEY_LEFT)  step_clamped(v, cur, -1, max);
        if (key == SU_KEY_RIGHT) step_clamped(v, cur, +1, max);
        if (key == SU_KEY_ENTER) confirm = 1;
    }
    if (!confirm) { settle(v); return; }
    picker_a_confirm(v, cur, orig, start, toggle);
}

/* 0x00468CED / 0x00468D0E: a second RETURN on an untouched picker toggles its
 * flag; off, the working value goes back to the original. */
static void toggle_gear(vm *v)
{
    int c = S(R(A_CURSOR));
    setflag(v, c, flag(v, c) ^ 1u);
    if (flag(v, c) != 1) W(A_GEAR, R(A_GEAR0));
}
static void toggle_tyre(vm *v)
{
    int c = S(R(A_CURSOR));
    setflag(v, c, flag(v, c) ^ 1u);
    if (flag(v, c) != 1) W(A_TYRE, R(A_TYRE0));
}
static void toggle_h(vm *v)
{
    int c = S(R(A_CURSOR));
    setflag(v, c, flag(v, c) ^ 1u);
    if (flag(v, c) != 1) W(A_H, R(A_H0));
}
static void toggle_s(vm *v)
{
    int c = S(R(A_CURSOR));
    setflag(v, c, flag(v, c) ^ 1u);
    if (flag(v, c) != 1) W(A_S, R(A_S0));
}

/* 0x00468802 (height, +0x114) and 0x004686CC (stiffness, +0x118): the same
 * picker, but a value equal to the player record's is a cancel, not a
 * charge. */
static void picker_b_confirm(vm *v, uint32_t cur, uint32_t orig,
                             uint32_t start, uint32_t rec_off, void (*toggle)(vm *))
{
    int c;
    uint32_t P = setup_player(v);
    c = S(R(A_CURSOR));
    if (R(cur) == R(P + rec_off)) {                            /* a cancel   */
        setflag(v, c, 0);
        W(A_EDIT, 0);
        settle(v);
        return;
    }
    if (R(start) != R(cur)) {                                  /* changed    */
        setflag(v, c, 0);
        charge(v);
        if (R(A_RES) == 2) { W(cur, R(orig)); W(A_EDIT, 0); settle(v); return; }
        after_charge_ok(v);
    } else {
        W(A_EDIT, 0);
        toggle(v);
        if (flag(v, c) == 1) {
            setflag(v, c, 0);
            charge(v);
            if (R(A_RES) == 2) { W(cur, R(orig)); W(A_EDIT, 0); settle(v); return; }
            after_charge_ok(v);
        }
    }
    settle(v);
}

static void picker_b(vm *v, unsigned key, uint32_t cur, uint32_t orig,
                     uint32_t start, uint32_t rec_off, void (*toggle)(vm *))
{
    int confirm = 0;
    if (key) {
        W(0x0070802C, 0);
        if (key == SU_KEY_LEFT)  step_clamped(v, cur, -1, 100);
        if (key == SU_KEY_RIGHT) step_clamped(v, cur, +1, 100);
        if (key == SU_KEY_ENTER) confirm = 1;
    }
    if (!confirm) { settle(v); return; }
    picker_b_confirm(v, cur, orig, start, rec_off, toggle);
}

/* The pickers' change handlers with EBX = 1 (0x0046857A / 0x004689A4 / 0x00468802 / 0x004686CC), the way
 * the car setup slot's load (0x0046BB78) calls them: no key is read, the RETURN branch runs at once.
 * `item` is the cursor global's value the load sets for the call (14, 15, 18, 19). */
void setup_picker_confirm(vm *v, int item)
{
    switch (item) {
    case 14: picker_a_confirm(v, A_GEAR, A_GEAR0, A_GEARS, toggle_gear); break;       /* 0x0046857A */
    case 15: picker_a_confirm(v, A_TYRE, A_TYRE0, A_TYRES, toggle_tyre); break;       /* 0x004689A4 */
    case 18: picker_b_confirm(v, A_H, A_H0, A_HS, 0x114, toggle_h); break;            /* 0x00468802 */
    case 19: picker_b_confirm(v, A_S, A_S0, A_SS, 0x118, toggle_s); break;            /* 0x004686CC */
    }
}

/* 0x0046896E / 0x00468938: steering sensitivity and brake balance, edited in
 * the player record at once and free. */
static void picker_live(vm *v, unsigned key, uint32_t rec_off)
{
    uint32_t P = setup_player(v);
    if (!key) return;
    W(0x0070802C, 0);
    if (key == SU_KEY_LEFT)  step_clamped(v, P + rec_off, -1, 100);
    if (key == SU_KEY_RIGHT) step_clamped(v, P + rec_off, +1, 100);
    if (key == SU_KEY_ENTER) W(A_EDIT, 0);
}

void setup_edit_frame(vm *v, unsigned key)
{
    if (R(A_EDIT) != 1) return;
    switch (R(A_PANEL)) {
    case 1: picker_a(v, key, A_GEAR, A_GEAR0, A_GEARS, toggle_gear, 4); break;
    case 2: picker_a(v, key, A_TYRE, A_TYRE0, A_TYRES, toggle_tyre, 5); break;
    case 3: picker_live(v, key, 0x11C); break;
    case 4: picker_live(v, key, 0x110); break;
    case 5: picker_b(v, key, A_H, A_H0, A_HS, 0x114, toggle_h); break;
    case 6: picker_b(v, key, A_S, A_S0, A_SS, 0x118, toggle_s); break;
    }
}

/* ---- the commit: FUN_00468DAD -------------------------------------------- */

/* item 14, 3, 7, 11, 13: the damage tables the repair clears */
static void zero_block(vm *v, uint32_t P, uint32_t from, uint32_t to)
{
    uint32_t o;
    for (o = from; o <= to; o += 4) W(P + o, 0);
}

void setup_commit(vm *v)
{
    uint32_t P = setup_player(v);
    int item;
    W(0x0070CAA0, 1);
    if (flag(v, 14) == 1) zero_block(v, P, 0x17C, 0x1A0);
    if (flag(v, 3) == 1)  zero_block(v, P, 0x1A4, 0x1B8);
    if (flag(v, 7) == 1)  zero_block(v, P, 0x1BC, 0x1D4);
    if (flag(v, 11) == 1) zero_block(v, P, 0x1D8, 0x1E8);
    if (flag(v, 13) == 1) zero_block(v, P, 0x1EC, 0x1F4);
    /* 0x00468F51: FUN_00482ED9 clears the tyres' damage table (the level
     * it also clears is put back and wound down below) */
    if (flag(v, 15) == 1) zero_block(v, P, 0x1F8, 0x208);
    for (item = 2; item <= 15; item++) {
        uint32_t off = (item % 2 == 0) ? 0xD4u + 8u * (uint32_t)((item - 2) / 2)
                                       : 0xD8u + 8u * (uint32_t)((item - 3) / 2);
        if (flag(v, item) != 1) continue;
        W(P + off, 0);                       /* the animation winds it down */
        setflag(v, item, 0);
        if (item == 14) { W(A_GEAR0, R(A_GEAR)); W(P + 0x120, R(A_GEAR)); }
        if (item == 15) { W(A_TYRE0, R(A_TYRE)); W(P + 0x10C, R(A_TYRE)); }
    }
    if (flag(v, 18) == 1) {
        W(P + 0x114, 0);
        setflag(v, 18, 0);
        W(A_H0, R(A_H));
        W(P + 0x114, R(A_H));
    }
    if (flag(v, 19) == 1) {
        W(P + 0x118, 0);
        setflag(v, 19, 0);
        W(A_S0, R(A_S));
        W(P + 0x118, R(A_S));
    }
    if (R(P + 0x108) == 0) W(P + 0x278, R(P + 0x278) | 0x1Eu);
    if (R(P + 0xD8) == 0) {                                    /* 0x00469C09 */
        uint32_t o;
        W(P + 0x278, R(P + 0x278) | 0xFFFFFFE1u);
        for (o = 0; o < 8; o++) W(P + 0x24C + 4 * o, 0);       /* REP STOSD  */
    }
}

/* ---- the words ----------------------------------------------------------- */

void setup_get_sliders(vm *v, setup_sliders *o)
{
    uint32_t P = setup_player(v);
    o->tyre = S(R(P + 0x10C)); o->bbal = S(R(P + 0x110));
    o->seth = S(R(P + 0x114)); o->sets = S(R(P + 0x118));
    o->stes = S(R(P + 0x11C)); o->setg = S(R(P + 0x120));
}

/* graham_to_shaun 0x004452xx..0x004453F8 */
static int32_t scale256(int32_t m)
{
    if (m < 0) return 0;
    if (m >= 100) return 0x100;
    return (int32_t)nearbyintl((long double)m * (long double)2.56f);
}

void setup_car_words_from(const setup_sliders *s, setup_car_words *o)
{
    o->tyretype = s->tyre;
    o->set_g = s->setg;
    o->b_bal = 0x100 - scale256(s->bbal);                      /* 0x004453ED */
    o->set_h = scale256(s->seth);
    o->set_s = scale256(s->sets);
    o->ste_s = scale256(s->stes);
}
