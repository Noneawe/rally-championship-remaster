/* fe_logic.c - see fe_logic.h.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps
 * as the original's does. */
#include "fe_logic.h"

#include <setjmp.h>
#include <string.h>

#define R(a)    vm_rd32(v, (a))
#define W(a, x) vm_wr32(v, (a), (x))

enum {
    A_PLAYERS = 0x00708DE8,   /* pointer table, [0x7082F8] picks the entry  */
    A_PIDX    = 0x007082F8,
};

/* the current player's record: mov eax,[0x7082F8]; mov edi,[eax*4+0x708DE8] */
static uint32_t cur_player(vm *v) { return R(A_PLAYERS + 4u * R(A_PIDX)); }

/* player 0's record, for the handlers that name it (mov eax,0) */
static uint32_t player0(vm *v) { return R(A_PLAYERS); }

/* 0x00464A30: flips the current player's +0x15C */
static void f464A30(vm *v)
{
    uint32_t P = cur_player(v);
    W(P + 0x15C, R(P + 0x15C) ^ 1u);
}

/* 0x00464AE0: player 0's +0x178 into [0x517954] (the units) */
static void f464AE0(vm *v)
{
    W(0x00517954, R(player0(v) + 0x178));
}

/* 0x00464F15: the redefine screen's wait for a key: [0x708014] = 1, [0x708018] = 0 */
static void f464F15(vm *v)
{
    W(0x00708014, 1);
    W(0x00708018, 0);
}

/* 0x00468054: one more point on the current player's +0x44 while the
 * other two of the 100 leave more than 10 (signed) */
static void f468054(vm *v)
{
    uint32_t P = cur_player(v);
    int32_t left = (int32_t)(100u - R(P + 0x40) - R(P + 0x44));
    if (left > 10) W(P + 0x44, R(P + 0x44) + 1u);
}

/* 0x0046C6A9: RETIRE's LEFT / RIGHT */
static void f46C6A9(vm *v)
{
    W(0x00707BC0, R(0x00707BC0) ^ 1u);
}

/* 0x0046C83A / 0x0046C8A9: OPTIONS' four items on / off - the item records
 * at 0x703098 + 4 + 0x114, 0x5C apart: [+0x14] the text, [+0x48] and [+0x1C]
 * the arrows' offsets */
static void items(vm *v, uint32_t text0, uint32_t a48, uint32_t a1c, int on)
{
    uint32_t e = 0x00703098u + 4u + 0x114u;
    int k;
    for (k = 0; k < 4; k++, e += 0x5C) {
        W(e + 0x14, on ? text0 + (uint32_t)k : 0u);
        W(e + 0x48, a48);
        W(e + 0x1C, a1c);
    }
}
static void f46C83A(vm *v) { items(v, 0x171, 0xFFFFFC1Bu, 0xFFFFFFA7u, 1); }
static void f46C8A9(vm *v) { items(v, 0, 0xFFFFFC1Cu, 0xFFFFFFA9u, 0); }

/* 0x0046D44B: ADVANCED AUDIO's six working words from their defaults */
static void f46D44B(vm *v)
{
    W(0x00707FD4, R(0x00501C60));
    W(0x00707FE8, R(0x00501C64));
    W(0x00707FE4, R(0x00501C74));
    W(0x00707FE0, R(0x00501C70));
    W(0x00707FDC, R(0x00501C6C));
    W(0x00707FD8, R(0x00501C68));
}

/* 0x00467C2E: the controller screen's start: player 0 current, its +0x2C
 * into [0x70822C], its +0x50..+0x7B (44 bytes) to 0x707F19 */
static void f467C2E(vm *v)
{
    uint32_t P, i;
    W(A_PIDX, 0);
    P = cur_player(v);
    W(0x0070822C, R(P + 0x2C));
    for (i = 0; i < 0x2C; i++) vm_wr8(v, 0x00707F19u + i, vm_rd8(v, P + 0x50u + i));
}

/* 0x0047D46C: every player out - +0x88 = 0x36EE80, +0xD0 = 6 - and [0x708294] = 0 */
static void f47D46C(vm *v)
{
    uint32_t k;
    for (k = 0; k < 4; k++) {
        uint32_t P = R(A_PLAYERS + 4u * k);
        W(P + 0x88, 0x0036EE80u);
        W(P + 0xD0, 6);
    }
    W(0x00708294, 0);
}

/* 0x00484244: is the level's score (+0x14 of the entry at [0x708044] +
 * level * 0x320 + 0x2D0) beaten: [0x708280] = 1 unless [0x70AB20] is less */
static void f484244(vm *v)
{
    uint32_t e = R(0x00708044) + R(0x0070AB54) * 0x320u + 0x2D0u;
    if (!((int32_t)R(0x0070AB20) < (int32_t)R(e + 0x14))) W(0x00708280, 1);
}

/* 0x00484274: the stage's time against the table: [0x70827C] = the
 * player's +0x88; [0x708280] = 1 unless it is greater than the record's +0x14 */
static void f484274(vm *v)
{
    uint32_t e = R(0x006E7ECDu + 4u * R(0x0070AB54)) + R(0x0070AB58) * 0xA4u;
    uint32_t t = R(R(0x00708048) + R(e + 0x3C) * 0x618u + 0x57Cu + 0x14u);
    uint32_t mine = R(cur_player(v) + 0x88);
    W(0x0070827C, mine);
    if (!((int32_t)mine > (int32_t)t)) W(0x00708280, 1);
}

/* 0x00486374: the level's score: [0x70AB10] = [0x70AB20]; [0x70AAF0] = player
 * 0's +0x94; [0x70AAE0] = (stages - first + 1) * 1000; [0x70AB00] = that +
 * [0x70AAC0] + [0x70AAF0]; [0x70AB20] = [0x70AB10] + [0x70AB00] */
static void f486374(vm *v)
{
    uint32_t t;
    W(0x0070AB10, R(0x0070AB20));
    W(0x0070AAF0, R(player0(v) + 0x94));
    W(0x0070AAE0, (R(0x0070AB60) - R(0x0070AAD0) + 1u) * 1000u);
    W(0x0070AB00, R(0x0070AAE0) + R(0x0070AAC0) + R(0x0070AAF0));
    t = R(0x0070AB10) + R(0x0070AB00);
    W(0x0070AB20, t);
}

/* 0x00486CEA: ten records of the level's table cleared at +0x40, 0x50 apart */
static void f486CEA(vm *v)
{
    uint32_t e = R(0x00708044) + R(0x0070AB54) * 0x320u;
    int k;
    for (k = 0; k < 10; k++, e += 0x50) W(e + 0x40, 0);
}

/* ---- batch 2 (2026-10-30): handlers that only read and write the menus' words ---- */

/* 0x0049D5A9: the current player's +0xD0 is 1 once [0x70CBE8] reaches 100
 * (signed), else 2 */
static void f49D5A9(vm *v)
{
    W(cur_player(v) + 0xD0, (int32_t)R(0x0070CBE8) < 100 ? 2u : 1u);
}

/* 0x0049E231: each of the [0x7082F0] players (a do-while, so at least one):
 * unless its +0xD0 is 1 the +0x88 and +0x8C are both 0x36EE80, else +0x8C
 * gains +0x88 */
static void f49E231(vm *v)
{
    uint32_t k = 0;
    do {
        uint32_t P = R(A_PLAYERS + 4u * k);
        if (R(P + 0xD0) != 1) {
            W(P + 0x88, 0x0036EE80u);
            W(P + 0x8C, 0x0036EE80u);
        } else {
            W(P + 0x8C, R(P + 0x8C) + R(P + 0x88));
        }
        k++;
    } while (k != R(0x007082F0));
}

/* 0x004A5249: the four players' +0x284 to -1 */
static void f4A5249(vm *v)
{
    uint32_t k;
    for (k = 0; k < 4; k++) W(R(A_PLAYERS + 4u * k) + 0x284, 0xFFFFFFFFu);
}

/* 0x00486DDA: 64 bytes of the stage record's +0x5C (the record of the entry at
 * [0x70AB54] / [0x70AB58], its +0x3C picking the 0x618-byte one in [0x708048])
 * to 0x70CF98 */
static void f486DDA(vm *v)
{
    uint32_t e = R(0x006E7ECDu + 4u * R(0x0070AB54)) + R(0x0070AB58) * 0xA4u;
    uint32_t s = R(0x00708048) + R(e + 0x3C) * 0x618u + 0x5Cu, i;
    for (i = 0; i < 0x40; i++) vm_wr8(v, 0x0070CF98u + i, vm_rd8(v, s + i));
}

/* the stage record the two handlers below pick: the table at 0x6E7E85 by
 * [0x708230], that array by [0x708238], the entry by [0x708234] (0xA4 each);
 * its +0x3C picks the 0x618-byte record in [0x70804C] (when [0x708230] is 1)
 * or [0x708050] */
static uint32_t sel_record(vm *v)
{
    uint32_t a = R(0x00708230);
    uint32_t e = R(R(0x006E7E85u + 4u * a) + 4u * R(0x00708238)) + R(0x00708234) * 0xA4u;
    return (a == 1 ? R(0x0070804C) : R(0x00708050)) + R(e + 0x3C) * 0x618u;
}

/* 0x004887B0: [0x708280] = 0; [0x70827C] = the player's +0x88; beating the
 * record's +0x14 (signed) leaves it 0, else both [0x708280] and [0x708288] = 1 */
static void f4887B0(vm *v)
{
    uint32_t rec, t, mine;
    W(0x00708280, 0);
    rec = sel_record(v) + 0x57Cu;
    t = R(rec + 0x14);
    mine = R(cur_player(v) + 0x88);
    W(0x0070827C, mine);
    if (!((int32_t)mine > (int32_t)t)) {
        W(0x00708280, 1);
        W(0x00708288, 1);
    }
}

/* 0x0048883E: the ten 0x9C-apart words at +0x40 of that record cleared */
static void f48883E(vm *v)
{
    uint32_t e = sel_record(v);
    int k;
    for (k = 0; k < 10; k++, e += 0x9C) W(e + 0x40, 0);
}

/* ---- batch 3 (2026-10-30): the five exchange sorts of the standings ----
 *
 * 0x004775CB, 0x004776DE (the 180 records of 0x9C bytes at [0x708054], by the
 * dword at +0x14 / +0x18) and 0x004A02EF, 0x004A18E6, 0x004A1776 (the 180
 * records of 0x78 bytes at [0x708058], by the dword at +0x34 / +0x50 / +0x6C)
 * are one loop: for i in 0..0xB2, for j in i+1..0xB3, swap the records i and j
 * when the keys compare (signed) in the sort's own direction - and a TIE SWAPS
 * TOO (<=), so equal keys are not left in place.  A swap exchanges the listed
 * dwords and the 31-byte name, the name through the scratch at 0x7081EC (which
 * keeps the last name moved); the other bytes of the records stay.  The two
 * counters end at 0xB3 / 0xB4. */
enum { A_SCRATCH = 0x007081EC, A_SORT_I = 0x00708320, A_SORT_J = 0x00708324 };

typedef struct {
    uint32_t table;         /* the address of the pointer to the records     */
    uint32_t stride;
    uint32_t name;          /* the name's offset in a record                 */
    uint32_t key;           /* the key's dword index                         */
    int      down;          /* swap when A's key <= B's (else B's <= A's)    */
    const uint8_t *words;   /* the dwords a swap exchanges                   */
    uint32_t nwords;
} sortspec;

static const uint8_t SW_9C[] = { 3, 5, 1, 0, 2, 7, 4, 6, 0x13 };
static const uint8_t SW_78[] = { 0xD, 1, 0, 2, 4, 3, 0xE, 0xF, 0x10, 0x11, 0x12, 0x13, 0x14,
                                 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };

static void exch_sort(vm *v, const sortspec *s)
{
    uint32_t i, j, k;
    for (i = 0; i != 0xB3; i++) {
        for (j = i + 1; j != 0xB4; j++) {
            uint32_t a = R(s->table) + i * s->stride, b = R(s->table) + j * s->stride;
            int32_t ka = (int32_t)R(a + 4u * s->key), kb = (int32_t)R(b + 4u * s->key);
            if (s->down ? ka <= kb : kb <= ka) {
                for (k = 0; k < s->nwords; k++) {
                    uint32_t t = R(a + 4u * s->words[k]);
                    W(a + 4u * s->words[k], R(b + 4u * s->words[k]));
                    W(b + 4u * s->words[k], t);
                }
                for (k = 0; k < 0x1F; k++) vm_wr8(v, A_SCRATCH + k, vm_rd8(v, b + s->name + k));
                for (k = 0; k < 0x1F; k++) vm_wr8(v, b + s->name + k, vm_rd8(v, a + s->name + k));
                for (k = 0; k < 0x1F; k++) vm_wr8(v, a + s->name + k, vm_rd8(v, A_SCRATCH + k));
            }
        }
    }
    W(A_SORT_I, 0xB3);
    W(A_SORT_J, 0xB4);
}

static const sortspec SORT_4775CB = { 0x00708054, 0x9C, 0x20, 5, 0, SW_9C, sizeof SW_9C };
static const sortspec SORT_4776DE = { 0x00708054, 0x9C, 0x20, 6, 0, SW_9C, sizeof SW_9C };
static const sortspec SORT_4A02EF = { 0x00708058, 0x78, 0x14, 0xD, 0, SW_78, sizeof SW_78 };
static const sortspec SORT_4A18E6 = { 0x00708058, 0x78, 0x14, 0x14, 1, SW_78, sizeof SW_78 };
static const sortspec SORT_4A1776 = { 0x00708058, 0x78, 0x14, 0x1B, 1, SW_78, sizeof SW_78 };

static void f4775CB(vm *v) { exch_sort(v, &SORT_4775CB); }   /* by the stage time   */
static void f4776DE(vm *v) { exch_sort(v, &SORT_4776DE); }   /* by the total        */
static void f4A02EF(vm *v) { exch_sort(v, &SORT_4A02EF); }
static void f4A18E6(vm *v) { exch_sort(v, &SORT_4A18E6); }
static void f4A1776(vm *v) { exch_sort(v, &SORT_4A1776); }

/* ---- batch 4 (2026-10-30): memory-only handlers the big-handler reading found ---- */

/* 0x0047A87F: each 0x94-byte record of the table at 0x6EDFE8 (a do-while, ended
 * by the NEXT record's first word being -1): its dwords 0xB..0x16 = 100 */
static void f47A87F(vm *v)
{
    uint32_t e = 0x006EDFE8u, k;
    do {
        for (k = 0xB; k <= 0x16; k++) W(e + 4u * k, 100);
        e += 0x94;
    } while (R(e) != 0xFFFFFFFFu);
}

/* 0x0046E617: every one of the [0x7082F0] players (a do-while) at 3,600,000
 * in +0x8C and +0x88 */
static void f46E617(vm *v)
{
    uint32_t k = 0;
    do {
        uint32_t P = R(A_PLAYERS + 4u * k);
        W(P + 0x8C, 3600000u);
        W(P + 0x88, 3600000u);
        k++;
    } while (k != R(0x007082F0));
}

/* 0x0046DA16: dwords from 0x70BE7A (an odd address) zeroed up to the one before
 * the first that reads -1 */
static void f46DA16(vm *v)
{
    uint32_t p = 0x0070BE7Au;
    do {
        W(p, 0);
        p += 4;
    } while (R(p) != 0xFFFFFFFFu);
}

/* the car's thirteen counters: the dword j of 0x70CB40 (the threshold) against
 * the player's +0xD4 + 4j, the fourteenth word of each (j = 4) having none, so
 * check k is word j = k + (k >= 4) */
static uint32_t chk_word(uint32_t k) { return k + (k >= 4 ? 1u : 0u); }

/* has the player's counter j reached its threshold (signed)? */
static int reached(vm *v, uint32_t P, uint32_t j)
{
    return (int32_t)R(0x0070CB40u + 4u * j) <= (int32_t)R(P + 0xD4u + 4u * j);
}

/* 0x0049D3BA: the thirteen flags at 0x70BF82 (an odd address, 4 apart) cleared,
 * then each set where the counter has reached its threshold */
static void f49D3BA(vm *v)
{
    uint32_t k, P;
    for (k = 0; k < 13; k++) W(0x0070BF82u + 4u * k, 0);
    P = cur_player(v);
    for (k = 0; k < 13; k++)
        if (reached(v, P, chk_word(k))) W(0x0070BF82u + 4u * k, 1);
}

/* 0x0049D5D9: [0x708358] the sum of the penalties (0x70CBB0 + 4j) of the
 * counters that reached their threshold and whose flag at 0x70BE82 + 4j is 0;
 * [0x70CBE8] = 100 - that; the player's +0xD0 = 2 if it is under 100 (signed),
 * else 1 */
static void f49D5D9(vm *v)
{
    uint32_t P = cur_player(v), k, sum = 0;
    for (k = 0; k < 13; k++) {
        uint32_t j = chk_word(k);
        if (reached(v, P, j) && R(0x0070BE82u + 4u * j) == 0)
            sum += R(0x0070CBB0u + 4u * j);
    }
    W(0x00708358, sum);
    W(0x0070CBE8, 100u - sum);
    W(cur_player(v) + 0xD0, (int32_t)(100u - sum) < 100 ? 2u : 1u);
}

/* 0x00486D12: the ten 0x9C-apart words at +0x40 of the stage record cleared
 * (0x00486CEA's twin, by the entry's +0x3C into [0x708048]) */
static void f486D12(vm *v)
{
    uint32_t e = R(0x006E7ECDu + 4u * R(0x0070AB54)) + R(0x0070AB58) * 0xA4u;
    uint32_t r = R(0x00708048) + R(e + 0x3C) * 0x618u;
    int k;
    for (k = 0; k < 10; k++, r += 0x9C) W(r + 0x40, 0);
}

/* 0x00482BCE: the first slot of the list at 0x70C0D2 (an odd address) that
 * reads -1 takes the pointer [0x70C19E + 4 * [0x7082FC]] */
static void f482BCE(vm *v)
{
    uint32_t p = 0x0070C0D2u;
    while (R(p) != 0xFFFFFFFFu) p += 4;
    W(p, R(0x0070C19Eu + 4u * R(0x007082FC)));
}

/* ---- batch 5 (2026-10-30): the draw layer, and the straight-line handlers over it ---- */

static const fel_draw *DRAW;

void fel_set_draw(const fel_draw *d) { DRAW = d; }

/* 0x00402311 / 0x0040239C as the handlers call them: the font record first */
static void tok(uint32_t font, uint32_t token, uint32_t x, uint32_t y)
{
    DRAW->tok(DRAW->ud, font, (int32_t)token, (int)x, (int)y);
}
static void txt(uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    DRAW->text(DRAW->ud, font, str, (int)x, (int)y);
}

enum { FONT_A = 0x005183CC, FONT_B = 0x005183EC };

/* 0x004767E2: VISUAL PREFERENCES' nine values, each a token (the player's word
 * + 0x56B, the last + 0x373) at x 0x1D1 down the column; [0x708358] is the
 * scratch the original builds each token in, so it ends holding the last */
static void f4767E2(vm *v)
{
    static const struct { uint16_t off, y; } ROW[] = {
        { 0x16C, 0xC8 }, { 0x170, 0xE5 }, { 0x174, 0x102 }, { 0x15C, 0x11F },
        { 0x168, 0x13C }, { 0x160, 0x159 }, { 0x164, 0x176 }, { 0x158, 0x193 },
        { 0x178, 0x1B0 },
    };
    uint32_t P = cur_player(v), k;
    for (k = 0; k < 9; k++) {
        W(0x00708358, R(P + ROW[k].off) + (k == 8 ? 0x373u : 0x56Bu));
        tok(FONT_B, R(0x00708358), 0x1D1, ROW[k].y);
    }
}

/* 0x0046A0B7: the slots' ten names (0x20 bytes each from 0x5206DC + [0x707BB0]
 * * 0x140): five down x 0xCF, five down x 0x21C, y 0xB5 on in steps of 0x1D */
static void f46A0B7(vm *v)
{
    uint32_t s = 0x005206DCu + R(0x00707BB0) * 0x140u, k;
    for (k = 0; k < 10; k++, s += 0x20)
        txt(FONT_B, s, k < 5 ? 0xCFu : 0x21Cu, 0xB5u + 0x1Du * (k % 5));
}

/* 0x0046C09C: the REPLAY slots' twenty names (0x20 bytes each from 0x0052245C + [0x707BBC] * 0x280):
 * ten down x 0xC6, then ten down x 0x213, y from 0xB5 in steps of 0x1B; the loops' globals [0x708384] / [0x708388]
 * / [0x708358] are the original's counters and end where it leaves them */
static void f46C09C(vm *v)
{
    uint32_t s = 0x0052245Cu + R(0x00707BBC) * 0x280u, col;
    for (col = 0; col < 2; col++) {
        W(0x00708384, col ? 0x213u : 0xC6u);
        W(0x00708388, 0xB5);
        W(0x00708358, 0);
        do {
            txt(FONT_B, s, R(0x00708384), R(0x00708388));
            s += 0x20;
            W(0x00708388, R(0x00708388) + 0x1B);
            W(0x00708358, R(0x00708358) + 1u);
        } while (R(0x00708358) != 0xA);
    }
}

/* 0x0046BD89: the settings slots' thirty names (0x20 bytes each from 0x5213A8 +
 * [0x707BB8] * 0x3C0), fifteen down x 0xE9 and fifteen down x 0x238, y from 0x44
 * in steps of 0x1D; the loops' globals [0x708384] / [0x708388] / [0x708358]
 * are the original's counters and end where it leaves them */
static void slot_names(vm *v)
{
    uint32_t s = 0x005213A8u + R(0x00707BB8) * 0x3C0u, col;
    for (col = 0; col < 2; col++) {
        W(0x00708384, col ? 0x238u : 0xE9u);
        W(0x00708388, 0x44);
        W(0x00708358, 0);
        do {
            txt(FONT_A, s, R(0x00708384), R(0x00708388));
            s += 0x20;
            W(0x00708388, R(0x00708388) + 0x1D);
            W(0x00708358, R(0x00708358) + 1u);
        } while (R(0x00708358) != 0xF);
    }
}

/* 0x0046BCDD / 0x0046BD33: SAVE / LOAD SETTINGS' slots - the title (token 0x4DC /
 * 0x4DD) and the two column heads, then the names */
static void slots_frame(vm *v, uint32_t title)
{
    tok(FONT_A, title, 0x190, 0xE);
    tok(FONT_A, 0x4E3, 0xE9, 0x1F7);
    tok(FONT_A, 0x4E4, 0x238, 0x1F7);
    slot_names(v);
}
static void f46BCDD(vm *v) { slots_frame(v, 0x4DC); }
static void f46BD33(vm *v) { slots_frame(v, 0x4DD); }

/* 0x0046C6CC: RETIRE's question; 0x00484F23: the lost level's line */
static void f46C6CC(vm *v) { (void)v; tok(FONT_A, 0x537, 0x197, 0x122); }
static void f484F23(vm *v) { (void)v; tok(FONT_A, 0x1FC, 0x19A, 0x118); }

/* 0x00482F5F: the current player's counters back to the start of a level:
 * the eight words 0xD4.. 0xFC, the six blocks 0x482D55 .. 0x482F26 clear, +0x278 = -1
 * and the eight words from +0x24C.  A block is a run of words and the one word
 * (+0xD8 .. +0x108) that heads it; the order of the stores does not matter,
 * none of them overlaps. */
static void zero_run(vm *v, uint32_t P, uint32_t from, uint32_t to)
{
    for (; from <= to; from += 4) W(P + from, 0);
}
static void f482F5F(vm *v)
{
    uint32_t P = cur_player(v), k;
    static const uint16_t HEAD[] = { 0xD4, 0xDC, 0xE0, 0xE4, 0xEC, 0xF0, 0xF4, 0xFC };
    for (k = 0; k < sizeof HEAD / sizeof HEAD[0]; k++) W(P + HEAD[k], 0);
    zero_run(v, P, 0x17C, 0x1A0); W(P + 0x104, 0);              /* 0x00482D55 */
    zero_run(v, P, 0x1A4, 0x1B8); W(P + 0x0D8, 0);              /* 0x00482DD4 */
    zero_run(v, P, 0x1BC, 0x1D4); W(P + 0x0E8, 0);              /* 0x00482E2B */
    zero_run(v, P, 0x1D8, 0x1E8); W(P + 0x0F8, 0);              /* 0x00482E8C */
    zero_run(v, P, 0x1F8, 0x208); W(P + 0x108, 0);              /* 0x00482ED9 */
    zero_run(v, P, 0x1EC, 0x1F4); W(P + 0x100, 0);              /* 0x00482F26 */
    W(P + 0x278, 0xFFFFFFFFu);
    zero_run(v, P, 0x24C, 0x268);
}

/* 0x0046D48A: the two lists of pointers (0x70C0D2, 0x70C19E; each ended by a -1)
 * copied by value - the dword each points at - to 0x707BCC / 0x707C6C, both
 * of forty words filled with -1 first */
static void list_values(vm *v, uint32_t list, uint32_t out)
{
    uint32_t k;
    for (k = 0; k < 0x28; k++) W(out + 4u * k, 0xFFFFFFFFu);
    for (; R(list) != 0xFFFFFFFFu; list += 4, out += 4) W(out, R(R(list)));
}

/* 0x00482BF1: the list at 0x70C19E closes up over the entry [0x7082FC] (every
 * later one a place down, the -1 that ended it too), then the values are
 * rebuilt (0x0046D48A) */
static void f482BF1(vm *v)
{
    uint32_t s = 0x0070C19Eu + 4u * R(0x007082FC), d = s + 4;
    do {
        W(s, R(d));
        d += 4;
        s += 4;
    } while (R(s) != 0xFFFFFFFFu);
    list_values(v, 0x0070C0D2u, 0x00707BCCu);
    list_values(v, 0x0070C19Eu, 0x00707C6Cu);
}

/* 0x00464A01: 31 bytes zeroed at EDI (the callers' pointer) */
static void zero31(vm *v, uint32_t at)
{
    uint32_t k;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, at + k, 0);
    W(0x00707ED9, 0);
}

/* 0x00464871: unless [0x707E40] is 1, the 31 bytes the current player's +0x18
 * points at go to 0x7081EC (0x0046496C) and are zeroed in place (0x00464A01) */
static void f464871(vm *v)
{
    if (R(0x00707E40) != 1) {
        uint32_t p = R(cur_player(v) + 0x18), k;
        for (k = 0; k < 0x1F; k++) vm_wr8(v, 0x007081ECu + k, vm_rd8(v, p + k));
        zero31(v, p);
        W(0x00708010, 1);
    }
    W(0x00707ED9, 0);
}

/* 0x0046BB2D: the slot the cursor ([0x707E91], 1 on, with the gap at 0x11 not
 * counted) is on in the settings' list at 0x5213A8 + [0x707BB8] * 0x3C0: its 31
 * bytes copied to 0x70820C (0x0046A229) and zeroed in place */
static void f46BB2D(vm *v)
{
    uint32_t cur = R(0x00707E91), n = cur - 1u, p, k;
    if ((int32_t)cur >= 0x11) n--;
    p = 0x005213A8u + R(0x00707BB8) * 0x3C0u + n * 0x20u;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, 0x0070820Cu + k, vm_rd8(v, p + k));
    zero31(v, p);
    W(0x00708010, 1);
    W(0x00707ED9, 0);
}

/* ---- batch 5, the points tables: 0x004A045F / 0x004A05A8 ----
 *
 * The standings are the 0x78-byte rows at [0x708058] (a do-while to the row
 * whose first dword is -1): [row] the id, [row+4] the player's number + 1 (0 is
 * an opponent), [row+0x10] the round the row is in.  A row's points go to
 * its owner: the 0x94-byte opponent record at 0x6EDFE8 with that id
 * (0x004A0674; the original jumps into 0x4D5DC9, a crash, when there is none -
 * the C stops there) or the player's record. */
static uint32_t rec_by_id(vm *v, uint32_t id)
{
    uint32_t e = 0x006EDFE8u;
    while (R(e) != id) {
        e += 0x94;
        if (R(e) == 0xFFFFFFFFu) return 0;
    }
    return e;
}

/* the row's six scores from `base`, the negative ones (-1: not run) left out */
static uint32_t row_sum(vm *v, uint32_t row, uint32_t base)
{
    uint32_t sum = 0, k;
    for (k = 0; k < 6; k++) {
        int32_t t = (int32_t)R(row + base + 4u * k);
        if (t >= 0) sum += (uint32_t)t;
    }
    return sum;
}

/* a row's owner: the opponent record or the player's, and the owner's word
 * the opponent's / the player's value lands in */
static uint32_t row_owner(vm *v, uint32_t row)
{
    if (R(row + 4) == 0) return rec_by_id(v, R(row));
    return R(A_PLAYERS + 4u * (R(row + 4) - 1u));
}

/* 0x004A045F: the rally's points per place into the standings.  For each round
 * ebx (0 .. [0x708260]-1, at least once) the rows of that round take the
 * place's points - [0x708318], from 20 down by one, never below 0 - into the
 * score [0x7082C4] of their six at +0x54, and the owner keeps it (+0x78 + 4 *
 * score for an opponent whose +0x24 is 1, +0x98 for a player whose +0xD0 is 1)
 * or the row's is reset to 0 along with the owner's.  Then every row's total
 * (the non-negative ones) goes to +0x6C, and the owner's +0x90 / +0xB0. */
static void f4A045F(vm *v)
{
    uint32_t b = 0, row, idx;
    do {
        W(0x00708318, 0x14);
        row = R(0x00708058);
        W(0x0070825C, b);
        do {
            if (R(row + 0x10) == b) {
                int32_t pts = (int32_t)R(0x00708318);
                uint32_t o, cell, keep;
                idx = R(0x007082C4);
                W(row + 4u * idx + 0x54, (uint32_t)pts);
                if (--pts >= 0) W(0x00708318, (uint32_t)pts);
                o = row_owner(v, row);
                if (!o) return;
                if (R(row + 4) == 0) { cell = o + 4u * idx + 0x78; keep = R(o + 0x24) == 1; }
                else                 { cell = o + 4u * idx + 0x98; keep = R(o + 0xD0) == 1; }
                if (keep) {
                    W(cell, R(row + 4u * idx + 0x54));
                } else {
                    W(row + 4u * idx + 0x54, 0);
                    W(cell, 0);
                }
            }
            row += 0x78;
        } while (R(row) != 0xFFFFFFFFu);
        b++;
    } while (b != R(0x00708260));
    row = R(0x00708058);
    do {
        uint32_t sum = row_sum(v, row, 0x54), o;
        W(row + 0x6C, sum);
        o = row_owner(v, row);
        if (!o) return;
        W(o + (R(row + 4) == 0 ? 0x90u : 0xB0u), sum);
        row += 0x78;
    } while (R(row) != 0xFFFFFFFFu);
}

/* 0x004A05A8: the championship's points per place: one pass, the rows whose
 * +0x10 is not 0, the scores at +0x38; the owner's cell is +0x5C + 4 * score
 * (an opponent) or +0xB4 + 4 * score (a player) whatever the flags say; the
 * totals go to +0x50 and the owner's +0x74 / +0xCC */
static void f4A05A8(vm *v)
{
    uint32_t row, idx;
    W(0x00708318, 0x14);
    row = R(0x00708058);
    do {
        if (R(row + 0x10) != 0) {
            int32_t pts = (int32_t)R(0x00708318);
            uint32_t o;
            idx = R(0x007082C4);
            W(row + 4u * idx + 0x38, (uint32_t)pts);
            if (--pts >= 0) W(0x00708318, (uint32_t)pts);
            o = row_owner(v, row);
            if (!o) return;
            W(o + 4u * idx + (R(row + 4) == 0 ? 0x5Cu : 0xB4u), R(row + 4u * idx + 0x38));
        }
        row += 0x78;
    } while (R(row) != 0xFFFFFFFFu);
    row = R(0x00708058);
    do {
        uint32_t sum = row_sum(v, row, 0x38), o;
        W(row + 0x50, sum);
        o = row_owner(v, row);
        if (!o) return;
        W(o + (R(row + 4) == 0 ? 0x74u : 0xCCu), sum);
        row += 0x78;
    } while (R(row) != 0xFFFFFFFFu);
}

/* 0x004A1683: the +0x44 of every row of the stage results ([0x708054], 0x9C
 * bytes a row, a do-while to the -1) cleared; 0x004A169E: the +0x70 of every
 * standings row */
static void f4A1683(vm *v)
{
    uint32_t row = R(0x00708054);
    do {
        W(row + 0x44, 0);
        row += 0x9C;
    } while (R(row) != 0xFFFFFFFFu);
}

static void f4A169E(vm *v)
{
    uint32_t row = R(0x00708058);
    do {
        W(row + 0x70, 0);
        row += 0x78;
    } while (R(row) != 0xFFFFFFFFu);
}

/* 0x004A1707: the standings' positions - the rows numbered 1, 2, ... in their
 * order into +0x70, and the owner player's +0x148.  When [0x70CAA8] is 2 only
 * the rows of a round (+0x10 not 0) are numbered, the others keep theirs. */
static void f4A1707(vm *v)
{
    uint32_t row = R(0x00708058), pos = 0;
    int rounds = R(0x0070CAA8) == 2;
    do {
        if (!rounds || R(row + 0x10) != 0) {
            pos++;
            if (R(row + 4) != 0) W(R(A_PLAYERS + 4u * (R(row + 4) - 1u)) + 0x148, pos);
            W(row + 0x70, pos);
        }
        row += 0x78;
    } while (R(row) != 0xFFFFFFFFu);
}

/* 0x0049DD17: the stage results' positions - 0x004A1707 over the 0x9C-byte
 * rows at [0x708054]: numbered into +0x44 and the owner's +0x150; with
 * [0x70CAA8] = 2 only the rows whose +0x1C is not 0.  The count is left in
 * [0x7082E8]. */
static void f49DD17(vm *v)
{
    uint32_t row = R(0x00708054), pos = 0;
    int rounds = R(0x0070CAA8) == 2;
    do {
        if (!rounds || R(row + 0x1C) != 0) {
            pos++;
            if (R(row + 4) != 0) W(R(A_PLAYERS + 4u * (R(row + 4) - 1u)) + 0x150, pos);
            W(row + 0x44, pos);
        }
        row += 0x9C;
    } while (R(row) != 0xFFFFFFFFu);
    W(0x007082E8, pos);
}

/* ----: var\ral.cfg's parser ---------------------------------------- */

/* 0x00411322: the line scan for the key at `key` in the text from `at`: the
 * address of the value's first character, or 0 (CF in the original).  The
 * text's characters are folded through the table at 0x004EBB9C before the
 * compare with the key's (which is not folded); a character that is not the
 * key's end is a different key and the scan goes on to the next line. */
static uint32_t cfg_find_key(vm *v, uint32_t key, uint32_t at)
{
    uint8_t al, ah;
    uint32_t k;
next:                                           /* 0x00411326: a line's first character */
    al = vm_rd8(v, at++);
    if (al == 0) return 0;
    if (al <= 0x20) goto next;
    k = key;                                    /* 0x00411340 */
    for (;;) {
        ah = vm_rd8(v, k++);
        al = vm_rd8(v, 0x004EBB9Cu + al);
        if (al != ah) break;
        al = vm_rd8(v, at++);
    }
    if (ah == 0) {                              /* the key ended: what follows it */
        if (al == 0x3D) goto value;
        if (al <= 0x20) {
            for (;;) {                          /* 0x00411365: blanks, then "=" */
                if (al == 0) return 0;
                if (al == 0x0A) goto found;
                if (al > 0x20) break;
                al = vm_rd8(v, at++);
            }
            if (al != 0x3D) goto found;
            goto value;
        }
    }
    for (;;) {                                  /* 0x00411333: to the line's end */
        if (al == 0) return 0;
        if (al == 0x0A) goto next;
        al = vm_rd8(v, at++);
    }
value:                                          /* 0x00411388: skip to the value */
    for (;;) {
        al = vm_rd8(v, at++);
        if (al == 0) return 0;
        if (al == 0x0A || al > 0x20) break;
    }
found:
    return at - 1;
}

/* 0x004113C8: the number at `p`: an optional sign, then "$" or "0x" for hex,
 * else decimal; stops at the first character that is not a digit; no overflow
 * check (it wraps) */
static uint32_t cfg_parse_int(vm *v, uint32_t p)
{
    uint32_t n = 0;
    int neg = 0;
    uint8_t b;
    if (p == 0) return 0;
    if (vm_rd8(v, p) == 0x2B) p++;
    else if (vm_rd8(v, p) == 0x2D) { neg = 1; p++; }
    if (vm_rd8(v, p) != 0x24) {
        if ((vm_rd8(v, p) | (vm_rd8(v, p + 1) << 8)) != 0x7830) {
            for (;;) {
                b = (uint8_t)(vm_rd8(v, p++) - 0x30);
                if (b >= 10) break;
                n = n * 10u + b;
            }
            return neg ? 0u - n : n;
        }
        p++;
    }
    p++;
    for (;;) {                                  /* 0x00411403 */
        uint8_t c = vm_rd8(v, p++);
        b = (uint8_t)(c - 0x30);
        if (c < 0x30) break;
        if (b >= 10) {
            b = (uint8_t)(b - 7);
            if (b < 10) break;
            if (b >= 0x10) {
                b = (uint8_t)(b - 0x20);
                if (b < 10) break;
                if (b >= 0x10) break;
            }
        }
        n = (n << 4) + b;
    }
    return neg ? 0u - n : n;
}

/* 0x004112A4 (PUSHAD ... POPAD: every register as it was): the keys of the
 * record table at 0x00512991 (the name, NUL, the dword that gets the value)
 * looked for in the text at `text` (ESI); the ones found are parsed into
 * their words, the others left. */
void fel_cfg_parse_keys(vm *v, uint32_t text)
{
    uint32_t e = 0x00512991u;
    do {
        uint32_t name = e, dest, at;
        while (vm_rd8(v, e++) != 0) {}
        dest = R(e);
        e += 4;
        at = cfg_find_key(v, name, text);
        if (at) W(dest, cfg_parse_int(v, at));
    } while (vm_rd8(v, e) != 0);
}

/* 0x00403071 (PUSHAD ... POPAD, then EAX = 0x008F8550): the template at `tmpl`
 * (EAX) copied to 0x008F8550, the LAST "!!" in it - else the last "00" - becomes
 * the two low decimal digits of `idx` (EBX; 0x00402011 with ECX = 0x12, which
 * writes the digits without their NUL).  The scans start at the last character
 * and never reach the first, as the original's.  Returns 0 when neither pair is
 * there: the original then sets 0x004F0274/78 and jumps to 0x00407383 (a fatal
 * path no caller's constant template can reach; the C does the two stores). */
int fel_numbered_path(vm *v, uint32_t tmpl, uint32_t idx)
{
    const uint32_t buf = 0x008F8550u;
    uint32_t n = 0, p, pair;
    uint8_t c;
    do { c = vm_rd8(v, tmpl + n); vm_wr8(v, buf + n, c); n++; } while (c);
    for (pair = 0x2121; pair; pair = pair == 0x2121 ? 0x3030 : 0) {
        for (p = buf + n - 2; ; p--) {
            if (((uint32_t)vm_rd8(v, p) | (uint32_t)vm_rd8(v, p + 1) << 8) == pair) {
                vm_wr8(v, p, (uint8_t)('0' + idx / 10 % 10));
                vm_wr8(v, p + 1, (uint8_t)('0' + idx % 10));
                return 1;
            }
            if (p <= buf + 1) break;
        }
    }
    W(0x004F0274u, 0x80F19001u);
    W(0x004F0278u, 0);
    return 0;
}

/* 0x004018A0: the game's generator.  [0x004DB13C] is stepped by 0x004018B4
 * (with EBX = 0): bit 0 takes bit 22, then bit 2 (as it then is), then bit 1,
 * and the word rotates right by one.  Returns the new state, which is also
 * stored back. */
uint32_t fel_rng_next(vm *v)
{
    uint32_t s = R(0x004DB13Cu), al = s & 0xFFu;
    al ^= (s & 0x400000u) != 0;
    al ^= (al & 4u) != 0;
    al ^= (al & 2u) != 0;
    s = (s & ~0xFFu) | al;
    s = (s >> 1) | (s << 31);
    W(0x004DB13Cu, s);
    return s;
}

/* 0x0040188E: the next state mod `n` (EAX); n = 0 is the original's #DE and
 * is not answered (returns 0 with the state untouched). */
int fel_rng_mod(vm *v, uint32_t n, uint32_t *out)
{
    if (n == 0) return 0;
    *out = fel_rng_next(v) % n;
    return 1;
}

/* 0x00402011 (PUSHAD ... POPAD, but the pushed EBX comes back advanced): the
 * unsigned `val` (EAX) as ten decimal digits, cut by `flags` (ECX) and put
 * at `dest` (EBX) without a NUL; returns the end.  The low nibble of the
 * flags is the width (the last w characters of the digits, padded to the left
 * by the '0's the original keeps just below its buffer), bit 0x10 keeps them
 * as they are, bit 0x20 turns the leading '0's into spaces (an all-zero
 * number keeps its last '0'), neither strips them (the last digit stays).
 * Widths past 15 would read the original's stale stack: not answered. */
uint32_t fel_fmt_num(vm *v, uint32_t val, uint32_t flags, uint32_t dest)
{
    char buf[16 + 11];                       /* 5 '0's below the digits, 10 digits, NUL */
    char *d = buf + 5 + 10, *s;
    uint32_t w = flags & 0xFu, i;
    memset(buf, '0', 5);
    for (i = 0; i < 10; i++) { buf[5 + 9 - i] = (char)('0' + val % 10); val /= 10; }
    buf[15] = 0;
    s = d - w;
    if (!(flags & 0x10u)) {
        if (flags & 0x20u) {
            char *e = s;
            while (*e == '0') *e++ = ' ';
            if (*e == 0 && e > buf) e[-1] = '0';
        } else {
            s = buf + 5;
            while (*s == '0') s++;
            if (*s == 0) s--;
        }
    }
    for (; *s; s++) vm_wr8(v, dest++, (uint8_t)*s);
    return dest;
}

/* ----: 0x00478937's best-times seeding ----------------------
 *
 * With no hst.dat read ([0x6F9F88] != 1) 0x00478937 fills the best-times
 * tables with made-up entries (0x004789C8..0x004789E6), and with [0x707EE9]
 * != 1 names the thirteen championship tables' empty rows (0x00478A8E).  Each
 * routine is PUSHAD ... POPAD.  A made-up entry is a random driver of the 0xAA
 * records at 0x006EDFE8 (0x94 each: +0 its name's index into the token table
 * at 0x006F47FC, 0x14 apart; +8, +0x14, +0x18 copied), its name written by
 * 0x00401F8F (`ts`, the frontend's token text).  The draws' order is the
 * original's: every 0x0040188E is fel_rng_mod in the same place. */
enum { BT_DRIVERS = 0x006EDFE8, BT_TOKENS = 0x006F47FC, BT_RALLIES = 0x006E7EE9 };

static fel_tokstr TS;
static void *TS_UD;

static uint32_t rmod(vm *v, uint32_t n)
{
    uint32_t r = 0;
    fel_rng_mod(v, n, &r);
    return r;
}

/* the driver record of draw `r`, and its name's token */
static uint32_t driver(uint32_t r) { return BT_DRIVERS + r * 0x94u; }
static void name_at(vm *v, uint32_t e, uint32_t dest)
{
    TS(TS_UD, v, (int32_t)R(BT_TOKENS + R(e) * 0x14u), dest);
}

/* 0x00478E1C (with_50) and 0x00478DD5: one row at `d` (EDI) */
static void row_e1c(vm *v, uint32_t d, int with_50)
{
    uint32_t e = driver(rmod(v, 0xAA));
    W(d + 0x1C, R(e + 8));
    W(d + 8, R(e + 0x14));
    if (with_50) W(d + 0x50, 0xB);
    W(d + 0x10, R(e + 0x18));
    W(d + 0xC, R(BT_TOKENS + R(e) * 0x14u));
    name_at(v, e, d + 0x20);
}

/* 0x00478E6A: one row at `d`, class `c` (ECX): class 4 draws from all 0xB3,
 * the others draw (and drop) that one, then [0x70CA68+4c] from [0x70CA78+4c] */
static void row_e6a(vm *v, uint32_t d, uint32_t c)
{
    uint32_t r = rmod(v, 0xB3), e;
    if (c != 4) r = rmod(v, R(0x0070CA68u + 4u * c)) + R(0x0070CA78u + 4u * c);
    e = driver(r);
    W(d + 0x10, R(e + 8));
    W(d + 8, R(e + 0x14));
    W(d + 0xC, R(e + 0x18));
    name_at(v, e, d + 0x18);
}

/* 0x00478BCE: five classes of ten at [0x70803C] (0x258 / 0x3C), +0x38 50..41 */
static void s478BCE(vm *v)
{
    uint32_t c, b, d;
    for (c = 0; c < 5; c++) {
        d = R(0x0070803C) + c * 0x258u;
        for (b = 0; b < 10; b++, d += 0x3C) {
            W(d + 0x38, 0x32u - b);
            row_e6a(v, d, c);
        }
    }
}

/* 0x00478C08: the rallies (0xA4 each from 0x6E7EE9, to a -1) whose type is 0 */
static uint32_t count_rallies(vm *v)
{
    uint32_t e = BT_RALLIES, n = 0;
    do { if (R(e) == 0) n++; e += 0xA4; } while (R(e) != 0xFFFFFFFFu);
    return n;
}

/* 0x00478C1E (`key` 0x24, [0x708040], the stages) and 0x00478CF1 (`key` 0x18,
 * [0x708050]): for the c-th type-0 rally (or the -1 that ends them, when
 * there are fewer) ten rows from its time + 24000, 0x9F apart.  Then the
 * running sum of the type-0 rallies' +0x18 + 0xB6 in [0x70831C], which a type
 * 3 or 4 rally (0x00478C1E only) seeds twenty rows at [0x6F560C + 4n] from
 * (0x00478DB2) and resets. */
static void s_rally_rows(vm *v, uint32_t key, uint32_t base, int seed_runs)
{
    uint32_t c = 0, e, k, b, t, d, x;
    W(0x00708318, count_rallies(v));
    do {
        for (e = BT_RALLIES, k = 0; ; ) {
            if (R(e) == 0) { if (c == k) break; k++; }
            e += 0xA4;
            if (R(e) == 0xFFFFFFFFu) break;
        }
        x = R(e + key) + 0x5DC0;
        d = R(base) + c * 0x618u;
        for (b = 1; b < 11; b++, x += 0x9F, d += 0x9C) {
            W(d + 0x14, x);
            row_e1c(v, d, 1);
        }
        c++;
    } while (c != R(0x00708318));
    c = 0;
    W(0x0070831C, 0);
    e = BT_RALLIES;
    do {
        t = R(e);
        if (t == 0) W(0x0070831C, R(0x0070831C) + R(e + 0x18) + 0xB6);
        else if (t == 3 || t == 4) {
            if (seed_runs) {                    /* 0x00478DB2 */
                d = R(0x006F560Cu + 4u * c);
                x = R(0x0070831C);
                for (b = 0; b < 20; b++, x += 0xB2, d += 0x9C) {
                    W(d + 0x18, x);
                    row_e1c(v, d, 1);
                }
            }
            c++;
            W(0x0070831C, 0);
        }
        e += 0xA4;
    } while (R(e) != 0xFFFFFFFFu);
}
static void s478C1E(vm *v) { s_rally_rows(v, 0x24, 0x00708040, 1); }
static void s478CF1(vm *v) { s_rally_rows(v, 0x18, 0x00708050, 0); }

/* 0x00478B8E: seven tables of ten at [0x708044] (0x320 / 0x50), +0x14 from
 * 100000 down by 0xAB3 */
static void s478B8E(vm *v)
{
    uint32_t c, b, d, x;
    for (c = 0; c < 7; c++) {
        d = R(0x00708044) + c * 0x320u;
        for (b = 0, x = 0x186A0; b < 10; b++, x -= 0xAB3, d += 0x50) {
            W(d + 0x14, x);
            row_e1c(v, d, 0);
        }
    }
}

/* 0x00478AAC ([0x708048]) and 0x00478B1D ([0x70804C]): the six stages of the
 * six championships at [0x6E7ECD + 4c], each ten rows (0x9C) of the table its
 * +0x3C picks (0x618 each) from its +0x18 + 24000, 0x8B apart */
static void s_champ_rows(vm *v, uint32_t base)
{
    uint32_t c, s, b, e, d, x;
    for (c = 0; c < 6; c++) {
        e = R(0x006E7ECDu + 4u * c);
        for (s = 0; s < 6; s++) {
            e += 0xA4;
            W(0x00708318, R(e + 0x18) + 0x5DC0);
            d = R(base) + R(e + 0x3C) * 0x618u;
            x = R(0x00708318);
            for (b = 0; b < 10; b++, x += 0x8B, d += 0x9C) {
                W(d + 0x14, x);
                row_e1c(v, d, 1);
            }
        }
    }
}
static void s478AAC(vm *v) { s_champ_rows(v, 0x00708048); }
static void s478B1D(vm *v) { s_champ_rows(v, 0x0070804C); }

/* 0x00478EC8: two random names and two random scores 1000..1169 for the row
 * at `d` */
static void name_ec8(vm *v, uint32_t d)
{
    uint32_t e = driver(rmod(v, 0xAA));
    W(d + 0x74, R(e + 0x14));
    W(d + 0x78, R(e + 0x18));
    name_at(v, e, d + 0x54);
    W(d + 0x30, rmod(v, 0xAA) + 0x3E8);
    e = driver(rmod(v, 0xAA));
    W(d + 0x9C, R(e + 0x14));
    W(d + 0xA0, R(e + 0x18));
    name_at(v, e, d + 0x7C);
    W(d + 0x2C, rmod(v, 0xAA) + 0x3E8);
}

/* 0x00478A8E (EDI the table): unless its first row is named (+0x54), every
 * type-0 row (0xA4 each, to a -1) gets 0x00478EC8's.  Returns ZF: 0 when the
 * table was already named, 1 after the walk. */
static int s478A8E(vm *v, uint32_t d)
{
    if (vm_rd8(v, d + 0x54) != 0) return 0;
    do {
        if (R(d) == 0) name_ec8(v, d);
        d += 0xA4;
    } while (R(d) != 0xFFFFFFFFu);
    return 1;
}

static const uint32_t SEED_VA[] = {
    0x00478BCE, 0x00478C1E, 0x00478CF1, 0x00478B8E, 0x00478AAC, 0x00478B1D, 0x00478A8E,
};

int fel_seed_count(void) { return (int)(sizeof SEED_VA / sizeof SEED_VA[0]); }
uint32_t fel_seed_va(int i) { return SEED_VA[i]; }

int fel_seed(vm *v, uint32_t va, uint32_t edi, fel_tokstr ts, void *ud)
{
    int zf = 1;
    TS = ts;
    TS_UD = ud;
    switch (va) {
    case 0x00478BCE: s478BCE(v); break;
    case 0x00478C1E: s478C1E(v); break;
    case 0x00478CF1: s478CF1(v); break;
    case 0x00478B8E: s478B8E(v); break;
    case 0x00478AAC: s478AAC(v); break;
    case 0x00478B1D: s478B1D(v); break;
    case 0x00478A8E: zf = s478A8E(v, edi); break;
    default: zf = -1; break;
    }
    TS = NULL;
    TS_UD = NULL;
    return zf;
}

/* ----: the rest of the boot, 0x00411013 and 0x00478937 --------------
 *
 * What the two entry points run around the pieces above, so that the boot
 * interprets nothing.  The platform services they reach (the file layer, the
 * page allocator, the user / machine names, the token text, the text measure,
 * the loading screen and the buttons) are the hooks frontend.c installs,
 * reached by vm_invoke with the registers the original's call has; a service
 * with no hook is a boot error (BOOT_ERR), never an interpreted fallback.
 * The original's fatal exits (0x00407383 with [0x4F0274] / [0x4F0278] set:
 * an allocation that failed, a ral.dat without its "####") store the same two
 * words and end the boot (fel_boot_* returns -1). */
static jmp_buf BOOT_JB;
static int BOOT_ERR;

/* a hooked service; returns its CF */
static int svc(vm *v, uint32_t va, uint32_t r[8])
{
    if (!vm_invoke(v, va, r)) {
        BOOT_ERR = (int)va;
        longjmp(BOOT_JB, 1);
    }
    return vm_cf(v);
}

static void fatal(vm *v, uint32_t code, uint32_t text)
{
    W(0x004F0274u, code);
    W(0x004F0278u, text);
    longjmp(BOOT_JB, 1);
}

static void svc_tokstr(void *ud, vm *v, int32_t tok, uint32_t dest)
{
    uint32_t r[8] = { 0 };
    (void)ud;
    r[VM_EAX] = (uint32_t)tok;
    r[VM_EBX] = dest;
    svc(v, 0x00401F8F, r);
}

/* 0x00401F8F then 0x00401F02: the token's text at `dest`, NUL-ended; returns
 * the NUL's address + 1 (EBX after 0x00401F02) */
static uint32_t tok_nul(vm *v, uint32_t tok, uint32_t dest)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = tok;
    r[VM_EBX] = dest;
    svc(v, 0x00401F8F, r);
    vm_wr8(v, r[VM_EBX], 0);
    return r[VM_EBX] + 1;
}

static void copy_bytes(vm *v, uint32_t dst, uint32_t src, uint32_t n)
{
    while (n--) vm_wr8(v, dst++, vm_rd8(v, src++));
}

/* 0x0040299B: the file named at `path` read into `buf`, at most `max` bytes
 * from `off`.  Returns CF; *got is the count when the read was made (the
 * original's EAX, else its EAX is left). */
static int read_file(vm *v, uint32_t path, uint32_t buf, uint32_t max, uint32_t off, uint32_t *got)
{
    uint32_t r[8] = { 0 }, h;
    int ok;
    r[VM_EAX] = path; r[VM_EBX] = 0; r[VM_ECX] = max; r[VM_EDX] = off;
    if (svc(v, 0x00402BCD, r)) return 1;
    h = r[VM_EAX];
    r[VM_ECX] = off; r[VM_EDX] = h;
    if (off) {
        r[VM_EBX] = 0;
        if (svc(v, 0x00402C5A, r)) { ok = 0; goto close; }
        r[VM_EAX] = h;
    }
    r[VM_EBX] = buf; r[VM_ECX] = max;
    ok = !svc(v, 0x00402C22, r);
    *got = r[VM_EAX];
close:
    r[VM_EAX] = h;
    if (svc(v, 0x00402C08, r)) return 1;
    return !ok;
}

/* 0x00402844: `size` bytes on the block list at `list` (0x40 of header: the
 * next block, the size); the running totals at 0x4DB57C / 0x4DB578 and the
 * peak at 0x4DB574.  Returns CF, *at the block. */
static int alloc_block(vm *v, uint32_t size, uint32_t list, uint32_t *at)
{
    uint32_t r[8] = { 0 }, a, t;
    r[VM_EAX] = size + 0x40; r[VM_ECX] = size; r[VM_EBX] = list;
    if (svc(v, 0x004028FF, r)) { *at = r[VM_EAX]; return 1; }
    a = r[VM_EAX];
    W(a, R(list));
    W(list, a);
    W(a + 4, size);
    W(list + 4, R(list + 4) + size);
    W(0x004DB57C, R(0x004DB57C) + size);
    t = size + R(0x004DB578);
    W(0x004DB578, t);
    if (t > R(0x004DB574)) W(0x004DB574, t);
    *at = a + 0x40;
    return 0;
}

/* 0x004787D0 (once: [0x6F9F84]): the best-times blocks on the list at
 * 0x6F9F44, each one's size stored before it is asked for; two of them end in
 * a -1, two are 0x618 per type-0 rally */
static void alloc_tables(vm *v)
{
    static const struct { uint32_t size, ptr, fail; int end, per_rally; } T[] = {
        { 0x1C20,  0x006F9F8C, 0x807010F1, 0, 0 }, { 0xBB8,   0x006F9F94, 0x807010FB, 0, 0 },
        { 0,       0x006F9F9C, 0x80701000, 0, 1 }, { 0x15E0,  0x006F9FA4, 0x80701001, 0, 0 },
        { 0xDB60,  0x006F9FAC, 0x80701002, 0, 0 }, { 0x4C2C4, 0x006F9FB4, 0x80701003, 1, 0 },
        { 0x3A984, 0x006F9FBC, 0x8070100A, 1, 0 }, { 0xDB60,  0x006F9FC4, 0x80701004, 0, 0 },
        { 0,       0x006F9FCC, 0x80701005, 0, 1 },
    };
    size_t i;
    if (R(0x006F9F84) == 1) return;
    W(0x006F9F84, 1);
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        uint32_t size = T[i].per_rally ? count_rallies(v) * 0x618u : T[i].size, at;
        W(T[i].ptr + 4, size);
        if (alloc_block(v, size, 0x006F9F44, &at)) fatal(v, T[i].fail, 0);
        W(T[i].ptr, at);
        if (T[i].end) W(at + size - 4, 0xFFFFFFFFu);
    }
}

/* 0x0046D4F1: the two id lists (0x707BCC, 0x707C6C, to a -1) as their records'
 * pointers (0x708E0C) at 0x70C0D2 / 0x70C19E, after 30 and 20 -1s */
static void id_lists(vm *v)
{
    uint32_t i, p, q;
    for (i = 0; i < 0x1E; i++) W(0x0070C0D2 + 4 * i, 0xFFFFFFFFu);
    for (i = 0; i < 0x14; i++) W(0x0070C19E + 4 * i, 0xFFFFFFFFu);
    for (p = 0x00707BCC, q = 0x0070C0D2; R(p) != 0xFFFFFFFFu; p += 4, q += 4)
        W(q, R(0x00708E0C + 4 * R(p)));
    for (p = 0x00707C6C, q = 0x0070C19E; R(p) != 0xFFFFFFFFu; p += 4, q += 4)
        W(q, R(0x00708E0C + 4 * R(p)));
}

/* 0x004A6696: var\ral.dat (0x523100) into 0x8E8550, "R5CI" version 0x50004,
 * then its fields from +0x10 in this order (a length of 4 a dword); the
 * "####" after them, else fatal.  [0x707EE9] = 1 once read. */
static void read_ral_dat(vm *v)
{
    static const uint32_t F[][2] = {
        {0x512938,4},{0x4EF918,4},{0x4EF91C,4},{0x4F00A4,4},{0x4F3284,4},{0x4F3288,4},
        {0x4F3298,4},{0x511E80,4},{0x511E84,4},{0x511E88,4},{0x511E8C,4},{0x511E90,4},
        {0x5242B8,0x40},{0x5242F8,4},{0x52430C,0x40},{0x52434C,4},{0x5243BC,0x10},
        {0x5186D8,4},{0x52135C,4},{0x522028,4},{0x5744CC,0x20},{0x5744EC,4},
        {0x4F16AD,4},{0x4F16B1,4},{0x4F16B5,4},{0x4F16B9,4},
        {0x7083B8,0x28C},{0x708644,0x28C},{0x7088D0,0x28C},{0x708B5C,0x28C},
        {0x707FEC,4},{0x707FF0,4},{0x707FF4,4},{0x707FF8,4},{0x707FFC,4},{0x708000,4},
        {0x708004,4},{0x708008,4},{0x707FA8,0x2C},{0x707FD4,0x18},{0x707BC8,4},
        {0x707E34,4},{0x70CC34,4},{0x70AB68,6},{0x707BCC,0xA0},{0x707C6C,0x50},
        {0x6E7EE9,0x2A48},{0x6EBFB5,0x47C},{0x6EBB35,0x47C},{0x6EB6B5,0x47C},
        {0x6EB235,0x47C},{0x6EADB5,0x47C},{0x6EA935,0x47C},{0x6EC435,0x47C},
        {0x6EC8B5,0x47C},{0x6ECD35,0x47C},{0x6ED1B5,0x47C},{0x6ED635,0x47C},
        {0x6EDAB5,0x47C},{0x6F5624,0xC30},{0x6F6254,0xC30},{0x6F6E84,0xC30},
        {0x6F7AB4,0xC30},{0x6F86E4,0xC30},{0x6F9314,0xC30},{0x7080EC,0x20},
        {0x70810C,0x20},{0x70812C,0x20},{0x70814C,0x20},
    };
    const uint32_t buf = 0x008E8550u;
    uint32_t got, s = buf + 0x10;
    size_t i;
    W(0x00707EE9, 0);
    if (read_file(v, 0x00523100, buf, 0x10000, 0, &got)) return;
    if (R(buf) != 0x49433552u || R(buf + 8) != 0x50004u) return;
    for (i = 0; i < sizeof F / sizeof F[0]; i++) {
        copy_bytes(v, F[i][0], s, F[i][1]);
        s += F[i][1];
    }
    if (R(s) != 0x23232323u) fatal(v, 0x80ACFF01u, 0x004DAB9Bu);
    id_lists(v);
    W(0x00707EE9, 1);
}

/* 0x004A6D80: a read of `n` into `at` from the open hst.dat; a short or
 * failed read closes it and fails */
static int hst_part(vm *v, uint32_t h, uint32_t at, uint32_t n)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = h; r[VM_EBX] = at; r[VM_ECX] = n; r[VM_EDX] = h;
    if (!svc(v, 0x00402C22, r) && r[VM_EAX] == r[VM_ECX]) return 0;
    r[VM_EAX] = h;
    svc(v, 0x00402C08, r);
    return 1;
}

/* 0x004A6CB9: the tables (0x004787D0), then var\hst.dat (0x52310c): a 0x10
 * header "R5HS" version 0x50004 (a wrong one leaves the file open, as the
 * original), the six blocks whole; [0x6F9F88] = 1 once read */
static void read_hst_dat(vm *v)
{
    static const uint32_t B[] = { 0x006F9F94, 0x006F9F9C, 0x006F9FA4, 0x006F9FAC, 0x006F9FC4, 0x006F9FCC };
    uint32_t r[8] = { 0 }, h;
    size_t i;
    alloc_tables(v);
    r[VM_EAX] = 0x0052310C; r[VM_EBX] = 0;
    if (svc(v, 0x00402BCD, r)) return;
    h = r[VM_EAX];
    if (hst_part(v, h, 0x008E8550, 0x10)) return;
    if (R(0x008E8550) != 0x53483552u || R(0x008E8558) != 0x50004u) return;
    for (i = 0; i < 6; i++)
        if (hst_part(v, h, R(B[i]), R(B[i] + 4))) return;
    r[VM_EAX] = h;
    svc(v, 0x00402C08, r);
    W(0x006F9F88, 1);
}

/* 0x004017DE: at most n - 1 characters of `src` to `dst` (n = 0: unbounded),
 * the control characters (low 7 bits under 0x20) dropped, folded through
 * 0x004EBB9C unless [0x4DB140] = 1; NUL-ended */
static void copy_name(vm *v, uint32_t src, uint32_t dst, uint32_t n)
{
    if (src) {
        while (--n) {
            uint8_t c = vm_rd8(v, src++);
            if (!c) break;
            if ((c & 0x7Fu) < 0x20) continue;
            if (R(0x004DB140) != 1) c = vm_rd8(v, 0x004EBB9Cu + c);
            vm_wr8(v, dst++, c);
        }
    }
    vm_wr8(v, dst, 0);
}

/* the three 100-file probes (0x004A7041, 0x0041DEB8, 0x004A6E82): file `idx`
 * of the template read into `buf`; with the magic and the version, its 0x20
 * name bytes (at buf + 0x10) go to the idx-th slot of `names` */
static void probe(vm *v, uint32_t tmpl, uint32_t buf, uint32_t max, uint32_t magic,
                  uint32_t ver, uint32_t names, uint32_t idx)
{
    uint32_t got;
    fel_numbered_path(v, tmpl, idx);
    if (read_file(v, 0x008F8550, buf, max, 0, &got)) return;
    if (R(buf) != magic || R(buf + 8) != ver) return;
    copy_bytes(v, names + idx * 0x20, buf + 0x10, 0x20);
}

/* the default names of the slots no file filled: token, then the number */
static void name_slots(vm *v, uint32_t names, uint32_t tok)
{
    uint32_t i, r[8] = { 0 };
    for (i = 0; i < 100; i++) {
        uint32_t at = names + i * 0x20;
        if (vm_rd8(v, at)) continue;
        r[VM_EAX] = tok; r[VM_EBX] = at;
        svc(v, 0x00401F8F, r);
        vm_wr8(v, fel_fmt_num(v, i, 0x12, r[VM_EBX]), 0);
    }
}

/* a name from 0x00410642 / 0x00410630 (the user, the machine), else the token */
static void host_name(vm *v, uint32_t svcva, uint32_t at, uint32_t n, uint32_t tok)
{
    uint32_t r[8] = { 0 };
    if (vm_rd8(v, at)) return;
    r[VM_EBX] = at;
    svc(v, svcva, r);
    copy_name(v, r[VM_EBX], r[VM_EBX], n);
    if (!vm_rd8(v, r[VM_EBX])) tok_nul(v, tok, r[VM_EBX]);
}

int fel_boot_411013(vm *v)
{
    uint32_t i;
    BOOT_ERR = 0;
    if (setjmp(BOOT_JB)) return -1;
    read_ral_dat(v);
    read_hst_dat(v);
    {                                           /* 0x0041124B: var\ral.cfg */
        uint32_t got;
        if (!read_file(v, 0x005230F4, 0x008E8550, 0xFFFF, 0, &got)) {
            vm_wr8(v, 0x008E8550u + got, 0);
            fel_cfg_parse_keys(v, 0x008E8550);
        }
    }
    W(0x00512938, R(0x00512938) + 1);
    host_name(v, 0x00410642, 0x007080EC, 0x1F, 0x18C);
    for (i = 1; i < 4; i++)
        if (!vm_rd8(v, 0x007080ECu + 0x20 * i)) tok_nul(v, 0x18C + i, 0x007080ECu + 0x20 * i);
    host_name(v, 0x00410630, 0x005242B8, 0x40, 0x14D);
    if (!vm_rd8(v, 0x005744CC)) {               /* a random driver, not "andr..." */
        uint32_t r[8] = { 0 }, n;
        do {
            fel_rng_mod(v, 0xB4, &n);
            r[VM_EAX] = n + 0x5A5; r[VM_EBX] = 0x005744CC;
            svc(v, 0x00401F8F, r);
        } while (R(0x005744CC) == 0x72646E61u);
        vm_wr8(v, r[VM_EBX], 0);
        W(0x005744EC, 0x5208);
    }
    for (i = 0; i < 100; i++) probe(v, 0x00523223, 0x005186DC, 0x8000, 0x47533552, 0x51122, 0x005206DC, i);
    name_slots(v, 0x005206DC, 0x4E0);
    for (i = 0; i < 100; i++) probe(v, 0x00523273, 0x0052202C, 0x430, 0x4C504552, 0x50400, 0x0052245C, i);
    name_slots(v, 0x0052245C, 0x4E2);
    for (i = 0; i < 100; i++) probe(v, 0x0052323E, 0x00521360, 0x48, 0x55535243, 0x50000, 0x005213A8, i);
    name_slots(v, 0x005213A8, 0x4E1);
    return 0;
}

/* 0x00411219: the name at `at` cut from the end until 0x004022D2 measures it
 * at most 0x15E in the font 0x51834C */
static void fit_name(vm *v, uint32_t at)
{
    uint32_t e = at, r[8] = { 0 };
    while (vm_rd8(v, e)) e++;
    for (;;) {
        vm_wr8(v, e, 0);
        r[VM_EAX] = 0x0051834C; r[VM_EBX] = at; r[VM_EDX] = e;
        svc(v, 0x004022D2, r);
        if (r[VM_ECX] <= 0x15E) break;
        e--;
    }
}

int fel_boot_478937(vm *v)
{
    static const uint32_t TABLES[13] = {
        0x006E7EE9, 0x006EBFB5, 0x006EBB35, 0x006EB6B5, 0x006EB235, 0x006EADB5, 0x006EA935,
        0x006EC435, 0x006EC8B5, 0x006ECD35, 0x006ED1B5, 0x006ED635, 0x006EDAB5,
    };
    static const uint32_t ALIAS[][2] = {           /* the blocks' working pointers */
        {0x6F9F8C,0x70805C},{0x6F9F94,0x70803C},{0x6F9F9C,0x708040},{0x6F9FA4,0x708044},
        {0x6F9FAC,0x708048},{0x6F9FB4,0x708054},{0x6F9FBC,0x708058},{0x6F9FC4,0x70804C},
        {0x6F9FCC,0x708050},
    };
    uint32_t i, r[8] = { 0 };
    BOOT_ERR = 0;
    if (setjmp(BOOT_JB)) return -1;
    for (i = 0; i < 4; i++) {                   /* 0x0046D203: the players' names */
        uint32_t P = R(A_PLAYERS + 4 * i), name = 0x007080ECu + 0x20 * i;
        W(P + 0x18, name);
        copy_bytes(v, P + 0x124, name, 0x20);
    }
    svc(v, 0x00478F6B, r);                      /* the loading screen */
    svc(v, 0x00478799, r);                      /* the buttons */
    for (i = 0; i < 4; i++) fit_name(v, 0x007080ECu + 0x20 * i);   /* 0x004111EA */
    W(0x00511E8C, R(0x00707FF0));
    W(0x00511E90, R(0x00707FF4));
    alloc_tables(v);
    for (i = 0; i < sizeof ALIAS / sizeof ALIAS[0]; i++) W(ALIAS[i][1], R(ALIAS[i][0]));
    if (R(0x006F9F88) != 1) {
        for (i = 0; i < 6; i++) fel_seed(v, SEED_VA[i], 0, svc_tokstr, NULL);
        W(0x006F9F88, 1);
    }
    if (R(0x00707EE9) != 1)
        for (i = 0; i < 13; i++) fel_seed(v, 0x00478A8E, TABLES[i], svc_tokstr, NULL);
    return 0;
}

int fel_boot_error(void) { return BOOT_ERR; }

/* ---- batch 7 (2026-11-07): the handlers that end in a platform service ---- */

static uint32_t fe_svcr(uint32_t va, uint32_t eax, uint32_t ebx, uint32_t ecx,
                        uint32_t edx, uint32_t ebp)
{
    return DRAW->svc(DRAW->ud, va, eax, ebx, ecx, edx, ebp);
}
static uint32_t fe_svc(uint32_t va, uint32_t eax, uint32_t ecx)
{
    return fe_svcr(va, eax, 0, ecx, 0, 0);
}

/* 0x00401F8F then 0x00401F02: the token's text at `dest`, then its NUL */
static void tok_at(vm *v, uint32_t token, uint32_t dest)
{
    uint32_t n = fe_svcr(0x00401F8F, token, dest, 0, 0, 0);
    vm_wr8(v, dest + n, 0);
}

/* `inc [a]` then `cmp [a], hi; jle` (signed) or `dec [a]` then `cmp [a], 0; jge`:
 * one step of a volume word, held in 0..hi; returns the word */
static uint32_t vol_step(vm *v, uint32_t a, int up, uint32_t hi)
{
    uint32_t x = R(a) + (up ? 1u : 0xFFFFFFFFu);
    if (up) { if (!((int32_t)x <= (int32_t)hi)) x = hi; }
    else    { if (!((int32_t)x >= 0)) x = 0; }
    W(a, x);
    return x;
}

/* 0x00464B42 / 0x00464B66: the overall volume [0x707FEC] */
static void f464B42(vm *v) { fe_svc(0x00409A8B, vol_step(v, 0x00707FEC, 0, 0x40), 0); }
static void f464B66(vm *v) { fe_svc(0x00409A8B, vol_step(v, 0x00707FEC, 1, 0x40), 0); }

/* 0x00464B8A / 0x00464BB3: the music volume [0x707FF0], copied to [0x511E8C] */
static void f464B8A(vm *v)
{
    uint32_t x = vol_step(v, 0x00707FF0, 0, 0x40);
    W(0x00511E8C, x);
    fe_svc(0x0040F651, x, 0);
}
static void f464BB3(vm *v)
{
    uint32_t x = vol_step(v, 0x00707FF0, 1, 0x40);
    W(0x00511E8C, x);
    fe_svc(0x0040F651, x, 0);
}

/* 0x00464C24 / 0x00464C48: [0x707FFC], sound request 0x80 */
static void f464C24(vm *v) { vol_step(v, 0x00707FFC, 0, 0x40); fe_svc(0x0040EE7C, 0x80, 0); }
static void f464C48(vm *v) { vol_step(v, 0x00707FFC, 1, 0x40); fe_svc(0x0040EE7C, 0x80, 0); }

/* 0x00464C6C / 0x00464C97: [0x708000], the narration at 0x5045EB at that volume */
static void f464C6C(vm *v) { fe_svc(0x0040F566, 0x005045EB, vol_step(v, 0x00708000, 0, 0x40)); }
static void f464C97(vm *v) { fe_svc(0x0040F566, 0x005045EB, vol_step(v, 0x00708000, 1, 0x40)); }

/* 0x00464CC2 .. 0x00464D4C: [0x708004] and [0x708008], each copied to [0x707E54]
 * before sound request 0x10 / 0x20 / 0x40 / 0x40 */
static void vol_req(vm *v, uint32_t a, int up, uint32_t req)
{
    W(0x00707E54, vol_step(v, a, up, 0x40));
    fe_svc(0x0040EE7C, req, 0);
}
static void f464CC2(vm *v) { vol_req(v, 0x00708004, 0, 0x10); }
static void f464CF0(vm *v) { vol_req(v, 0x00708004, 1, 0x20); }
static void f464D1E(vm *v) { vol_req(v, 0x00708008, 0, 0x40); }
static void f464D4C(vm *v) { vol_req(v, 0x00708008, 1, 0x40); }

/* 0x00464D7A / 0x00464D9E: [0x707FF8], sound request 1 */
static void f464D7A(vm *v) { vol_step(v, 0x00707FF8, 0, 0x40); fe_svc(0x0040EE7C, 1, 0); }
static void f464D9E(vm *v) { vol_step(v, 0x00707FF8, 1, 0x40); fe_svc(0x0040EE7C, 1, 0); }

/* 0x00467D2F .. 0x00467E58: the controller screen's row k waits for a key:
 * [0x707B64] = 1, [0x707B68] = the clock, [0x707B6C] = k (PUSHAD ... POPAD) */
static void wait_row(vm *v, uint32_t k)
{
    W(0x00707B64, 1);
    W(0x00707B68, fe_svc(0x00410E0C, 0, 0));
    W(0x00707B6C, k);
}
static void f467D2F(vm *v) { wait_row(v, 1); }
static void f467D50(vm *v) { wait_row(v, 2); }
static void f467D71(vm *v) { wait_row(v, 3); }
static void f467D92(vm *v) { wait_row(v, 4); }
static void f467DB3(vm *v) { wait_row(v, 5); }
static void f467DD4(vm *v) { wait_row(v, 6); }
static void f467DF5(vm *v) { wait_row(v, 7); }
static void f467E16(vm *v) { wait_row(v, 8); }
static void f467E37(vm *v) { wait_row(v, 9); }
static void f467E58(vm *v) { wait_row(v, 10); }

/* SELECT STAGE's arrows (0x0047E041 ..): step [0x708230] (the rally set) /
 * [0x708238] (the rally, 0..5) / [0x708234] (the stage, 1..6), then the map
 * 0x00465011 */
static void f47E041(vm *v) { W(0x00708230, R(0x00708230) ^ 1u); fe_svc(0x00465011, 0, 0); }
static void f47E04E(vm *v)
{
    W(0x00708238, R(0x00708238) >= 5u ? 0u : R(0x00708238) + 1u);
    fe_svc(0x00465011, 0, 0);
}
static void f47E06F(vm *v)
{
    W(0x00708238, (int32_t)R(0x00708238) < 1 ? 5u : R(0x00708238) - 1u);
    fe_svc(0x00465011, 0, 0);
}
static void f47E090(vm *v)
{
    W(0x00708234, R(0x00708234) >= 6u ? 1u : R(0x00708234) + 1u);
    fe_svc(0x00465011, 0, 0);
}
static void f47E0B1(vm *v)
{
    W(0x00708234, (int32_t)R(0x00708234) < 2 ? 6u : R(0x00708234) - 1u);
    fe_svc(0x00465011, 0, 0);
}

/* 0x00466544: [0x707E5C] flips; its text is token 0x21F, or 0x221 at 0, at 0xA2C354 */
static void f466544(vm *v)
{
    uint32_t x = R(0x00707E5C) ^ 1u;
    W(0x00707E5C, x);
    tok_at(v, x == 0 ? 0x221u : 0x21Fu, 0x00A2C354);
}

/* 0x00466576 / 0x004665C2: [0x707E5C] steps round 0..2, is the current player's
 * +0x30, and its text is token 0x21F + it, at 0xA2C354 */
static void round3(vm *v, uint32_t x)
{
    W(0x00707E5C, x);
    W(cur_player(v) + 0x30, x);
    tok_at(v, x + 0x21Fu, 0x00A2C354);
}
static void f466576(vm *v)
{
    uint32_t x = R(0x00707E5C) + 1u;
    round3(v, (int32_t)x < 3 ? x : 0u);
}
static void f4665C2(vm *v)
{
    uint32_t x = R(0x00707E5C) - 1u;
    round3(v, (int32_t)x >= 0 ? x : 2u);
}

/* 0x0046D1D0: [0x70CA98] flips; its text is token 0x4FC + it, at 0xA2C254 */
static void f46D1D0(vm *v)
{
    uint32_t x = R(0x0070CA98) ^ 1u;
    W(0x0070CA98, x);
    tok_at(v, 0x4FCu + x, 0x00A2C254);
}

/* 0x004680E6: the current player's device, configured: 0x00409250 (a no-op in
 * the host - DirectInput's) with EAX = +0x2C - 1, EBX = +0x34, ECX = +0x38,
 * EDX = +0x44, EBP = 0 (PUSHAD ... POPAD: no register changes) */
static void dev_set(vm *v)
{
    uint32_t P = cur_player(v);
    fe_svcr(0x00409250, R(P + 0x2C) - 1u, R(P + 0x34), R(P + 0x38), R(P + 0x44), 0);
}

/* 0x00467128 / 0x00467174: the controller screen's device arrows: the current
 * player's +0x2C steps round 0..[0x4F28A0]; 0 releases the device (0x00468114 ->
 * 0x00409285, EAX the new value), any other value configures it */
static void dev_pick(vm *v, uint32_t x)
{
    W(cur_player(v) + 0x2C, x);
    if (x == 0) fe_svcr(0x00409285, x, 0, 0, 0, 0);
    else dev_set(v);
}
static void f467128(vm *v)
{
    uint32_t x = R(cur_player(v) + 0x2C);
    dev_pick(v, x == R(0x004F28A0) ? 0u : x + 1u);
}
static void f467174(vm *v)
{
    uint32_t x = R(cur_player(v) + 0x2C);
    dev_pick(v, (int32_t)x > 0 ? x - 1u : R(0x004F28A0));
}

/* 0x0046807E: with a device chosen (+0x2C not 0) and the player's +0x34 above 0
 * (signed), one less, and the device configured again */
static void f46807E(vm *v)
{
    uint32_t P = cur_player(v);
    if (R(P + 0x2C) != 0 && (int32_t)R(P + 0x34) > 0) {
        W(P + 0x34, R(P + 0x34) - 1u);
        dev_set(v);
    }
}

/* the handlers that draw: answered only while a draw layer is installed */
static const uint32_t DRAWERS[] = {
    0x004767E2, 0x0046A0B7, 0x0046BCDD, 0x0046BD33, 0x0046C6CC, 0x00484F23, 0x0046C09C,
    0x00464B42, 0x00464B66, 0x00464B8A, 0x00464BB3, 0x00464C24, 0x00464C48,
    0x00464C6C, 0x00464C97, 0x00464CC2, 0x00464CF0, 0x00464D1E, 0x00464D4C,
    0x00464D7A, 0x00464D9E, 0x00467D2F, 0x00467D50, 0x00467D71, 0x00467D92,
    0x00467DB3, 0x00467DD4, 0x00467DF5, 0x00467E16, 0x00467E37, 0x00467E58,
    0x0047E041, 0x0047E04E, 0x0047E06F, 0x0047E090, 0x0047E0B1,
    0x00466544, 0x00466576, 0x004665C2, 0x0046D1D0,
    0x00467128, 0x00467174, 0x0046807E,
};

static int uses_draw(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof DRAWERS / sizeof DRAWERS[0]; i++)
        if (DRAWERS[i] == va) return 1;
    return 0;
}

/* ---- batch 6 (2026-11-07): the options screens' arrows and toggles ----
 *
 * The item handlers of the tables (frontend.py items): each is a few
 * instructions over the settings words, read from the image with
 * tools/ralimg.py disasm.  None reads a register it did not set. */

/* a counter that wraps: up from `top` to 0 (inc, cmp, jne), down from 0 to `top` */
static void wrap_up(vm *v, uint32_t a, uint32_t top)
{
    W(a, R(a) == top ? 0u : R(a) + 1u);
}
static void wrap_dn(vm *v, uint32_t a, uint32_t top)
{
    W(a, R(a) == 0 ? top : R(a) - 1u);
}
static void flip(vm *v, uint32_t a) { W(a, R(a) ^ 1u); }

/* the current player's words 0x4, 0xC and 0x10: the three selectors (0..0x27, 0..6, 0..5) */
static void f464721(vm *v) { wrap_up(v, cur_player(v) + 0x04, 0x27); }
static void f464745(vm *v) { wrap_dn(v, cur_player(v) + 0x04, 0x27); }
static void f464769(vm *v) { wrap_up(v, cur_player(v) + 0x0C, 6); }
static void f46478D(vm *v) { wrap_dn(v, cur_player(v) + 0x0C, 6); }
static void f4647B1(vm *v) { wrap_up(v, cur_player(v) + 0x10, 5); }
static void f4647D5(vm *v) { wrap_dn(v, cur_player(v) + 0x10, 5); }

/* VISUAL PREFERENCES' on / off rows (0x464A30, ported in batch 1, is the rear-view mirror's) and
 * 0x0046D151's +0x3C: the current player's word, flipped */
static void f464813(vm *v) { flip(v, cur_player(v) + 0x154); }
static void f464A1A(vm *v) { flip(v, cur_player(v) + 0x158); }
static void f464A46(vm *v) { flip(v, cur_player(v) + 0x160); }
static void f464A5C(vm *v) { flip(v, cur_player(v) + 0x164); }
static void f464A72(vm *v) { flip(v, cur_player(v) + 0x168); }
static void f464A88(vm *v) { flip(v, cur_player(v) + 0x16C); }
static void f464A9E(vm *v) { flip(v, cur_player(v) + 0x170); }
static void f464AB4(vm *v) { flip(v, cur_player(v) + 0x174); }
static void f464ACA(vm *v) { flip(v, cur_player(v) + 0x178); }
static void f46D151(vm *v) { flip(v, cur_player(v) + 0x3C); }

/* 0x00464B30: a flag flipped and copied to [0x70CC68] */
static void f464B30(vm *v) { flip(v, 0x007082E4); W(0x0070CC68, R(0x007082E4)); }

/* 0x00464BDC / 0x00464C00: a value stepped down to 0 / up to 0x40 (signed) and
 * copied to [0x511E90] */
static void f464BDC(vm *v)
{
    W(0x00707FF4, R(0x00707FF4) - 1u);
    if ((int32_t)R(0x00707FF4) < 0) W(0x00707FF4, 0);
    W(0x00511E90, R(0x00707FF4));
}
static void f464C00(vm *v)
{
    W(0x00707FF4, R(0x00707FF4) + 1u);
    if ((int32_t)R(0x00707FF4) > 0x40) W(0x00707FF4, 0x40);
    W(0x00511E90, R(0x00707FF4));
}

static void f464E53(vm *v) { flip(v, 0x00708060); }
static void f464E5B(vm *v) { flip(v, 0x00708064); }

/* 0x00464E63 / 0x00464E7D: [0x70AB76] 0..5 and 0x00464E97 / 0x00464EB1: [0x70AB7A] 1..6, each
 * stepped, then put back inside by a SIGNED test (the original steps first) */
static void f464E63(vm *v)
{
    W(0x0070AB76, R(0x0070AB76) - 1u);
    if ((int32_t)R(0x0070AB76) < 0) W(0x0070AB76, 5);
}
static void f464E7D(vm *v)
{
    W(0x0070AB76, R(0x0070AB76) + 1u);
    if ((int32_t)R(0x0070AB76) > 5) W(0x0070AB76, 0);
}
static void f464E97(vm *v)
{
    W(0x0070AB7A, R(0x0070AB7A) - 1u);
    if ((int32_t)R(0x0070AB7A) < 1) W(0x0070AB7A, 6);
}
static void f464EB1(vm *v)
{
    W(0x0070AB7A, R(0x0070AB7A) + 1u);
    if ((int32_t)R(0x0070AB7A) > 6) W(0x0070AB7A, 1);
}

/* the redefine screen's rows: [0x708014] = 1 and [0x708018] = the row (0x464F15 is row 0) */
static void redef(vm *v, uint32_t row) { W(0x00708014, 1); W(0x00708018, row); }
static void f464F2A(vm *v) { redef(v, 1); }
static void f464F3F(vm *v) { redef(v, 2); }
static void f464F54(vm *v) { redef(v, 3); }
static void f464F69(vm *v) { redef(v, 4); }
static void f464F7E(vm *v) { redef(v, 5); }
static void f464F93(vm *v) { redef(v, 6); }
static void f464FA8(vm *v) { redef(v, 7); }
static void f464FD2(vm *v) { redef(v, 9); }
static void f464FE7(vm *v) { redef(v, 0xA); }
static void f464FFC(vm *v) { redef(v, 0xB); }

/* CAR SETUP's sliders (the player's +0x40 / +0x44 / +0x48; 0x468054 is +0x44's up) */
static void f467FF0(vm *v)      /* +0x40 down, to 0 */
{
    uint32_t P = cur_player(v);
    if ((int32_t)R(P + 0x40) > 0) W(P + 0x40, R(P + 0x40) - 1u);
}
static void f46800D(vm *v)      /* +0x40 up while the other two leave more than 10 */
{
    uint32_t P = cur_player(v);
    if ((int32_t)(100u - R(P + 0x40) - R(P + 0x44)) > 10) W(P + 0x40, R(P + 0x40) + 1u);
}
static void f468035(vm *v)      /* +0x44 down, to 0 */
{
    uint32_t P = cur_player(v);
    if (R(P + 0x44) != 0) W(P + 0x44, R(P + 0x44) - 1u);
}
static void f468188(vm *v)      /* +0x48 up by 10 unless it is 100 */
{
    uint32_t P = cur_player(v);
    if (R(P + 0x48) != 100) W(P + 0x48, R(P + 0x48) + 10u);
}
static void f4681AC(vm *v)      /* +0x48 down by 10 unless it is 0 */
{
    uint32_t P = cur_player(v);
    if (R(P + 0x48) != 0) W(P + 0x48, R(P + 0x48) - 10u);
}

/* the service area's six buttons, 0x004682D4 / 0x0046831D / 0x00468366 / 0x0046837A / 0x0046838E / 0x004683D7:
 * the selected part flips [0x70CA08] and names itself in [0x70CA0C].  Four of them (the parts that cost
 * something) first check the money: unless the cursor's flag is set, the sum so far plus
 * the part's cost must not pass the budget [0x7082A4] (signed) */
static int can_afford(vm *v, uint32_t cost)
{
    if (R(0x0070BE7Au + 4u * R(0x00707E91)) == 1) return 1;
    return (int32_t)(R(0x007082A4) - (R(0x0070CBF4) + R(cost))) >= 0;
}
static void f4682D4(vm *v)
{
    if (!can_afford(v, 0x0070CAF8)) return;
    W(0x0070CC00, R(0x0070CBF8));
    flip(v, 0x0070CA08);
    W(0x0070CA0C, 1);
}
static void f46831D(vm *v)
{
    if (!can_afford(v, 0x0070CAFC)) return;
    flip(v, 0x0070CA08);
    W(0x0070CA0C, 2);
    W(0x0070CC0C, R(0x0070CC04));
}
static void f468366(vm *v) { flip(v, 0x0070CA08); W(0x0070CA0C, 3); }
static void f46837A(vm *v) { flip(v, 0x0070CA08); W(0x0070CA0C, 4); }
static void f46838E(vm *v)
{
    if (!can_afford(v, 0x0070CB00)) return;
    flip(v, 0x0070CA08);
    W(0x0070CA0C, 5);
    W(0x0070CC18, R(0x0070CC10));
}
static void f4683D7(vm *v)
{
    if (!can_afford(v, 0x0070CB04)) return;
    flip(v, 0x0070CA08);
    W(0x0070CA0C, 6);
    W(0x0070CC24, R(0x0070CC1C));
}

/* the sum of the costs the flags choose: the words 0x70BE7A.. are the flags (-1 ended), the
 * words 0x70BF22.. pointers to the costs; the total goes to [0x70CBF4] */
static void parts_total(vm *v)
{
    uint32_t fl = 0x0070BE7A, pt = 0x0070BF22, sum = 0;
    do {
        if (R(fl) != 0) sum += R(R(pt));
        fl += 4; pt += 4;
    } while (R(fl) != 0xFFFFFFFFu);
    W(0x0070CBF4, sum);
}

/* 0x00468420: the cursor's part toggled; [0x70CBF4] the total; then the budget decides: 900 or more
 * to spare clears the "afford" words (0x70BECE..), any to spare sets the cursor's, a negative one
 * undoes the toggle and flags [0x70CA10] */
static void f468420(vm *v)
{
    uint32_t i, left;
    W(0x0070CA10, 0);
    W(0x0070CA08, 0);
    flip(v, 0x0070BE7Au + 4u * R(0x00707E91));
    parts_total(v);
    left = R(0x007082A4) - R(0x0070CBF4);
    if ((int32_t)left >= 0x384) {
        for (i = 0x0070BECE; ; i += 4) {
            W(i, 0);
            if (R(i + 4) == 0xFFFFFFFFu) break;
        }
    } else if ((int32_t)left >= 0) {
        W(0x0070BECEu + 4u * R(0x00707E91), 1);
    } else {
        flip(v, 0x0070BE7Au + 4u * R(0x00707E91));
        W(0x0070CA10, 1);
    }
    parts_total(v);
}

/* 0x00469D5B / 0x00469D75: [0x707BB4] 0..10 (the slots' cursor; down from 0 goes to 10) */
static void f469D5B(vm *v)
{
    W(0x00707BB4, R(0x00707BB4) + 1u);
    if ((int32_t)R(0x00707BB4) >= 0xB) W(0x00707BB4, 0);
}
static void f469D75(vm *v)
{
    W(0x00707BB4, R(0x00707BB4) - 1u);
    if ((int32_t)R(0x00707BB4) < 0) W(0x00707BB4, 0xA);
}
/* 0x0046A04F / 0x0046A064: [0x707BB0] 0..9; 0x0046BE3D: [0x707BB8] 0..2, up */
static void f46A04F(vm *v)
{
    W(0x00707BB0, R(0x00707BB0) - 1u);
    if ((int32_t)R(0x00707BB0) < 0) W(0x00707BB0, 9);
}
static void f46A064(vm *v)
{
    W(0x00707BB0, R(0x00707BB0) + 1u);
    if ((int32_t)R(0x00707BB0) > 9) W(0x00707BB0, 0);
}
static void f46BE3D(vm *v)
{
    uint32_t n = R(0x00707BB8) + 1u;
    W(0x00707BB8, (int32_t)n < 3 ? n : 0u);
}

/* the sound / controller options' flags, each flipped and copied on:
 * 0x0046CC0A / 0x0046CC1E flip a second word in the engine's block as well */
static void f46CC0A(vm *v) { flip(v, 0x00707FA8); flip(v, 0x004F00A4); }
static void f46CC1E(vm *v) { flip(v, 0x00707FAC); flip(v, 0x004F00A0); }
static void f46CC32(vm *v)      /* [0x4F009C] one back, round from the count [0x4F00B8] */
{
    uint32_t x = R(0x004F009C) - 1u;
    if ((int32_t)x < 0) x = R(0x004F00B8) - 1u;
    W(0x004F009C, x);
}
static void f46CC46(vm *v)      /* [0x4F009C] one on, round to 0; [0x707FB0] flips */
{
    uint32_t x = R(0x004F009C) + 1u;
    if ((int32_t)x > (int32_t)(R(0x004F00B8) - 1u)) x = 0;
    W(0x004F009C, x);
    flip(v, 0x00707FB0);
}
static void f46CC8C(vm *v) { flip(v, 0x00707FB8); W(0x0070CC6C, R(0x00707FB8)); }
static void f46CD5E(vm *v) { flip(v, 0x00707FC4); W(0x0070CC78, R(0x00707FC4)); }
static void f46CD70(vm *v) { flip(v, 0x00707FC8); W(0x0070CC7C, R(0x00707FC8)); }
static void f46CDE2(vm *v) { flip(v, 0x00707FD0); W(0x0070CC84, R(0x00707FD0)); }
static void f46CD82(vm *v)      /* [0x707FCC] 0..2, down */
{
    W(0x00707FCC, R(0x00707FCC) == 0 ? 2u : R(0x00707FCC) - 1u);
    W(0x0070CC80, R(0x00707FCC));
}
static void f46CDB2(vm *v)      /* ... and up */
{
    W(0x00707FCC, R(0x00707FCC) == 2 ? 0u : R(0x00707FCC) + 1u);
    W(0x0070CC80, R(0x00707FCC));
}

/* 0x0046CFFE: every row of the table at 0x6EDFE8 (0x94 bytes each, -1 ended): +0x24 = 1, +0xC and +0x10 = 0 */
static void f46CFFE(vm *v)
{
    uint32_t p = 0x006EDFE8;
    do {
        W(p + 0x24, 1); W(p + 0x0C, 0); W(p + 0x10, 0);
        p += 0x94;
    } while (R(p) != 0xFFFFFFFFu);
}

/* 0x0046D9D2 / 0x0046D9F4: the current player's +0x28, 1..90 (wraps both ways) */
static void f46D9D2(vm *v)
{
    uint32_t P = cur_player(v), x = R(P + 0x28) + 1u;
    W(P + 0x28, (int32_t)x > 0x5A ? 1u : x);
}
static void f46D9F4(vm *v)
{
    uint32_t P = cur_player(v), x = R(P + 0x28) - 1u;
    W(P + 0x28, (int32_t)x > 0 ? x : 0x5Au);
}
/* 0x0046DB58 / 0x0046DB80: the current player's +0x280, 0..100 (clamped) */
static void f46DB58(vm *v)
{
    uint32_t P = cur_player(v), x = R(P + 0x280) + 1u;
    W(P + 0x280, (int32_t)x > 0x64 ? 0x64u : x);
}
static void f46DB80(vm *v)
{
    uint32_t P = cur_player(v), x = R(P + 0x280) - 1u;
    W(P + 0x280, (int32_t)x > 0 ? x : 0u);
}

/* 0x0046DA2D: every player's stage results blanked (words 0x98.. 0xCC: -1, with +0xB0 and +0xCC 0),
 * the four records the pointer table names, then the table at 0x6EDFE8's +0x5C.. +0x90 the same way */
static void blank_results(vm *v, uint32_t p, uint32_t first)
{
    uint32_t k;
    for (k = 0; k < 14; k++) W(p + first + 4u * k, k == 6 || k == 13 ? 0u : 0xFFFFFFFFu);
}
static void f46DA2D(vm *v)
{
    uint32_t k, p;
    for (k = 0; k < 4; k++) blank_results(v, R(A_PLAYERS + 4u * k), 0x98);
    p = 0x006EDFE8;
    do {
        blank_results(v, p, 0x5C);
        p += 0x94;
    } while (R(p) != 0xFFFFFFFFu);
}

/* the three that only return or leave a back-to-menu state: 0x0046E2FE / 0x0046E2FF are
 * bare returns, 0x0046E300 is BACK */
static void f46E2FE(vm *v) { (void)v; }
static void f46E2FF(vm *v) { (void)v; }
static void f46E300(vm *v)
{
    W(0x00707EC9, 0);
    W(0x00707ECD, 1);
    W(0x00708378, 0xFFFFFFFFu);
}

/* 0x00478F98 / 0x00478FF9: the results table's previous / next player */
static void f478F98(vm *v)
{
    if (R(0x0070C9E4) != 1 && R(0x0070C5CC) != 1) {
        W(0x0070C9E8, R(0x0070C9E8) - 1u);
        if ((int32_t)R(0x0070C9E8) < 0) W(0x0070C9E8, R(0x0070C9E4) - 1u);
    }
    W(0x0070C5CC, 0xFFFFFFFFu);
    W(0x0070C9EC, 0xFFFFFFFFu);
    if (R(0x007082FC) == 0) W(0x007082FC, R(0x00708E08));
    else W(0x007082FC, R(0x007082FC) - 1u);
    W(0x00707ECD, 0);
}
static void f478FF9(vm *v)
{
    if (R(0x0070C9E4) != 1 && R(0x0070C5CC) == 1) {
        W(0x0070C9E8, R(0x0070C9E8) + 1u);
        if ((int32_t)R(0x0070C9E8) >= (int32_t)R(0x0070C9E4)) W(0x0070C9E8, 0);
    }
    W(0x0070C5CC, 1);
    W(0x0070C9EC, 0);
    if (R(0x007082FC) == R(0x00708E08)) W(0x007082FC, 0);
    else W(0x007082FC, R(0x007082FC) + 1u);
    W(0x00707ECD, 0);
}
static void f47A97E(vm *v) { (void)v; W(0x00707ED5, 1); }

static const struct { uint32_t va; fel_fn fn; } TAB[] = {
    { 0x00464A30, f464A30 }, { 0x00464AE0, f464AE0 }, { 0x00464F15, f464F15 },
    { 0x00467C2E, f467C2E }, { 0x00468054, f468054 }, { 0x0046C6A9, f46C6A9 },
    { 0x0046C83A, f46C83A }, { 0x0046C8A9, f46C8A9 }, { 0x0046D44B, f46D44B },
    { 0x0047D46C, f47D46C }, { 0x00484244, f484244 }, { 0x00484274, f484274 },
    { 0x00486374, f486374 }, { 0x00486CEA, f486CEA },
    { 0x0049D5A9, f49D5A9 }, { 0x0049E231, f49E231 }, { 0x004A5249, f4A5249 },
    { 0x00486DDA, f486DDA }, { 0x004887B0, f4887B0 }, { 0x0048883E, f48883E },
    { 0x0047A87F, f47A87F }, { 0x0046E617, f46E617 }, { 0x0046DA16, f46DA16 },
    { 0x0049D3BA, f49D3BA }, { 0x0049D5D9, f49D5D9 }, { 0x00486D12, f486D12 },
    { 0x00482BCE, f482BCE },
    { 0x00482F5F, f482F5F }, { 0x00482BF1, f482BF1 }, { 0x00464871, f464871 },
    { 0x0046BB2D, f46BB2D }, { 0x004A045F, f4A045F }, { 0x004A05A8, f4A05A8 },
    { 0x004A1683, f4A1683 }, { 0x004A169E, f4A169E }, { 0x004A1707, f4A1707 },
    { 0x0049DD17, f49DD17 },
    { 0x004767E2, f4767E2 }, { 0x0046A0B7, f46A0B7 }, { 0x0046BCDD, f46BCDD },
    { 0x0046BD33, f46BD33 }, { 0x0046C6CC, f46C6CC }, { 0x00484F23, f484F23 },
    { 0x0046C09C, f46C09C },                         /* the replay slots' names */
    /* batch 6 (2026-11-07): the options screens' arrows and toggles */
    { 0x00464721, f464721 }, { 0x00464745, f464745 }, { 0x00464769, f464769 },
    { 0x0046478D, f46478D }, { 0x004647B1, f4647B1 }, { 0x004647D5, f4647D5 },
    { 0x00464813, f464813 }, { 0x00464A1A, f464A1A }, { 0x00464A46, f464A46 },
    { 0x00464A5C, f464A5C }, { 0x00464A72, f464A72 }, { 0x00464A88, f464A88 },
    { 0x00464A9E, f464A9E }, { 0x00464AB4, f464AB4 }, { 0x00464ACA, f464ACA },
    { 0x00464B30, f464B30 }, { 0x00464BDC, f464BDC }, { 0x00464C00, f464C00 },
    { 0x00464E53, f464E53 }, { 0x00464E5B, f464E5B }, { 0x00464E63, f464E63 },
    { 0x00464E7D, f464E7D }, { 0x00464E97, f464E97 }, { 0x00464EB1, f464EB1 },
    { 0x00464F2A, f464F2A }, { 0x00464F3F, f464F3F }, { 0x00464F54, f464F54 },
    { 0x00464F69, f464F69 }, { 0x00464F7E, f464F7E }, { 0x00464F93, f464F93 },
    { 0x00464FA8, f464FA8 }, { 0x00464FD2, f464FD2 }, { 0x00464FE7, f464FE7 },
    { 0x00464FFC, f464FFC }, { 0x00467FF0, f467FF0 }, { 0x0046800D, f46800D },
    { 0x00468035, f468035 }, { 0x00468188, f468188 }, { 0x004681AC, f4681AC },
    { 0x004682D4, f4682D4 }, { 0x0046831D, f46831D }, { 0x00468366, f468366 },
    { 0x0046837A, f46837A }, { 0x0046838E, f46838E }, { 0x004683D7, f4683D7 },
    { 0x00468420, f468420 }, { 0x00469D5B, f469D5B }, { 0x00469D75, f469D75 },
    { 0x0046A04F, f46A04F }, { 0x0046A064, f46A064 }, { 0x0046BE3D, f46BE3D },
    { 0x0046CC0A, f46CC0A }, { 0x0046CC1E, f46CC1E }, { 0x0046CC32, f46CC32 },
    { 0x0046CC46, f46CC46 }, { 0x0046CC8C, f46CC8C }, { 0x0046CD5E, f46CD5E },
    { 0x0046CD70, f46CD70 }, { 0x0046CD82, f46CD82 }, { 0x0046CDB2, f46CDB2 },
    { 0x0046CDE2, f46CDE2 }, { 0x0046CFFE, f46CFFE }, { 0x0046D151, f46D151 },
    { 0x0046D9D2, f46D9D2 }, { 0x0046D9F4, f46D9F4 }, { 0x0046DA2D, f46DA2D },
    { 0x0046DB58, f46DB58 }, { 0x0046DB80, f46DB80 }, { 0x0046E2FE, f46E2FE },
    { 0x0046E2FF, f46E2FF }, { 0x0046E300, f46E300 }, { 0x00478F98, f478F98 },
    { 0x00478FF9, f478FF9 }, { 0x0047A97E, f47A97E },
    /* the five sorts last: each call is millions of the original's instructions,
     * so the gate (and every mutant that fails before them) reaches them late */
    { 0x004775CB, f4775CB }, { 0x004776DE, f4776DE }, { 0x004A02EF, f4A02EF },
    { 0x004A18E6, f4A18E6 }, { 0x004A1776, f4A1776 },
    { 0x00464B42, f464B42 }, { 0x00464B66, f464B66 }, { 0x00464B8A, f464B8A },
    { 0x00464BB3, f464BB3 }, { 0x00464C24, f464C24 }, { 0x00464C48, f464C48 },
    { 0x00464C6C, f464C6C }, { 0x00464C97, f464C97 }, { 0x00464CC2, f464CC2 },
    { 0x00464CF0, f464CF0 }, { 0x00464D1E, f464D1E }, { 0x00464D4C, f464D4C },
    { 0x00464D7A, f464D7A }, { 0x00464D9E, f464D9E }, { 0x00467D2F, f467D2F },
    { 0x00467D50, f467D50 }, { 0x00467D71, f467D71 }, { 0x00467D92, f467D92 },
    { 0x00467DB3, f467DB3 }, { 0x00467DD4, f467DD4 }, { 0x00467DF5, f467DF5 },
    { 0x00467E16, f467E16 }, { 0x00467E37, f467E37 }, { 0x00467E58, f467E58 },
    { 0x0047E041, f47E041 }, { 0x0047E04E, f47E04E }, { 0x0047E06F, f47E06F },
    { 0x0047E090, f47E090 }, { 0x0047E0B1, f47E0B1 }, { 0x00466544, f466544 },
    { 0x00466576, f466576 }, { 0x004665C2, f4665C2 }, { 0x0046D1D0, f46D1D0 },
    { 0x00467128, f467128 }, { 0x00467174, f467174 }, { 0x0046807E, f46807E },
};

fel_fn fel_lookup(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return uses_draw(va) && !DRAW ? NULL : TAB[i].fn;
    return NULL;
}

int fel_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fel_va(int i) { return TAB[i].va; }
int fel_uses_draw(int i) { return uses_draw(TAB[i].va); }
