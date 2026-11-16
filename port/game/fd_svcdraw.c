/* fd_svcdraw.c - see fd_svcdraw.h.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps as
 * the original's does.  The long run of draws in f4a1c11 (positions, tokens,
 * fonts) was written from the disassembly by a script, then checked by the gate
 * against the original's code run in the interpreter. */
#include "fd_svcdraw.h"

#include "fe_champ.h"
#include "fe_logic.h"

typedef struct { vm *v; const fdsg_draw *d; } cx;

#define R(a)     vm_rd32(c->v, (uint32_t)(a))
#define W(a, x)  vm_wr32(c->v, (uint32_t)(a), (uint32_t)(x))
#define RB(a)    vm_rd8(c->v, (uint32_t)(a))
#define WB(a, x) vm_wr8(c->v, (uint32_t)(a), (uint8_t)(x))

enum {
    A_PLAYERS = 0x00708DE8,
    A_STR     = 0x00A2BF54,     /* the text buffer the clock's lines and the figures build in */
    A_STR2    = 0x008E80D0,     /* the title's */
    A_TEXT    = 0x00A2C054,     /* the controller rows' */
    SP_X = 0x00707B98, SP_Y = 0x00707B9C, SP_W = 0x00707BA0, SP_A = 0x00707BA4,
    SP_B = 0x00707BA8, SP_V = 0x00707BAC      /* a bar's arguments */
};

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
/* 0x00401F8F: the token's text at EBX; EBX advanced (no NUL) */
static uint32_t p_tokstr(cx *c, uint32_t token, uint32_t ebx)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = token; r[VM_EBX] = ebx;
    svc(c, 0x00401F8Fu, r);
    return r[VM_EBX];
}

/* the text cursor's routines */
static uint32_t put(cx *c, uint32_t ebx, uint8_t ch) { WB(ebx, ch); return ebx + 1; }
static uint32_t nul(cx *c, uint32_t ebx)   { return put(c, ebx, 0); }      /* 0x00401F02 */
static uint32_t space(cx *c, uint32_t ebx) { return put(c, ebx, 0x20); }   /* 0x00401F15 */
static uint32_t colon(cx *c, uint32_t ebx) { return put(c, ebx, 0x3A); }   /* 0x00401F1F */
static uint32_t fmt(cx *c, uint32_t val, uint32_t flags, uint32_t ebx)
{
    return fel_fmt_num(c->v, val, flags, ebx);                              /* 0x00402011 */
}
/* 0x004019D7: EAX seconds as m:ss */
static uint32_t t4019d7(cx *c, uint32_t eax, uint32_t ebx)
{
    ebx = fmt(c, eax / 60u, 0, ebx);
    ebx = colon(c, ebx);
    return fmt(c, eax % 60u, 0x12, ebx);
}

/* ------------------------------------------------------------------------- */
/* the bars                                                                    */
/* ------------------------------------------------------------------------- */

/* 0x0046825C: the bar [0x707BAC] percent of [0x707BA0]: one sprite when the value is not above 0 */
static int f46825c(cx *c)
{
    uint32_t eax, ebx;
    if ((int32_t)R(SP_V) <= 0) { p_blit(c, R(SP_A), R(SP_X), R(SP_Y)); return 0; }
    eax = R(SP_W) * R(SP_V);
    if (eax == 0) return 1;                       /* div: the runtime's error exit */
    ebx = R(SP_X) + eax / 100u;
    W(SP_X, ebx);
    p_blit(c, R(SP_A), ebx, R(SP_Y));
    return 0;
}

/* 0x004681D4: the two-part bar: the first sprite is drawn with the clip's right edge
 * [0x4F0014] at the value's end (put back after), the second starts there */
static int f4681d4(cx *c)
{
    uint32_t eax, ebx, saved;
    if (R(SP_V) == 0) return 0;
    saved = R(0x004F0014u);
    eax = R(SP_W) * R(SP_V);
    if (eax == 0) return 1;
    ebx = R(SP_X) + eax / 100u;
    W(0x004F0014u, ebx);
    W(0x00708318u, ebx);
    p_blit(c, R(SP_A), R(SP_X), R(SP_Y));
    W(0x004F0014u, saved);
    p_blit(c, R(SP_B), R(0x00708318u), R(SP_Y));
    return 0;
}

/* a picker's bar: the six stores before every 0x004681D4 call in 0x004A1C11 */
static int picker(cx *c, uint32_t edi, uint32_t x, uint32_t y, uint32_t off)
{
    W(SP_X, x); W(SP_Y, y); W(SP_W, 0xA0); W(SP_A, 0x86); W(SP_B, 0x87);
    W(SP_V, R(edi + off));
    return f4681d4(c);
}

/* the cursor's bars: the stores, then 0x0046825C ([0x707BA8] is not set) */
static int cursor_bar(cx *c, uint32_t x, uint32_t y, uint32_t sprite, uint32_t value)
{
    W(SP_X, x); W(SP_Y, y); W(SP_W, 0xCC); W(SP_A, sprite); W(SP_V, value);
    return f46825c(c);
}

/* ------------------------------------------------------------------------- */
/* the clock                                                                   */
/* ------------------------------------------------------------------------- */

/* the token, ": ", two spaces, the time as m:ss, drawn in `font` at (x, 0x2C) */
static void clock_line(cx *c, uint32_t token, uint32_t secs, uint32_t font, uint32_t x)
{
    uint32_t ebx = p_tokstr(c, token, A_STR);
    ebx = colon(c, ebx);
    ebx = space(c, ebx);
    ebx = space(c, ebx);
    ebx = t4019d7(c, secs, ebx);
    nul(c, ebx);
    p_text(c, font, A_STR, x, 0x2C);
}

/* 0x004A1A56: time used and left, or the time short and the penalty for it */
static int f4a1a56(cx *c)
{
    uint32_t eax;
    W(0x0070CC28u, 0);
    W(0x00708318u, R(0x007082A4u) - 0x384u);
    eax = R(0x007082A4u) - R(0x0070CBF4u);
    if ((int32_t)eax >= 0x384) {
        W(0x00708318u, R(0x007082A4u) - 0x384u);
        clock_line(c, 0x48D, R(0x00708318u), 0x005180ACu, 0x42);
        W(0x00708358u, R(0x007082A4u) - 0x384u - R(0x0070CBF4u));
        clock_line(c, 0x48E, R(0x00708358u), 0x005180CCu, 0x2DE);
        return 0;
    }
    W(0x00708358u, 0x384u - eax);
    clock_line(c, 0x490, R(0x00708358u), 0x0051804Cu, 0x42);
    if (R(0x00708358u) != 0) W(0x00708358u, R(0x00708358u) + 0x3Cu);
    eax = R(0x00708358u);
    if (eax == 0) return 1;
    W(0x0070CC28u, eax / 0x3Cu * 10u);
    clock_line(c, 0x491, R(0x0070CC28u), 0x0051806Cu, 0x2DE);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 0x004A1C11's own helpers                                                    */
/* ------------------------------------------------------------------------- */

/* 0x004A2D46: the sixteen figures, 0x0070CAC8.. (two a row) as m:ss */
static void f4a2d46(cx *c)
{
    static const uint16_t Y[8] = { 0x6E, 0x8B, 0xA8, 0xC5, 0xE2, 0xFF, 0x11C, 0x1C4 };
    uint32_t i;
    for (i = 0; i < 16; i++) {
        uint32_t ebx = t4019d7(c, R(0x0070CAC8u + 4u * i), A_STR);
        nul(c, ebx);
        p_text(c, 0x00517F0Cu, A_STR, (i & 1u) ? 0x1B9u : 0x6Au, Y[i >> 1]);
    }
}

/* 0x004A2CEE / 0x004A2C96: drawn, except on the screen's own item (the pages' picker) when the
 * mode flag is 1 and the cursor is not blinking */
static int f4a2cee(cx *c, uint32_t edi)
{
    if (R(0x0070CA08u) == 1 && R(0x00707E91u) == 0x11 && R(0x00708310u) != 1) return 0;
    return cursor_bar(c, 0x1D0, 0x199, 0x76, R(edi + 0x110u));
}
static int f4a2c96(cx *c, uint32_t edi)
{
    if (R(0x0070CA08u) == 1 && R(0x00707E91u) == 0x10 && R(0x00708310u) != 1) return 0;
    return cursor_bar(c, 0x81, 0x199, 0x76, R(edi + 0x11Cu));
}
static int f4a2b26(cx *c) { return cursor_bar(c, 0x81, 0x1D1, 0x77, R(0x0070CC14u)); }
static int f4a2b67(cx *c) { return cursor_bar(c, 0x1D0, 0x1D1, 0x77, R(0x0070CC20u)); }

/* 0x004A2BA8 / 0x004A2C1F: on the cursor's item (mode flag 1, item `sel`) drawn while it blinks;
 * else drawn when the two values differ, or when they are the same and the item's flag is 1 */
static int last_bar(cx *c, uint32_t sel, uint32_t x, uint32_t a, uint32_t b)
{
    if (R(0x0070CA08u) == 1 && R(0x00707E91u) == sel) {
        if (R(0x00708310u) != 1) return 0;
    } else if (R(b) == R(a)) {
        if (R(0x0070BE7Au + sel * 4u) != 1) return 0;
    }
    return cursor_bar(c, x, 0x1D1, 0x76, R(a));
}

/* 0x004A27C3: the inspection total: the cost of each part at or over its threshold whose flag
 * word is 0, taken from 100; drawn as the status bar and its two labels */
static int f4a27c3(cx *c)
{
    static const uint8_t PARTS[] = { 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint32_t edi, k;
    W(0x00708358u, 0);
    edi = R(A_PLAYERS + 4u * R(0x007082F8u));
    for (k = 0; k < sizeof PARTS; k++) {
        uint32_t n = PARTS[k] - 2u;
        if ((int32_t)R(edi + 0xD4u + 4u * n) >= (int32_t)R(0x0070CB40u + 4u * n)
            && R(0x0070BE7Au + 4u * PARTS[k]) == 0)
            W(0x00708358u, R(0x00708358u) + R(0x0070CBB0u + 4u * n));
    }
    W(0x0070CBE8u, 100u - R(0x00708358u));
    W(SP_X, 0x19D); W(SP_Y, 0x229); W(SP_W, 0xD0); W(SP_A, 0x89); W(SP_B, 0x87);
    W(SP_V, R(0x0070CBE8u));
    if (f4681d4(c)) return 1;
    p_blit(c, 0x88, 0x161, 0x228);
    p_tok(c, 0x00517EECu, 0x4AE, 0x17F, 0x22C);
    p_tok(c, 0x00517EECu, 0x4AF, 0x28C, 0x22C);
    return 0;
}

/* 0x004A1C11 */
static int f4a1c11(cx *c)
{
    uint32_t ebx, edi;
    W(0x005D45DCu, 0);
    ebx = p_tokstr(c, 0x190, A_STR2);
    ebx = space(c, ebx);
    ebx = fmt(c, R(0x007082F8u) + 1u, 0, ebx);                  /* 0x004019CD */
    ebx = space(c, ebx);
    ebx = p_tokstr(c, 0x486, ebx);
    nul(c, ebx);
    p_text(c, 0x005183CCu, A_STR2, 0x190, 0xE);
    if (f4a1a56(c)) return 1;
    p_tok(c, 0x005181CCu, 0x488, 0x190, 0x44);
    edi = R(A_PLAYERS + 4u * R(0x007082F8u));
    if (picker(c, edi, 0xEC, 0x64, 0xd4)) return 1;
    if (picker(c, edi, 0x23B, 0x64, 0xd8)) return 1;
    if (picker(c, edi, 0xEC, 0x81, 0xdc)) return 1;
    if (picker(c, edi, 0x23B, 0x81, 0xe0)) return 1;
    if (picker(c, edi, 0xEC, 0x9E, 0xe4)) return 1;
    if (picker(c, edi, 0x23B, 0x9E, 0xe8)) return 1;
    if (picker(c, edi, 0xEC, 0xBB, 0xec)) return 1;
    if (picker(c, edi, 0x23B, 0xBB, 0xf0)) return 1;
    if (picker(c, edi, 0xEC, 0xD8, 0xf4)) return 1;
    if (picker(c, edi, 0x23B, 0xD8, 0xf8)) return 1;
    if (picker(c, edi, 0xEC, 0xF5, 0xfc)) return 1;
    if (picker(c, edi, 0x23B, 0xF5, 0x100)) return 1;
    if (picker(c, edi, 0xEC, 0x112, 0x104)) return 1;
    if (picker(c, edi, 0x23B, 0x112, 0x108)) return 1;
    p_tok(c, 0x517f0c, 0x492, 0x6A, 0x61);
    p_tok(c, 0x517f0c, 0x499, 0x1B9, 0x61);
    p_tok(c, 0x517f0c, 0x493, 0x6A, 0x7E);
    p_tok(c, 0x517f0c, 0x49A, 0x1B9, 0x7E);
    p_tok(c, 0x517f0c, 0x494, 0x6A, 0x9B);
    p_tok(c, 0x517f0c, 0x49B, 0x1B9, 0x9B);
    if (R(edi + 0x90u) != 0) p_tok(c, 0x517eec, 0x578, 0x13F, 0x9F);
    p_tok(c, 0x517f0c, 0x495, 0x6A, 0xB8);
    p_tok(c, 0x517f0c, 0x49C, 0x1B9, 0xB8);
    p_tok(c, 0x517f0c, 0x496, 0x6A, 0xD5);
    p_tok(c, 0x517f0c, 0x49D, 0x1B9, 0xD5);
    p_tok(c, 0x517f0c, 0x497, 0x6A, 0xF2);
    p_tok(c, 0x517f0c, 0x49E, 0x1B9, 0xF2);
    p_tok(c, 0x517f0c, 0x498, 0x6A, 0x10F);
    p_tok(c, 0x517f0c, 0x49F, 0x1B9, 0x10F);
    p_tok(c, 0x517f0c, 0x4A0, 0x46, 0x152);
    p_tok(c, 0x517f2c, 0x4A1, 0x18D, 0x152);
    p_tok(c, 0x5181cc, 0x487, 0x190, 0x161);
    p_tok(c, 0x517f0c, 0x4A2, 0x6A, 0x17E);
    p_tok(c, 0x517f0c, 0x4A5, 0x1B9, 0x17E);
    p_tok(c, 0x517f0c, 0x4A8, 0x6A, 0x1B6);
    p_tok(c, 0x517f0c, 0x4AB, 0x1B9, 0x1B6);
    p_blit(c, 0x68, 0x43, 0x61);
    p_blit(c, 0x69, 0x192, 0x61);
    p_blit(c, 0x6A, 0x43, 0x7E);
    p_blit(c, 0x6B, 0x192, 0x7E);
    p_blit(c, 0x6C, 0x43, 0x9B);
    p_blit(c, 0x6D, 0x192, 0x9B);
    p_blit(c, 0x6E, 0x43, 0xB8);
    p_blit(c, 0x6F, 0x192, 0xB8);
    p_blit(c, 0x70, 0x43, 0xD5);
    p_blit(c, 0x71, 0x192, 0xD5);
    p_blit(c, 0x72, 0x43, 0xF2);
    p_blit(c, 0x73, 0x192, 0xF2);
    p_blit(c, 0x74, 0x43, 0x10F);
    p_blit(c, 0x75, 0x192, 0x10F);
    p_blit(c, 0x7B, 0x46, 0x12D);
    p_blit(c, 0x7C, 0x88, 0x12D);
    p_blit(c, 0x7D, 0xCA, 0x12D);
    p_blit(c, 0x7E, 0x10C, 0x12D);
    p_blit(c, 0x7F, 0x14E, 0x12D);
    p_blit(c, 0x80, 0x195, 0x12D);
    p_blit(c, 0x81, 0x1CC, 0x12D);
    p_blit(c, 0x82, 0x203, 0x12D);
    p_blit(c, 0x83, 0x23A, 0x12D);
    p_blit(c, 0x84, 0x271, 0x12D);
    p_blit(c, 0x85, 0x2A8, 0x12D);
    p_blit(c, 0x6F, 0x43, 0x17E);
    p_blit(c, 0x73, 0x192, 0x17E);
    p_blit(c, 0x71, 0x43, 0x1B6);
    p_blit(c, 0x71, 0x192, 0x1B6);
    p_blit(c, 0x7A, 0x46, 0x19C);
    p_blit(c, 0x7A, 0x195, 0x19C);
    p_blit(c, 0x7A, 0x46, 0x1D4);
    p_blit(c, 0x7A, 0x195, 0x1D4);
    p_tok(c, 0x51820c, 0x48F, 0x154, 0x227);
    p_tok(c, 0x5181cc, 0x489, 0xE9, 0x1EE);
    p_tok(c, 0x5181cc, 0x48A, 0x238, 0x1EE);
    p_tok(c, 0x5181cc, 0x48B, 0xE9, 0x20A);
    p_tok(c, 0x5181cc, 0x48C, 0x238, 0x20A);
    p_tok(c, 0x517eec, 0x4A3, 0x62, 0x19F);
    p_tok(c, 0x517eec, 0x4A4, 0x16F, 0x19F);
    p_tok(c, 0x517eec, 0x4A9, 0x62, 0x1D7);
    p_tok(c, 0x517eec, 0x4AA, 0x16F, 0x1D7);
    p_tok(c, 0x517eec, 0x4A6, 0x1B1, 0x19F);
    p_tok(c, 0x517eec, 0x4A7, 0x2BE, 0x19F);
    p_tok(c, 0x517eec, 0x4AC, 0x1B1, 0x1D7);
    p_tok(c, 0x517eec, 0x4AD, 0x2BE, 0x1D7);
    W(0x00708358u, R(0x0070CC04u) + 0x4B0u);
    p_tok(c, 0x00517EECu, R(0x00708358u), 0x238, 0x152);
    f4a2d46(c);
    if (f4a2cee(c, edi) || f4a2c96(c, edi) || f4a2b26(c) || f4a2b67(c)
        || last_bar(c, 0x12, 0x81, 0x0070CC10u, 0x0070CC14u)
        || last_bar(c, 0x13, 0x1D0, 0x0070CC1Cu, 0x0070CC20u)
        || f4a27c3(c))
        return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* LEG DETAILS' pre-draw 0x0046F2EB                                            */
/* ------------------------------------------------------------------------- */

/* 0x004019A7: a time in 1/100 as seconds: EAX / 100 * 60 + EAX % 100 */
static uint32_t t4019a7(uint32_t eax) { return eax / 100u * 60u + eax % 100u; }
/* 0x0040190A: (EAX * 0x3EDD + 0x1388) / 0x2710 (a unit conversion, the sum 64-bit) */
static uint32_t t40190a(uint32_t eax) { return (uint32_t)(((uint64_t)eax * 0x3EDDu + 0x1388u) / 0x2710u); }
/* 0x00401B73: a value in hundredths ("n.mm") */
static uint32_t t401b73(cx *c, uint32_t eax, uint32_t ebx)
{
    if (R(0x00517954u) != 0) eax = t40190a(eax);
    ebx = fmt(c, eax / 100u, 0, ebx);
    ebx = put(c, ebx, 0x2E);                                                /* 0x00401F33 */
    return fmt(c, eax % 100u, 0x12, ebx);
}
/* 0x00462AC2: EAX seconds as mm:ss (the minutes padded to two places below ten) */
static uint32_t t462ac2(cx *c, uint32_t eax, uint32_t ebx)
{
    uint32_t m = eax / 60u, s = eax % 60u;
    if ((int32_t)m < 10) ebx = put(c, ebx, 0x30);                           /* 0x00462AF4 */
    ebx = fmt(c, m, 0, ebx);
    ebx = colon(c, ebx);
    return fmt(c, s, 0x12, ebx);
}
/* 0x00462B9F: the first five bytes of the string at `str`, one character at a time (copied to
 * str + 0x40 with a NUL after it, drawn through 0x004022CA = 0x0040239C), the pen at x - 100
 * then 13, 10, 10 and 13 along */
static void spaced(cx *c, uint32_t font, uint32_t str, uint32_t x, uint32_t y, const uint8_t *step, uint32_t n)
{
    uint32_t k;
    x -= 0x64u;
    for (k = 0; k < n; k++) {
        x += step[k];
        WB(str + 0x40u, RB(str + k));
        WB(str + 0x41u, 0);
        p_text(c, font, str + 0x40u, x, y);
    }
}
static void f462b9f(cx *c, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    static const uint8_t STEP5[5] = { 0, 13, 10, 10, 13 };
    spaced(c, font, str, x, y, STEP5, 5);
}
/* 0x00462B21: the same over the first eight bytes (a time as hh:mm:ss.cc is eight places) */
static void f462b21(cx *c, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    static const uint8_t STEP8[8] = { 0, 13, 10, 10, 13, 10, 10, 13 };
    spaced(c, font, str, x, y, STEP8, 8);
}

/* the original's fatal exit at 0x004D54E6 (an itinerary entry of no kind it knows) */
static int fatal_kind(cx *c)
{
    W(0x004F0274u, 0x8070D91Au);
    W(0x004F0278u, 0x004DA5E6u);
    return 1;
}

/* 0x0046F2EB: the legs of the rally in the itinerary [0x6E7EE9..], one row each, the stage
 * names, distances and clocks; the current leg [0x7082C0] in the bright font */
static int f46f2eb(cx *c)
{
    uint32_t edi, ebx = R(0x007082C4u), n;
    int rc;
    edi = fec_46ebbc(c->v, 0x006E7EE9u, 0, ebx, &rc);
    if (rc) return 1;
    if (R(0x00708038u) == 0x13) p_tok(c, 0x005184CCu, R(0x0070C2AEu + ebx * 4u), 0x2EB, 0x77);
    else p_tok(c, 0x005184CCu, R(edi + 0x38u), 0x2EB, 0x77);
    p_tok(c, 0x005180ACu, 0x482, 0xBA, 0xD1);
    p_tok(c, 0x005180ACu, 0x47E, 0x10A, 0xD1);
    p_tok(c, 0x005180ACu, 0x480, 0x1F1, 0xD1);
    p_tok(c, 0x005180ACu, 0x47F, 0x257, 0xD1);
    W(0x00708318u, 0xE8);
    W(0x0070831Cu, 1);
    W(0x00708320u, 1);
    W(0x00708378u, 0);
    edi = fec_46ebbc(c->v, 0x006E7EE9u, 1, R(0x007082C4u), &rc);
    if (rc) return 1;
    for (n = 0; n < 100000u; n++) {
        uint32_t kind = R(edi), y;
        if (kind == 1) {                                  /* a stage's time only */
            W(0x00708374u, 0x478u + R(0x00708378u));
            p_tok(c, 0x005180ACu, R(0x00708374u), 0x10A, R(0x00708318u));
            W(0x00708378u, R(0x00708378u) + 1u);
            ebx = t462ac2(c, t4019a7(R(edi + 0x10u)), A_STR);
            nul(c, ebx);
            f462b9f(c, 0x0051808Cu, A_STR, 0x2C2, R(0x00708318u));
        } else if (kind == 4 || kind == 3) {
            return 0;
        } else if (kind != 0) {
            return fatal_kind(c);
        } else {                                          /* a stage: its name, distance, number */
            uint32_t rec, eax, font;
            rec = fec_46f686(c->v, edi, NULL, &rc);
            if (rc) return 1;
            font = R(0x007082C0u) == R(0x0070831Cu) ? 0x0051804Cu : 0x005180ACu;
            y = R(0x00708318u);
            p_tok(c, font, R(rec), 0x10A, y);
            eax = R(rec + 0xCu);
            ebx = A_STR;
            if (eax == 0) ebx = nul(c, ebx);
            else {
                ebx = t401b73(c, eax, ebx);
                ebx = p_tokstr(c, R(0x00517954u) != 0 ? 0x570u : 0x56Fu, ebx);
            }
            nul(c, ebx);
            p_text(c, font, A_STR, 0x1F1, y);
            nul(c, fmt(c, R(0x00708320u), 0, A_STR));
            p_text(c, font, A_STR, 0xBA, y);
            W(0x00708320u, R(0x00708320u) + 1u);
            ebx = t462ac2(c, t4019a7(R(edi + 0x10u)), A_STR);
            nul(c, ebx);
            f462b9f(c, R(0x007082C0u) == R(0x0070831Cu) ? 0x0051802Cu : 0x0051808Cu, A_STR, 0x2C2, y);
        }
        W(0x00708318u, R(0x00708318u) + 0x19u);
        W(0x0070831Cu, R(0x0070831Cu) + 1u);
        edi += 0xA4u;
        if (R(edi) == 0xFFFFFFFFu) return 0;
    }
    return 1;                                             /* a walk the original would never end */
}

/* ------------------------------------------------------------------------- */
/* the stage-information times list 0x0048A411                                 */
/* ------------------------------------------------------------------------- */

static uint32_t rd16(cx *c, uint32_t a) { return (uint32_t)RB(a) | ((uint32_t)RB(a + 1u) << 8); }

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
    ebx = put(c, ebx, 0x2E);
    return fmt(c, eax % 100u, 0x12, ebx);
}
/* 0x00401AFC: a value in tenths ("n.m"), with the unit's token when ECX is not 0 */
static uint32_t t401afc(cx *c, uint32_t eax, uint32_t ecx, uint32_t ebx)
{
    if (R(0x00517954u) != 0) eax = t40190a(eax);
    ebx = fmt(c, eax / 10u, 0, ebx);
    ebx = put(c, ebx, 0x2E);
    ebx = fmt(c, eax % 10u, 0, ebx);
    if (ecx) ebx = p_tokstr(c, rd16(c, 0x005186B0u + 2u * R(0x00517954u)), ebx);
    return ebx;
}
/* 0x00401F51: EAX, '/', EDX */
static uint32_t of_n(cx *c, uint32_t eax, uint32_t edx, uint32_t ebx)
{
    ebx = fmt(c, eax, 0, ebx);
    ebx = put(c, ebx, 0x2F);
    return fmt(c, edx, 0, ebx);
}

/* 0x00402666: the two words as "aabb-ccdd-eeff-gghh" (each word's bytes low first, two hex digits
 * a byte from the table at 0x004DB148) at 0x004DB540, no NUL (the buffer's zeros end it) */
static void f402666(cx *c, uint32_t a, uint32_t b)
{
    uint32_t ebx = 0x004DB540u, k, w;
    W(0x004DB538u, a);
    W(0x004DB53Cu, b);
    for (w = 0; w < 2; w++) {
        uint32_t v = w ? b : a;
        for (k = 0; k < 4; k++) {
            uint32_t byte = (v >> (8u * k)) & 0xFFu;
            WB(ebx, RB(0x004DB148u + ((byte >> 4) & 0xFu)));
            WB(ebx + 1u, RB(0x004DB148u + (byte & 0xFu)));
            ebx += 2u;
            if (k == 1 || (k == 3 && w == 0)) ebx = put(c, ebx, 0x2D);
        }
    }
}

/* 0x00464ECB: EDI on the EBX-th kind-4 entry of the itinerary (none when EBX is 0), then on the
 * EAX-th kind-0 entry from there (itself counted).  The original scans memory until it finds
 * one: no end is reported as 1 (a fatal exit). */
static int f464ecb(cx *c, uint32_t *pedi, uint32_t eax, uint32_t ebx)
{
    uint32_t edi = *pedi, ecx, guard = 0;
    if (ebx != 0) {
        ecx = 0;
        for (;;) {
            while (R(edi) != 4) {
                edi += 0xA4u;
                if (++guard > 100000u) return 1;
            }
            ecx++;
            if (ecx == ebx) break;
            edi += 0xA4u;
            if (++guard > 100000u) return 1;
        }
    }
    ecx = 0;
    for (;;) {
        while (R(edi) != 0) {
            edi += 0xA4u;
            if (++guard > 100000u) return 1;
        }
        ecx++;
        if (ecx == eax) break;
        edi += 0xA4u;
        if (++guard > 100000u) return 1;
    }
    *pedi = edi;
    return 0;
}

/* the tail's fields: the entry's best of the two (a sprite, a name, a second sprite, a value) */
static const uint16_t XINFO[4] = { 0x14F, 0x172, 0x225, 0x262 };
static void info_tail(cx *c, uint32_t edi, uint32_t y, const uint16_t *x, uint32_t sa, uint32_t name, uint32_t sb, uint32_t val)
{
    uint32_t eax = 0x1Cu + R(edi + sa), ebx;
    p_blit(c, eax, x[0], y);
    p_text(c, 0x005180ACu, edi + name, x[1], y);
    eax = 0x44u + R(edi + sb);
    W(0x00708320u, eax);
    p_blit(c, eax, x[2], y);
    ebx = t401afc(c, R(edi + val), 1, A_STR);
    nul(c, ebx);
    p_text(c, 0x005180ACu, A_STR, x[3], y);
}

/* 0x0048A411: SELECT's best-times page: the headings' tokens stored into the item list, the title
 * string, ten rows (name, time or the key string, the gap to the first, the number, two sprites),
 * and the entry's own line.  A row table that ends before the tenth row (-1) leaves the original's
 * stack one pair short: it returns into nothing, a fatal exit. */
static int f48a411(cx *c)
{
    uint32_t eax, ebx, edi, entry, ecx, edx, rec, items;
    int rc;
    eax = R(0x00708060u) == 1 ? 0x19Cu : 0x19Bu;
    W(R(0x00707EF5u) + 4u + 1u * 0x5Cu + 0x14u, eax);
    eax = R(0x00708064u) == 1 ? 0x19Eu : 0x19Du;
    W(R(0x00707EF5u) + 4u + 2u * 0x5Cu + 0x14u, eax);
    edi = R(R(0x006E7E85u + R(0x00708060u) * 4u) + R(0x0070AB76u) * 4u);
    items = R(0x00707EF5u) + 4u + 3u * 0x5Cu;
    W(items + 0x14u, R(edi + 0x38u));
    edi += R(0x0070AB7Au) * 0xA4u;
    W(0x00A2C554u, R(edi + 4u));
    entry = edi;
    ebx = p_tokstr(c, 0x481, 0x00A2C154u);
    ebx = space(c, ebx);
    ebx = of_n(c, R(edi + 0x40u), 6, ebx);
    ebx = colon(c, ebx);
    ebx = space(c, ebx);
    rec = fec_46f686(c->v, edi, NULL, &rc);
    if (rc) return 1;
    ebx = p_tokstr(c, R(rec), ebx);
    nul(c, ebx);
    edi = R(0x006E7E71u + R(0x00708060u) * 4u + (R(0x00708064u) == 1 ? 8u : 0u));
    edi = R(edi) + R(entry + 0x3Cu) * 0x618u;
    edx = 0xF0;
    for (ecx = 1; ; ) {
        W(0x00708318u, ecx);
        p_text(c, 0x005180ACu, edi + 0x20u, 0xF0, edx);
        if (R(0x0070CA98u) == 1) {
            uint32_t x = 0x20A, bp = 0x004DB540u;
            f402666(c, R(edi + 0x54u), R(edi + 0x58u));
            W(0x00708358u, edx);
            do {
                W(0x00A2C054u, 0);
                WB(0x00A2C054u, RB(bp));
                WB(0x00A2C055u, 0);
                p_text(c, 0x0051808Cu, 0x00A2C054u, x, edx);
                x += 0xE;
                bp++;
            } while (RB(bp) != 0);
        } else {
            ebx = t401a25(c, R(edi + 0x14u), A_STR);
            nul(c, ebx);
            f462b21(c, 0x0051808Cu, A_STR, 0x276, edx);
            if (R(0x00708318u) == 1) {
                W(0x0070CA8Cu, R(edi + 0x14u));
            } else {
                W(0x0070835Cu, R(edi + 0x14u) - R(0x0070CA8Cu));
                p_tok(c, 0x005180ACu, 0x579, 0x276, edx);
                ebx = t401a25(c, R(0x0070835Cu), A_STR);
                nul(c, ebx);
                f462b21(c, 0x0051808Cu, A_STR, 0x2EE, edx);
            }
        }
        ebx = fmt(c, R(0x00708318u), 0, A_STR);
        nul(c, ebx);
        p_text(c, 0x005180CCu, A_STR, 0xC8, edx);
        W(0x0070831Cu, edx);
        p_blit(c, 0x1Cu + R(edi + 8u), 0xD2, R(0x0070831Cu));
        eax = 0x44u + R(edi + 0x10u);
        W(0x00708320u, eax);
        p_blit(c, eax, 0x1CC, R(0x0070831Cu));
        edi += 0x9Cu;
        if (R(edi) == 0xFFFFFFFFu) return 1;
        edx += 0x19;
        ecx++;
        if (ecx == 0xB) break;
    }
    edi = entry;
    p_tok(c, 0x0051820Cu, 0x1A2, 0x142, 0x1F1);
    if (((R(0x00708060u) ^ 1u) & (R(0x00708064u) ^ 1u)) == 1u) {
        edi = 0x006E7EE9u;
        if (f464ecb(c, &edi, R(0x0070AB7Au), R(0x0070AB76u))) return 1;
        info_tail(c, edi, 0x1F1, XINFO, 0x74, 0x54, 0x78, 0x30);
    } else if (R(0x00708064u) == 1) {
        info_tail(c, edi, 0x1F1, XINFO, 0x9C, 0x7C, 0xA0, 0x2C);
    } else {
        info_tail(c, edi, 0x1F1, XINFO, 0x74, 0x54, 0x78, 0x30);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* the best-time rows 0x0047091E / 0x00470AFA (EDI the hst.dat row)             */
/* ------------------------------------------------------------------------- */

/* "<token 0x34B>:  " and "<token 0x34C>:  " (the second token is token 0's text: empty), then
 * the row's sprite, name, second sprite and time, and below them the screen's own stage entry
 * (EDI = `entry`) in the same layout: its sprite, name, second sprite and tenths value */
static const uint16_t XROW_B[4] = { 0x18A, 0x1A6, 0x266, 0x2A8 };
static void best_row(cx *c, uint32_t edi)
{
    uint32_t ebx, eax;
    ebx = p_tokstr(c, 0x34B, A_STR);
    ebx = colon(c, ebx);
    ebx = space(c, ebx);
    ebx = space(c, ebx);
    ebx = p_tokstr(c, 0, ebx);
    nul(c, ebx);
    p_text(c, 0x005180CCu, A_STR, 0x188, 0x1FC);
    ebx = p_tokstr(c, 0x34C, A_STR);
    ebx = colon(c, ebx);
    ebx = space(c, ebx);
    ebx = space(c, ebx);
    ebx = p_tokstr(c, 0, ebx);
    nul(c, ebx);
    p_text(c, 0x005180CCu, A_STR, 0x188, 0x213);
    p_blit(c, 0x1Cu + R(edi + 8u), 0x18A, 0x1FC);
    p_text(c, 0x005180ACu, edi + 0x20u, 0x1A6, 0x1FC);
    eax = 0x44u + R(edi + 0x10u);
    W(0x00708320u, eax);
    p_blit(c, eax, 0x266, 0x1FC);
    ebx = t401a25(c, R(edi + 0x14u), A_STR);
    nul(c, ebx);
    f462b21(c, 0x0051808Cu, A_STR, 0x314, 0x1FC);
}
/* 0x0047091E: the entry is the screen's own (the list 0x006E7E85 by [0x00708230], [0x00708238], [0x00708234]) */
static int f47091e(cx *c, uint32_t edi)
{
    uint32_t e;
    best_row(c, edi);
    e = R(R(0x006E7E85u + R(0x00708230u) * 4u) + R(0x00708238u) * 4u) + R(0x00708234u) * 0xA4u;
    info_tail(c, e, 0x213, XROW_B, 0x74, 0x54, 0x78, 0x30);
    return 0;
}
/* 0x00470AFA: the same over the arcade's entry (the list 0x006E7ECD by [0x0070AB54], [0x0070AB58]) */
static int f470afa(cx *c, uint32_t edi)
{
    uint32_t e;
    best_row(c, edi);
    e = R(0x006E7ECDu + R(0x0070AB54u) * 4u) + R(0x0070AB58u) * 0xA4u;
    info_tail(c, e, 0x213, XROW_B, 0x74, 0x54, 0x78, 0x30);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* the small pre-draws: text lines and two list draws       */
/* ------------------------------------------------------------------------- */

/* the text item n of a screen's item table at `base` (0x007004DC, 0x00700654): +0x14 is its token */
static uint32_t item_tok(uint32_t base, uint32_t n) { return base + 4u + 0x5Cu * n + 0x14u; }

/* 0x00469D88: the token [0x70C2CA + 4 * [0x707BB4]] into 0x00A2C254 */
static int f469d88(cx *c)
{
    uint32_t ebx = p_tokstr(c, R(0x0070C2CAu + R(0x00707BB4u) * 4u), 0x00A2C254u);
    nul(c, ebx);
    return 0;
}

/* 0x0049DC42: "<[0x708020]>-<[0x708020] + 9>" at 0x00A2C254 */
static int f49dc42(cx *c)
{
    uint32_t ebx = fmt(c, R(0x00708020u), 0, 0x00A2C254u);
    ebx = put(c, ebx, 0x2D);
    ebx = fmt(c, R(0x00708020u) + 9u, 0, ebx);
    nul(c, ebx);
    return 0;
}

/* 0x004A4ED3 (stride 0x3C, rows at [0x70803C] + 0x258 * the list) and 0x00469DC2 (stride 0x50, rows at
 * [0x708044] + 0x320 * the list): up to ten rows, y from 0xF0 by 0x19: the name, a value, the row number, two
 * sprites.  A -1 at the next row ends the list through a POPAD with the loop's ECX still pushed (one word
 * out: the RET lands on the entry's EAX), a fatal exit like.  Only the first sets [0x70831C]. */
static int rows_list(cx *c, int first)
{
    uint32_t edi = first ? R(0x0070803Cu) : R(0x00708044u), y = 0xF0, n;
    uint32_t stride = first ? 0x3Cu : 0x50u;
    edi += R(0x0070C2FAu + R(0x00707BB4u) * 4u) * (first ? 0x258u : 0x320u);
    for (n = 1; n != 0xBu; n++) {
        uint32_t ebx, eax;
        W(0x00708318u, n);
        p_text(c, 0x005180ACu, edi + (first ? 0x18u : 0x20u), 0xF0, y);
        ebx = fmt(c, R(edi + (first ? 0x38u : 0x14u)), 0, A_STR);
        nul(c, ebx);
        p_text(c, 0x005180ACu, A_STR, 0x21C, y);
        ebx = fmt(c, R(0x00708318u), 0, A_STR);
        nul(c, ebx);
        p_text(c, 0x005180CCu, A_STR, 0xC8, y);
        if (first) W(0x0070831Cu, y);
        p_blit(c, 0x1Cu + R(edi + 8u), 0xD2, y);
        eax = 0x44u + R(edi + (first ? 0xCu : 0x10u));
        W(0x00708320u, eax);
        p_blit(c, R(0x00708320u), 0x1CC, y);
        edi += stride;
        if (R(edi) == 0xFFFFFFFFu) return 1;
        y += 0x19;
    }
    return 0;
}
static int f4a4ed3(cx *c) { return rows_list(c, 1); }
static int f469dc2(cx *c) { return rows_list(c, 0); }
/* 0x00469DA9: [0x707BB4] over 4 (signed): the second list, else the first */
static int f469da9(cx *c) { return (int32_t)R(0x00707BB4u) > 4 ? f469dc2(c) : f4a4ed3(c); }

/* the "<token> <n>/6: <stage's name>" line of the two stage pre-draws, from the entry at EDI: the
 * token (0x482 / 0x481), a space, [entry + 0x40] of 6, ": ", the stage record's name (0x0046F686) */
static int stage_line(cx *c, uint32_t edi, uint32_t token, uint32_t *ebxp)
{
    uint32_t ebx, rec;
    int rc;
    ebx = p_tokstr(c, token, 0x00A2C154u);
    ebx = space(c, ebx);
    ebx = of_n(c, R(edi + 0x40u), 6, ebx);
    ebx = colon(c, ebx);
    ebx = space(c, ebx);
    rec = fec_46f686(c->v, edi, NULL, &rc);
    if (rc) return 1;
    ebx = p_tokstr(c, R(rec), ebx);
    nul(c, ebx);
    *ebxp = ebx;
    return 0;
}

/* 0x00466062 */
static int f466062(cx *c)
{
    uint32_t edi, ebx;
    W(item_tok(0x007004DCu, 0), R(0x0070AA80u + R(0x00708230u) * 4u));
    edi = R(R(0x006E7E85u + R(0x00708230u) * 4u) + R(0x00708238u) * 4u);
    W(item_tok(R(0x007004DCu), 1), R(edi + 0x38u));
    edi += R(0x00708234u) * 0xA4u;
    return stage_line(c, edi, 0x482, &ebx);
}

/* 0x00465ED7 */
static int f465ed7(cx *c)
{
    uint32_t edi, ebx;
    W(item_tok(0x00700654u, 1), R(0x00708060u) == 1 ? 0x19Cu : 0x19Bu);
    W(item_tok(0x00700654u, 2), R(0x00708064u) == 1 ? 0x19Eu : 0x19Du);
    edi = R(R(0x006E7E85u + R(0x00708060u) * 4u) + R(0x0070AB76u) * 4u);
    W(item_tok(0x00700654u, 3), R(edi + 0x38u));
    edi += R(0x0070AB7Au) * 0xA4u;
    if (stage_line(c, edi, 0x481, &ebx)) return 1;
    ebx = p_tokstr(c, 0x4FCu + R(0x0070CA98u), 0x00A2C254u);
    nul(c, ebx);
    return 0;
}

/* 0x0046CA47 (the visual options' pre-draw; no PUSHAD: the registers are not returned): the item
 * 2's two words by the setting (0x00468156 / 0x0046816F, 0x0046E362 finds the item: [0x707EF5] + 4 + 0x5C),
 * then the option tokens and the "NxNxN" line */
static int f46ca47(cx *c)
{
    static const struct { uint32_t at, add, y; } T[] = {
        { 0x004F00A0u, 0x391, 0x112 }, { 0x004F00A4u, 0x56D, 0x12D }, { 0x00707FB8u, 0x56D, 0x148 },
        { 0x00707FC4u, 0x56D, 0x163 }, { 0x00707FC8u, 0x396, 0x17E }, { 0x00707FCCu, 0x393, 0x199 },
        { 0x00707FD0u, 0x394, 0x1B4 } };
    uint32_t item = R(0x00707EF5u) + 4u + 0x5Cu, edx, ebx;
    unsigned k;
    if (R(0x004F00A8u) + R(0x004F00ACu) != 2u) { W(item + 0x48u, 0xFFFFFC1Cu); W(item + 0x18u, 0x0051852Cu); }
    else { W(item + 0x48u, 0xFFFFFC1Bu); W(item + 0x18u, 0x0051840Cu); }
    W(0x00708358u, R(0x004F0098u) + 0x38Fu);
    p_tok(c, 0x005183ECu, R(0x00708358u), 0x1D1, 0xDC);
    edx = R(0x004F009Cu) * 0x10u + 0x004F00BCu;
    ebx = fmt(c, R(edx), 0, A_STR2);
    ebx = put(c, ebx, 0x78);
    ebx = fmt(c, R(edx + 4u), 0, ebx);
    ebx = put(c, ebx, 0x78);
    ebx = fmt(c, R(edx + 8u), 0, ebx);
    nul(c, ebx);
    p_text(c, 0x005183ECu, A_STR2, 0x1D1, 0xF7);
    for (k = 0; k < sizeof T / sizeof T[0]; k++) {
        W(0x00708358u, R(T[k].at) + T[k].add);
        p_tok(c, 0x005183ECu, R(0x00708358u), 0x1D1, T[k].y);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* the controller screens' pre-draws: AUDIO SETUP 0x00472DB4,   */
/* SETUP CONTROLLER 0x004671BD, program controller 0x00467E9A and 0x00467592        */
/* ------------------------------------------------------------------------- */

/* 0x0046E362 + 0x00468156 / 0x0046816F: item n (1-based) of the screen's item table [0x707EF5] + 4, 0x5C each:
 * its +0x48 word and +0x18 pointer, "greyed" (0xFFFFFC1C, 0x0051852C) or "live" (0xFFFFFC1B, 0x0051840C) */
static void item_state(cx *c, uint32_t n, int live)
{
    uint32_t it = R(0x00707EF5u) + 4u + 0x5Cu * (n - 1u);
    W(it + 0x48u, live ? 0xFFFFFC1Bu : 0xFFFFFC1Cu);
    W(it + 0x18u, live ? 0x0051840Cu : 0x0051852Cu);
}

static uint32_t cur_player(cx *c) { return R(A_PLAYERS + 4u * R(0x007082F8u)); }

/* 0x00472DB4: eight bars ([0x707FEC + 4k] * 100 / 64 percent of 0x76), each with its labels */
static int f472db4(cx *c)
{
    uint32_t k;
    W(0x00708318u, R(0x007082E4u) + 0x56Du);
    p_tok(c, 0x005183ECu, R(0x00708318u), 0x1EA, 0x1B3);
    for (k = 0; k < 8; k++) p_blit(c, 0x19, 0x222, 0xDAu + 0x1Cu * k);
    for (k = 0; k < 8; k++) {
        uint32_t v = R(0x00707FECu + 4u * k);
        W(SP_X, 0x221); W(SP_Y, 0xDAu + 0x1Cu * k); W(SP_W, 0x76); W(SP_A, 0x18); W(SP_B, 0x87);
        if (v != 0) {
            v *= 100u;
            if (v == 0) return 1;                     /* the runtime's error exit */
            v /= 0x40u;
        }
        W(SP_V, v);
        if (f4681d4(c)) return 1;
    }
    for (k = 0; k < 8; k++) {
        p_blit(c, 0x1B, 0x1E6, 0xD9u + 0x1Cu * k);
        p_tok(c, 0x00517EECu, 0x547, 0x203, 0xDDu + 0x1Cu * k);
        p_tok(c, 0x00517EECu, 0x548, 0x2B7, 0xDDu + 0x1Cu * k);
    }
    return 0;
}

/* 0x004671BD: SETUP CONTROLLER.  The player's device [+0x2C] 0: the rows greyed and one token.
 * Else the rows live, the device's name, its force-feedback row (record +0x1C: 1 shows the player's
 * strength +0x48 as a percent), the two sliders' sprites and the pad's X axis as a marker.  The pad's
 * four words are the platform's (0x00409290, through 0x0046810E). */
static int f4671bd(cx *c)
{
    uint32_t edi = cur_player(c), n, dev, eax, ebx, ecx, sv10, sv14, r[8] = { 0 };
    if (R(edi + 0x2Cu) == 0) {
        for (n = 3; n <= 8; n++) item_state(c, n, 0);
        p_tok(c, 0x005183ECu, 0x56D, 0x1BC, 0xD5);                       /* 0x00467C13 */
        return 0;
    }
    for (n = 3; n <= 8; n++) item_state(c, n, 1);
    dev = R(edi + 0x2Cu);
    edi = (dev - 1u) * 0x50u + 0x004F28A4u;
    W(0x00708378u, edi);
    p_text(c, 0x005183ECu, edi + 0x30u, 0x1BC, 0xD5);
    edi = cur_player(c);
    W(0x00708318u, R(edi + 0x3Cu) + 0x56Du);
    p_tok(c, 0x005183ECu, R(0x00708318u), 0x1BC, 0xF0);
    edi = R(0x00708378u);
    item_state(c, 4, R(edi + 0x1Cu) == 1);
    edi = R(0x00708378u);
    if (R(edi + 0x1Cu) != 1) p_tok(c, 0x005183ECu, 0x56D, 0x1BC, 0x10B);
    else {
        edi = cur_player(c);
        if ((int32_t)R(edi + 0x48u) > 0) {
            ebx = fmt(c, R(edi + 0x48u), 0, A_STR);                      /* 0x00401AB7: n, '%' */
            ebx = put(c, ebx, 0x25);
            nul(c, ebx);
            p_text(c, 0x005183ECu, A_STR, 0x1BC, 0x10B);
        } else p_tok(c, 0x005183ECu, 0x56D, 0x1BC, 0x10B);
    }
    edi = cur_player(c);
    W(0x00708318u, R(edi + 0x34u) + 0x3A1u);
    p_tok(c, 0x005183ECu, R(0x00708318u), 0x1BC, 0x126);
    p_blit(c, 5, 0x1B8, 0x14C);
    eax = R(edi + 0x40u);
    sv14 = R(0x004F0014u); sv10 = R(0x004F0010u);
    ebx = 0x281u - eax - 1u;
    W(0x004F0014u, ebx);
    W(0x0070834Cu, ebx + 1u);
    ebx = 0x1C0u + eax;
    W(0x004F0010u, ebx);
    W(0x00708348u, ebx - 4u);
    p_blit(c, 3, 0x1BC, 0x14C);
    eax = 0x64u - R(edi + 0x44u);
    ebx = 0x285u - eax - 1u;
    W(0x004F0014u, ebx);
    W(0x00708354u, ebx + 1u);
    ebx = 0x1BCu + eax;
    W(0x004F0010u, ebx);
    W(0x00708350u, ebx - 4u);
    p_blit(c, 4, 0x1BC, 0x14C);
    W(0x004F0010u, sv10);
    W(0x004F0014u, sv14);
    p_blit(c, 7, R(0x00708348u), 0x14C);
    p_blit(c, 7, R(0x0070834Cu), 0x14C);
    p_blit(c, 6, R(0x00708350u), 0x14C);
    p_blit(c, 6, R(0x00708354u), 0x14C);
    svc(c, 0x00409290u, r);                                               /* 0x0046810E */
    W(0x0070835Cu, r[VM_EBX]);
    ecx = r[VM_ECX];
    if (ecx == 0) eax = 0x21A;
    else if ((int32_t)ecx < 0) {
        ecx = 0u - ecx;
        eax = ecx * 100u;
        if (eax == 0) return 1;
        eax = 0u - eax / 0x100u + 1u + 0x21Au;
    } else {
        eax = ecx * 100u;
        if (eax == 0) return 1;
        eax = eax / 0x100u + 0x21Au;
    }
    W(0x00708358u, eax);
    p_blit(c, R(0x0070835Cu) != 0 ? 9 : 8, eax, 0x156);
    return 0;
}

/* 0x0040929E with EAX not 0: the first of the six axes (lX..) whose value moved from the snapshot
 * 0x004F3230 by more than 0x80 either way, among those the device's mask [+0x14] lists: AL its index
 * (| 0x10 for up); CF 0 then.  CF 1 when none moved. */
static int axis_test(cx *c, uint32_t *al)
{
    uint32_t dev = R(0x004F1EB8u) * 0x50u + 0x004F28A4u, k;
    for (k = 0; k < 6; k++) {
        int32_t d = (int32_t)(R(0x004F19F0u + 4u * k) - R(0x004F3230u + 4u * k));
        if ((R(dev + 0x14u) >> k) & 1u) {
            if (d < -0x80) { *al = k; return 0; }
            if (d > 0x80) { *al = k | 0x10u; return 0; }
        }
    }
    return 1;
}

static uint32_t popcount32(uint32_t x) { uint32_t n = 0; while (x) { n += x & 1u; x >>= 1; } return n; }

/* 0x0046663C: the pad's four words (EBX the buttons, ECX X, EDX the brake, EAX the throttle) as one
 * code in [0x708358]: a lone button's number, or an axis held (0x400 / 0x800 / 0x100 / 0x200) once
 * the last 32 samples of the other three had at least 0x1D of them at rest.  The four shift words
 * 0x707B54..0x707B60 take one bit a call (set: the axis was within 0x80 of rest).  The log calls in
 * it are the host's no-ops. */
static void f46663c(cx *c, uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t eax)
{
    uint32_t ebp = 0, m;
    if (ebx != 0) {
        do { ebp++; m = ebx & 1u; ebx >>= 1; } while (m == 0);
        if (ebx != 0) ebp = 0;
    }
    ebx = ecx;
    if ((int32_t)ebx > 0) ebx = 0;
    if ((int32_t)ebx < 0) ebx = 0u - ebx;
    if ((int32_t)ecx < 0) ecx = 0;
    W(0x00707B54u, (R(0x00707B54u) << 1) | (ebx < 0x80u));
    W(0x00707B58u, (R(0x00707B58u) << 1) | (ecx < 0x80u));
    W(0x00707B5Cu, (R(0x00707B5Cu) << 1) | (edx < 0x80u));
    W(0x00707B60u, (R(0x00707B60u) << 1) | (eax < 0x80u));
    ebx = popcount32(R(0x00707B54u));
    ecx = popcount32(R(0x00707B58u));
    edx = popcount32(R(0x00707B5Cu));
    eax = popcount32(R(0x00707B60u));
    if (ebx == 0) {
        if (eax > ecx) eax = ecx;
        if (eax > edx) eax = edx;
        ebp = eax < 0x1Du ? 0 : ebp | 0x400u;
    } else if (ecx == 0) {
        if (eax > ebx) eax = ebx;
        if (eax > edx) eax = edx;
        ebp = eax < 0x1Du ? 0 : ebp | 0x800u;
    } else if (edx == 0) {
        if (eax > ebx) eax = ebx;
        if (eax > ecx) eax = ecx;
        ebp = eax < 0x1Du ? 0 : ebp | 0x100u;
    } else if (eax == 0) {
        if (ebx > ecx) ebx = ecx;
        if (ebx > edx) ebx = edx;
        ebp = ebx < 0x1Du ? 0 : ebp | 0x200u;
    }
    W(0x00708358u, ebp);
}

/* 0x00467E9A: program controller, the waiting row.  While [0x707B64] is 1 the row [0x707B6C] waits
 * for a press: past 3000 ms it gives up (row -1); the cursor's item's own two words are drawn;
 * on a device whose record says +0x34 = 3 (a pad's axis), rows 1 and 2 take the first axis that
 * moved (0x0040929E); else the pad's words give a code (0x0046663C); the key SPACE (0x39) clears.
 * A code in [0x708358] is stored into the player's row [+0x50 + 4 * (row - 1)]. */
static int f467e9a(cx *c)
{
    uint32_t edi, eax, r[8] = { 0 }, al = 0;
    int cf = 1;
    if (R(0x00707B64u) == 0) return 0;
    svc(c, 0x00410E0Cu, r);                                               /* the clock */
    eax = r[VM_EAX] - R(0x00707B68u);
    if ((int32_t)eax >= 0xBB8) {
        W(0x00707B64u, 0);
        W(0x00707B6Cu, 0xFFFFFFFFu);
        return 0;
    }
    if (R(0x00708310u) == 1) {
        edi = R(0x00707EF5u) + 4u + (R(0x00707E91u) - 1u) * 0x5Cu;
        W(0x00708318u, R(edi + 8u));
        W(0x0070831Cu, R(edi + 0xCu));
        p_tok(c, 0x005183ECu, 0x3BE, 0x1B8, R(0x0070831Cu));
    }
    edi = cur_player(c);
    if (R(edi + 0x34u) == 3 && (R(0x00707B6Cu) == 1 || R(0x00707B6Cu) == 2)) {
        uint32_t sel = R(0x00707B6Cu);
        cf = axis_test(c, &al);
        if (!cf) {
            WB(edi + (sel == 1 ? 0x3Bu : 0x3Au), al);
            W(0x004F1ED0u, R(edi + 0x38u));
            W(0x00708358u, sel == 1 ? 0x200u : 0x100u);
            W(0x00707B64u, 0);
            goto store;
        }
    } else {
        uint32_t d[8] = { 0 };
        svc(c, 0x00409290u, d);                                           /* 0x0046810E */
        f46663c(c, d[VM_EBX], d[VM_ECX], d[VM_EDX], d[VM_EAX]);
    }
    {
        uint32_t k[8] = { 0 };
        svc(c, 0x00407CDCu, k);                                           /* the key */
        if ((k[VM_EAX] & 0xFFu) == 0x39) {
            W(0x00708358u, 0);
            W(0x00707B64u, 0);
            goto put_code;
        }
    }
store:
    if (R(0x00708358u) == 0) return 0;
put_code:
    edi = cur_player(c) + 0x50u;
    edi = edi + R(0x00707B6Cu) * 4u - 4u;
    W(edi, R(0x00708358u));
    return 0;
}

/* 0x00462BFB: " (", the sign (+ when bit 4 of AL is set), the axis letter (x y z for 0..2, then
 * r-prefixed: the original writes 'r' and overwrites it with the letter, one place on), ")" */
static uint32_t f462bfb(cx *c, uint8_t al, uint32_t ebx)
{
    uint8_t ah = 0x2D;
    ebx = space(c, ebx);
    ebx = put(c, ebx, 0x28);
    if (al & 0x10u) { al = (uint8_t)(al - 0x10u); ah = 0x2B; }
    ebx = put(c, ebx, ah);
    if (al >= 3) { WB(ebx, 0x72); al = (uint8_t)(al - 3u); }
    al = (uint8_t)(al + 0x78u);
    ebx = put(c, ebx, al);
    return put(c, ebx, 0x29);
}

/* 0x00467592: the ten bindings' texts, one per row at y 0xD5 + 0x1B * row: the player's word
 * [+0x50 + 4 * row] (button in the low byte, axis in the next); nothing while the row waits for a press
 * (the row [0x707B6C]); "none" (token 0x3BD) for 0; else the axis's token (the second table when the
 * device record's +0x34 is 2 or more), rows 1 and 2 with the axis's " (-x)" tail when it is 3, then
 * "+" and "<token 0x3C0> <button>" */
static int f467592(cx *c)
{
    static const uint16_t Y[10] = { 0xD5, 0xF0, 0x10B, 0x126, 0x141, 0x15C, 0x177, 0x192, 0x1AD, 0x1C8 };
    uint32_t edi = cur_player(c), k;
    for (k = 0; k < 10; k++) {
        uint32_t edx = R(edi + 0x50u + 4u * k), ebx = A_TEXT, dh = (edx >> 8) & 0xFFu, dl = edx & 0xFFu;
        if (R(0x00707B64u) == 1 && R(0x00707B6Cu) == k + 1u) continue;
        if (edx == 0) ebx = p_tokstr(c, 0x3BD, ebx);
        else {
            if (dh != 0) {
                uint32_t tok = rd16(c, 0x00707B86u + 2u * dh);
                if (R(edi + 0x34u) < 2) tok = rd16(c, 0x00707B74u + 2u * dh);
                ebx = p_tokstr(c, tok, ebx);
                if (k < 2 && R(edi + 0x34u) == 3) ebx = f462bfb(c, RB(edi + (k == 0 ? 0x3Bu : 0x3Au)), ebx);
            }
            if (dl != 0) {
                if (dh != 0) ebx = put(c, ebx, 0x2B);                      /* 0x00462BF6 */
                ebx = p_tokstr(c, 0x3C0, ebx);
                ebx = space(c, ebx);
                ebx = fmt(c, dl, 0, ebx);
            }
        }
        nul(c, ebx);
        p_text(c, 0x005183ECu, A_TEXT, 0x1B8, Y[k]);
    }
    return 0;
}

/* 0x00479F65: the INSPECTION AREA's notice (PUSHAD ... POPAD).  "PLAYER n" in the title font, "your car has
 * failed the inspection check" (token 0x524) and the thirteen component names (0x525..0x531: three in the
 * big font, ten in the small), the inspection bar (0x004681D4) and its two labels, then one sprite per
 * entry of the failed list at 0x0070BF82 (a word each, -1 ends; sprite 0x9E + (word ^ 1), x 0x190, y from
 * 0x82 by 0x1B).  [0x708384] = 0x190 and the last y are left in [0x708384] / [0x708358]; a list with no end
 * runs the original out of memory: reported as a fatal exit. */
static int f479f65(cx *c)
{
    uint32_t ebx, edi, ebp, k, guard = 0;
    ebx = p_tokstr(c, 0x190, A_TEXT);
    ebx = space(c, ebx);
    ebx = fmt(c, R(0x007082F8u) + 1u, 0, ebx);                    /* 0x004019CD */
    nul(c, ebx);
    p_text(c, 0x0051842Cu, A_TEXT, 0x190, 0x14);
    p_tok(c, 0x0051842Cu, 0x524, 0x190, 0x64);
    p_tok(c, 0x0051846Cu, 0x525, 0x186, 0x82);
    p_tok(c, 0x0051846Cu, 0x526, 0x186, 0x9D);
    p_tok(c, 0x0051846Cu, 0x527, 0x186, 0xB8);
    for (k = 0; k < 10; k++) p_tok(c, 0x0051840Cu, 0x528u + k, 0x186, 0xD3u + 0x1Bu * k);
    W(SP_X, 0x128); W(SP_Y, 0x1E3); W(SP_W, 0xD0); W(SP_A, 0x89); W(SP_B, 0x87);
    W(SP_V, R(0x0070CBE8u));
    if (f4681d4(c)) return 1;
    p_blit(c, 0x88, 0xEC, 0x1E2);
    p_tok(c, 0x00517EECu, 0x4AE, 0x10A, 0x1E6);
    p_tok(c, 0x00517EECu, 0x4AF, 0x217, 0x1E6);
    W(0x00708384u, 0x190);
    ebp = 0x82;
    edi = 0x0070BF82u;
    while (R(edi) != 0xFFFFFFFFu) {
        uint32_t sprite = 0x9Eu + (R(edi) ^ 1u);
        if (++guard > 0x100000u) return 1;      /* more words than the image has memory */
        W(0x00708358u, ebp);
        W(0x0070835Cu, sprite);
        p_blit(c, sprite, R(0x00708384u), R(0x00708358u));
        edi += 4;
        ebp += 0x1B;
        W(0x00708358u, ebp);
    }
    return 0;
}

/* 0x00407C80: the key AL's name (AL & 0x7F) at EBX, EBX advanced: the token in the lexicon's key table
 * ([[0x004EB7A8] + 0x10], a word a key), else "0x" and the key's two hex digits (0x004020E4 builds all eight
 * of EAX and 0x00401F69 copies the last two) */
static uint32_t f407c80(cx *c, uint32_t key, uint32_t ebx)
{
    uint32_t tab = R(R(0x004EB7A8u) + 0x10u), tok;
    key &= 0x7Fu;
    tok = rd16(c, tab + 2u * key);
    if (tok) return p_tokstr(c, tok, ebx);
    ebx = put(c, ebx, 0x30);
    ebx = put(c, ebx, 0x78);
    ebx = put(c, ebx, RB(0x004DB148u + (key >> 4)));
    return put(c, ebx, RB(0x004DB148u + (key & 0xFu)));
}

/* 0x0047906A: the keyboard screen's key names (PUSHAD ... POPAD).  The player's eleven key bytes at
 * +0x1C .. +0x23 and +0x25 .. +0x27 (+0x24, the wipers, has no row:), one a row
 * at y 0xAF + 0x1D * row, x 0x1CC, in the small font.  The row being captured ([0x708018] = its slot) shows
 * token 4 instead when [0x708314] is 1, and nothing when not. */
static int f47906a(cx *c)
{
    static const uint8_t OFF[11] = { 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27 };
    static const uint8_t SLOT[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11 };
    uint32_t edi = R(A_PLAYERS + 4u * R(0x007082F8u)), k;
    for (k = 0; k < 11; k++) {
        uint32_t y = 0xAFu + 0x1Du * k, key = RB(edi + OFF[k]);
        if (R(0x00708018u) != SLOT[k]) {
            uint32_t ebx = f407c80(c, key, A_TEXT);
            nul(c, ebx);
            p_text(c, 0x005183ECu, A_TEXT, 0x1CC, y);
        } else if (R(0x00708314u) == 1) p_tok(c, 0x005183ECu, 4, 0x1CC, y);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

static int f4681d4_e(cx *c) { return f4681d4(c); }
static int f46825c_e(cx *c) { return f46825c(c); }

typedef struct { uint32_t va; int (*fn)(cx *c); } ent;
static const ent TAB[] = {
    { 0x004A1C11, f4a1c11 }, { 0x004A1A56, f4a1a56 },
    { 0x004681D4, f4681d4_e }, { 0x0046825C, f46825c_e },
    { 0x0046F2EB, f46f2eb },
    { 0x0048A411, f48a411 },
    { 0x00469D88, f469d88 }, { 0x0049DC42, f49dc42 }, { 0x00469DA9, f469da9 },
    { 0x00466062, f466062 }, { 0x00465ED7, f465ed7 }, { 0x0046CA47, f46ca47 },
    { 0x00472DB4, f472db4 }, { 0x004671BD, f4671bd }, { 0x00467E9A, f467e9a }, { 0x00467592, f467592 },
    { 0x00479F65, f479f65 }, { 0x0047906A, f47906a },
};
#define NTAB ((int)(sizeof TAB / sizeof TAB[0]))

int fdv_run(uint32_t va, vm *v, const fdsg_draw *d)
{
    int i;
    cx c;
    c.v = v; c.d = d;
    for (i = 0; i < NTAB; i++)
        if (TAB[i].va == va) return TAB[i].fn(&c) ? FDSG_FATAL : FDSG_OK;
    return FDSG_NOT_MINE;
}
int fdv_count(void) { return NTAB; }
uint32_t fdv_va(int i) { return TAB[i].va; }

typedef struct { uint32_t va; int (*fn)(cx *c, uint32_t edi); } rent;
static const rent RTAB[] = { { 0x0047091E, f47091e }, { 0x00470AFA, f470afa } };
#define NRTAB ((int)(sizeof RTAB / sizeof RTAB[0]))

int fdv_run_edi(uint32_t va, vm *v, const fdsg_draw *d, uint32_t edi)
{
    int i;
    cx c;
    c.v = v; c.d = d;
    for (i = 0; i < NRTAB; i++)
        if (RTAB[i].va == va) return RTAB[i].fn(&c, edi) ? FDSG_FATAL : FDSG_OK;
    return FDSG_NOT_MINE;
}
int fdv_edi_count(void) { return NRTAB; }
uint32_t fdv_edi_va(int i) { return RTAB[i].va; }
