/* fd_cspecs.c - see fd_cspecs.h.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps as
 * the original's does.  The original's text helpers (the 0x00401xxx routines
 * that append to a string at EBX) are C functions over the same cursor. */
#include "fd_cspecs.h"

#include "fe_logic.h"

typedef struct { vm *v; const fdsg_draw *d; int rc; } cx;

#define R(a)     vm_rd32(c->v, (a))
#define W(a, x)  vm_wr32(c->v, (a), (x))
#define RB(a)    vm_rd8(c->v, (a))
#define WB(a, x) vm_wr8(c->v, (a), (uint8_t)(x))

enum {
    A_STR     = 0x00A2BF54,     /* the buffer the pages build their values in */
    A_STR2    = 0x008E80D0,     /* the token-run buffer 0x00402262 builds in */
    A_NAME    = 0x00A2C254,     /* the car row's string (str#4) */
    A_PAGE    = 0x00A2C354,     /* the page row's string (str#5) */
    F_A = 0x005180AC, F_B = 0x005180CC,
    A_CARS    = 0x0070C5C8,     /* the pointer to the list of car records */
    A_CAR     = 0x007082FC,
    A_PAGEN   = 0x00708304,
    A_LAST    = 0x00708E08,     /* the last car's index */
    A_UNIT    = 0x00517954
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
/* 0x00401F8F: the token's text at EBX; EBX advanced */
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
static uint32_t nul(cx *c, uint32_t ebx)   { return put(c, ebx, 0); }      /* 0x00401F02 */
static uint32_t crlf(cx *c, uint32_t ebx)                                   /* 0x00401F0C */
{
    WB(ebx, 0x0D); WB(ebx + 1u, 0x0A);
    return ebx + 2u;
}
static uint32_t space(cx *c, uint32_t ebx) { return put(c, ebx, 0x20); }   /* 0x00401F15 */
static uint32_t at(cx *c, uint32_t ebx)    { return put(c, ebx, 0x40); }   /* 0x00401F29 */
static uint32_t slash(cx *c, uint32_t ebx) { return put(c, ebx, 0x2F); }   /* 0x00401F2E */
static uint32_t dot(cx *c, uint32_t ebx)   { return put(c, ebx, 0x2E); }   /* 0x00401F33 */
static uint32_t quote(cx *c, uint32_t ebx) { return put(c, ebx, 0x22); }   /* 0x00401F42 */

/* 0x00402011 */
static uint32_t fmt(cx *c, uint32_t val, uint32_t flags, uint32_t ebx)
{
    return fel_fmt_num(c->v, val, flags, ebx);
}

/* (EAX * m + 0x1388) / 0x2710 as 0x00401925 / 0x00401940 / 0x0040195B / 0x00401976
 * do it: a 64-bit product, the carry added in, one DIV (a quotient beyond 32 bits
 * is the original's #DE) */
static uint32_t scale(cx *c, uint32_t eax, uint32_t m)
{
    uint64_t q = ((uint64_t)eax * m + 0x1388u) / 0x2710u;
    if (q >> 32) c->rc = FDSG_FATAL;
    return (uint32_t)q;
}

/* 0x004019CD: EAX as a number */
static uint32_t t4019cd(cx *c, uint32_t eax, uint32_t ebx) { return fmt(c, eax, 0, ebx); }

/* 0x00401C2E: a length in the player's units.  An 0xFF in AH is "none" (a dash);
 * unit 0 shows EAX * 36 / 100 as hundredths and a quote, the others the scaled
 * value in tenths and the unit's token */
static uint32_t t401c2e(cx *c, uint32_t eax, uint32_t ebx)
{
    if (((eax >> 8) & 0xFFu) == 0xFFu) return p_tokstr(c, 0x578u, ebx);
    if (R(A_UNIT) != 0) {
        eax = scale(c, eax, 0x23B8u);
        ebx = fmt(c, eax / 10u, 0, ebx);
        ebx = dot(c, ebx);
        ebx = fmt(c, eax % 10u, 0, ebx);
        return p_tokstr(c, 0x55Au, ebx);
    } else {
        uint64_t p = (uint64_t)eax * 0x24u;
        uint32_t q = (uint32_t)(p / 100u);
        if ((p / 100u) >> 32) c->rc = FDSG_FATAL;
        ebx = fmt(c, q / 100u, 0, ebx);
        ebx = dot(c, ebx);
        ebx = fmt(c, q % 100u, 0x12, ebx);
        return quote(c, ebx);
    }
}

/* 0x00401CA6 / 0x00401CDD: EAX scaled when the unit is not 0 (by 0x1D24 / 0x11B8),
 * in tenths cut off, and the unit's token from the word table at `tab` */
static uint32_t t_unit1(cx *c, uint32_t eax, uint32_t ebx, uint32_t m, uint32_t tab)
{
    if (R(A_UNIT) != 0) eax = scale(c, eax, m);
    ebx = fmt(c, eax / 10u, 0, ebx);
    return p_tokstr(c, rd16(c, tab + 2u * R(A_UNIT)), ebx);
}
static uint32_t t401ca6(cx *c, uint32_t eax, uint32_t ebx) { return t_unit1(c, eax, ebx, 0x1D24u, 0x005186B4u); }
static uint32_t t401cdd(cx *c, uint32_t eax, uint32_t ebx) { return t_unit1(c, eax, ebx, 0x11B8u, 0x005186BCu); }

/* 0x00401D14: EAX scaled by 0x567 when the unit is not 0, tenths with a point
 * when it is not 0, and the unit's token (0x005186B8) */
static uint32_t t401d14(cx *c, uint32_t eax, uint32_t ebx)
{
    uint32_t rem;
    if (R(A_UNIT) != 0) eax = scale(c, eax, 0x567u);
    rem = eax % 10u;
    ebx = fmt(c, eax / 10u, 0, ebx);
    if (R(A_UNIT) != 0) {
        ebx = dot(c, ebx);
        ebx = fmt(c, rem, 0, ebx);
    }
    return p_tokstr(c, rd16(c, 0x005186B8u + 2u * R(A_UNIT)), ebx);
}

/* 0x00402262: EAX, EAX + 1, ... (N tokens) as text at 0x008E80D0 */
static uint32_t token_run(cx *c, uint32_t eax, uint32_t n)
{
    uint32_t b = A_STR2;
    for (; n; n--, eax++) b = p_tokstr(c, eax, b);
    nul(c, b);
    return A_STR2;
}

/* ---- the page draws ---------------------------------------------------------- */

/* a label (a token in the label font) at (x, y) */
static void label(cx *c, uint32_t x, uint32_t y, uint32_t token) { p_tok(c, F_B, token, x, y); }
/* the string built at A_STR, as text at (x, y) */
static void value(cx *c, uint32_t x, uint32_t y, uint32_t ebx)
{
    nul(c, ebx);
    p_text(c, F_A, A_STR, x, y);
}
/* a value that is a token of the record (the value font) at (x, y) */
static void tokval(cx *c, uint32_t x, uint32_t y, uint32_t token) { p_tok(c, F_A, token, x, y); }

/* "none" (the dash, token 0x578) when the field is 0, else the length at EBX */
static uint32_t len_or_dash(cx *c, uint32_t eax, uint32_t ebx)
{
    return eax == 0 ? p_tokstr(c, 0x578u, ebx) : t401c2e(c, eax, ebx);
}

/* 0x004669FE: page 0, the description: seven tokens from [EDI + 0x70] */
static void page0(cx *c, uint32_t edi)
{
    token_run(c, R(edi + 0x70u), 7);
    W(0x00708358u, A_STR2);
    p_text(c, F_A, A_STR2, 0x1A4u, 0xD5u);
}

/* 0x00466A2F: page 3, seven lengths (the car's dimensions) */
static void page3(cx *c, uint32_t edi)
{
    static const struct { uint32_t y, tok, fld; } row[7] = {
        { 0x10A, 0x25B, 0x54 }, { 0x121, 0x25C, 0x58 }, { 0x138, 0x25D, 0x5C },
        { 0x14F, 0x25E, 0x60 }, { 0x166, 0x25F, 0x64 }, { 0x17D, 0x260, 0x68 },
        { 0x194, 0x261, 0x6C }
    };
    int i;
    for (i = 0; i < 7; i++) {
        uint32_t e, b;
        label(c, 0x28Bu, row[i].y, row[i].tok);
        e = R(edi + row[i].fld);
        b = A_STR;
        if (i == 6) b = e == 0 ? p_tokstr(c, 0x578u, b) : t401cdd(c, e, b);
        else b = len_or_dash(c, e, b);
        value(c, 0x28Bu, row[i].y, b);
    }
}

/* 0x00466CA1: page 1, the engine and the gearbox */
static void page1(cx *c, uint32_t edi)
{
    uint32_t e, b;
    /* 0x24F */
    label(c, 0x244u, 0x10Au, 0x24Fu);
    e = R(edi + 0xCu);
    if (e == 0) b = p_tokstr(c, 0x578u, A_STR);
    else { b = t4019cd(c, e, A_STR); b = p_tokstr(c, 0x266u, b); }
    value(c, 0x244u, 0x10Au, b);
    /* 0x250: a token */
    label(c, 0x244u, 0x121u, 0x250u);
    tokval(c, 0x244u, 0x121u, R(edi + 0x10u));
    /* 0x251: a figure over a figure */
    label(c, 0x244u, 0x14Fu, 0x251u);
    e = R(edi + 0x14u);
    if (e == 0) b = p_tokstr(c, 0x578u, A_STR);
    else {
        b = t401ca6(c, e, A_STR);
        e = R(edi + 0x18u);
        if (e == 0) b = p_tokstr(c, 0x578u, b);
        else {
            b = slash(c, b);
            b = t4019cd(c, R(edi + 0x18u), b);
            b = p_tokstr(c, 0x559u, b);
        }
    }
    value(c, 0x244u, 0x14Fu, b);
    /* 0x252: a figure at a figure */
    label(c, 0x244u, 0x166u, 0x252u);
    e = R(edi + 0x1Cu);
    if (e == 0) b = p_tokstr(c, 0x578u, A_STR);
    else {
        b = t401d14(c, e, A_STR);
        b = at(c, b);
        b = t4019cd(c, R(edi + 0x20u), b);
        b = p_tokstr(c, 0x559u, b);
    }
    value(c, 0x244u, 0x166u, b);
    /* 0x253, 0x254: lengths */
    label(c, 0x244u, 0x17Du, 0x253u);
    b = len_or_dash(c, R(edi + 0x24u), A_STR);
    value(c, 0x244u, 0x17Du, b);
    label(c, 0x244u, 0x194u, 0x254u);
    b = len_or_dash(c, R(edi + 0x28u), A_STR);
    value(c, 0x244u, 0x194u, b);
    /* 0x255: a token */
    label(c, 0x244u, 0x1ABu, 0x255u);
    tokval(c, 0x244u, 0x1ABu, R(edi + 0x2Cu));
}

/* 0x00466F11: page 2, the drive and the chassis */
static void page2(cx *c, uint32_t edi)
{
    uint32_t b;
    label(c, 0x220u, 0xF3u, 0x256u);   tokval(c, 0x220u, 0xF3u, R(edi + 0x30u));
    label(c, 0x220u, 0x10Au, 0x257u);  tokval(c, 0x220u, 0x10Au, R(edi + 0x34u));
    label(c, 0x220u, 0x121u, 0x258u);  tokval(c, 0x220u, 0x121u, R(edi + 0x38u));
    /* 0x259: two lines, a length and a word each */
    label(c, 0x220u, 0x138u, 0x259u);
    b = p_tokstr(c, 0x271u, A_STR);
    b = len_or_dash(c, R(edi + 0x3Cu), b);
    b = space(c, b);
    b = p_tokstr(c, R(edi + 0x44u), b);
    b = crlf(c, b);
    b = p_tokstr(c, 0x272u, b);
    b = len_or_dash(c, R(edi + 0x40u), b);
    b = space(c, b);
    b = p_tokstr(c, R(edi + 0x48u), b);
    b = crlf(c, b);
    value(c, 0x220u, 0x138u, b);
    /* 0x25A: two lines of two words */
    label(c, 0x220u, 0x187u, 0x25Au);
    b = p_tokstr(c, 0x271u, A_STR);
    b = p_tokstr(c, R(edi + 0x4Cu), b);
    b = crlf(c, b);
    b = p_tokstr(c, 0x272u, b);
    b = p_tokstr(c, R(edi + 0x50u), b);
    b = crlf(c, b);
    value(c, 0x220u, 0x187u, b);
}

/* ---- the screen's own routines -------------------------------------------- */

/* the record of the car [0x7082FC] in the list [0x70C5C8] */
static uint32_t car_record(cx *c) { return R(R(A_CARS) + 4u * R(A_CAR)); }

/* 0x004650C4: the art of this page of this car, [[0x70BDE2 + page * 4] + id * 4] */
static void f4650c4(cx *c)
{
    uint32_t tab = R(0x0070BDE2u + 4u * R(A_PAGEN)), id = R(car_record(c));
    uint32_t r[8] = { 0 };
    r[VM_EAX] = R(tab + 4u * id);
    svc(c, 0x0047B0C2u, r);
}

/* ---- SLIDESHOW: the car's pictures, files\g\slides\<code>sl00..99 ---------- */

enum { A_PIC = 0x00708300, A_PATH = 0x008F8550, A_REQ = 0x00707ECD };

/* the draw layer's one flag-returning service: CF as the original's routine left it
 * (CF = 1 failed / not there), read from the VM straight after the call */
static int svc_cf(cx *c, uint32_t va, uint32_t eax, uint32_t ebx)
{
    uint32_t r[8] = { 0 };
    r[VM_EAX] = eax; r[VM_EBX] = ebx;
    svc(c, va, r);
    return vm_cf(c->v) != 0;
}

/* 0x0046512D: EAX the direction (its sign).  The car's picture file at the number
 * [0x708300] - the template [0x70BE1E + id * 4] through 0x00403071, tried as .png
 * (0x00402CA3: CF = not there), then with "cx" over the last two letters (.pcx) -
 * and, when neither is there, the number stepped by the direction's sign round 0..99
 * until it is back where it began.  Then the picture is loaded with the title bar
 * (0x006E46BC) over it (0x004115E1; if that fails the title bar alone, 0x00411573)
 * and shown (0x00405E6D). */
static void f46512d(cx *c, uint32_t dir)
{
    uint32_t id = R(car_record(c)), start = R(A_PIC);
    for (;;) {
        uint32_t tmpl = R(0x0070BE1Eu + 4u * id), len = 0;
        int32_t n;
        if (!fel_numbered_path(c->v, tmpl, R(A_PIC))) { c->rc = FDSG_FATAL; return; }
        if (!svc_cf(c, 0x00402CA3u, A_PATH, 0)) break;                /* the .png */
        while (RB(A_PATH + len)) len++;                           /* 0x0040227F */
        WB(A_PATH + len - 2u, 0x63); WB(A_PATH + len - 1u, 0x78);  /* MOV word [..], 0x7863 */
        if (!svc_cf(c, 0x00402CA3u, A_PATH, 0)) break;                /* the .pcx */
        n = (int32_t)R(A_PIC);
        if ((int32_t)dir < 0) { n--; if (n < 0) n = 0x63; }
        else                  { n++; if (n > 0x63) n = 0; }
        W(A_PIC, (uint32_t)n);
        if (R(A_PIC) == start) break;
    }
    if (svc_cf(c, 0x004115E1u, A_PATH, 0x006E46BCu))
        svc_cf(c, 0x00411573u, 0x006E46BCu, 0);
    svc_cf(c, 0x00405E6Du, 0, 0);
}

/* 0x00466869: the car's narration list [0x70C342 + id * 4] starts */
static void f466869(cx *c)
{
    uint32_t id = R(car_record(c)), list = R(0x0070C342u + 4u * id), r[8] = { 0 };
    W(0x00A2C588u, 1);
    W(0x00A2C584u, list);
    W(0x00A2C580u, 0);
    r[VM_EAX] = R(list);
    r[VM_ECX] = R(0x00708000u);
    svc(c, 0x0040F566u, r);
}

/* 0x00466837 / 0x004668BB: the car, one back / on, round the list */
static void car_step(cx *c, int dirn)
{
    if (dirn < 0) { if (R(A_CAR) == 0) W(A_CAR, R(A_LAST)); else W(A_CAR, R(A_CAR) - 1u); }
    else          { if (R(A_CAR) == R(A_LAST)) W(A_CAR, 0); else W(A_CAR, R(A_CAR) + 1u); }
    W(0x00707ECDu, 0);
    f4650c4(c);
    f466869(c);
}

/* 0x0046696C / 0x0046698F: the page, round the four, then the art */
static void page_step(cx *c, int dirn)
{
    if (dirn < 0) { if ((int32_t)R(A_PAGEN) <= 0) W(A_PAGEN, 3); else W(A_PAGEN, R(A_PAGEN) - 1u); }
    else          { if (R(A_PAGEN) == 3) W(A_PAGEN, 0); else W(A_PAGEN, R(A_PAGEN) + 1u); }
    f4650c4(c);
}

/* 0x004668F3 / 0x0046692E (SLIDESHOW): the car, one back / on, round the list; the first
 * picture of the new car (the number 0, then the series from there), then its narration */
static void slide_car_step(cx *c, int dirn)
{
    int32_t n = (int32_t)R(A_CAR);
    if (dirn < 0) { n--; if (n < 0) n = (int32_t)R(A_LAST); }
    else          { n++; if (n > (int32_t)R(A_LAST)) n = 0; }
    W(A_CAR, (uint32_t)n);
    W(A_REQ, 0);
    W(A_PIC, 0);
    f46512d(c, 1);
    if (c->rc == FDSG_OK) f466869(c);
}

/* 0x004669B2 / 0x004669D8 (SLIDESHOW): the picture number one back / on, round 0..99, then
 * the series from there in the arrow's direction */
static void pic_step(cx *c, int dirn)
{
    int32_t n = (int32_t)R(A_PIC);
    if (dirn < 0) { n--; if (n < 0) n = 0x63; }
    else          { n++; if (n > 0x63) n = 0; }
    W(A_PIC, (uint32_t)n);
    f46512d(c, dirn < 0 ? 0xFFFFFFFFu : 1u);
}

/* 0x004667D6 / 0x004667FC: a token's text, as the row's string */
static void name_row(cx *c, uint32_t token, uint32_t dst)
{
    uint32_t b = p_tokstr(c, token, dst);
    nul(c, b);
}

/* 0x00478F77: the page's draw over the car's record */
static void f478f77(cx *c)
{
    uint32_t edi = car_record(c), pg = R(0x0070BDF6u + 4u * R(A_PAGEN));
    switch (pg) {
    case 0x004669FEu: page0(c, edi); break;
    case 0x00466CA1u: page1(c, edi); break;
    case 0x00466F11u: page2(c, edi); break;
    case 0x00466A2Fu: page3(c, edi); break;
    default: c->rc = FDSG_FATAL; break;      /* a call through a word that is not one */
    }
}

/* ------------------------------------------------------------------------- */

static const uint32_t VAS[] = {
    0x004650C4u, 0x00466869u, 0x00466837u, 0x004668BBu, 0x0046696Cu, 0x0046698Fu,
    0x004667D6u, 0x004667FCu, 0x00478F77u,
    0x004669FEu, 0x00466CA1u, 0x00466F11u, 0x00466A2Fu,
    0x0046512Du, 0x004668F3u, 0x0046692Eu, 0x004669B2u, 0x004669D8u, 0x0046681Fu     /* SLIDESHOW */
};

int fdq_count(void) { return (int)(sizeof VAS / sizeof VAS[0]); }
uint32_t fdq_va(int i) { return VAS[i]; }

int fdq_run(uint32_t va, vm *v, const fdsg_draw *d, const uint32_t r[8])
{
    cx cc, *c = &cc;
    uint32_t edi = r ? r[VM_EDI] : 0;
    cc.v = v; cc.d = d; cc.rc = FDSG_OK;
    switch (va) {
    case 0x004650C4u: f4650c4(c); break;
    case 0x00466869u: f466869(c); break;
    case 0x00466837u: car_step(c, -1); break;
    case 0x004668BBu: car_step(c, 1); break;
    case 0x0046696Cu: page_step(c, -1); break;
    case 0x0046698Fu: page_step(c, 1); break;
    case 0x004667D6u:
        name_row(c, R(car_record(c) + 4u), A_NAME);
        break;
    case 0x004667FCu:
        name_row(c, R(0x0070BE0Au + 4u * R(A_PAGEN)), A_PAGE);
        break;
    case 0x00478F77u: f478f77(c); break;
    case 0x0046512Du: f46512d(c, r ? r[VM_EAX] : 0); break;
    case 0x004668F3u: slide_car_step(c, -1); break;
    case 0x0046692Eu: slide_car_step(c, 1); break;
    case 0x004669B2u: pic_step(c, -1); break;
    case 0x004669D8u: pic_step(c, 1); break;
    case 0x0046681Fu: name_row(c, 0x33Du, A_PAGE); break;
    case 0x004669FEu: page0(c, edi); break;
    case 0x00466CA1u: page1(c, edi); break;
    case 0x00466F11u: page2(c, edi); break;
    case 0x00466A2Fu: page3(c, edi); break;
    default: return FDSG_NOT_MINE;
    }
    return cc.rc;
}
