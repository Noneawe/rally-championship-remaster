/* fd_career.c - see fd_career.h, family D, 2026-11-09.
 *
 * Every function names the original it transcribes; the state is the VM's data
 * copy at the original's own addresses, arithmetic is 32-bit and wraps as the
 * original's does. */
#include "fd_career.h"

#include "fe_champ.h"
#include "fe_logic.h"

#include <string.h>

#define R(a)    vm_rd32(v, (uint32_t)(a))
#define W(a, x) vm_wr32(v, (uint32_t)(a), (uint32_t)(x))

static const fdc_host *HOST;

void fdc_set_host(const fdc_host *h) { HOST = h; }

/* a platform service; every register not named is 0 (the gate does not compare them) */
static uint32_t svc(vm *v, uint32_t va, uint32_t r[8])
{
    return HOST->svc(HOST->ud, v, va, r);
}

/* the original's own scan limit: a loop that never reaches its -1 reads off the
 * end of the data and the interpreter stops; so does this */
#define SCAN_MAX 100000

/* ---- 0x0046F209: the new career's counters ----
 *
 * The 0xAA drivers' +0x10 / +0xC / +0x20 cleared and +0x24 = 1 (a do-while: the
 * first record is always done), then each of the four players: +0x8C, +0x84 = 0,
 * +0xD0 = 1, 0x00482F5F (the counters back to the start), and, unless
 * [0x708038] is 3, +0x120 = 2, +0x110 = 0x32, +0x10C = 3, +0x114 = 0x46,
 * +0x118 = 0x1E, +0x11C = +0x280.  [0x7082F8] ends at 3. */
static int f46F209(vm *v)
{
    uint32_t d = 0x006EDFE8u, k, P, n = 0;
    fel_fn f482f5f = fel_lookup(0x00482F5Fu);
    W(0x00707BC8, 0);
    W(0x007082B8, 0);
    do {
        W(d + 0x10, 0); W(d + 0xC, 0); W(d + 0x20, 0); W(d + 0x24, 1);
        d += 0x94;
        if (++n > SCAN_MAX) return FDC_FATAL;
    } while (R(d) != 0xFFFFFFFFu);
    W(0x00708294, 1);
    W(0x007082E0, 0);
    for (k = 0; k < 4; k++) {
        W(0x007082F8, k);
        P = R(0x00708DE8u + 4u * k);
        W(P + 0x8C, 0); W(P + 0x84, 0); W(P + 0xD0, 1);
        f482f5f(v);
        if (R(0x00708038) != 3) {
            W(P + 0x120, 2); W(P + 0x110, 0x32); W(P + 0x10C, 3);
            W(P + 0x114, 0x46); W(P + 0x118, 0x1E);
            W(P + 0x11C, R(P + 0x280));
        }
    }
    return 0;
}

/* ---- 0x004A65C9: the A8 championship's new career ----
 *
 * 0x0046F209's own copy, called by FUN_004A60A7 in its place: the same
 * counters ([0x707BC8], [0x7082B8], the drivers' +0x10 / +0xC / +0x20 / +0x24,
 * [0x708294], [0x7082E0]), then the four players from the LAST to the first:
 * +0x8C, +0x84 = 0, +0xD0 = 1 - and nothing else: no 0x00482F5F, no
 * setup defaults (+0x120 ..), and [0x7082F8] is not written. */
static int f4A65C9(vm *v)
{
    uint32_t d = 0x006EDFE8u, k, P, n = 0;
    W(0x00707BC8, 0);
    W(0x007082B8, 0);
    do {
        W(d + 0x10, 0); W(d + 0xC, 0); W(d + 0x20, 0); W(d + 0x24, 1);
        d += 0x94;
        if (++n > SCAN_MAX) return FDC_FATAL;
    } while (R(d) != 0xFFFFFFFFu);
    W(0x00708294, 1);
    W(0x007082E0, 0);
    for (k = 4; k-- > 0;) {
        P = R(0x00708DE8u + 4u * k);
        W(P + 0x8C, 0); W(P + 0x84, 0); W(P + 0xD0, 1);
    }
    return 0;
}

/* ---- 0x0046D70C: the rivals "shuffled" ----
 *
 * Four tables of 0x94-byte records (count, base): [0x70CA6C] 0x006F13F0,
 * [0x70CA70] 0x006F30D8, [0x70CA74] 0x006F4230, [0x70CA68] 0x006EDFE8.  Per
 * entry two draws of the generator mod the count go to [0x708358] / [0x70835C];
 * then the "swap" - which never uses them: the products are in EAX and EBX /
 * EDX stay at the BASE, so the first record is copied through the two scratch
 * records 0x00707D0C / 0x00707DA0 and back onto itself (Q-nn, below).  A count
 * of 0 is a divide by zero (the original's #DE): FDC_FATAL. */
static void copy_rec(vm *v, uint32_t dst, uint32_t src)
{
    uint32_t k;
    for (k = 0; k < 0x94; k++) vm_wr8(v, dst + k, vm_rd8(v, src + k));
}
static int shuffle_table(vm *v, uint32_t cnt, uint32_t base)
{
    uint32_t i = 0, a, b;
    do {
        if (!fel_rng_mod(v, R(cnt), &a)) return FDC_FATAL;
        W(0x00708358, a);
        if (!fel_rng_mod(v, R(cnt), &b)) return FDC_FATAL;
        W(0x0070835C, b);
        copy_rec(v, 0x00707D0Cu, base);
        copy_rec(v, 0x00707DA0u, base);
        copy_rec(v, base, 0x00707D0Cu);
        copy_rec(v, base, 0x00707DA0u);
        i++;
    } while (i != R(cnt));
    return 0;
}
static int f46D70C(vm *v)
{
    if (shuffle_table(v, 0x0070CA6Cu, 0x006F13F0u)) return FDC_FATAL;
    if (shuffle_table(v, 0x0070CA70u, 0x006F30D8u)) return FDC_FATAL;
    if (shuffle_table(v, 0x0070CA74u, 0x006F4230u)) return FDC_FATAL;
    if (shuffle_table(v, 0x0070CA68u, 0x006EDFE8u)) return FDC_FATAL;
    return 0;
}

/* ---- 0x0046D95B: the entry [0x7082C0] + 1 of the itinerary ----
 * 0x0046EBBC from the itinerary's start with EAX = [0x7082C0] + 1, EBX = [0x7082C4];
 * [0x708358] = the entry's first word.  PUSHAD ... POPAD, then EAX = [0x708358]
 * (the host's vm_run passes no register and reads [0x708358]). */
static int f46D95B(vm *v)
{
    int rc;
    uint32_t e = fec_46ebbc(v, 0x006E7EE9u, R(0x007082C0) + 1u, R(0x007082C4), &rc);
    if (rc) return FDC_FATAL;
    W(0x00708358, R(e));
    return 0;
}

/* ---- the narration ---- */

/* 0x0047B4FA: the stage's narration - the entry [0x7082C0] of the itinerary; in
 * the arcade (+0x708038 = 2) its list [0x70C39E + 4 * the entry's second word]
 * starts: [0xA2C588] = 1, the list kept in [0xA2C584], the index [0xA2C580] = 0
 * and the first path played at the volume [0x708000] (0x0040F566). */
static int f47B4FA(vm *v)
{
    int rc;
    uint32_t e = fec_46ebbc(v, 0x006E7EE9u, R(0x007082C0), R(0x007082C4), &rc), w, list, r[8] = { 0 };
    if (rc) return FDC_FATAL;
    w = R(e + 4);
    if (R(0x00708038) == 2) {
        W(0x00A2C588, 1);
        list = R(w * 4u + 0x0070C39Eu);
        W(0x00A2C584, list);
        r[VM_EAX] = R(list);
        W(0x00A2C580, 0);
        r[VM_ECX] = R(0x00708000);
        svc(v, 0x0040F566u, r);
    }
    return 0;
}

/* 0x0047B4D2: the rally's narration: the key read (0x00407CDC, its flags unused),
 * and in the arcade the path [0x70C326 + 4 * [0x7082C4]] at the volume [0x708000] */
static int f47B4D2(vm *v)
{
    uint32_t r[8] = { 0 };
    svc(v, 0x00407CDCu, r);
    if (R(0x00708038) == 2) {
        memset(r, 0, sizeof r);
        r[VM_EAX] = R(R(0x007082C4) * 4u + 0x0070C326u);
        r[VM_ECX] = R(0x00708000);
        svc(v, 0x0040F566u, r);
    }
    return 0;
}

/* 0x0046C40F: the next narration file.  Returns the carry the original leaves:
 * with [0x708038] != 2 that of its first CMP (below 2, unsigned); with the
 * list not running 0; speech busy (ZF from 0x0040C37C) 0; the list's end (-1)
 * ends it ([0xA2C588] = 0) with 1; otherwise the next path is played, 0. */
static int f46C40F(vm *v)
{
    uint32_t r[8] = { 0 }, e;
    if (R(0x00708038) != 2) return R(0x00708038) < 2u;
    if (R(0x00A2C588) != 1) return 0;
    if (svc(v, 0x0040C37Cu, r) & FDC_ZF) return 0;
    W(0x00A2C580, R(0x00A2C580) + 1u);
    e = R(R(0x00A2C584) + R(0x00A2C580) * 4u);
    if (e == 0xFFFFFFFFu) { W(0x00A2C588, 0); return 1; }
    r[VM_EAX] = e;
    r[VM_ECX] = R(0x00708000);
    svc(v, 0x0040F566u, r);
    return 0;
}

/* ---- VIEW STAGE's < > ---- */

/* 0x0046C745 / 0x0046C77E: [0x7082D4] steps down (0 -> [0x7082D0]) / up
 * ([0x7082D0] and over -> 0, a signed test), [0x7082C0] = [0x7082C8] + it, then
 * the stage map (0x00465011) and the narration */
static int view_step(vm *v, int up)
{
    uint32_t r[8] = { 0 };
    if (!up) {
        if (R(0x007082D4) == 0) W(0x007082D4, R(0x007082D0));
        else W(0x007082D4, R(0x007082D4) - 1u);
    } else {
        if ((int32_t)R(0x007082D4) < (int32_t)R(0x007082D0)) W(0x007082D4, R(0x007082D4) + 1u);
        else W(0x007082D4, 0);
    }
    W(0x007082C0, R(0x007082C8) + R(0x007082D4));
    svc(v, 0x00465011u, r);
    return f47B4FA(v);
}
static int f46C745(vm *v) { return view_step(v, 0); }
static int f46C77E(vm *v) { return view_step(v, 1); }

/* ---- the slot screens' numbers ---- */

/* `n` as the original writes a two-digit number: a "0" first below 10 (signed) */
static uint32_t two(vm *v, uint32_t d, uint32_t n)
{
    if ((int32_t)n < 10) d = fel_fmt_num(v, 0, 0, d);
    return fel_fmt_num(v, n, 0, d);
}

/* 0x0046D166: the page's range, "<5 * [0x707BBC]>-<that + 19>" at 0x00A2C254
 * (0x004019CD, '-' by 0x00401F38, 0x00401F02's NUL) */
static void page_range(vm *v)
{
    uint32_t x = R(0x00707BBC) * 0x14u, d = 0x00A2C254u;
    d = fel_fmt_num(v, x, 0, d);
    vm_wr8(v, d++, '-');
    d = fel_fmt_num(v, x + 0x13u, 0, d);
    vm_wr8(v, d, 0);
}

/* 0x0046A080 / 0x0046A098: [0x707BBC] down (below 0 -> 4) / up (above 4 -> 0),
 * signed tests, then the range */
static int f46A080(vm *v)
{
    uint32_t x = R(0x00707BBC) - 1u;
    if ((int32_t)x < 0) x = 4;
    W(0x00707BBC, x);
    page_range(v);
    return 0;
}
static int f46A098(vm *v)
{
    uint32_t x = R(0x00707BBC) + 1u;
    if (!((int32_t)x <= 4)) x = 0;
    W(0x00707BBC, x);
    page_range(v);
    return 0;
}

/* 0x0046D166: the replay screens' entry list - page_range, which 0x0046A080 / 0x0046A098 end in */
static int f46D166(vm *v)
{
    page_range(v);
    return 0;
}

/* 0x00469EBF: the slots' numbers: the page's first and last (a two-digit pair,
 * "<n>-<n + 9>" at 0x00A2C254, n = 10 * [0x707BB0]) and the ten of the page,
 * 0x00A2C558 + 4 * k = n + k, each NUL-ended */
static int f469EBF(vm *v)
{
    uint32_t n = R(0x00707BB0) * 10u, d, k;
    d = two(v, 0x00A2C254u, n);
    vm_wr8(v, d++, '-');
    d = two(v, d, n + 9u);
    vm_wr8(v, d, 0);
    for (k = 0; k < 10; k++) {
        d = two(v, 0x00A2C558u + 4u * k, n + k);
        vm_wr8(v, d, 0);
    }
    return 0;
}

/* 0x0046C6B1: RETIRE's no / yes - the token [0x707BC0] + 0x56B at 0x00A2C254 */
static int f46C6B1(vm *v)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = R(0x00707BC0) + 0x56Bu;
    r[VM_EBX] = 0x00A2C254u;
    svc(v, 0x00401F8Fu, r);
    vm_wr8(v, r[VM_EBX], 0);
    return 0;
}

/* ---- the options screens' pre-draws, the item styles, the devices ---- */

/* the draw layer as the handlers call it: 0x00402311 (EBX the font record, EAX
 * the token, ECX x, EDX y), 0x0040239C (EAX the font record, EBX the string,
 * ECX x, EDX y), 0x004066F9 (EAX the picture, ECX x, EDX y, ESI the bundle) */
static void draw_tok(vm *v, uint32_t font, uint32_t token, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = font; r[VM_ECX] = x; r[VM_EDX] = y;
    svc(v, 0x00402311u, r);
}
static void draw_text(vm *v, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = font; r[VM_EBX] = str; r[VM_ECX] = x; r[VM_EDX] = y;
    svc(v, 0x0040239Cu, r);
}
static void draw_pic(vm *v, uint32_t pic, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = pic; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x00707EED);
    svc(v, 0x004066F9u, r);
}

/* 0x004022F3: the token EAX's text (0x00401F8F, then its NUL) at 0x008E80D0,
 * measured in the font record EBX by 0x004022D2: the width in ECX, the
 * font's line height in EDX */
static void measure_tok(vm *v, uint32_t token, uint32_t font, uint32_t *w, uint32_t *h)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = 0x008E80D0u;
    svc(v, 0x00401F8Fu, r);
    vm_wr8(v, r[VM_EBX], 0);
    memset(r, 0, sizeof r);
    r[VM_EAX] = font; r[VM_EBX] = 0x008E80D0u;
    svc(v, 0x004022D2u, r);
    *w = r[VM_ECX]; *h = r[VM_EDX];
}

/* the current player's record */
static uint32_t cur_player(vm *v) { return R(0x00708DE8u + 4u * R(0x007082F8)); }

/* 0x0046E362: the item record of id EBX in the screen's table [0x707EF5] (0x5C each,
 * the first at +4); EBX = 0 would walk 2^32 records and is not answered */
static uint32_t item_rec(vm *v, uint32_t id)
{
    uint32_t e = R(0x00707EF5) + 4u, k;
    for (k = 1; k < id; k++) e += 0x5C;
    return e;
}

/* 0x00468156 / 0x0046816F: the item id's record drawn as a greyed / live row:
 * +0x48 = 0xFFFFFC1C and its font 0x0051852C, or 0xFFFFFC1B and 0x0051840C */
static void item_style(vm *v, uint32_t id, int live)
{
    uint32_t e = item_rec(v, id);
    W(e + 0x48, live ? 0xFFFFFC1Bu : 0xFFFFFC1Cu);
    W(e + 0x18, live ? 0x0051840Cu : 0x0051852Cu);
}

/* 0x0046C4F6: the controller screen's pre-draw: item 9's style by [0x4F28A0]
 * (the devices), the player's name (+0x18) at 0x1BB, 0xD4 (0x004649A4: and, with
 * [0x708010], a marker token 4 after its width), the controller's name token,
 * the two selectors' tokens (+0xC, +0x10 over 0x70AD22 / 0x70AD42), the number
 * +0x28 as text, the token +0x154 + 0x18A, and the two pictures */
static int f46C4F6(vm *v)
{
    uint32_t P, w, h, t, r[8] = { 0 };
    item_style(v, 9, R(0x004F28A0) != 0);
    P = cur_player(v);
    draw_text(v, 0x005183ECu, R(P + 0x18), 0x1BB, 0xD4);
    if (R(0x00708010)) {                                    /* 0x004649A4 */
        memset(r, 0, sizeof r);
        r[VM_EAX] = 0x0051834Cu; r[VM_EBX] = R(P + 0x18);
        svc(v, 0x004022D2u, r);
        if (R(0x00708314) == 1) draw_tok(v, 0x005183ECu, 4, r[VM_ECX] + 0x1BB, 0xD4);
    }
    t = R(R(P + 4) * 4u + 0x0070AB7Eu);
    W(0x00708318, t);
    draw_tok(v, 0x005183ECu, t, 0x1BB, 0xEF);
    measure_tok(v, R(0x00708318), 0x0051834Cu, &w, &h);
    draw_pic(v, 0x1Cu + R(P + 4), w + 0x1CAu, 0xF3);
    t = R(0x0070AD22u + R(P + 0xC) * 4u);
    W(0x00708318, t);
    draw_tok(v, 0x005183ECu, t, 0x1BB, 0x10A);
    t = R(0x0070AD42u + R(P + 0x10) * 4u);
    W(0x00708318, t);
    draw_tok(v, 0x005183ECu, t, 0x1BB, 0x125);
    memset(r, 0, sizeof r);
    r[VM_EAX] = R(P + 0x28); r[VM_EBX] = 0x00A2BF54u;
    vm_wr8(v, fel_fmt_num(v, r[VM_EAX], 0, r[VM_EBX]), 0);
    draw_text(v, 0x005183ECu, 0x00A2BF54u, 0x1BB, 0x140);
    W(0x00708358, R(P + 0x154) + 0x18Au);
    draw_tok(v, 0x005183ECu, R(0x00708358), 0x1BB, 0x15B);
    draw_pic(v, 4, 0x1BA, 0x176);
    W(0x00708358, R(P + 0x280) * 2u);
    W(0x00708358, R(0x00708358) + 0x1BAu);
    draw_pic(v, 6, R(0x00708358), 0x176);
    return 0;
}

/* 0x0046CE81: the sound-setup pre-draw: items 4, 5 and 6 live, then greyed
 * where [0x501C78] / [0x501C80] / [0x501C7C] are not 1; six tokens at 0x1D1 */
static int f46CE81(vm *v)
{
    static const struct { uint16_t word; uint16_t add; uint16_t y; } ROW[] = {
        { 0x7FD4, 0x54B, 0x10B }, { 0x7FE8, 0x554, 0x126 }, { 0x7FD8, 0x56D, 0x141 },
        { 0x7FDC, 0x56D, 0x15C }, { 0x7FE0, 0x56D, 0x177 }, { 0x7FE4, 0x56D, 0x192 },
    };
    uint32_t k;
    item_style(v, 4, 1); item_style(v, 5, 1); item_style(v, 6, 1);
    if (R(0x00501C78) != 1) item_style(v, 4, 0);
    if (R(0x00501C80) != 1) item_style(v, 5, 0);
    if (R(0x00501C7C) != 1) item_style(v, 6, 0);
    for (k = 0; k < 6; k++) {
        W(0x00708358, R(0x00700000u + ROW[k].word) + ROW[k].add);
        draw_tok(v, 0x005183ECu, R(0x00708358), 0x1D1, ROW[k].y);
    }
    return 0;
}

/* 0x004680E6: the current player's device configured (0x00409250: EAX = +0x2C - 1,
 * EBX +0x34, ECX +0x38, EDX +0x44, EBP 0) */
static void device_cfg(vm *v)
{
    uint32_t P = cur_player(v), r[8] = { 0 };
    r[VM_EAX] = R(P + 0x2C) - 1u; r[VM_EBX] = R(P + 0x34);
    r[VM_ECX] = R(P + 0x38); r[VM_EDX] = R(P + 0x44);
    r[VM_EBP] = 0;
    svc(v, 0x00409250u, r);
}

/* 0x004759BE: the player's device, configured when its number (+0x2C) is not
 * above the count [0x4F28A0] (unsigned) */
static int f4759BE(vm *v)
{
    if (!(R(cur_player(v) + 0x2C) > R(0x004F28A0))) device_cfg(v);
    return 0;
}

/* 0x004680B9: EDI the item record (the caller's): with a device (+0x2C of the
 * RECORD not 0) and the player's +0x34 below 3 (signed), +0x34 + 1 and the
 * device configured.  PUSHAD ... POPAD. */
int fdc_4680b9(vm *v, uint32_t edi)
{
    uint32_t P;
    if (R(edi + 0x2C) == 0) return 0;
    P = cur_player(v);
    if ((int32_t)R(P + 0x34) < 3) {
        W(P + 0x34, R(P + 0x34) + 1u);
        device_cfg(v);
    }
    return 0;
}

/* 0x00468114: the device released (0x00409285) */
static int f468114(vm *v)
{
    uint32_t r[8] = { 0 };
    svc(v, 0x00409285u, r);
    return 0;
}

/* 0x0046A1CE: a slot picked: its name (0x20 bytes at 0x5206DC + [0x707BB0] * 0x140
 * + ([0x707E91] - 1) * 0x20; 0x1F of them) copied to 0x0070820C (0x0046A229), the
 * slot's own name cleared (0x00464A01: 0x1F zero bytes, [0x707ED9] = 0), [0x708010] = 1,
 * [0x707ED9] = 0.  The two log calls are no-ops. */
static int f46A1CE(vm *v)
{
    uint32_t e = 0x005206DCu + R(0x00707BB0) * 0x140u + (R(0x00707E91) - 1u) * 0x20u, k;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, 0x0070820Cu + k, vm_rd8(v, e + k));
    for (k = 0; k < 0x1F; k++) vm_wr8(v, e + k, 0);
    W(0x00707ED9, 0);
    W(0x00708010, 1);
    W(0x00707ED9, 0);
    return 0;
}

/* 0x0046C377: a REPLAY slot picked - 0x46A1CE's twin over the replay names (0x20 bytes at 0x0052245C +
 * [0x707BBC] * 0x280 + ([0x707E91] - 1) * 0x20; twenty to a page, five pages).  The same steps: the name's 0x1F bytes
 * to 0x0070820C (0x0046A229), the slot's own cleared (0x00464A01), [0x708010] = 1, [0x707ED9] = 0.  Its four log
 * calls are no-ops. */
static int f46C377(vm *v)
{
    uint32_t e = 0x0052245Cu + R(0x00707BBC) * 0x280u + (R(0x00707E91) - 1u) * 0x20u, k;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, 0x0070820Cu + k, vm_rd8(v, e + k));
    for (k = 0; k < 0x1F; k++) vm_wr8(v, e + k, 0);
    W(0x00707ED9, 0);
    W(0x00708010, 1);
    W(0x00707ED9, 0);
    return 0;
}

/* 0x0048C18C: the carousel's step for the angle EAX: ((EAX >> 3) + 0x4000000) *
 * [0x70C9F4] / 0x14 (the 64-bit product; a quotient past 32 bits is the original's #DE) */
static int carousel_step(vm *v, uint32_t eax, uint32_t *out)
{
    uint64_t p = (uint64_t)((eax >> 3) + 0x4000000u) * R(0x0070C9F4);
    if (p / 0x14u > 0xFFFFFFFFull) return FDC_FATAL;
    *out = (uint32_t)(p / 0x14u);
    return 0;
}

/* 0x0048C114: the car carousel's turn.  [0x70C5CC] the direction (0 still, negative:
 * [0x70C9EC] runs down, positive: up); at the end the angle is 0, the turn over and, going up,
 * the car [0x70C9E8] + 1 (wrapping at [0x70C9E4]) */
static int carousel_turn(vm *v)
{
    uint32_t c = R(0x0070C5CC), d, m;
    if (c == 0) return 0;
    m = R(0x0070C9EC);
    if ((int32_t)c < 0) {
        if (carousel_step(v, m, &d)) return FDC_FATAL;
        W(0x0070C9EC, m - d);
        if (m < d) { W(0x0070C9EC, 0); W(0x0070C5CC, 0); }
        return 0;
    }
    if (carousel_step(v, 0u - m, &d)) return FDC_FATAL;
    W(0x0070C9EC, m + d);
    if (m + d < m) {
        W(0x0070C9EC, 0);
        W(0x0070C5CC, 0);
        W(0x0070C9E8, R(0x0070C9E8) + 1u);
        if (R(0x0070C9E8) == R(0x0070C9E4)) W(0x0070C9E8, 0);
    }
    return 0;
}

/* 0x004664C9: the frame's time [0x5C6568] less the last ([0x70C9F0]) clamped to
 * 0x64 (unsigned) into [0x70C9F4], then the carousel turns */
static int f4664C9(vm *v)
{
    uint32_t t = R(0x005C6568), d = t - R(0x0070C9F0);
    W(0x0070C9F0, t);
    if (d >= 0x64) d = 0x64;
    W(0x0070C9F4, d);
    return carousel_turn(v);
}

/* 0x00415A0C: the notice's box: the token 0x1FE measured in the font 0x0051834C, the
 * box (+0x32 wide, +0x19 high) centred on the screen ([0x4EF974] x [0x4EF978]) and
 * kept in 0x00523364.., filled (0x004065B0, colour 1), outlined (0x00406426: four
 * lines, colour 0xA) and the token drawn in the font 0x0051842C */
static int f415A0C(vm *v)
{
    uint32_t w, h, r[8] = { 0 }, box = 0x00523364u, x, y, lw, lh;
    measure_tok(v, 0x1FE, 0x0051834Cu, &w, &h);
    w += 0x32; h += 0x19;
    W(0x0052336C, w);
    W(0x00523370, h);
    W(0x00523364, (uint32_t)((int32_t)(R(0x004EF974) - w) >> 1));
    W(0x00523368, (uint32_t)((int32_t)(R(0x004EF978) - h) >> 1));
    r[VM_EAX] = 1; r[VM_EBX] = box; r[VM_ESI] = R(0x005179D8);
    svc(v, 0x004065B0u, r);
    /* 0x00406426 with EAX = 0xA, EBX = the box, ECX = EDX = 0 */
    x = 0u + R(box); y = 0u + R(box + 4); lw = R(box + 8); lh = R(box + 0xC);
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0xA; r[VM_EBX] = lw; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x005179D8);
    svc(v, 0x00406457u, r);                                 /* the top row */
    y = y + lh - 1u;
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0xA; r[VM_EBX] = lw; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x005179D8);
    svc(v, 0x00406457u, r);                                 /* the bottom row */
    y -= lh - 2u;
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0xA; r[VM_EBX] = lh - 2u; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x005179D8);
    svc(v, 0x004064F6u, r);                                 /* the left column */
    x = x + lw - 1u;
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0xA; r[VM_EBX] = lh - 2u; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x005179D8);
    svc(v, 0x004064F6u, r);                                 /* the right column */
    draw_tok(v, 0x0051842Cu, 0x1FE, (uint32_t)((int32_t)R(0x004EF974) >> 1),
             (uint32_t)((int32_t)(R(0x004EF978) - R(0x0051842Cu + 8)) >> 1) - 1u);
    return 0;
}

/* 0x0047949B: the player's name being typed (while [0x708014]): a key (0x00407CDC,
 * AL; ZF = none), unless it is 1 taken 7 bits and looked for among the twelve
 * letters of 0x00707EFD (0x00479501: found, the letter at [0x708018] moves over it) or in the
 * forbidden list at 0x00707F89 (zero-ended: found, nothing more is done); else it is
 * stored at [0x708018] and the index becomes 0x3E7.  Then the twelve go to player 0's name
 * (+0x1C) and [0x708014] = 0. */
static int f47949B(vm *v)
{
    uint32_t r[8] = { 0 }, al, ecx, k, p;
    int found = 0, carry = 0;
    if (R(0x00708014) == 0) return 0;
    if (svc(v, 0x00407CDCu, r) & FDC_ZF) return 0;
    al = r[VM_EAX] & 0xFFu;
    if (al != 1) {
        al &= 0x7Fu;
        ecx = R(0x00708018);
        for (k = 0; k < 12; k++)
            if (al == vm_rd8(v, 0x00707EFDu + k)) { found = 1; break; }
        if (found) vm_wr8(v, 0x00707EFDu + k, vm_rd8(v, ecx + 0x00707EFDu));
        else {
            long n = 0;
            for (p = 0x00707F89u; vm_rd8(v, p) != 0; p++) {
                if (vm_rd8(v, p) == al) { carry = 1; break; }
                if (++n > SCAN_MAX) return FDC_FATAL;
            }
        }
        if (carry) return 0;
        vm_wr8(v, ecx + 0x00707EFDu, (uint8_t)al);
        W(0x00708018, 0x3E7);
    }
    for (k = 0; k < 12; k++) vm_wr8(v, R(0x00708DE8) + 0x1Cu + k, vm_rd8(v, 0x00707EFDu + k));
    W(0x00708014, 0);
    return 0;
}

/* ---- the keyboard: 0x00407B8A, and the controller name being typed: 0x004648A0 ---- */

static uint32_t rd8(vm *v, uint32_t a) { return vm_rd8(v, a); }

/* the machinery after the layout a key is looked up in (0x00407B8A, EAX = the scan
 * code, the character back in AL; 0 = none, a dead key is kept in [0x4F1A98]) */
static uint32_t translate(vm *v, uint32_t esi, uint32_t al, uint32_t ch)
{
    uint32_t ebx = R(esi + 4);
    if (ch) ebx = R(esi + 8);
    if (rd8(v, 0x004F1B08)) ebx = R(esi + 0xC);
    if (ebx == 0) return 0;
    return rd8(v, ebx + al);                                   /* XLATB */
}

uint32_t fdc_keychar(vm *v, uint32_t scan)
{
    uint32_t al = scan & 0xFFu, esi, ebx, ah, ch, cl, t, n = 0;
    if (al & 0x80u) return 0;
    if (rd8(v, 0x004EB7A6) == 2 && (rd8(v, 0x004F1ADC) | rd8(v, 0x004F1B08)) != 0) {
        uint32_t e = 0x004EB7CCu;
        int set = 1;
        if (rd8(v, 0x004F1ACE) == 0) { e = 0x004EB7E0u; set = rd8(v, 0x004F1ADA) != 0; }
        if (set) W(0x004EB7A8, e);
    }
    esi = R(0x004EB7A8);
    ch = rd8(v, 0x004F1ACE) | rd8(v, 0x004F1ADA);
    ebx = R(esi);
    if (ebx != 0) {                       /* the layout's own keys: three bytes each, 0 ends */
        for (;;) {
            if (al == rd8(v, ebx)) {
                ah = rd8(v, ebx + 1);
                if (ch) ah = rd8(v, ebx + 2);
                if (ah != 0) { vm_wr8(v, 0x004F1A98u, (uint8_t)ah); return 0; }       /* a dead key */
                break;
            }
            ebx += 3;
            if (rd8(v, ebx) == 0) break;
            if (++n > SCAN_MAX) return 0;
        }
    }
    /* BT [0x4F2FB0], EAX: the bit EAX of the bitmap (a held key) */
    if ((R(0x004F2FB0u + 4u * (al >> 5)) >> (al & 31u)) & 1u) return 0;
    if (rd8(v, 0x004F1A98) == 0) return translate(v, esi, al, ch);
    ah = rd8(v, 0x004F1A98);
    vm_wr8(v, 0x004F1A98u, 0);
    cl = al;
    al = rd8(v, R(esi + 4) + al);
    for (t = 0x004F2FC0u;;) {             /* the dead key's combinations: four bytes, 0 ends */
        if (ah == rd8(v, t) && al == rd8(v, t + 1))
            return ch ? rd8(v, t + 3) : rd8(v, t + 2);
        t += 4;
        if (rd8(v, t) == 0) break;
        if (++n > SCAN_MAX) return 0;
    }
    return translate(v, esi, cl, ch);
}

/* 0x00464958: EDI = the player's name pointer (+0x18); 0x00464988 restores the
 * name from its backup (0x1F bytes from 0x007081EC); 0x004649DD takes the last
 * letter off (the index [0x707ED9] down, a NUL there) */
static uint32_t name_ptr(vm *v) { return R(cur_player(v) + 0x18); }
static void name_restore(vm *v)
{
    uint32_t d = name_ptr(v), k;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, d + k, vm_rd8(v, 0x007081ECu + k));
}
static void name_backspace(vm *v)
{
    if (R(0x00707ED9) == 0) return;
    W(0x00707ED9, R(0x00707ED9) - 1u);
    vm_wr8(v, R(0x00707ED9) + name_ptr(v), 0);
}

/* 0x004648A0: the controller name typed, once a frame while [0x708010] is 1: a key
 * (0x00407CDC; none ends it): 1 (ESCAPE) puts the name back and ends the typing; 0x1C
 * (RETURN) does the same unless a letter was typed; 0x6E clears 0x1F bytes at EDI -
 * the caller's, which the host leaves 0: the original stops there (FDC_FATAL); 0xE
 * takes a letter off; any other key is looked up (0x00407B8A) and, with room ([0x707ED9]
 * not above [0x707EDD], signed), stored at the cursor, which moves on; a name that
 * then measures 0xEC (signed) wide or more loses the letter again. */
static int f4648A0(vm *v)
{
    uint32_t r[8] = { 0 }, al, c, d;
    if (svc(v, 0x00407CDCu, r) & FDC_ZF) return 0;
    al = r[VM_EAX] & 0xFFu;
    if (al == 1) { name_restore(v); W(0x00708010, 0); return 0; }
    if (al == 0x1C) {
        if (R(0x00707ED9) == 0) name_restore(v);
        W(0x00708010, 0);
        return 0;
    }
    if (al == 0x6E) return FDC_FATAL;
    if (al == 0x0E) { name_backspace(v); return 0; }
    c = fdc_keychar(v, al) & 0xFFu;
    if (c == 0) return 0;
    if ((int32_t)R(0x00707ED9) > (int32_t)R(0x00707EDD)) return 0;
    d = name_ptr(v);
    vm_wr8(v, R(0x00707ED9) + d, (uint8_t)c);
    W(0x00707ED9, R(0x00707ED9) + 1u);
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0x0051834Cu; r[VM_EBX] = d;
    svc(v, 0x004022D2u, r);
    if ((int32_t)r[VM_ECX] >= 0xEC) name_backspace(v);
    return 0;
}

/* ---- the load screens' slot details: 0x0046A4CC ----
 *
 * The slot under the cursor read whenever it changes (the file service 0x004A7041, CF = no such slot),
 * then the slot's details - the rally, the four players, their standings - or "n/a" rows.  The slot's
 * data is the service's: the seven words at 0x0051870C, the four 0x28C-byte players after them. */

/* 0x00401F8F: the token's text at EBX, EBX advanced */
static uint32_t slot_tokstr(vm *v, uint32_t token, uint32_t ebx)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = ebx;
    svc(v, 0x00401F8Fu, r);
    return r[VM_EBX];
}

/* a figure drawn in the small font at (x, y): '-' alone when negative (0x00401F38), else the decimal
 * (0x004019CD), NUL-ended in 0x00A2BF54 */
static void slot_num(vm *v, uint32_t val, uint32_t x, uint32_t y)
{
    uint32_t d = 0x00A2BF54u;
    if ((int32_t)val < 0) vm_wr8(v, d++, '-');
    else d = fel_fmt_num(v, val, 0, d);
    vm_wr8(v, d, 0);
    draw_text(v, 0x005180ACu, 0x00A2BF54u, x, y);
}

static const uint32_t SLOT_Y[4] = { 0x193, 0x1AC, 0x1C5, 0x1DE };

/* the three column heads and the player numbers 1..4, the start of both draws */
static void slot_head(vm *v)
{
    uint32_t n;
    draw_tok(v, 0x005180ACu, 0x4E5, 0x8F, 0x17A);
    draw_tok(v, 0x005180ACu, 0x4E6, 0x1D0, 0x17A);
    draw_tok(v, 0x005180ACu, 0x4E7, 0x271, 0x17A);
    for (n = 0; n < 4; n++) {
        uint32_t d = fel_fmt_num(v, n + 1u, 0, 0x00A2BF54u);
        vm_wr8(v, d, 0);
        draw_text(v, 0x005180CCu, 0x00A2BF54u, 0x8F, SLOT_Y[n]);
    }
}

/* a player's row: the two standings, then the two best figures (a "n/a" mark for a player with none) */
static void slot_nums(vm *v, uint32_t esi, uint32_t y)
{
    slot_num(v, R(esi + 0x14C), 0x1D0, y);
    slot_num(v, R(esi + 0x150), 0x215, y);
    if (R(esi + 0xB0) == 0) draw_tok(v, 0x005180ACu, 0x57A, 0x271, y);
    else slot_num(v, R(esi + 0x144), 0x271, y);
    if (R(esi + 0xCC) == 0) draw_tok(v, 0x005180ACu, 0x57A, 0x2B6, y);
    else slot_num(v, R(esi + 0x148), 0x2B6, y);
}

/* the player's car and driver sprites, 0x1C + [+4] and 0x44 + [+8] (the second through [0x708320]) */
static void slot_pics(vm *v, uint32_t esi, uint32_t y)
{
    draw_pic(v, 0x1Cu + R(esi + 4), 0xAF, y);
    W(0x00708320, 0x44u + R(esi + 8));
    draw_pic(v, R(0x00708320), 0x18B, y);
}

/* 0x0046AF0F: no slot - the heads, and "n/a" marks for every field of the four rows */
static int f46AF0F(vm *v)
{
    static const uint16_t X[6] = { 0xB8, 0x1A1, 0x1D0, 0x215, 0x271, 0x2B6 };
    uint32_t k, j;
    slot_head(v);
    draw_tok(v, 0x0051810Cu, 0x578, 0x8F, 0x161);
    for (k = 0; k < 4; k++) {
        draw_tok(v, 0x005180ACu, 0x578, 0xD6, SLOT_Y[k]);
        for (j = 0; j < 6; j++) draw_tok(v, 0x005180ACu, 0x57A, X[j], SLOT_Y[k]);
    }
    return 0;
}

/* 0x0046A544: the slot's details.  The seven words at 0x0051870C go to 0x00707AC0.. (the players, the
 * rally's place on the itinerary [0x707AC8] / [0x707ACC], the mode [0x707AD4] ...); the title line is
 * "<token> - <token>" from the itinerary entry (or, in the championship [0x707AD4] = 0x13, the table at
 * 0x0070C2AE) and then "<n>/6" of the entry's second place; four player rows, a row with no player (the count
 * [0x707AC0] below its number) has three "-" marks instead of its name and sprites. */
static int f46A544(vm *v)
{
    uint32_t esi = 0x0051870Cu, ebx, e, a, tok, k;
    int rc;
    slot_head(v);
    for (k = 0; k < 7; k++) W(0x00707AC0u + 4u * k, R(esi + 4u * k));
    esi += 0x1Cu;
    W(0x00707ADC, esi);
    e = fec_46ebbc(v, 0x006E7EE9u, 0, R(0x00707ACC), &rc);
    if (rc) return FDC_FATAL;
    if (R(0x00707AD4) == 0x13) { a = R(0x0070C2AEu + 4u * R(0x00707ACC)); tok = 0x1DA; }
    else { a = R(e + 0x38); tok = 0x1D9; }
    ebx = slot_tokstr(v, tok, 0x00A2BF54u);
    vm_wr8(v, ebx++, 0x20);
    vm_wr8(v, ebx++, '-');
    vm_wr8(v, ebx++, 0x20);
    ebx = slot_tokstr(v, a, ebx);
    e = fec_46ebbc(v, 0x006E7EE9u, R(0x00707AC8), R(0x00707ACC), &rc);
    if (rc) return FDC_FATAL;
    vm_wr8(v, ebx++, 0x20);
    a = R(e + 0x40);
    if (a == 0) vm_wr8(v, ebx, 0);
    else {
        ebx = fel_fmt_num(v, a, 0, ebx);                    /* 0x00401F51: EAX '/' EDX */
        vm_wr8(v, ebx++, '/');
        ebx = fel_fmt_num(v, 6, 0, ebx);
        vm_wr8(v, ebx, 0);
    }
    draw_text(v, 0x0051810Cu, 0x00A2BF54u, 0x8F, 0x161);
    esi = R(0x00707ADC);
    draw_text(v, 0x005180ACu, esi + 0x124u, 0xD6, SLOT_Y[0]);
    slot_nums(v, esi, SLOT_Y[0]);
    slot_pics(v, esi, SLOT_Y[0]);
    for (k = 1; k < 4; k++) {
        esi += 0x28Cu;
        if ((int32_t)R(0x00707AC0) < (int32_t)(k + 1u)) {
            draw_tok(v, 0x005180ACu, 0x578, 0xD6, SLOT_Y[k]);
            draw_tok(v, 0x005180ACu, 0x57A, 0xB8, SLOT_Y[k]);
            draw_tok(v, 0x005180ACu, 0x57A, 0x1A1, SLOT_Y[k]);
        } else {
            draw_text(v, 0x005180ACu, esi + 0x124u, 0xD6, SLOT_Y[k]);
            slot_pics(v, esi, SLOT_Y[k]);
        }
        slot_nums(v, esi, SLOT_Y[k]);                       /* the absent player's too: the jump lands here */
    }
    return 0;
}

/* 0x0046A4CC: the cursor past the ten slots, or a slot with no file: the "n/a" draw; else the details.
 * The slot is read (0x004A7041, EAX 0, EBX the slot) only when it changed: [0x707AB8] is the last read,
 * [0x707ABC] its answer (1 = no such slot). */
static int f46A4CC(vm *v)
{
    uint32_t slot, r[8] = { 0 };
    if ((int32_t)R(0x00707E91) > 10) {
        W(0x00707ABC, 1);
        W(0x00707AB8, 0x3E7);
        return f46AF0F(v);
    }
    slot = R(0x00707BB0) * 10u + R(0x00707E91) - 1u;
    if (R(0x00707AB8) != slot) {
        W(0x00707AB8, slot);
        r[VM_EAX] = 0; r[VM_EBX] = slot;
        W(0x00707ABC, (svc(v, 0x004A7041u, r) & FDC_CF) ? 1u : 0u);
    }
    return R(0x00707ABC) == 1 ? f46AF0F(v) : f46A544(v);
}

/* ---- a slot's name typed: 0x0046A2E2 (the svgame?? slots) and 0x0046BE55 (the carset?? slots) ----
 *
 * Once a frame while [0x708010] is 1, like 0x004648A0, over the slot under the cursor: the name's bytes at
 * 0x005206DC + 0x140 * [0x707BB0] + 0x20 * (cursor - 1) (the first), 0x005213A8 + 0x3C0 * [0x707BB8] + 0x20 *
 * (cursor - 1 - (cursor >= 0x11)) (the second); the file written by 0x004A709B / 0x004A6EDC (the original's code:
 * a service, EBX the slot).  The caller's registers matter: ECX (RETURN on an empty name restores the backup
 * and 0x0046C150 counts the name into ECX inside a PUSHAD / POPAD, so the count is lost and the cursor becomes
 * the caller's ECX + 1: the original's slip, kept), and EDI (the key 0x6E clears 0x1F bytes there). */
static uint32_t typing_name(vm *v, int setup)
{
    uint32_t cur = R(0x00707E91), idx = cur - 1u;
    if (!setup) return 0x005206DCu + 0x140u * R(0x00707BB0) + 0x20u * idx;
    if ((int32_t)cur >= 0x11) idx--;
    return 0x005213A8u + 0x3C0u * R(0x00707BB8) + 0x20u * idx;
}

/* 0x0046A240: the backup name (0x1F bytes from 0x0070820C) over the slot's, a NUL after */
static void typing_restore(vm *v, int setup)
{
    uint32_t d = typing_name(v, setup), k;
    for (k = 0; k < 0x1F; k++) vm_wr8(v, d + k, vm_rd8(v, 0x0070820Cu + k));
    vm_wr8(v, d + 0x1F, 0);
}

/* 0x0046A25A / 0x0046A299: the last letter off */
static void typing_back(vm *v, int setup)
{
    if (R(0x00707ED9) == 0) return;
    W(0x00707ED9, R(0x00707ED9) - 1u);
    vm_wr8(v, typing_name(v, setup) + R(0x00707ED9), 0);
}

static int typing(vm *v, int setup, const uint32_t in[8])
{
    uint32_t k[8] = { 0 }, al, c, r[8] = { 0 }, e, slot;
    if (svc(v, 0x00407CDCu, k) & FDC_ZF) goto end;
    al = k[VM_EAX] & 0xFFu;
    if (al == 1) { typing_restore(v, setup); W(0x00708010, 0); return 0; }
    if (al == 0x1C) {
        if (R(0x00707ED9) == 0) {
            typing_restore(v, setup);
            W(0x00707ED9, in[VM_ECX] + 1u);                    /* 0x0046C150's lost count */
        }
        W(0x00708010, 0);
        vm_wr8(v, typing_name(v, setup) + R(0x00707ED9), 0);
        if (setup) {
            slot = 0x1Eu * R(0x00707BB8) + R(0x00707E91) - 1u;
            if ((int32_t)R(0x00707E91) >= 0x11) slot--;
        } else slot = 10u * R(0x00707BB0) + R(0x00707E91) - 1u;
        r[VM_EBX] = slot;
        svc(v, setup ? 0x004A6EDCu : 0x004A709Bu, r);
        vm_wr8(v, typing_name(v, setup) + R(0x00707ED9), 0);
        W(0x00708010, 0);
        return 0;
    }
    if (al == 0x6E) {
        for (e = 0; e < 0x1F; e++) vm_wr8(v, in[VM_EDI] + e, 0);
        W(0x00707ED9, 0);
        goto end;
    }
    if (al == 0x0E) { typing_back(v, setup); goto end; }
    c = fdc_keychar(v, al) & 0xFFu;
    if (c == 0) goto end;
    if ((int32_t)R(0x00707ED9) > (int32_t)R(0x00707EDD)) goto end;
    e = typing_name(v, setup);
    vm_wr8(v, e + R(0x00707ED9), (uint8_t)c);
    W(0x00707ED9, R(0x00707ED9) + 1u);
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0x0051834Cu; r[VM_EBX] = e;
    svc(v, 0x004022D2u, r);
    if ((int32_t)r[VM_ECX] >= 0xEC) typing_back(v, setup);
end:
    {   /* the cursor: token 4 while [0x708314] is 1 (always, on the second screen); the NUL after the name */
        uint32_t ebx = typing_name(v, setup) + R(0x00707ED9);
        if (setup || R(0x00708314) == 1) ebx = slot_tokstr(v, 4, ebx);      /* the second screen's has no flag test */
        vm_wr8(v, ebx, 0);
    }
    return 0;
}

/* ---- a REPLAY slot's name typed: 0x0046C169 ----
 *
 * typing()'s third reader, once a frame while [0x708010] is 1: the name's bytes at 0x0052245C + 0x280 * [0x707BBC] +
 * 0x20 * (cursor - 1).  What differs from the other two, all read in the original:
 *  RETURN writes no file.  The screen's loop reads the slot afterwards, 10 * [0x707BBC] + cursor - 1 (TEN a page
 *     where the names go twenty a page: the original's slip), and the save copies the name of
 *     THAT slot into the file.  A RETURN on an empty name keeps the old one and the caller's ECX + 1 letters of it
 *     (0x0046C150's lost count, as the others);
 *  ESC puts the old name back and sets [0x70AB6E] = 1: the loop leaves with no slot;
 *  both end the frame there, with no cursor token after the name;
 *  BACKSPACE is 0x0046A25A, which clears its letter in the SVGAME names (0x005206DC + 0x140 * [0x707BB0] ...),
 *     not these: the replay name keeps the letter until the next one (or the cursor token) overwrites it, and the
 *     championship's name entry beside it loses a byte. */
static int rtyping(vm *v, const uint32_t in[8])
{
    uint32_t k[8] = { 0 }, al, c, r[8] = { 0 }, e, nm;
    nm = 0x0052245Cu + 0x280u * R(0x00707BBC) + 0x20u * (R(0x00707E91) - 1u);
    if (svc(v, 0x00407CDCu, k) & FDC_ZF) goto end;
    al = k[VM_EAX] & 0xFFu;
    if (al == 1) {
        for (e = 0; e < 0x1F; e++) vm_wr8(v, nm + e, vm_rd8(v, 0x0070820Cu + e));
        vm_wr8(v, nm + 0x1F, 0);
        W(0x00708010, 0);
        W(0x0070AB6E, 1);
        return 0;
    }
    if (al == 0x1C) {
        if (R(0x00707ED9) == 0) {
            for (e = 0; e < 0x1F; e++) vm_wr8(v, nm + e, vm_rd8(v, 0x0070820Cu + e));
            vm_wr8(v, nm + 0x1F, 0);
            W(0x00707ED9, in[VM_ECX] + 1u);
        }
        W(0x00708010, 0);
        vm_wr8(v, nm + R(0x00707ED9), 0);
        vm_wr8(v, nm + R(0x00707ED9), 0);
        W(0x00708010, 0);
        return 0;
    }
    if (al == 0x6E) {
        for (e = 0; e < 0x1F; e++) vm_wr8(v, in[VM_EDI] + e, 0);
        W(0x00707ED9, 0);
        goto end;
    }
    if (al == 0x0E) { typing_back(v, 0); goto end; }
    c = fdc_keychar(v, al) & 0xFFu;
    if (c == 0) goto end;
    if ((int32_t)R(0x00707ED9) > (int32_t)R(0x00707EDD)) goto end;
    vm_wr8(v, nm + R(0x00707ED9), (uint8_t)c);
    W(0x00707ED9, R(0x00707ED9) + 1u);
    memset(r, 0, sizeof r);
    r[VM_EAX] = 0x0051834Cu; r[VM_EBX] = nm;
    svc(v, 0x004022D2u, r);
    if ((int32_t)r[VM_ECX] >= 0xEC) typing_back(v, 0);
end:
    {
        uint32_t ebx = nm + R(0x00707ED9);
        if (R(0x00708314) == 1) ebx = slot_tokstr(v, 4, ebx);
        vm_wr8(v, ebx, 0);
    }
    return 0;
}

/* ---- the slot loads: 0x0046B312 (a svgame?? slot) and 0x0046BB78 (a carset?? slot) ----
 *
 * What they call and is not here is the original's code, a service answered by the host: the file reads
 * 0x004A7041 / 0x004A6E82 (CF = failure), the pickers' change handlers 0x0046857A / 0x004689A4 / 0x00468802 /
 * 0x004686CC (the cursor global [0x707E91] says which, EBX = 1), the arcade's start 0x004A60A7, the
 * championship's start 0x0049CB25 (the port takes over there), the error 0x0040EE7C.  The small ones
 * (0x0046E5DD, 0x0046D203, 0x0046E300, 0x0046DA16) and the log calls (no-ops in the host) are inline. */

static void copy_mem(vm *v, uint32_t dst, uint32_t src, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) vm_wr8(v, dst + k, vm_rd8(v, src + k));
}

/* ---- the slot files: 0x004A7041 / 0x004A6E82 read, 0x004A709B / 0x004A6EDC write ----
 *
 * Over the file layer's platform services, which stay services by decision (the platform layer is the remaster's):
 * 0x00402BCD open (EAX the name, EBX the mode -> EAX the handle, CF), 0x00402C22 read and 0x00402C3C write (EAX the
 * handle, EBX the buffer, ECX the count -> EAX the bytes, CF), 0x00402C08 close (EAX the handle, CF), and the clock
 * 0x00410D81 (-> EAX).  The name is built by 0x00403071 (`fel_numbered_path`, C): the template at [EAX] with the
 * slot's two digits, at 0x008F8550.  Each routine is PUSHAD ... POPAD: it returns the flags of its closing
 * `CMP EDX, 1` (EDX = 1 when it worked: ZF; else CF) and every register as it was given. */
enum { NAME_BUF = 0x008F8550 };

/* 0x0040299B with EDX = 0 (no seek): the file named at 0x008F8550 read into `buf`, at most `max` bytes; 1 = failure */
static int f40299B(vm *v, uint32_t buf, uint32_t max)
{
    uint32_t r[8] = { 0 }, h;
    int ok;
    r[VM_EAX] = NAME_BUF; r[VM_EBX] = 0; r[VM_ECX] = max;
    if (svc(v, 0x00402BCDu, r) & FDC_CF) return 1;
    h = r[VM_EAX];
    memset(r, 0, sizeof r);
    r[VM_EAX] = h; r[VM_EBX] = buf; r[VM_ECX] = max;
    ok = !(svc(v, 0x00402C22u, r) & FDC_CF);
    memset(r, 0, sizeof r);
    r[VM_EAX] = h;
    if (svc(v, 0x00402C08u, r) & FDC_CF) return 1;
    return !ok;
}

/* 0x004029E0: `count` bytes at `buf` written to the file named at 0x008F8550 (opened for writing); 1 = failure */
static int f4029E0(vm *v, uint32_t buf, uint32_t count)
{
    uint32_t r[8] = { 0 }, h;
    int ok;
    r[VM_EAX] = NAME_BUF; r[VM_EBX] = 1; r[VM_ECX] = count;
    if (svc(v, 0x00402BCDu, r) & FDC_CF) return 1;
    h = r[VM_EAX];
    memset(r, 0, sizeof r);
    r[VM_EAX] = h; r[VM_EBX] = buf; r[VM_ECX] = count;
    ok = !(svc(v, 0x00402C3Cu, r) & FDC_CF);
    memset(r, 0, sizeof r);
    r[VM_EAX] = h;
    if (svc(v, 0x00402C08u, r) & FDC_CF) return 1;
    return !ok;
}

static uint32_t stamp(vm *v)
{
    uint32_t r[8] = { 0 };
    svc(v, 0x00410D81u, r);
    return r[VM_EAX];
}

/* 0x004A7041 (slot EBX: the template [0x523223] numbered, 0x8000 bytes read to 0x005186DC; the magic 'R5SG' and
 * the version 0x00051122 checked; the name at +0x10 copied to 0x005206DC + 0x20 * slot) and 0x004A6E82 (the same
 * for the car setups: [0x0052323E], 0x48 bytes to 0x00521360, 'CRSU', 0x00050000, the name to 0x005213A8 + ...) */
static uint32_t slot_read(vm *v, uint32_t slot, int setup)
{
    const uint32_t tmpl = setup ? 0x0052323Eu : 0x00523223u, buf = setup ? 0x00521360u : 0x005186DCu;
    fel_numbered_path(v, tmpl, slot);
    if (!f40299B(v, buf, setup ? 0x48u : 0x8000u) &&
        R(buf) == (setup ? 0x55535243u : 0x47533552u) &&
        (R(buf + 8) ^ (setup ? 0x00050000u : 0x00051122u)) == 0) {
        copy_mem(v, (setup ? 0x005213A8u : 0x005206DCu) + slot * 0x20u, buf + 0x10u, 0x20);
        return FDC_ZF;
    }
    return FDC_CF;
}

/* 0x004A6F8B (EDI the buffer): the seven championship words, then for each of the four players its name copied
 * over the record's own (+0x124: from the pointer at +0x18) and the 0x28C-byte record, then the 0xB4 drivers' 0x94 bytes */
static void f4A6F8B(vm *v, uint32_t edi)
{
    static const uint32_t G[7] = { 0x007082F0, 0x007082B8, 0x007082C0, 0x007082C4, 0x007082F4, 0x00708038, 0x0070CAA8 };
    uint32_t k, esi;
    for (k = 0; k < 7; k++) { W(edi, R(G[k])); edi += 4; }
    for (k = 0; k < 4; k++) {
        esi = R(0x00708DE8u + 4u * k);
        copy_mem(v, esi + 0x124u, R(esi + 0x18u), 0x20);
        copy_mem(v, edi, esi, 0x28C);
        edi += 0x28Cu;
    }
    esi = 0x006EDFE8u;
    for (k = 0; k < 0xB4; k++) { copy_mem(v, edi, esi, 0x94); edi += 0x94u; esi += 0x94u; }
}

/* 0x004A709B (slot EBX: the head, the clock's stamp, the name copied from 0x005206DC + 0x20 * slot, the career's
 * state by 0x004A6F8B, the file written) and 0x004A6EDC (the car setup: the head 'CRSU', the stamp, the name, the
 * six values of the pickers and the player's record, 0x48 bytes) */
static uint32_t slot_write(vm *v, uint32_t slot, int setup)
{
    int bad;
    if (!setup) {
        W(0x005186DC, 0x47533552); W(0x005186E0, 0); W(0x005186E4, 0x00051122);
        W(0x005186E8, stamp(v));
        copy_mem(v, 0x005186ECu, 0x005206DCu + slot * 0x20u, 0x20);
        f4A6F8B(v, 0x0051870Cu);
        fel_numbered_path(v, 0x00523223u, slot);
        bad = f4029E0(v, 0x005186DCu, 0x8000u);
    } else {
        uint32_t P;
        W(0x00521360, 0x55535243); W(0x00521364, 0); W(0x00521368, 0x00050000);
        W(0x0052136C, stamp(v));
        copy_mem(v, 0x00521370u, 0x005213A8u + slot * 0x20u, 0x20);
        P = R(0x00708DE8u + 4u * R(0x007082F8));
        W(0x00521390, R(0x0070CC04)); W(0x00521394, R(P + 0x110)); W(0x00521398, R(0x0070CC10));
        W(0x0052139C, R(0x0070CC1C)); W(0x005213A0, R(P + 0x11C)); W(0x005213A4, R(0x0070CBF8));
        fel_numbered_path(v, 0x0052323Eu, slot);
        bad = f4029E0(v, 0x00521360u, 0x48u);
    }
    return bad ? FDC_CF : FDC_ZF;
}

int fdc_is_slot_file(uint32_t va)
{
    return HOST && (va == 0x004A7041u || va == 0x004A6E82u || va == 0x004A709Bu || va == 0x004A6EDCu);
}

uint32_t fdc_slot_file(vm *v, uint32_t va, const uint32_t r[8])
{
    switch (va) {
    case 0x004A7041u: return slot_read(v, r[VM_EBX], 0);
    case 0x004A6E82u: return slot_read(v, r[VM_EBX], 1);
    case 0x004A709Bu: return slot_write(v, r[VM_EBX], 0);
    default:          return slot_write(v, r[VM_EBX], 1);
    }
}

/* 0x0046E300 */
static void f46E300x(vm *v) { W(0x00707EC9, 0); W(0x00707ECD, 1); W(0x00708378, 0xFFFFFFFFu); }

static int f46B312(vm *v)
{
    uint32_t r[8] = { 0 }, esi, i, P, ebp, n = 0, slot;
    do {                                                       /* 0x0046E5DD: the players' counters */
        P = R(0x00708DE8u + 4u * n);
        W(P + 0xD0, 1); W(P + 0x88, 0); W(P + 0x94, 0);
        if (++n > SCAN_MAX) return FDC_FATAL;
    } while (n != R(0x007082F0));
    W(0x00708294, 1);
    W(0x00707BC4, 0);
    W(0x007082E0, 0);
    slot = R(0x00707BB0) * 10u + R(0x00707E91) - 1u;
    r[VM_EAX] = 0; r[VM_EBX] = slot;
    if (svc(v, 0x004A7041u, r) & FDC_CF) {                     /* no such slot: the error, back */
        memset(r, 0, sizeof r);
        r[VM_EAX] = 8;
        svc(v, 0x0040EE7Cu, r);
        f46E300x(v);
        return 0;
    }
    esi = 0x0051870Cu;
    W(0x007082F0, R(esi));       esi += 4;
    W(0x007082B8, R(esi));       esi += 4;
    W(0x007082C0, R(esi));       esi += 4;
    W(0x007082C4, R(esi));       esi += 4;
    W(0x007082F4, R(esi));       esi += 4;
    W(0x00708038, R(esi));       esi += 4;
    W(0x0070CAA8, R(esi));       esi += 4;
    W(0x00707ADC, esi);
    for (i = 0; i < 4; i++) {            /* each player: the slot's 0x28C bytes over the record, its name to the old name pointer */
        P = R(0x00708DE8u + 4u * i);
        ebp = R(P + 0x18);
        copy_mem(v, P, esi, 0x28C);
        copy_mem(v, ebp, P + 0x124u, 0x20);
        esi += 0x28Cu;
    }
    for (i = 0; i < 0xB4; i++) { copy_mem(v, 0x006EDFE8u + 0x94u * i, esi, 0x94); esi += 0x94u; }
    {
        int rc;
        uint32_t kind;
        if ((rc = f46D95B(v)) != 0) return rc;
        kind = R(0x00708358);
        if (kind == 4 || kind == 5) { W(0x007082C4, R(0x007082C4) + 1u); W(0x007082C0, 0); }
        else if (kind == 3) { f46E300x(v); return 0; }
        else if (kind == 1 || kind == 0) W(0x007082C0, R(0x007082C0) + 1u);
        else {                                                  /* 0x004D5437: the corrupt save's exit */
            W(0x004F0274, 0x80702821u);
            W(0x004F0278, 0x004DA197u);
            return FDC_FATAL;
        }
    }
    W(0x007082D8, 1);
    for (i = 0; i < 4; i++) {                                   /* 0x0046D203: the players' names */
        P = R(0x00708DE8u + 4u * i);
        W(P + 0x18, 0x007080ECu + 0x20u * i);
        copy_mem(v, P + 0x124u, 0x007080ECu + 0x20u * i, 0x20);
    }
    memset(r, 0, sizeof r);
    svc(v, R(0x00708038) == 0x13 ? 0x004A60A7u : 0x0049CB25u, r);
    f46E300x(v);
    return 0;
}

/* 0x0046BB78: the car setup loaded: the file (0x004A6E82, CF = failure: nothing), the six values into the
 * pickers' globals and the player record, the two lists of counters cleared, and each picker whose value
 * differs from its last ([0x70CBFC] / [0x70CC08] / [0x70CC14] / [0x70CC20] against [0x70CBF8] / [0x70CC04] /
 * [0x70CC10] / [0x70CC1C]) told to change, with the cursor [0x707E91] set to its item for the call */
static int f46BB78(vm *v)
{
    static const struct { uint32_t last, now, item, handler; } PK[4] = {
        { 0x0070CBFC, 0x0070CBF8, 0x0E, 0x0046857A }, { 0x0070CC08, 0x0070CC04, 0x0F, 0x004689A4 },
        { 0x0070CC14, 0x0070CC10, 0x12, 0x00468802 }, { 0x0070CC20, 0x0070CC1C, 0x13, 0x004686CC } };
    uint32_t r[8] = { 0 }, slot, P, p, n, cursor, k;
    slot = 0x1Eu * R(0x00707BB8) + R(0x00707E91) - 1u;
    if ((int32_t)R(0x00707E91) >= 0x11) slot--;
    r[VM_EAX] = 0; r[VM_EBX] = slot;
    if (svc(v, 0x004A6E82u, r) & FDC_CF) return 0;
    p = 0x0070BE7Au;                                            /* 0x0046DA16: zero up to the first -1 (the first word always) */
    n = 0;
    do {
        W(p, 0);
        p += 4;
        if (++n > SCAN_MAX) return FDC_FATAL;
    } while (R(p) != 0xFFFFFFFFu);
    P = R(0x00708DE8u + 4u * R(0x007082F8));
    W(0x0070CC04, R(0x00521360u + 0x30));
    W(P + 0x110, R(0x00521360u + 0x34));
    W(0x0070CC10, R(0x00521360u + 0x38));
    W(0x0070CC1C, R(0x00521360u + 0x3C));
    W(P + 0x11C, R(0x00521360u + 0x40));
    W(0x0070CBF8, R(0x00521360u + 0x44));
    for (k = 0; k < 2; k++) {                                   /* the two lists, each to its -1 */
        p = k ? 0x0070BECEu : 0x0070BE7Au;
        for (n = 0; R(p) != 0xFFFFFFFFu; p += 4) {
            W(p, 0);
            if (++n > SCAN_MAX) return FDC_FATAL;
        }
    }
    cursor = R(0x00707E91);
    for (k = 0; k < 4; k++) {
        if (R(PK[k].last) == R(PK[k].now)) continue;
        W(0x00707E91, PK[k].item);
        memset(r, 0, sizeof r);
        r[VM_EBX] = 1;
        svc(v, PK[k].handler, r);
    }
    W(0x00707E91, cursor);
    f46E300x(v);
    return 0;
}

int fdc_is_typing(uint32_t va) { return HOST && (va == 0x0046A2E2u || va == 0x0046BE55u || va == 0x0046C169u); }
int fdc_typing(vm *v, uint32_t va, const uint32_t r[8])
{
    if (va == 0x0046C169u) return rtyping(v, r);                 
    return typing(v, va == 0x0046BE55u, r);
}

/* ---- the table ---- */

static const struct { uint32_t va; fdc_fn fn; int host; } TAB[] = {
    { 0x0046B312, f46B312, 1 }, { 0x0046BB78, f46BB78, 1 },
    { 0x0046A4CC, f46A4CC, 1 }, { 0x0046A544, f46A544, 1 }, { 0x0046AF0F, f46AF0F, 1 },
    { 0x0046C4F6, f46C4F6, 1 }, { 0x0046CE81, f46CE81, 1 }, { 0x004759BE, f4759BE, 1 },
    { 0x00468114, f468114, 1 }, { 0x0046A1CE, f46A1CE, 0 }, { 0x004664C9, f4664C9, 0 },
    { 0x00415A0C, f415A0C, 1 }, { 0x0047949B, f47949B, 1 },
    { 0x004648A0, f4648A0, 1 },
    { 0x0046F209, f46F209, 0 }, { 0x0046D70C, f46D70C, 0 }, { 0x0046D95B, f46D95B, 0 },
    { 0x004A65C9, f4A65C9, 0 },
    { 0x0047B4FA, f47B4FA, 1 }, { 0x0047B4D2, f47B4D2, 1 }, { 0x0046C40F, f46C40F, 1 },
    { 0x0046C745, f46C745, 1 }, { 0x0046C77E, f46C77E, 1 },
    { 0x0046A080, f46A080, 0 }, { 0x0046A098, f46A098, 0 }, { 0x00469EBF, f469EBF, 0 },
    { 0x0046C6B1, f46C6B1, 1 },
    { 0x0046C377, f46C377, 0 }, { 0x0046D166, f46D166, 0 },       /* the replay slot screen */
};

fdc_fn fdc_lookup(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return TAB[i].host && !HOST ? NULL : TAB[i].fn;
    return NULL;
}

int fdc_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fdc_va(int i) { return TAB[i].va; }
int fdc_uses_host(int i) { return TAB[i].host; }
