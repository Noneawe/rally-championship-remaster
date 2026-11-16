/* fd_stage.c - see fd_stage.h. family C, 2026-11-09.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps as
 * the original's does.  A handler is written over a small context (`cx`): the
 * VM, the draw layer, and a result code.  The original's text helpers (the
 * 0x00401xxx routines that append to a string at EBX) are C functions over the
 * same cursor. */
#include "fd_stage.h"

#include "fe_champ.h"
#include "fe_logic.h"

typedef struct { vm *v; const fdsg_draw *d; int rc; } cx;

#define R(a)    vm_rd32(c->v, (a))
#define W(a, x) vm_wr32(c->v, (a), (x))
#define RB(a)   vm_rd8(c->v, (a))
#define WB(a, x) vm_wr8(c->v, (a), (uint8_t)(x))

enum {
    A_PLAYERS = 0x00708DE8,
    A_STR     = 0x00A2BF54,     /* the text buffer every panel builds its strings in */
    A_STR2    = 0x008E80D0,     /* the token-run buffer 0x00402262 / 0x004022F3 build in */
    A_ITINS   = 0x006E7ECD,
    F_A = 0x005180AC, F_B = 0x005180CC, F_C = 0x005180EC,
    F_L = 0x0051804C, F_M = 0x0051806C,
    F_RES1 = 0x005181EC, F_RES2 = 0x0051820C
};

/* ---- the draw layer: one call, the original's registers ------------------ */
static void svc(cx *c, uint32_t va, uint32_t r[8]) { c->d->call(c->d->ud, va, r); }

/* 0x00402311: EBX the font record, EAX the token, ECX x, EDX y */
static void p_tok(cx *c, uint32_t font, uint32_t token, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = font; r[VM_ECX] = x; r[VM_EDX] = y;
    svc(c, 0x00402311u, r);
}
/* 0x0040239C: EAX the font record, EBX the string, ECX x, EDX y */
static void p_text(cx *c, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = font; r[VM_EBX] = str; r[VM_ECX] = x; r[VM_EDX] = y;
    svc(c, 0x0040239Cu, r);
}
/* 0x004066F9: EAX the sprite, ECX x, EDX y, ESI the bundle [0x00707EED] */
static void p_blit(cx *c, uint32_t sprite, uint32_t x, uint32_t y)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = sprite; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_ESI] = R(0x00707EEDu);
    svc(c, 0x004066F9u, r);
}
/* 0x00401F8F: the token's text at EBX; EBX advanced (EAX is kept) */
static uint32_t p_tokstr(cx *c, uint32_t token, uint32_t ebx)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = ebx;
    svc(c, 0x00401F8Fu, r);
    return r[VM_EBX];
}

static uint32_t rd16(cx *c, uint32_t a) { return (uint32_t)RB(a) | ((uint32_t)RB(a + 1u) << 8); }

/* ---- the text cursor's one-byte routines ---------------------------------- */
static uint32_t put(cx *c, uint32_t ebx, uint8_t ch) { WB(ebx, ch); return ebx + 1; }
static uint32_t nul(cx *c, uint32_t ebx)    { return put(c, ebx, 0); }      /* 0x00401F02 */
static uint32_t space(cx *c, uint32_t ebx)  { return put(c, ebx, 0x20); }   /* 0x00401F15 */
static uint32_t colon(cx *c, uint32_t ebx)  { return put(c, ebx, 0x3A); }   /* 0x00401F1F */
static uint32_t slash(cx *c, uint32_t ebx)  { return put(c, ebx, 0x2F); }   /* 0x00401F2E */
static uint32_t dot(cx *c, uint32_t ebx)    { return put(c, ebx, 0x2E); }   /* 0x00401F33 */

/* 0x00402011 */
static uint32_t fmt(cx *c, uint32_t val, uint32_t flags, uint32_t ebx)
{
    return fel_fmt_num(c->v, val, flags, ebx);
}
/* 0x00401F51: EAX, '/', EDX */
static uint32_t of_n(cx *c, uint32_t eax, uint32_t edx, uint32_t ebx)
{
    ebx = fmt(c, eax, 0, ebx);
    ebx = slash(c, ebx);
    return fmt(c, edx, 0, ebx);
}
/* 0x004019A7: centiseconds-as-minutes to seconds: EAX / 100 * 60 + EAX % 100 */
static uint32_t t4019a7(uint32_t eax) { return eax / 100u * 60u + eax % 100u; }
/* 0x004019D7: EAX seconds as m:ss */
static uint32_t t4019d7(cx *c, uint32_t eax, uint32_t ebx)
{
    ebx = fmt(c, eax / 60u, 0, ebx);
    ebx = colon(c, ebx);
    return fmt(c, eax % 60u, 0x12, ebx);
}
/* 0x00401A25: EAX a time in 1/100 s: [hh:]mm:ss.cc */
static uint32_t t401a25(cx *c, uint32_t eax, uint32_t ebx)
{
    if (R(0x004DB144u) == 1 || eax >= 0x57E40u) {
        ebx = fmt(c, eax / 0x57E40u, 0x12, ebx);
        ebx = colon(c, ebx);
        eax %= 0x57E40u;
    }
    ebx = fmt(c, eax / 0x1770u, 0x12, ebx);
    ebx = colon(c, ebx);
    eax %= 0x1770u;
    ebx = fmt(c, eax / 100u, 0x12, ebx);
    ebx = dot(c, ebx);
    return fmt(c, eax % 100u, 0x12, ebx);
}
/* 0x004019CD */
static uint32_t t4019cd(cx *c, uint32_t eax, uint32_t ebx) { return fmt(c, eax, 0, ebx); }

/* ---- the screens' own small helpers ---------------------------------------- */

/* 0x004022D2 measured through 0x004022F3: the token EAX's text in the record
 * EBX, measured; ECX the width, EDX the height */
static void measure_tok(cx *c, uint32_t token, uint32_t font, uint32_t *ecx, uint32_t *edx)
{
    uint32_t r[8] = { 0 }, b = p_tokstr(c, token, A_STR2);
    nul(c, b);
    r[VM_EAX] = font; r[VM_EBX] = A_STR2;
    svc(c, 0x004022D2u, r);
    *ecx = r[VM_ECX]; *edx = r[VM_EDX];
}

/* 0x00406457 / 0x004064F6: a row / a column of colour EAX */
static void line(cx *c, uint32_t va, uint32_t col, uint32_t x, uint32_t y, uint32_t len)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = col; r[VM_ECX] = x; r[VM_EDX] = y; r[VM_EBX] = len;
    svc(c, va, r);
}
/* 0x00406426: the outline of the box at EBX {x, y, w, h}, colour EAX, offset ECX / EDX */
static void box_outline(cx *c, uint32_t col, uint32_t box, uint32_t x, uint32_t y)
{
    uint32_t w = R(box + 8), h = R(box + 0xC), ecx = x + R(box), edx = y + R(box + 4);
    line(c, 0x00406457u, col, ecx, edx, w);
    edx += h; edx--;
    line(c, 0x00406457u, col, ecx, edx, w);
    h -= 2u;
    edx -= h;
    line(c, 0x004064F6u, col, ecx, edx, h);
    ecx += w; ecx--;
    line(c, 0x004064F6u, col, ecx, edx, h);
}

/* 0x00415A0C: the notice box (the box, its outline, its text) */
static void f415a0c(cx *c)
{
    uint32_t ecx, edx, r[8] = { 0 }, x;
    measure_tok(c, 0x1FEu, 0x0051834Cu, &ecx, &edx);
    ecx += 0x32u; edx += 0x19u;
    W(0x0052336Cu, ecx); W(0x00523370u, edx);
    W(0x00523364u, (uint32_t)((int32_t)(R(0x004EF974u) - ecx) >> 1));
    W(0x00523368u, (uint32_t)((int32_t)(R(0x004EF978u) - edx) >> 1));
    r[VM_EAX] = 1; r[VM_EBX] = 0x00523364u; r[VM_ESI] = R(0x005179D8u);
    svc(c, 0x004065B0u, r);
    box_outline(c, 0xA, 0x00523364u, 0, 0);
    x = (uint32_t)((int32_t)R(0x004EF974u) >> 1);
    p_tok(c, 0x0051842Cu, 0x1FEu, x,
          (uint32_t)((int32_t)(R(0x004EF978u) - R(0x0051842Cu + 8)) >> 1) - 1u);
}

/* 0x00486DB0: SELECT LEVEL's lock notice */
static int f486db0(cx *c)
{
    uint32_t eax;
    if (R(0x0070AA78u) & 1u) return 0;
    eax = R(0x00707E91u);
    if (eax == 7 || eax == 1) return 0;
    if (RB(eax - 2u + 0x0070AB68u) == 1) return 0;
    f415a0c(c);
    return 0;
}

/* 0x0047C0F5: how many entries (kind 0) follow the current stage in the itinerary */
static int f47c0f5(cx *c)
{
    int rc;
    uint32_t edi = fec_46ebbc(c->v, 0x006E7EE9u, R(0x007082C0u), R(0x007082C4u), &rc), ecx = 0;
    if (rc) return FDSG_FATAL;
    for (;;) {
        edi += 0xA4u;
        if (R(edi) != 0) break;
        ecx++;
        if (ecx > 100000u) return FDSG_FATAL;
    }
    W(0x007082D0u, ecx);
    return 0;
}

/* a name copy: 0x1F bytes from `src` to `dst` (the byte loops in the score tables) */
static void copy31(cx *c, uint32_t dst, uint32_t src)
{
    uint32_t k;
    for (k = 0; k < 0x1F; k++) WB(dst + k, RB(src + k));
}
static void swap_w(cx *c, uint32_t a, uint32_t b)
{
    uint32_t x = R(a), y = R(b);
    W(a, y); W(b, x);
}

/* 0x0048629F: the new TOP SCORES row bubbled up (signed: a row swaps with the
 * one above it when its score is >= theirs); nine passes up the table */
static void f48629f(cx *c)
{
    uint32_t esi = R(0x00708044u) + R(0x0070AB54u) * 0x320u + 0x2D0u, edi = esi - 0x50u, k;
    static const uint8_t F[] = { 0xC, 0x14, 4, 8, 0x1C, 0x10, 0x18, 0x40 };
    for (k = 9; k; k--) {
        if ((int32_t)R(esi + 0x14) >= (int32_t)R(edi + 0x14)) {
            unsigned j;
            for (j = 0; j < sizeof F; j++) swap_w(c, esi + F[j], edi + F[j]);
            copy31(c, 0x007081ECu, edi + 0x20);
            copy31(c, edi + 0x20, esi + 0x20);
            copy31(c, esi + 0x20, 0x007081ECu);
        }
        edi -= 0x50u; esi -= 0x50u;
    }
}

/* 0x00486030: TOP SCORES' new entry */
static int f486030(cx *c)
{
    uint32_t edi = R(0x00708044u) + R(0x0070AB54u) * 0x320u + 0x2D0u;
    uint32_t esi = R(A_PLAYERS + 4u * R(0x007082F8u)), eax = R(0x0070AB20u);
    if (R(0x007082F8u) == 1) eax = R(0x0070AB24u);
    W(edi + 0x14, eax);
    W(edi + 0x1C, R(esi + 0x90));
    W(edi + 8, R(esi + 4));
    W(edi + 0xC, R(esi + 0x18));
    W(edi + 0x10, R(esi + 8));
    W(edi + 4, 1);
    W(edi + 0x40, 1);
    W(edi + 0x20, R(esi + 0x18));
    copy31(c, edi + 0x20, R(edi + 0xC));
    f48629f(c);
    return 0;
}

/* 0x00462B21: the string at EBX drawn a character at a time, eight, from
 * x = ECX - 0x64 at the original's spacing, through the scratch string at
 * EBX + 0x40 (so its first character and the NUL are left there) */
static void f462b21(cx *c, uint32_t font, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    static const uint8_t STEP[] = { 0, 0xD, 0xA, 0xA, 0xD, 0xA, 0xA, 0xD };
    uint32_t esi = ebx, buf = ebx + 0x40u, k;
    ecx -= 0x64u;
    for (k = 0; k < 8; k++) {
        ecx += STEP[k];
        WB(buf, RB(esi)); esi++;
        WB(buf + 1, 0);
        p_text(c, font, buf, ecx, edx);
    }
}

/* the line under the heading, shared by 0x00486ADB, 0x00488FBA and 0x00463A4F:
 * "<token> n/6: <the stage's name>" for the itinerary entry `entry` */
static int stage_line(cx *c, uint32_t token, uint32_t entry)
{
    uint32_t b, rec;
    int rc;
    b = p_tokstr(c, token, A_STR);
    b = space(c, b);
    b = of_n(c, R(entry + 0x40), 6, b);
    b = colon(c, b);
    b = space(c, b);
    rec = fec_46f686(c->v, entry, NULL, &rc);
    if (rc) return FDSG_FATAL;
    b = p_tokstr(c, R(rec), b);
    nul(c, b);
    p_text(c, 0x005184CCu, A_STR, 0x2EB, 0x91);
    return 0;
}

/* 0x00486ADB: ARCADE BEST TIMES' heading (the rally block [0x70AB54]'s table) */
static int f486adb(cx *c)
{
    uint32_t base = R(A_ITINS + 4u * R(0x0070AB54u));
    p_tok(c, 0x005184CCu, R(base + 0x38), 0x2EB, 0x77);
    return stage_line(c, 0x482, base + R(0x0070AB58u) * 0xA4u);
}
/* 0x00488FBA: TIME TRIAL BEST TIMES' heading ([0x708230] / [0x708238] / [0x708234]) */
static int f488fba(cx *c)
{
    uint32_t base = R(R(0x006E7E85u + 4u * R(0x00708230u)) + 4u * R(0x00708238u));
    p_tok(c, 0x005184CCu, R(base + 0x38), 0x2EB, 0x77);
    return stage_line(c, 0x482, base + R(0x00708234u) * 0xA4u);
}
/* 0x00463A4F: the same line for the entry in EDI (the caller's), token 0x481 */
static int f463a4f(cx *c, uint32_t edi) { return stage_line(c, 0x481, edi); }

/* the ten rows of a table of scores / times.  Each row: the name (`fn`, `fx`),
 * the value formatted by `fmtv`, the rank, and two sprites; `stride` is the
 * row's size.  Shared shape of 0x00486B6C, 0x00486939 and 0x00488E1A:
 *   kind 0  TOP SCORES: value by 0x004019CD at x = 0x21C, y + 4 for the sprites
 *   kind 1  BEST TIMES: value by 0x00401A25, drawn spaced by 0x00462B21 at 0x276
 *   kind 2  the time trial's: as 1, the sprites at the row's own y */
static int rows(cx *c, int kind, uint32_t edi, uint32_t stride)
{
    uint32_t ecx = 1, edx = 0xF0, b;
    for (;;) {
        uint32_t fa, fb, fc, sp;
        int mark = R(edi + 0x40) == 1;
        W(0x00708318u, ecx);
        fa = mark ? F_L : F_A;         /* the name and value */
        fb = mark ? 0x0051802Cu : 0x0051808Cu;
        fc = mark ? F_M : F_B;         /* the rank */
        p_text(c, fa, edi + 0x20, 0xF0, edx);
        b = A_STR;
        b = kind == 0 ? t4019cd(c, R(edi + 0x14), b) : t401a25(c, R(edi + 0x14), b);
        nul(c, b);
        if (kind == 0) p_text(c, fa, A_STR, 0x21C, edx);
        else f462b21(c, fb, A_STR, 0x276, edx);
        b = A_STR;
        b = t4019cd(c, R(0x00708318u), b);
        nul(c, b);
        p_text(c, fc, A_STR, 0xC8, edx);
        if (kind == 2) sp = edx;
        else { W(0x0070831Cu, edx); W(0x0070831Cu, R(0x0070831Cu) + 4u); sp = R(0x0070831Cu); }
        p_blit(c, 0x1Cu + R(edi + 8), 0xD2, sp);
        W(0x00708320u, 0x44u + R(edi + 0x10));
        p_blit(c, R(0x00708320u), 0x1CC, sp);
        edi += stride;
        if (R(edi) == 0xFFFFFFFFu) return FDSG_FATAL;   /* the original leaves a stray push: its RET lands on EAX */
        edx += 0x19u;
        ecx++;
        if (ecx == 0xBu) return 0;
    }
}

/* 0x00486B6C: ARCADE TOP SCORES' rows */
static int f486b6c(cx *c)
{
    return rows(c, 0, R(0x00708044u) + R(0x0070AB54u) * 0x320u, 0x50u);
}
/* 0x00486939: ARCADE BEST TIMES' heading and rows */
static int f486939(cx *c)
{
    uint32_t edi = R(A_ITINS + 4u * R(0x0070AB54u)) + R(0x0070AB58u) * 0xA4u;
    int rc = f486adb(c);
    if (rc) return rc;
    return rows(c, 1, R(0x00708048u) + R(edi + 0x3C) * 0x618u, 0x9Cu);
}
/* 0x00488E1A: TIME TRIAL BEST TIMES' heading and rows */
static int f488e1a(cx *c)
{
    uint32_t edi = R(R(0x006E7E85u + 4u * R(0x00708230u)) + 4u * R(0x00708238u)) + R(0x00708234u) * 0xA4u;
    uint32_t blk = R(0x0070804Cu);
    int rc = f488fba(c);
    if (rc) return rc;
    if (R(0x00708230u) != 1) blk = R(0x00708050u);
    return rows(c, 2, blk + R(edi + 0x3C) * 0x618u, 0x9Cu);
}

/* ---- more of the text routines -------------------------------------------- */

/* 0x00401991: Celsius to Fahrenheit, signed: EAX * 9 / 5 + 32 (the original's IDIV
 * would fault on a quotient that does not fit; the VM truncates, so does this) */
static uint32_t t401991(uint32_t eax) { return (uint32_t)((int64_t)(int32_t)eax * 9 / 5) + 0x20u; }
/* 0x0040190A: (EAX * 0x3EDD + 0x1388) / 0x2710 (a unit conversion; the 64-bit sum, the DIV's
 * quotient truncated to 32 bits) */
static uint32_t t40190a(uint32_t eax) { return (uint32_t)(((uint64_t)eax * 0x3EDDu + 0x1388u) / 0x2710u); }

/* 0x00401AB7: EAX and '%' */
static uint32_t t401ab7(cx *c, uint32_t eax, uint32_t ebx)
{
    return put(c, fmt(c, eax, 0, ebx), 0x25);
}
/* 0x00401AC5: a signed value with the unit's token ([0x517954]: 0 shows Fahrenheit) */
static uint32_t t401ac5(cx *c, uint32_t eax, uint32_t ebx)
{
    uint32_t u = R(0x00517954u);
    if (u == 0) eax = t401991(eax);
    if ((int32_t)eax < 0) { eax = 0u - eax; ebx = put(c, ebx, 0x2D); }
    ebx = fmt(c, eax, 0, ebx);
    u = R(0x00517954u);
    return p_tokstr(c, (uint32_t)rd16(c, 0x005186ACu + 2u * u), ebx);
}
/* 0x00401AFC: a value in tenths ("n.m"), with the unit's token when ECX is not 0 */
static uint32_t t401afc(cx *c, uint32_t eax, uint32_t ecx, uint32_t ebx)
{
    if (R(0x00517954u) != 0) eax = t40190a(eax);
    ebx = fmt(c, eax / 10u, 0, ebx);
    ebx = dot(c, ebx);
    ebx = fmt(c, eax % 10u, 0, ebx);
    if (ecx) ebx = p_tokstr(c, (uint32_t)rd16(c, 0x005186B0u + 2u * R(0x00517954u)), ebx);
    return ebx;
}
/* 0x00401B73: a value in hundredths ("n.mm") */
static uint32_t t401b73(cx *c, uint32_t eax, uint32_t ebx)
{
    if (R(0x00517954u) != 0) eax = t40190a(eax);
    ebx = fmt(c, eax / 100u, 0, ebx);
    ebx = dot(c, ebx);
    return fmt(c, eax % 100u, 0x12, ebx);
}

/* ---- the panels: SELECT STAGE, VIEW STAGE (two), the championship's and the
 * arcade's NEXT STAGE.  One body, five prologues and trailers (a diff of the five
 * disassemblies: every difference is the code before the body, the [0x70CC3C]
 * store, the font of two labels, and the best-time row at the end) */

/* 0x00469CF8: the two sprites at the positions the entry in EDI holds */
static void f469cf8(cx *c, uint32_t edi)
{
    W(0x00708384u, R(edi + 0x44)); W(0x00708388u, R(edi + 0x48));
    p_blit(c, 0x9C, R(0x00708384u), R(0x00708388u));
    W(0x00708384u, R(edi + 0x4C)); W(0x00708388u, R(edi + 0x50));
    p_blit(c, 0x9D, R(0x00708384u), R(0x00708388u));
}

/* 0x00402262: EAX, EAX + 1, ... (N tokens) as text at 0x008E80D0 */
static uint32_t token_run(cx *c, uint32_t eax, uint32_t n)
{
    uint32_t b = A_STR2;
    for (; n; n--, eax++) b = p_tokstr(c, eax, b);
    nul(c, b);
    return A_STR2;
}

/* the heading's string, built in the second buffer (0x00A2C154) and not drawn here:
 * 0x00463A4F's line with the 0x482 token */
static int inline_heading(cx *c, uint32_t entry)
{
    uint32_t b = 0x00A2C154u, rec;
    int rc;
    b = p_tokstr(c, 0x482, b);
    b = space(c, b);
    b = of_n(c, R(entry + 0x40), 6, b);
    b = colon(c, b);
    b = space(c, b);
    rec = fec_46f686(c->v, entry, NULL, &rc);
    if (rc) return FDSG_FATAL;
    b = p_tokstr(c, R(rec), b);
    nul(c, b);
    return 0;
}

/* the best-time row's service (the host skips it when the block is not loaded) */
static void best_row(cx *c, uint32_t va, uint32_t edi)
{
    uint32_t r[8] = { 0 };
    r[VM_EDI] = edi;
    svc(c, va, r);
}

/* the string built at EBX ended and drawn at (x, y) in `font` */
static void value_text(cx *c, uint32_t font, uint32_t b, uint32_t x, uint32_t y)
{
    nul(c, b);
    p_text(c, font, A_STR, x, y);
}
static void value_line(cx *c, uint32_t b, uint32_t x, uint32_t y) { value_text(c, F_A, b, x, y); }

static int panel_body(cx *c, uint32_t entry, int store_cc3c, uint32_t f_label)
{
    uint32_t b, rec, eax;
    int rc;
    f469cf8(c, entry);
    p_tok(c, F_B, 0x34D, 0x211, 0xD1);
    value_line(c, t4019d7(c, t4019a7(R(entry + 0x10)), A_STR), 0x215, 0xD1);
    W(0x00708358u, R(entry + 0x10));
    rec = fec_46f686(c->v, entry, NULL, &rc);
    if (rc) return FDSG_FATAL;
    W(0x0070835Cu, R(0x0070C49Au + 4u * R(rec + 8)) + R(0x00708358u));
    value_line(c, t4019d7(c, t4019a7(R(0x0070835Cu)), A_STR), 0x2BC, 0xD1);
    p_tok(c, F_B, 0x34E, 0x2B8, 0xD1);
    W(0x00708358u, R(0x0070C49Au + 4u * R(rec + 8)));
    value_line(c, t4019d7(c, t4019a7(R(0x00708358u)), A_STR), 0x215, 0xE8);
    p_tok(c, F_B, 0x356, 0x211, 0xE8);
    if (store_cc3c) W(0x0070CC3Cu, R(rec + 0x34));
    b = A_STR;
    eax = R(rec + 0xC);
    if (eax == 0) b = nul(c, b);
    else {
        b = t401b73(c, eax, b);
        b = p_tokstr(c, R(0x00517954u) ? 0x570u : 0x56Fu, b);
    }
    value_line(c, b, 0x2BC, 0xE8);
    p_tok(c, F_B, 0x34F, 0x2B8, 0xE8);
    p_tok(c, f_label, 0x345, 0x266, 0x107);
    value_line(c, t401ab7(c, R(rec + 0x24), A_STR), 0x215, 0x120);
    value_line(c, t401ab7(c, R(rec + 0x2C), A_STR), 0x2BC, 0x120);
    value_line(c, t401ab7(c, R(rec + 0x28), A_STR), 0x215, 0x137);
    value_line(c, t401ab7(c, R(rec + 0x30), A_STR), 0x2BC, 0x137);
    p_tok(c, F_B, 0x346, 0x211, 0x120);
    p_tok(c, F_B, 0x347, 0x2B8, 0x120);
    p_tok(c, F_B, 0x348, 0x211, 0x137);
    p_tok(c, F_B, 0x349, 0x2B8, 0x137);
    p_tok(c, f_label, 0x354, 0x266, 0x156);
    value_line(c, t401ac5(c, R(rec + 0x1C), A_STR), 0x215, 0x16F);
    p_tok(c, F_B, 0x355, 0x211, 0x16F);
    value_line(c, t401afc(c, R(rec + 0x20), 1, A_STR), 0x2BC, 0x16F);
    p_tok(c, F_B, 0x352, 0x2B8, 0x16F);
    p_tok(c, F_A, R(rec + 0x14), 0x198, 0x186);
    p_text(c, 0x00517F0Cu, token_run(c, R(rec + 0x10), 3), 0x198, 0x1BD);
    W(0x00708358u, R(rec + 0x38));
    b = p_tokstr(c, R(rec + 0x3C) == 1 ? 0x358u : 0x359u, A_STR);
    b = space(c, b);
    b = t4019cd(c, R(0x00708358u), b);
    nul(c, b);
    p_text(c, 0x00517EECu, A_STR, 0xC0, 0x206);
    return 0;
}

/* the trailer that tests the block pointers, SELECT STAGE's and the time trial's */
static void trailer_guarded(cx *c, uint32_t entry)
{
    uint32_t off = R(entry + 0x3C) * 0x618u, blk = R(0x00708048u);
    if (blk == 0) return;
    if (R(0x00708230u) != 1) blk = R(0x00708040u);
    if (blk == 0) return;
    best_row(c, 0x0047091Eu, blk + off);
}

/* 0x0047DB0E: SELECT STAGE's panel */
static int f47db0e(cx *c)
{
    uint32_t k = R(0x00708230u), item = R(0x00707EF5u) + 4u, base, entry;
    int rc;
    W(item + 0x14, R(0x0070AA80u + 4u * k));
    W(0x0070CC58u, k);
    base = R(R(0x006E7E85u + 4u * k) + 4u * R(0x00708238u));
    W(item + 0x5Cu + 0x14, R(base + 0x38));
    entry = base + R(0x00708234u) * 0xA4u;
    if ((rc = inline_heading(c, entry)) != 0) return rc;
    if ((rc = panel_body(c, entry, 1, F_C)) != 0) return rc;
    trailer_guarded(c, entry);
    return 0;
}

/* 0x0048300D: the time trial's VIEW STAGE panel */
static int f48300d(cx *c)
{
    uint32_t k = R(0x00708230u), base, entry;
    int rc;
    p_tok(c, 0x005184CCu, R(0x0070AA80u + 4u * k), 0x2EB, 0x77);
    W(0x0070CC58u, k);
    base = R(R(0x006E7E85u + 4u * k) + 4u * R(0x00708238u));
    p_tok(c, 0x005184CCu, R(base + 0x38), 0x2EB, 0x94);
    entry = base + R(0x00708234u) * 0xA4u;
    if ((rc = inline_heading(c, entry)) != 0) return rc;
    p_text(c, 0x005184CCu, 0x00A2C154u, 0x2EB, 0xB1);
    if ((rc = panel_body(c, entry, 1, F_C)) != 0) return rc;
    trailer_guarded(c, entry);
    return 0;
}

/* the itinerary's current stage: 0x0046EBBC from the start, EBX = [0x7082C4], EAX = 0 (the
 * first stage of the rally) and EAX = [0x7082C0] (the current one) */
static int itin_pair(cx *c, uint32_t *first, uint32_t *cur)
{
    int rc;
    *first = fec_46ebbc(c->v, 0x006E7EE9u, 0, R(0x007082C4u), &rc);
    if (rc) return FDSG_FATAL;
    *cur = fec_46ebbc(c->v, 0x006E7EE9u, R(0x007082C0u), R(0x007082C4u), &rc);
    return rc ? FDSG_FATAL : 0;
}
/* the title token of a VIEW STAGE / NEXT STAGE (a leg's own, or the stage entry's) */
static void view_title(cx *c, uint32_t first)
{
    if (R(0x00708038u) == 0x13)
        p_tok(c, 0x005184CCu, R(0x0070C2AEu + 4u * R(0x007082C4u)), 0x2EB, 0x77);
    else
        p_tok(c, 0x005184CCu, R(first + 0x38), 0x2EB, 0x77);
}
static void view_trailer(cx *c, uint32_t entry)
{
    best_row(c, 0x0047091Eu, R(0x00708040u) + R(entry + 0x3C) * 0x618u);
}

/* 0x0047C127: VIEW STAGE's panel */
static int f47c127(cx *c)
{
    uint32_t first, cur;
    int rc = itin_pair(c, &first, &cur);
    if (rc) return rc;
    view_title(c, first);
    if (R(0x00708038u) == 2 && R(0x007082D0u) == 0 && (rc = f463a4f(c, cur)) != 0) return rc;
    if ((rc = inline_heading(c, cur)) != 0) return rc;
    if ((rc = panel_body(c, cur, 0, F_C)) != 0) return rc;
    view_trailer(c, cur);
    return 0;
}

/* 0x00470443: the championship's NEXT STAGE panel */
static int f470443(cx *c)
{
    uint32_t first, cur;
    int rc = itin_pair(c, &first, &cur);
    if (rc) return rc;
    view_title(c, first);
    if ((rc = f463a4f(c, cur)) != 0) return rc;
    if ((rc = panel_body(c, cur, 0, F_C)) != 0) return rc;
    view_trailer(c, cur);
    return 0;
}

/* 0x00485AB9: the arcade's NEXT STAGE panel */
static int f485ab9(cx *c)
{
    uint32_t entry = R(A_ITINS + 4u * R(0x0070AB54u)) + R(0x0070AB58u) * 0xA4u;
    int rc = f486adb(c);
    if (rc) return rc;
    if ((rc = panel_body(c, entry, 1, 0x0051808Cu)) != 0) return rc;
    best_row(c, 0x00470AFAu, R(0x00708048u) + R(entry + 0x3C) * 0x618u);
    return 0;
}

/* ---- the results screens ----------------------------------------------------- */

/* 0x00464958: EDI = the current player's name (the pointer at +0x18) */
static uint32_t f464958(cx *c) { return R(R(A_PLAYERS + 4u * R(0x007082F8u)) + 0x18u); }
static uint32_t cur_player(cx *c) { return R(A_PLAYERS + 4u * R(0x007082F8u)); }

/* 0x004854A1: the arcade RESULTS' draw */
static int f4854a1(cx *c)
{
    static const struct { uint32_t tok, x, y; } LBL[] = {
        { 0x20E, 0x155, 0x12B }, { 0x20F, 0x155, 0x142 }, { 0x208, 0x155, 0x159 },
        { 0x210, 0x155, 0x187 }, { 0x211, 0x155, 0x19F }, { 0x212, 0x155, 0x1CD } };
    static const struct { uint32_t at, y; } COL[] = {
        { 0x0070AAE0u, 0x12B }, { 0x0070AAC0u, 0x142 }, { 0x0070AAF0u, 0x159 },
        { 0x0070AB00u, 0x187 }, { 0x0070AB10u, 0x19F }, { 0x0070AB20u, 0x1CD } };
    uint32_t P, e;
    unsigned k;
    int rc;
    p_tok(c, F_RES1, 0x213, 0xED, 0xE7);
    for (k = 0; k < 6; k++) p_tok(c, F_RES2, LBL[k].tok, LBL[k].x, LBL[k].y);
    if ((rc = f486adb(c)) != 0) return rc;
    value_text(c, F_RES1, t4019cd(c, R(0x0070AAD0u), A_STR), 0x18C, 0x12B);
    P = cur_player(c);
    if (R(P + 0x88) == 0x980000u) p_tok(c, F_RES1, 0x513, 0x1CF, 0xE7);
    else value_text(c, F_RES1, t401a25(c, R(P + 0x88), A_STR), 0x23A, 0xE7);
    value_text(c, F_RES1, t401a25(c, R(0x0070AAC0u), A_STR), 0x18C, 0x142);
    value_text(c, F_RES1, t401afc(c, R(P + 0x94), 1, A_STR), 0x18C, 0x159);
    for (k = 0; k < 6; k++) value_text(c, F_RES1, t4019cd(c, R(COL[k].at), A_STR), 0x21C, COL[k].y);
    p_blit(c, 0x1Cu + R(P + 4), 0x146, 0xE7);
    e = f464958(c);
    p_text(c, F_A, e, 0x168, 0xE7);
    W(0x00708320u, 0x44u + R(P + 8));
    p_blit(c, R(0x00708320u), 0x1FE, 0xE7);
    return 0;
}

/* 0x0048229E: RESULTS' draw */
static int f48229e(cx *c)
{
    uint32_t P, e, r[8] = { 0 }, x;
    int rc;
    p_tok(c, F_RES2, 0x206, 0x1EC, 0xCF);
    p_tok(c, F_RES2, 0x207, 0x1EC, 0xFC);
    p_tok(c, F_RES2, 0x208, 0x1EC, 0x129);
    if ((rc = f488fba(c)) != 0) return rc;
    P = cur_player(c);
    value_text(c, F_RES1, t401a25(c, R(P + 0x88), A_STR), 0x1FE, 0xFC);
    value_text(c, F_RES1, t401afc(c, R(P + 0x94), 1, A_STR), 0x1FE, 0x12A);
    p_blit(c, 0x1Cu + R(P + 4), 0x1FE, 0xCF);
    e = f464958(c);
    p_text(c, F_RES1, e, 0x221, 0xCF);
    r[VM_EAX] = 0x0051814Cu; r[VM_EBX] = e;
    svc(c, 0x004022D2u, r);                         /* ECX the name's width */
    x = r[VM_ECX] + 0x221u;
    W(0x00708320u, 0x44u + R(P + 8));
    p_blit(c, R(0x00708320u), x, 0xCF);
    return 0;
}

/* ---- the table's checksum: the original's own bit-permutation cipher ------------
 * 0x004027E5 gathers bits (a table of {count, positions...}) from the 64-bit
 * value EDX:EAX into ESI:EDI; 0x004026D2 runs sixteen rounds over the block at
 * 0x004DB530 with a key schedule from 0x004DB528; 0x00402614 and 0x0049F9B3 feed
 * it a score and the car.  The tables are the image's own (at 0x004DB158..). */

/* 0x004027E5: the bits listed at `tab` out of EDX:EAX, into the 64-bit *hi:*lo
 * (each step shifts the result left one and puts the bit in at the bottom) */
static void des_gather(cx *c, uint32_t eax, uint32_t edx, uint32_t tab, uint32_t *hi, uint32_t *lo)
{
    uint32_t n = RB(tab), esi = 0, edi = 0;
    int32_t k;
    tab++;
    for (k = (int32_t)n; k >= 0; k--) {
        uint32_t bit = RB(tab++), src, cf, carry;
        src = (bit & 0x20u) ? eax : edx;
        cf = (src >> (bit & 0x1Fu)) & 1u;
        carry = edi >> 31;
        edi = (edi << 1) | cf;
        esi = (esi << 1) | carry;
    }
    *hi = esi; *lo = edi;
}
static uint32_t rot28(uint32_t x)           /* BTR x, 27 ; ADC x, x */
{
    uint32_t cf = (x >> 27) & 1u;
    x &= ~(1u << 27);
    return x + x + cf;
}
/* 0x004026D2: the block at 0x004DB530 enciphered into 0x004DB538 */
static void des_core(cx *c)
{
    uint32_t a, d, hi, lo, k, eax, edx, esi, edi, ecx, ebx;
    des_gather(c, R(0x004DB528u), R(0x004DB52Cu), 0x004DB158u, &hi, &lo);
    a = lo;                                  /* the first gather's low half */
    des_gather(c, R(0x004DB528u), R(0x004DB52Cu), 0x004DB175u, &hi, &lo);
    d = lo;
    for (k = 0; k < 0x10; k++) {
        a = rot28(a); d = rot28(d);
        if (RB(0x004DB1C3u + k) != 0) { a = rot28(a); d = rot28(d); }
        des_gather(c, a, d, 0x004DB192u, &hi, &lo);
        W(0x004DB4A8u + 8u * k, hi);
        W(0x004DB4ACu + 8u * k, lo);
    }
    des_gather(c, R(0x004DB530u), R(0x004DB534u), 0x004DB1D3u, &esi, &edi);
    for (k = 0; k < 0x10; k++) {
        uint32_t keep_esi = esi, keep_edi = edi, tab, n, j;
        tab = 0x004DB255u; n = RB(tab++); eax = 0; edx = 0;
        for (j = 0; j <= n; j++) {          /* the expansion: EDI's bits, 64 bit result EAX:EDX */
            uint32_t bit = RB(tab++), cf = (edi >> (bit & 0x1Fu)) & 1u, carry = edx >> 31;
            edx = (edx << 1) | cf;
            eax = (eax << 1) | carry;
        }
        eax ^= R(0x004DB4A8u + 8u * k);
        edx ^= R(0x004DB4ACu + 8u * k);
        ebx = 0;
        ecx = 0x004DB2A7u;
        for (j = 0; j < 8; j++) {            /* eight S boxes */
            eax = (eax << 6) | (edx >> 26);
            edx <<= 6;
            ebx <<= 4;
            ebx |= RB(((eax >> 0x10) & 0x3Fu) + ecx);
            ecx += 0x40u;
        }
        tab = 0x004DB286u; n = RB(tab++); eax = 0;
        for (j = 0; j <= n; j++) {           /* the permutation of the S boxes' output */
            uint32_t bit = RB(tab++), cf = (ebx >> (bit & 0x1Fu)) & 1u;
            eax = (eax << 1) | cf;
        }
        edi = keep_esi ^ eax;
        esi = keep_edi;
    }
    des_gather(c, edi, esi, 0x004DB214u, &hi, &lo);
    W(0x004DB538u, hi);
    W(0x004DB53Cu, lo);
}
/* 0x00402614: EAX the score, BL the car, CL the mode, DL the stage's block: the
 * checksum words, in *eax and *ebx (and left at 0x004DB538 / 0x004DB53C) */
static void f402614(cx *c, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t *oa, uint32_t *ob)
{
    uint32_t k;
    uint8_t al = 0x5A;
    if (R(0x0070AA78u) & 0xFFFFE003u) eax = 0u - eax;
    W(0x004DB530u, eax);
    WB(0x004DB534u, ebx);
    WB(0x004DB535u, ecx);
    WB(0x004DB536u, edx);
    for (k = 0; k < 7; k++) al = (uint8_t)(al + (uint8_t)~RB(0x004DB530u + k));
    WB(0x004DB537u, al);
    des_core(c);
    *oa = R(0x004DB538u);
    *ob = R(0x004DB53Cu);
}
/* 0x0049F9B3: the checksum of the current player's score, car and mode for the block in EAX;
 * the words are left in [0x708358] / [0x70835C] and returned */
static void f49f9b3(cx *c, uint32_t eax, uint32_t *oa, uint32_t *ob)
{
    uint32_t P = cur_player(c), ra, rb;
    f402614(c, R(P + 0x88), R(P + 8), R(0x0070CC40u), eax, &ra, &rb);
    W(0x00708358u, ra);
    W(0x0070835Cu, rb);
    *oa = R(0x00708358u);
    *ob = R(0x0070835Cu);
}

/* ---- the best-times tables' insertion (signed and unsigned bubbles) ---------- */
static void bubble9(cx *c, uint32_t esi, int is_signed)
{
    static const uint8_t F[] = { 0xC, 0x14, 4, 8, 0x1C, 0x10, 0x18, 0x40, 0x50, 0x54, 0x58,
        0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x94, 0x98 };
    uint32_t edi = esi - 0x9Cu, k;
    for (k = 9; k; k--) {
        uint32_t x = R(esi + 0x14), y = R(edi + 0x14);
        int skip = is_signed ? (int32_t)x > (int32_t)y : x >= y;
        if (!skip) {
            unsigned j;
            for (j = 0; j < sizeof F; j++) swap_w(c, esi + F[j], edi + F[j]);
            copy31(c, 0x007081ECu, edi + 0x20);
            copy31(c, edi + 0x20, esi + 0x20);
            copy31(c, esi + 0x20, 0x007081ECu);
        }
        edi -= 0x9Cu; esi -= 0x9Cu;
    }
}
/* 0x004860BF: the arcade table's new row bubbled up (swaps unless its value is greater) */
static void f4860bf(cx *c)
{
    uint32_t entry = R(A_ITINS + 4u * R(0x0070AB54u)) + R(0x0070AB58u) * 0xA4u;
    bubble9(c, R(0x00708048u) + R(entry + 0x3C) * 0x618u + 0x57Cu, 1);
}
/* 0x00488979: the time trial's (an unsigned test) */
static void f488979(cx *c)
{
    uint32_t entry = R(R(0x006E7E85u + 4u * R(0x00708230u)) + 4u * R(0x00708238u)) + R(0x00708234u) * 0xA4u;
    uint32_t blk = R(0x0070804Cu);
    if (R(0x00708230u) != 1) blk = R(0x00708050u);
    bubble9(c, blk + R(entry + 0x3C) * 0x618u + 0x57Cu, 0);
}

/* the new row's own fields, as both insertions write them */
static void new_row(cx *c, uint32_t edi, uint32_t block)
{
    uint32_t esi = cur_player(c), a, b, k, src;
    W(edi + 0x14, R(esi + 0x88));
    W(edi + 0x1C, R(esi + 0x90));
    W(edi + 8, R(esi + 4));
    W(edi + 0xC, R(esi + 0x18));
    W(edi + 0x10, R(esi + 8));
    W(edi + 0x50, R(0x0070AA78u));
    f49f9b3(c, block, &a, &b);
    W(edi + 0x54, a);
    W(edi + 0x58, b);
    W(edi + 4, 1);
    W(edi + 0x40, 1);
    W(edi + 0x20, R(esi + 0x18));
    src = esi + 0x20Cu;
    for (k = 0; k < 0x40; k++) WB(edi + 0x5Cu + k, RB(src + k));
    copy31(c, edi + 0x20, R(edi + 0xC));
}
/* 0x00485F67: BEST TIMES' new entry (the arcade's) */
static int f485f67(cx *c)
{
    uint32_t entry = R(A_ITINS + 4u * R(0x0070AB54u)) + R(0x0070AB58u) * 0xA4u, blk = R(entry + 0x3C);
    W(0x00708358u, blk);
    new_row(c, R(0x00708048u) + blk * 0x618u + 0x57Cu, blk);
    f4860bf(c);
    return 0;
}
/* 0x00488898: the player's row in the time trial's table, bubbled up */
static int f488898(cx *c)
{
    uint32_t entry = R(R(0x006E7E85u + 4u * R(0x00708230u)) + 4u * R(0x00708238u)) + R(0x00708234u) * 0xA4u;
    uint32_t blk = R(entry + 0x3C), tab = R(0x0070804Cu);
    W(0x00708358u, blk);
    if (R(0x00708230u) != 1) tab = R(0x00708050u);
    new_row(c, tab + blk * 0x618u + 0x57Cu, blk);
    f488979(c);
    return 0;
}

/* ---- the handlers that take EDI (the stage record the caller holds) ----------- */

/* the best-speed record kept in a stage's table row: the current player's top speed
 * (+0x94), car (+4, +8) and name (+0x18, 0x20 bytes) when it beats the row's */
static void best_speed(cx *c, uint32_t edi, uint32_t at_speed, uint32_t at_a, uint32_t at_b, uint32_t at_name)
{
    uint32_t esi = cur_player(c), ebx = R(esi + 0x94), k, src;
    if ((int32_t)R(edi + at_speed) >= (int32_t)ebx) return;
    W(edi + at_speed, ebx);
    W(edi + at_a, R(esi + 4));
    W(edi + at_b, R(esi + 8));
    src = R(esi + 0x18);
    for (k = 0; k < 0x20; k++) WB(edi + at_name + k, RB(src + k));
}
/* 0x00489D20: the time trial's stage row; 0x00484207: the arcade's */
static int f489d20(cx *c, uint32_t edi) { best_speed(c, edi, 0x2C, 0x9C, 0xA0, 0x7C); return 0; }
static int f484207(cx *c, uint32_t edi) { best_speed(c, edi, 0x30, 0x74, 0x78, 0x54); return 0; }

typedef struct { uint32_t va; int (*fn)(cx *c, uint32_t edi); } fdsg_rent;
static const fdsg_rent RTAB[] = { { 0x00489D20, f489d20 }, { 0x00484207, f484207 } };
#define NRTAB ((int)(sizeof RTAB / sizeof RTAB[0]))

int fdsg_run_edi(uint32_t va, vm *v, const fdsg_draw *d, uint32_t edi)
{
    int i;
    cx c;
    c.v = v; c.d = d; c.rc = 0;
    for (i = 0; i < NRTAB; i++)
        if (RTAB[i].va == va) return RTAB[i].fn(&c, edi) ? FDSG_FATAL : FDSG_OK;
    return FDSG_NOT_MINE;
}
int fdsg_edi_count(void) { return NRTAB; }
uint32_t fdsg_edi_va(int i) { return RTAB[i].va; }

/* ---- the table ----------------------------------------------------------- */
typedef struct { uint32_t va; int (*fn)(cx *c); } fdsg_ent;

static const fdsg_ent TAB[] = {
    { 0x00486DB0, f486db0 }, { 0x0047C0F5, f47c0f5 }, { 0x00486030, f486030 },
    { 0x00486B6C, f486b6c }, { 0x00486939, f486939 }, { 0x00488E1A, f488e1a },
    { 0x0047DB0E, f47db0e }, { 0x0048300D, f48300d }, { 0x0047C127, f47c127 },
    { 0x00470443, f470443 }, { 0x00485AB9, f485ab9 }, { 0x004854A1, f4854a1 },
    { 0x0048229E, f48229e }, { 0x00485F67, f485f67 }, { 0x00488898, f488898 },
};
#define NTAB ((int)(sizeof TAB / sizeof TAB[0]))

int fdsg_run(uint32_t va, vm *v, const fdsg_draw *d)
{
    int i;
    cx c;
    c.v = v; c.d = d; c.rc = 0;
    for (i = 0; i < NTAB; i++)
        if (TAB[i].va == va) return TAB[i].fn(&c) ? FDSG_FATAL : FDSG_OK;
    return FDSG_NOT_MINE;
}
int fdsg_count(void) { return NTAB; }
uint32_t fdsg_va(int i) { return TAB[i].va; }
