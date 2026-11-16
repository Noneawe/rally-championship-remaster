/* fd_standings.c - see fd_standings.h. family B, 2026-11-09.
 *
 * Every function names the original it transcribes.  The state is the VM's data
 * copy at the original's own addresses; arithmetic is 32-bit and wraps as the
 * original's does.  The callees the original runs interpreted (the number and
 * time formatters, the row finders, the 7-character right-aligned text) are
 * transcribed here too, as static helpers named by address; the services the
 * host answers (the draw calls, the token text, the screens' bodies) go through
 * fdb_host.svc at the original's own address. */
#include "fd_standings.h"

#include "fe_champ.h"
#include "fe_logic.h"

#include <string.h>

#define R(a)    vm_rd32(v, (uint32_t)(a))
#define W(a, x) vm_wr32(v, (uint32_t)(a), (uint32_t)(x))
#define B8(a)   vm_rd8(v, (uint32_t)(a))
#define W8(a, x) vm_wr8(v, (uint32_t)(a), (uint8_t)(x))

#define FUNC(name) static int name(vm *v, const fdb_host *h, uint32_t r[8])
#define UNUSED (void)h; (void)r

enum {
    BUF       = 0x00A2BF54,     /* the scratch string every row builds its text in */
    BUF2      = 0x00A2C254,     /* the page range's                                */
    F_CC      = 0x005180CC, F_AC = 0x005180AC, F_8C = 0x0051808C,
    F_6C      = 0x0051806C, F_4C = 0x0051804C, F_2C = 0x0051802C,
    F_TITLE   = 0x005184CC, F_BIG = 0x0051846C,
    A_ROWS    = 0x00708054,     /* [..]: the stage results, 0x9C a row, -1 ends     */
    A_ROWS2   = 0x00708058,     /* [..]: the standings, 0x78 a row, -1 ends          */
    A_PLAYERS = 0x00708DE8,     /* the player pointers                               */
    A_ITIN    = 0x006E7EE9      /* the itinerary                                     */
};

/* ---- the original's fatal exits: two words, then 0x00407383 ---------------- */
static int fatal(vm *v, uint32_t code, uint32_t arg)
{
    W(0x004F0274u, code);
    W(0x004F0278u, arg);
    return FDS_FATAL;
}

/* ---- the services -------------------------------------------------------- */
static uint32_t svc(const fdb_host *h, vm *v, uint32_t va, uint32_t eax, uint32_t ebx,
                    uint32_t ecx, uint32_t edx, uint32_t esi)
{
    uint32_t r[8];
    memset(r, 0, sizeof r);
    r[VM_EAX] = eax; r[VM_EBX] = ebx; r[VM_ECX] = ecx; r[VM_EDX] = edx; r[VM_ESI] = esi;
    h->svc(h->ud, v, va, r);
    return r[VM_EBX];
}
/* 0x00402311: EAX the token, EBX the font record, ECX x, EDX y */
static void tok(const fdb_host *h, vm *v, uint32_t font, uint32_t token, uint32_t x, uint32_t y)
{
    svc(h, v, 0x00402311u, token, font, x, y, 0);
}
/* 0x0040239C: EAX the font record, EBX the string's address, ECX x, EDX y */
static void text(const fdb_host *h, vm *v, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    svc(h, v, 0x0040239Cu, font, str, x, y, 0);
}
/* 0x004066F9: EAX the sprite, ECX x, EDX y, ESI the bundle [0x00707EED] */
static void blit(const fdb_host *h, vm *v, uint32_t sprite, uint32_t x, uint32_t y)
{
    svc(h, v, 0x004066F9u, sprite, 0, x, y, R(0x00707EEDu));
}
/* 0x004495D3: the quad whose corners are the words 0x005D6CA4 .. */
static void quad(const fdb_host *h, vm *v) { svc(h, v, 0x004495D3u, 0, 0, 0, 0, 0); }
/* 0x00401F8F: the token EAX's text at EBX, EBX advanced */
static uint32_t tokstr(const fdb_host *h, vm *v, uint32_t token, uint32_t ebx)
{
    return svc(h, v, 0x00401F8Fu, token, ebx, 0, 0, 0);
}

/* ---- the string builders (EBX the end of the text so far) -------------------- */
static uint32_t put(vm *v, uint32_t ebx, uint8_t c) { W8(ebx, c); return ebx + 1; }
static uint32_t nul(vm *v, uint32_t ebx)   { return put(v, ebx, 0); }      /* 0x00401F02 */
static uint32_t space(vm *v, uint32_t ebx) { return put(v, ebx, 0x20); }   /* 0x00401F15 */
static uint32_t colon(vm *v, uint32_t ebx) { return put(v, ebx, 0x3A); }   /* 0x00401F1F */
static uint32_t slash(vm *v, uint32_t ebx) { return put(v, ebx, 0x2F); }   /* 0x00401F2E */
static uint32_t dot(vm *v, uint32_t ebx)   { return put(v, ebx, 0x2E); }   /* 0x00401F33 */
static uint32_t minus(vm *v, uint32_t ebx) { return put(v, ebx, 0x2D); }   /* 0x00401F38 */
/* 0x004019CD: EAX in decimal, the flags 0 */
static uint32_t dec(vm *v, uint32_t eax, uint32_t ebx) { return fel_fmt_num(v, eax, 0, ebx); }

/* 0x00401A25: a time in hundredths: [h:]mm:ss.cc.  Hours when [0x004DB144] == 1
 * or the time is 0x57E40 (an hour in 1/100 s) or more. */
static uint32_t timestr(vm *v, uint32_t eax, uint32_t ebx)
{
    if (R(0x004DB144u) == 1 || eax >= 0x57E40u) {
        ebx = fel_fmt_num(v, eax / 0x57E40u, 0x12, ebx);
        ebx = colon(v, ebx);
        eax = eax % 0x57E40u;
    }
    ebx = fel_fmt_num(v, eax / 0x1770u, 0x12, ebx);
    ebx = colon(v, ebx);
    eax = eax % 0x1770u;
    ebx = fel_fmt_num(v, eax / 0x64u, 0x12, ebx);
    ebx = dot(v, ebx);
    return fel_fmt_num(v, eax % 0x64u, 0x12, ebx);          /* ECX is still 0x12 */
}

/* 0x00462B21: the string at EBX drawn one character at a time (eight of them, a
 * time's "mm:ss.cc": from ECX - 0x64 on, 13 / 10 / 10 / 13 / 10 / 10 / 13 apart),
 * each copied to 0x40 past EBX and ended there. */
static void chars7(const fdb_host *h, vm *v, uint32_t font, uint32_t str, uint32_t x, uint32_t y)
{
    static const uint8_t STEP[] = { 0, 0xD, 0xA, 0xA, 0xD, 0xA, 0xA, 0xD };
    uint32_t k, buf = str + 0x40u;
    x -= 0x64u;
    for (k = 0; k < 8; k++) {
        x += STEP[k];
        W8(buf, B8(str + k));
        W8(buf + 1, 0);
        text(h, v, font, buf, x, y);
    }
}

/* ---- the row finders ---------------------------------------------------------- */

/* 0x004777F1: the first row from EDI whose +0x1C is EAX (EDI at the -1 that
 * ends the list if none); EAX = its +0x18 when EBX == 1, else +0x14 */
static uint32_t f4777f1(vm *v, uint32_t eax, uint32_t ebx, uint32_t edi)
{
    while (R(edi + 0x1C) != eax) {
        edi += 0x9C;
        if (R(edi) == 0xFFFFFFFFu) break;
    }
    return R(edi + (ebx == 1 ? 0x18u : 0x14u));
}

/* 0x0049EEFB: EAX == 2: EDI on to the first row whose +0x1C is not 0 (fatal
 * with none); then the row's +0x18 when EBX == 1, else +0x14 */
static int f49eefb(vm *v, uint32_t eax, uint32_t ebx, uint32_t edi, uint32_t *out)
{
    if (eax == 2) {
        while (R(edi + 0x1C) == 0) {
            edi += 0x9C;
            if (R(edi) == 0xFFFFFFFFu) return fatal(v, 0x8070A982u, 0x004DA970u);
        }
    }
    *out = R(edi + (ebx == 1 ? 0x18u : 0x14u));
    return FDS_OK;
}

/* 0x0049E1CB: the row of class EBX and rank (+0x44) EAX from EDI */
static int f49e1cb(vm *v, uint32_t eax, uint32_t ebx, uint32_t *edi)
{
    uint32_t e = *edi;
    for (;;) {
        if (R(e + 0x1C) == ebx && R(e + 0x44) == eax) { *edi = e; return FDS_OK; }
        e += 0x9C;
        if (R(e) == 0xFFFFFFFFu) return fatal(v, 0x8070A7F9u, 0x004DA89Au);
    }
}

/* 0x0049E208: the row of rank EAX from EDI */
static int f49e208(vm *v, uint32_t eax, uint32_t *edi)
{
    uint32_t e = *edi;
    for (;;) {
        if (R(e + 0x44) == eax) { *edi = e; return FDS_OK; }
        e += 0x9C;
        if (R(e) == 0xFFFFFFFFu) return fatal(v, 0x8070A7FFu, 0x004DA8F2u);
    }
}

/* 0x0049E1F0 / 0x004A16F4 (the standings, 0x78 a row): class EBX and rank (+0x70) EAX / rank EAX */
static int f49e1f0(vm *v, uint32_t eax, uint32_t ebx, uint32_t *edi)
{
    uint32_t e = *edi;
    for (;;) {
        if (R(e + 0x10) == ebx && R(e + 0x70) == eax) { *edi = e; return FDS_OK; }
        e += 0x78;
        if (R(e) == 0xFFFFFFFFu) return fatal(v, 0x8070A7FAu, 0x004DA8C6u);
    }
}
static int f4a16f4(vm *v, uint32_t eax, uint32_t *edi)
{
    uint32_t e = *edi;
    for (;;) {
        if (R(e + 0x70) == eax) { *edi = e; return FDS_OK; }
        e += 0x78;
        if (R(e) == 0xFFFFFFFFu) return fatal(v, 0x8070A7FCu, 0x004DAA09u);
    }
}

/* 0x0049DCAA / 0x004A078B: in the rows of `stride` bytes from EDI, the first of the
 * class EBX (EBX == 0x63: with a class at all) that has a place (+4 >= 1, unsigned);
 * EDX its +rank.  The original's fatal exit with none.  (It counts the rows it passes
 * in ECX; no caller reads that: every one is PUSHAD ... POPAD.) */
static int scan_class(vm *v, uint32_t stride, uint32_t cls, uint32_t rank, uint32_t ebx,
                      uint32_t fcode, uint32_t farg, uint32_t edi, uint32_t *edx)
{
    for (;;) {
        int hit = ebx == 0x63u ? R(edi + cls) != 0 : R(edi + cls) == ebx;
        if (hit && R(edi + 4) >= 1u) { *edx = R(edi + rank); return FDS_OK; }
        edi += stride;
        if (R(edi) == 0xFFFFFFFFu) return fatal(v, fcode, farg);
    }
}

/* ---- the page ------------------------------------------------------------------- */

/* 0x0049E0C4: [0x70801C] rows -> the first row of the page [0x708020].  The
 * original's EAX / EDX / EBP are spent when it divides. */
static void f49e0c4(vm *v, uint32_t r[8])
{
    uint32_t n = R(0x0070801Cu);
    if ((int32_t)n <= 10) { W(0x00708020u, 1); return; }
    r[VM_EDX] = n % 10u;
    r[VM_EBP] = 10;
    r[VM_EAX] = (n / 10u) * 10u + 1u;
    if (r[VM_EDX] == 0) r[VM_EAX] -= 10u;
    W(0x00708020u, r[VM_EAX]);
}

/* 0x0049DC42: the page's first and last row, "n-m", at 0x00A2C254 (PUSHAD) */
static void f49dc42(vm *v)
{
    uint32_t p = BUF2, n = R(0x00708020u);
    p = dec(v, n, p);
    p = minus(v, p);
    p = dec(v, n + 9u, p);
    nul(v, p);
}

/* 0x0049DC01 / 0x0049DC6C / 0x0049EF98 / 0x0049F81D (PUSHAD): the class (EBX; 0x63
 * for the second pair) of the page's rows; how many rows it has, clamped; the first
 * row of the page; the range; the screen's body (a service) */
static int page_of(vm *v, const fdb_host *h, uint32_t ebx, uint32_t clamp_if, uint32_t clamp_to,
                   uint32_t body, int store)
{
    uint32_t edx, rr[8];
    int rc;
    if (store) W(0x0070825Cu, ebx);        /* only 0x0049DC01 keeps the class (ebx 0x63 is a literal) */
    rc = scan_class(v, 0x9C, 0x1C, 0x44, ebx, 0x8070A0A0u, 0x004DA872u, R(A_ROWS), &edx);
    if (rc) return rc;
    W(0x0070801Cu, edx);
    if ((int32_t)R(0x0070801Cu) > (int32_t)clamp_if) W(0x0070801Cu, clamp_to);
    memset(rr, 0, sizeof rr);
    f49e0c4(v, rr);
    f49dc42(v);
    svc(h, v, body, 0, 0, 0, 0, 0);
    return FDS_OK;
}
FUNC(f49dc01) { return page_of(v, h, r[VM_EBX], 0x33, 0x33, 0x00462F22u, 1); }
FUNC(f49dc6c) { (void)r; return page_of(v, h, 0x63, 0xAB, 0xAB, 0x004631EBu, 0); }
FUNC(f49f81d) { (void)r; return page_of(v, h, 0x63, 0xAB, 0x51, 0x0046377Du, 0); }
FUNC(f49ef98)
{
    /* the class's rows: 0x51 on the overall list (the table [0x708038] == 0x13), else 0x33 */
    uint32_t edx, rr[8];
    int rc;
    W(0x0070825Cu, r[VM_EBX]);
    rc = scan_class(v, 0x9C, 0x1C, 0x44, r[VM_EBX], 0x8070A0A0u, 0x004DA872u, R(A_ROWS), &edx);
    if (rc) return rc;
    W(0x0070801Cu, edx);
    if (R(0x00708038u) == 0x13) {
        if ((int32_t)R(0x0070801Cu) > 0x51) W(0x0070801Cu, 0x51);
    } else if ((int32_t)R(0x0070801Cu) > 0x33) W(0x0070801Cu, 0x33);
    memset(rr, 0, sizeof rr);
    f49e0c4(v, rr);
    f49dc42(v);
    svc(h, v, 0x004634B4u, 0, 0, 0, 0, 0);
    return FDS_OK;
}

/* 0x004A074A (PUSHAD): the same for the championship's standings (EBX the class) */
FUNC(f4a074a)
{
    uint32_t edx, rr[8];
    int rc;
    W(0x0070825Cu, r[VM_EBX]);
    rc = scan_class(v, 0x78, 0x10, 0x70, r[VM_EBX], 0x8070A0AFu, 0x004DA9B1u, R(A_ROWS2), &edx);
    if (rc) return rc;
    W(0x0070801Cu, edx);
    if ((int32_t)R(0x0070801Cu) > 0x33) W(0x0070801Cu, 0x33);
    memset(rr, 0, sizeof rr);
    f49e0c4(v, rr);
    f49dc42(v);
    svc(h, v, 0x004A07E0u, 0, 0, 0, 0, 0);
    return FDS_OK;
}

/* 0x004A16B6 (PUSHAD): the overall championship's page.  0x004A07BA (EBX 0x63) finds the first
 * standings row with a class (+0x10) and a place (+4) and gives its +0x70; clamped to 0xAB rows; the page's
 * first row and range; the screen's body 0x00463D3B (a service, then 0x00463D3B's own code runs there).
 * No row at all: 0x004A07BA's fatal exit (0x8070A0AE / 0x004DA9D9). */
FUNC(f4a16b6)
{
    uint32_t edx, rr[8];
    int rc;
    (void)r;
    rc = scan_class(v, 0x78, 0x10, 0x70, 0x63, 0x8070A0AEu, 0x004DA9D9u, R(A_ROWS2), &edx);
    if (rc) return rc;
    W(0x0070801Cu, edx);
    if ((int32_t)R(0x0070801Cu) > 0xAB) W(0x0070801Cu, 0xAB);
    memset(rr, 0, sizeof rr);
    f49e0c4(v, rr);
    f49dc42(v);
    svc(h, v, 0x00463D3Bu, 0, 0, 0, 0, 0);
    return FDS_OK;
}

/* 0x0049DCDC / 0x004A0712 (EBX the class): number the rows of the class 1, 2, ... in +0x44
 * / +0x70 and tell the player whose place (+4) the row is, at +0x14C / +0x144.  Spends EAX,
 * ECX, ESI, EDI. */
static void number_rows(vm *v, uint32_t r[8], uint32_t list, uint32_t stride, uint32_t cls,
                        uint32_t rank, uint32_t pl)
{
    uint32_t e = R(list), ecx = 0, eax, esi;
    for (;;) {
        if (R(e + cls) == r[VM_EBX]) {
            ecx++;
            if (R(e + 4) != 0) {
                eax = R(e + 4) - 1u;
                r[VM_EAX] = eax;
                esi = R(A_PLAYERS + eax * 4u);
                r[VM_ESI] = esi;
                W(esi + pl, ecx);
            }
            W(e + rank, ecx);
        }
        e += stride;
        if (R(e) == 0xFFFFFFFFu) break;
    }
    r[VM_ECX] = ecx;
    r[VM_EDI] = e;
}
FUNC(f49dcdc) { (void)h; number_rows(v, r, A_ROWS, 0x9C, 0x1C, 0x44, 0x14C); return FDS_OK; }
FUNC(f4a0712) { (void)h; number_rows(v, r, A_ROWS2, 0x78, 0x10, 0x70, 0x144); return FDS_OK; }

/* the page arrows: the page [0x708020] a page (10 rows) back / on; `last` the first
 * row of the last page (EAX is left with it, as the original's SUB / MOV does) */
FUNC(f49f3b4)
{
    if ((int32_t)R(0x00708020u) > 10) W(0x00708020u, R(0x00708020u) - 10u);
    else { r[VM_EAX] = R(0x007082E8u) - 9u; W(0x00708020u, r[VM_EAX]); }
    f49dc42(v); UNUSED; return FDS_OK;
}
FUNC(f49f3dd)
{
    r[VM_EAX] = R(0x007082E8u) - 9u;
    if ((int32_t)R(0x00708020u) < (int32_t)r[VM_EAX]) W(0x00708020u, R(0x00708020u) + 10u);
    else W(0x00708020u, 1);
    f49dc42(v); UNUSED; return FDS_OK;
}
FUNC(f49f40a)
{
    if ((int32_t)R(0x00708020u) > 10) W(0x00708020u, R(0x00708020u) - 10u);
    else { r[VM_EAX] = 0x5A - 9u; W(0x00708020u, r[VM_EAX]); }
    f49dc42(v); UNUSED; return FDS_OK;
}
FUNC(f49f433)
{
    r[VM_EAX] = 0x5A - 9u;
    if ((int32_t)R(0x00708020u) < (int32_t)r[VM_EAX]) W(0x00708020u, R(0x00708020u) + 10u);
    else W(0x00708020u, 1);
    f49dc42(v); UNUSED; return FDS_OK;
}

/* ---- the header of a results page: 0x0049E101 (PUSHAD) -------------------------------- */

/* the rally's name under the title (the table [0x708038] == 0x13: the token in the
 * list at 0x0070C2AE, else the itinerary's +0x38) */
static int rally_name(vm *v, const fdb_host *h, uint32_t *edi_out)
{
    int rc;
    uint32_t ebx = R(0x007082C4u), edi = fec_46ebbc(v, A_ITIN, 0, ebx, &rc);
    if (rc) return FDS_RUNAWAY;
    if (R(0x00708038u) == 0x13) tok(h, v, F_TITLE, R(ebx * 4u + 0x0070C2AEu), 0x2EB, 0x77);
    else tok(h, v, F_TITLE, R(edi + 0x38), 0x2EB, 0x77);
    if (edi_out) *edi_out = edi;
    return FDS_OK;
}

static int f49e101_(vm *v, const fdb_host *h)
{
    int rc;
    uint32_t edi, p, regs[8];
    if ((rc = rally_name(v, h, NULL)) != FDS_OK) return rc;
    edi = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    p = tokstr(h, v, 0x481, BUF);                    /* the stage's number: "STAGE n/6:" */
    p = space(v, p);
    p = dec(v, R(edi + 0x40), p);
    p = slash(v, p);
    p = dec(v, 6, p);
    p = colon(v, p);
    p = space(v, p);
    edi = fec_46f686(v, edi, regs, &rc);
    if (rc == FEC_FATAL_LIST) return FDS_FATAL;
    if (rc) return FDS_RUNAWAY;
    p = tokstr(h, v, R(edi), p);
    nul(v, p);
    text(h, v, F_TITLE, BUF, 0x2EB, 0x91);
    return FDS_OK;
}
FUNC(f49e101) { (void)r; return f49e101_(v, h); }

/* ---- the four results pages: 0x0049DD98, 0x0049EBCA, 0x0049EFF7, 0x0049F460 -------------
 * Ten rows from [0x708020], 0x19 apart from y = 0xF0.  A row: its place (+0x44), name
 * (+0x20), then its time or, for a driver who did not finish, a token; the margin to the
 * leader (the first row's, [0x708328]); and the car's picture (two blits).  A row with a
 * place (+4 > 0) is a player's and is drawn in the other fonts.  `time` is the
 * row's time word; `cmp` the one the "retired" test reads (0x36EE8A). */
typedef struct {
    uint32_t base2;                 /* the word the margin is taken from              */
    uint32_t time_le, cmp_le;       /* the row's +4 <= 0                              */
    uint32_t time_gt, cmp_gt;       /* the row's +4 > 0                               */
    int by_class;                   /* the row found by class and rank (0x0049E1CB), else by rank */
} rowcfg;

static int rows_loop(vm *v, const fdb_host *h, const rowcfg *c)
{
    uint32_t edx = 0xF0, edi, p;
    W(0x00707EE5u, R(0x00708020u));
    W(0x00708024u, 10u + R(0x00708020u));
    do {
        int32_t pos;
        uint32_t fa, fb, fc, toff, coff;
        edi = R(A_ROWS);
        if (c->by_class) {
            if (f49e1cb(v, R(0x00707EE5u), R(0x0070825Cu), &edi)) return FDS_FATAL;
        } else if (f49e208(v, R(0x00707EE5u), &edi)) return FDS_FATAL;
        pos = (int32_t)R(edi + 4);
        if (pos > 0) { fa = F_6C; fb = F_4C; fc = F_2C; toff = c->time_gt; coff = c->cmp_gt; }
        else         { fa = F_CC; fb = F_AC; fc = F_8C; toff = c->time_le; coff = c->cmp_le; }
        p = dec(v, R(edi + 0x44), BUF);
        nul(v, p);
        text(h, v, fa, BUF, 0xBE, edx);
        text(h, v, fb, edi + 0x20, 0xFA, edx);
        if (R(edi + 0x4C) == 1) {
            p = timestr(v, R(edi + toff), BUF);
            nul(v, p);
            chars7(h, v, fc, BUF, 0x258, edx);
            if (R(edi + 0x44) != 1) {
                W(0x0070832Cu, R(edi + toff) - R(c->base2));
                tok(h, v, fa, 0x579, 0x276, edx);
                p = timestr(v, R(0x0070832Cu), BUF);
                nul(v, p);
                chars7(h, v, fc, BUF, 0x2E4, edx);
            }
        } else if (R(edi + coff) == 0x36EE8Au) tok(h, v, fb, 0x4FB, 0x1EF, edx);
        else tok(h, v, fb, 0x4FA, 0x1EF, edx);
        blit(h, v, 0x1Cu + R(edi + 8), 0xD2, edx);
        W(0x00708320u, 0x44u + R(edi + 0x10));
        blit(h, v, R(0x00708320u), 0x1AE, edx);
        edx += 0x19;
        W(0x00707EE5u, R(0x00707EE5u) + 1u);
    } while (R(0x00707EE5u) != R(0x00708024u));
    return FDS_OK;
}

static void toks3(const fdb_host *h, vm *v)
{
    tok(h, v, F_CC, 0x484, 0xC8, 0xDC);
    tok(h, v, F_AC, 0x485, 0xFA, 0xDC);
    tok(h, v, F_AC, 0x47F, 0x1F4, 0xDC);
}

/* the title of a rally's page: the big token by [0x70CA88], then the rally's name */
static int rally_title(vm *v, const fdb_host *h, uint32_t tok_normal, uint32_t tok_one)
{
    tok(h, v, F_BIG, R(0x0070CA88u) == 1 ? tok_one : tok_normal, 0x310, 0x30);
    return rally_name(v, h, NULL);
}

FUNC(f49dd98)
{
    static const rowcfg C = { 0x00708328, 0x14, 0x14, 0x14, 0x14, 1 };
    int rc;
    (void)r;
    if ((rc = f49e101_(v, h)) != FDS_OK) return rc;
    toks3(h, v);
    W(0x00708328u, f4777f1(v, R(0x0070825Cu), 0, R(A_ROWS)));
    return rows_loop(v, h, &C);
}

FUNC(f49ebca)
{
    static const rowcfg C = { 0x00708340, 0x14, 0x14, 0x14, 0x14, 0 };
    uint32_t base;
    int rc;
    (void)r;
    if ((rc = f49e101_(v, h)) != FDS_OK) return rc;
    toks3(h, v);
    if ((rc = f49eefb(v, 2, 0, R(A_ROWS), &base)) != FDS_OK) return rc;
    W(0x00708328u, base);
    W(0x00708340u, base);
    return rows_loop(v, h, &C);
}

FUNC(f49eff7)
{
    static const rowcfg C = { 0x00708328, 0x18, 0x18, 0x18, 0x18, 1 };
    int rc;
    (void)r;
    if ((rc = rally_title(v, h, 0x4EA, 0x4ED)) != FDS_OK) return rc;
    toks3(h, v);
    W(0x00708328u, f4777f1(v, R(0x0070825Cu), 1, R(A_ROWS)));
    return rows_loop(v, h, &C);
}

/* 0x0049F460: the rally's overall page.  A row with a place is tested for "retired" in
 * its +0x14 and not in its +0x18 as everywhere else in the page (the original's) */
FUNC(f49f460)
{
    static const rowcfg C = { 0x00708328, 0x18, 0x18, 0x18, 0x14, 0 };
    uint32_t base;
    int rc;
    (void)r;
    if ((rc = rally_title(v, h, 0x4EB, 0x4EE)) != FDS_OK) return rc;
    toks3(h, v);
    if ((rc = f49eefb(v, 2, 1, R(A_ROWS), &base)) != FDS_OK) return rc;
    W(0x00708328u, base);
    return rows_loop(v, h, &C);
}

/* ---- the results builders: 0x0049E7F9, 0x004A0115 ----------------------------------------
 * The stage results / the standings are made from the driver records (0x006EDFE8,
 * 0x94 a record, -1 ends) and each player is put into the row of the place it has
 * (the class +0x90): the rows of a class are the last of its block, counting down. */

/* the original's token text as fe_champ.c's services have it: the length back */
typedef struct { vm *v; const fdb_host *h; } adapter;
static uint32_t fec_tokstr(void *ud, uint32_t va, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    adapter *a = (adapter *)ud;
    uint32_t r[8];
    (void)ecx; (void)edx;
    if (va != 0x00401F8Fu) return 0;
    memset(r, 0, sizeof r);
    r[VM_EAX] = eax; r[VM_EBX] = ebx;
    a->h->svc(a->h->ud, a->v, va, r);
    return r[VM_EBX] - ebx;
}

/* 0x00462D9B (PUSHAD): the player ESI as the stage results' row EDI of place EBX + 1 */
static void f462d9b(vm *v, uint32_t edi, uint32_t esi, uint32_t ebx)
{
    uint32_t src = R(esi + 0x18), k;
    W(edi + 4, ebx + 1u);
    W(edi + 0x14, R(esi + 0x88));
    W(edi + 0x1C, R(esi + 0x90));
    W(edi + 0x18, R(esi + 0x8C));
    W(edi + 0xC, src);
    for (k = 0; k < 0x20; k++) W8(edi + 0x20 + k, B8(src + k));      /* REP MOVSB: forward */
    W(edi + 8, R(esi + 4));
    W(edi + 0x10, R(esi + 8));
    W(edi + 0x4C, R(esi + 0xD0));
}

FUNC(f49e7f9)
{
    static const uint16_t OFF[4] = { 0x363C, 0x54B4, 0x66FC, 0x6D14 };
    adapter ad;
    fec_services fs;
    uint32_t k;
    (void)r;
    ad.v = v; ad.h = h;
    fs.svc = fec_tokstr; fs.ud = &ad;
    fec_4a51d5(v, &fs);                  /* 0x0049E7FA .. 0x0049E868 is 0x004A51D5's body (gated by fe_champ_check) */
    for (k = 0; k < 4; k++) {
        uint32_t edi = R(A_ROWS) + OFF[k], ebx = 0;
        do {
            uint32_t esi = R(A_PLAYERS + ebx * 4u);
            if (R(esi + 0x90) == k) {
                f462d9b(v, edi, esi, ebx);
                edi -= 0x9C;
            }
            ebx++;
        } while (ebx != R(0x007082F0u));
    }
    return FDS_OK;
}

/* 0x00462DE6 (PUSHAD): the player ESI as the standings' row EDI of place EBX + 1 */
static void f462de6(vm *v, uint32_t edi, uint32_t esi, uint32_t ebx)
{
    uint32_t src = R(esi + 0x18), k;
    W(edi + 4, ebx + 1u);
    for (k = 0; k < 0x20; k++) W8(edi + 0x14 + k, B8(src + k));
    W(edi + 8, R(esi + 4));
    W(edi + 0xC, R(esi + 8));
    W(edi + 0x10, R(esi + 0x90));
    W(edi + 0x34, R(esi + 0x8C));
    W(edi + 0x38, R(esi + 0xB4));
    W(edi + 0x3C, R(esi + 0xB8));
    W(edi + 0x40, R(esi + 0xBC));
    W(edi + 0x44, R(esi + 0xC0));
    W(edi + 0x48, R(esi + 0xC4));
    W(edi + 0x4C, R(esi + 0xC8));
    W(edi + 0x50, R(esi + 0xCC));
    W(edi + 0x54, R(esi + 0x98));
    W(edi + 0x58, R(esi + 0x9C));
    W(edi + 0x5C, R(esi + 0xA0));
    W(edi + 0x60, R(esi + 0xA4));
    W(edi + 0x64, R(esi + 0xA8));
    W(edi + 0x68, R(esi + 0xAC));
    W(edi + 0x6C, R(esi + 0xB0));
}

/* 0x004A02B9 (PUSHAD): the 200 standings rows' five first words -1 */
static void f4a02b9(vm *v)
{
    uint32_t esi = R(A_ROWS2), k;
    for (k = 0; k < 0xC8; k++, esi += 0x78) {
        W(esi, 0xFFFFFFFFu); W(esi + 4, 0xFFFFFFFFu); W(esi + 8, 0xFFFFFFFFu);
        W(esi + 0xC, 0xFFFFFFFFu); W(esi + 0x10, 0xFFFFFFFFu);
    }
}

FUNC(f4a0115)
{
    static const uint16_t OFF[4] = { 0x29B8, 0x37C8, 0x4F38, 0x53E8 };
    uint32_t edi = 0x006EDFE8u, esi, k, t, p;
    (void)r;
    f4a02b9(v);
    esi = R(A_ROWS2);
    do {
        uint32_t x;
        W(esi + 4, 0);
        W(esi + 0x10, R(edi + 8));
        W(esi + 8, R(edi + 0x14));
        W(esi + 0xC, R(edi + 0x18));
        W(esi, R(edi));
        W(esi + 0x34, R(edi + 0x10));
        for (x = 0; x < 14; x++) W(esi + 0x38 + 4 * x, R(edi + 0x5C + 4 * x));
        t = 0x006F47FCu + R(edi) * 0x14u;
        p = tokstr(h, v, R(t), esi + 0x14);
        nul(v, p);
        edi += 0x94;
        esi += 0x78;
    } while (R(edi) != 0xFFFFFFFFu);
    for (k = 0; k < 4; k++) {
        uint32_t e = R(A_ROWS2) + OFF[k], ebx = 0;
        do {
            uint32_t pl = R(A_PLAYERS + ebx * 4u);
            if (R(pl + 0x90) == k) {
                f462de6(v, e, pl, ebx);
                e -= 0x78;
            }
            ebx++;
        } while (ebx != R(0x007082F0u));
    }
    return FDS_OK;
}

/* 0x004A4D6D (PUSHAD): the fifty best-score rows ([0x0070803C], 0x3C a row) marked free */
FUNC(f4a4d6d)
{
    uint32_t edi = R(0x0070803Cu), k;
    UNUSED;
    for (k = 0; k < 0x32; k++, edi += 0x3C) { W(edi + 0x14, 0); W(edi + 4, 0); }
    return FDS_OK;
}

/* 0x004A4277 / 0x004A42F5 (PUSHAD): "the score's entry" - the current player's score goes into the last row (the
 * tenth, +0x21C) of its class's block of ten best scores ([0x0070803C] + class * 0x258, 0x3C a row), and the block is
 * sorted by one bubbling pass from the bottom (0x004A4372; 0x004A4E02 for the overall list, class 4).  The first takes
 * the score from the player's +0xB0 and the class from +0x90, the second the score from +0xCC and class 4; both
 * write +0x90 as the row's class.  The row: +0x38 the score, +4 the place (the player's number + 1), +0x14 1 (in use),
 * +8 / +0xC the player's +4 / +8, +0x10 the class, +0 and +0x18 the name pointer (+0x18) and then the name's 0x1F
 * bytes over the +0x18 (a byte at a time: the name may overlap the row). */
static void sort_scores(vm *v, uint32_t cls)
{
    static const uint32_t SWAP[] = { 0x38, 0x14, 4, 8, 0x10, 0xC };
    uint32_t esi = R(0x0070803Cu) + cls * 0x258u + 0x21Cu, edi = esi - 0x3C, n, k, f;
    for (n = 9; n != 0; n--, edi -= 0x3C, esi -= 0x3C) {
        if ((int32_t)R(esi + 0x38) < (int32_t)R(edi + 0x38)) continue;     /* a lower score stays below */
        for (f = 0; f < sizeof SWAP / sizeof *SWAP; f++) {
            uint32_t t = R(edi + SWAP[f]);
            W(edi + SWAP[f], R(esi + SWAP[f]));
            W(esi + SWAP[f], t);
        }
        for (k = 0; k < 0x1F; k++) W8(0x007081ECu + k, B8(edi + 0x18 + k));    /* the names swap through [0x7081EC] */
        for (k = 0; k < 0x1F; k++) W8(edi + 0x18 + k, B8(esi + 0x18 + k));
        for (k = 0; k < 0x1F; k++) W8(esi + 0x18 + k, B8(0x007081ECu + k));
    }
}
static void score_entry(vm *v, uint32_t cls, uint32_t score_at)
{
    uint32_t p = R(A_PLAYERS + R(0x007082F8u) * 4u), esi = R(0x0070803Cu) + cls * 0x258u + 0x21Cu, ebx, k;
    W(esi + 0x38, R(p + score_at));
    W(esi + 4, R(0x007082F8u) + 1u);
    W(esi + 0x14, 1);
    W(esi + 8, R(p + 4));
    W(esi + 0xC, R(p + 8));
    W(esi + 0x10, R(p + 0x90));
    W(esi, R(p + 0x18));
    W(esi + 0x18, R(p + 0x18));
    ebx = R(esi);
    for (k = 0; k < 0x1F; k++) W8(esi + 0x18 + k, B8(ebx + k));
    sort_scores(v, cls);
}
FUNC(f4a4277)
{
    uint32_t p = R(A_PLAYERS + R(0x007082F8u) * 4u);
    UNUSED;
    score_entry(v, R(p + 0x90), 0xB0);
    return FDS_OK;
}
FUNC(f4a42f5)
{
    UNUSED;
    score_entry(v, 4, 0xCC);
    return FDS_OK;
}

/* 0x0049FB06 (PUSHAD): the rally's best score ([0x006E7EE9 + ..] +0x30) taken from the players, in
 * turn, while each beats it: its holder's place (+4), car (+8) and name (+0x18's 0x20 bytes).  The
 * first player that does not beat it ENDS the whole scan (the original's). */
FUNC(f49fb06)
{
    int rc;
    uint32_t edi = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc), ecx = 0;
    UNUSED;
    if (rc) return FDS_RUNAWAY;
    do {
        uint32_t esi, ebx, k, src;
        W(0x007082F8u, ecx);
        esi = R(A_PLAYERS + R(0x007082F8u) * 4u);
        ebx = R(esi + 0x94);
        if ((int32_t)R(edi + 0x30) >= (int32_t)ebx) break;
        W(edi + 0x30, ebx);
        W(edi + 0x74, R(esi + 4));
        W(edi + 0x78, R(esi + 8));
        src = R(esi + 0x18);
        for (k = 0; k < 0x20; k++) W8(edi + 0x54 + k, B8(src + k));
        ecx++;
    } while (ecx != R(0x007082F0u));
    return FDS_OK;
}

/* ---- the best scores' pages: 0x004A4998 (a class), 0x004A4B78 (the overall) ------------------
 * Ten rows of a class's block ([0x0070803C] + class * 0x258, 0x3C a row).  The original pushes
 * ECX and EDI for each row and POPs only EDI when the next row is the list's -1 end: the stack is
 * then one word out and its RET goes to what POPAD left in EAX (FDS_CRASH).  No retail list ends
 * there (the table is five blocks of ten and the word after it is 0). */
static int best_page(vm *v, const fdb_host *h, uint32_t class_, int title_by_class)
{
    int rc;
    uint32_t ebx = R(0x007082C4u), edi, edx = 0xF0, ecx = 1, p;
    (void)fec_46ebbc(v, A_ITIN, 0, ebx, &rc);
    if (rc) return FDS_RUNAWAY;
    if (R(0x00708038u) == 0x13) tok(h, v, F_TITLE, R(ebx * 4u + 0x0070C2AEu), 0x2EB, 0x77);
    else if (title_by_class) {
        W(0x00708358u, 0x35Eu + R(0x0070825Cu));
        tok(h, v, F_TITLE, R(0x00708358u), 0x2EB, 0x77);
    } else tok(h, v, F_TITLE, 0x362, 0x2EB, 0x77);
    edi = R(0x0070803Cu) + class_ * 0x258u;
    do {
        uint32_t fa, fb, fc;
        W(0x00708318u, ecx);
        if (R(edi + 0x14) == 1) { fa = F_4C; fb = F_4C; fc = F_6C; }
        else                    { fa = F_AC; fb = F_AC; fc = F_CC; }
        text(h, v, fa, edi + 0x18, 0xF0, edx);
        p = dec(v, R(edi + 0x38), BUF);
        nul(v, p);
        text(h, v, fb, BUF, 0x21C, edx);
        p = dec(v, R(0x00708318u), BUF);
        nul(v, p);
        text(h, v, fc, BUF, 0xC8, edx);
        W(0x0070831Cu, edx);
        blit(h, v, 0x1Cu + R(edi + 8), 0xD2, R(0x0070831Cu));
        W(0x00708320u, 0x44u + R(edi + 0xC));
        blit(h, v, R(0x00708320u), 0x1CC, R(0x0070831Cu));
        edi += 0x3C;
        if (R(edi) == 0xFFFFFFFFu) return FDS_CRASH;
        edx += 0x19;
        ecx++;
    } while (ecx != 0xB);
    return FDS_OK;
}
FUNC(f4a4998) { (void)r; return best_page(v, h, R(0x0070825Cu), 1); }
FUNC(f4a4b78) { (void)r; return best_page(v, h, 4, 0); }

/* ---- the screens' backdrops: quads (0x004495D3 reads the globals 0x005D6CA4 ..) ------------- */
static void qd(const fdb_host *h, vm *v, uint32_t x, uint32_t y, uint32_t dx, uint32_t dy,
               uint32_t colour, uint32_t alpha)
{
    W(0x005D6CA4u, x);
    W(0x005D6CB4u, y);
    W(0x005D6CA8u, x + dx);
    W(0x005D6CB8u, y + dy);
    W(0x005D6C7Cu, colour);
    W(0x005D6C80u, alpha);
    quad(h, v);
}
static void panel(const fdb_host *h, vm *v, uint32_t x, uint32_t y, uint32_t dx, uint32_t dy, uint32_t colour)
{
    qd(h, v, x, y, dx, dy, colour, R(0x0070CA00u));
}

/* 0x0049B994 (PUSHAD): the INSPECTION AREA's frame */
FUNC(f49b994)
{
    UNUSED;
    panel(h, v, 0, 0, 0x320, 0x1E3, 0x11166A);
    panel(h, v, 0, 0x1E3, 0xED, 0x75, 0x11166A);
    panel(h, v, 0xED, 0x1F6, 0x233, 0x62, 0x11166A);
    panel(h, v, 0x233, 0x1E3, 0xED, 0x13, 0x11166A);
    panel(h, v, 0x128, 0x1E2, 0xD2, 0x15, 0x1F1F1F);
    return FDS_OK;
}

/* 0x0049BC8A: two columns of sixteen strips (x 0x43 and 0x192, 0x14C wide, 3 high, down from y 0x5F
 * in 0x1D steps) */
static void f49bc8a(vm *v, const fdb_host *h)
{
    uint32_t col, k;
    static const uint16_t X[2] = { 0x43, 0x192 };
    for (col = 0; col < 2; col++) {
        uint32_t y = 0x5F;
        for (k = 0; k < 16; k++, y += 0x1D) panel(h, v, X[col], y, 0x14C, 3, 0x11166A);
    }
}

/* 0x0049BD3D (PUSHAD): two blocks of sixteen rows' outlines (the row [0x00707E91] is picked out in
 * yellow when [0x00708310] is 1); [0x00708358] counts the rows 1 .. 0x20, [0x00708384] / [0x00708388]
 * are the original's x / y */
static void f49bd3d(vm *v, const fdb_host *h)
{
    uint32_t block;
    W(0x00708358u, 1);
    for (block = 0; block < 2; block++) {
        W(0x00708384u, block ? 0x192u : 0x43u);
        W(0x00708388u, 0x45);
        do {
            uint32_t x = R(0x00708384u), y = R(0x00708388u);
            qd(h, v, x, y, 0x14D, 0x1B, 0x4FCF, R(0x0070C9FCu));
            qd(h, v, x, y, 0x14C, 1, 0, 0xFF);
            qd(h, v, x, y, 1, 0x1A, 0, 0xFF);
            qd(h, v, x + 0x14C, y, 1, 0x1B, 0, 0xFF);
            qd(h, v, x, y + 0x1A, 0x14C, 1, 0, 0xFF);
            if (R(0x00707E91u) == R(0x00708358u) && R(0x00708310u) == 1) {
                qd(h, v, x, y, 0x14C, 1, 0xFFFF00, 0xFF);
                qd(h, v, x, y, 1, 0x1A, 0xFFFF00, 0xFF);
                qd(h, v, x + 0x14C, y, 1, 0x1B, 0xFFFF00, 0xFF);
                qd(h, v, x, y + 0x1A, 0x14C, 1, 0xFFFF00, 0xFF);
                qd(h, v, x - 1, y - 1, 0x14E, 1, 0xFFFF00, 0xFF);
                qd(h, v, x - 1, y - 1, 1, 0x1C, 0xFFFF00, 0xFF);
                qd(h, v, x + 0x14D, y - 1, 1, 0x1D, 0xFFFF00, 0xFF);
                qd(h, v, x - 1, y + 0x1B, 0x14E, 1, 0xFFFF00, 0xFF);
            }
            W(0x00708388u, R(0x00708388u) + 0x1D);
            W(0x00708358u, R(0x00708358u) + 1u);
        } while (R(0x00708358u) != (block ? 0x21u : 0x11u));
    }
}

/* 0x0049BB10 (PUSHAD): the rally results screen's backdrop and the sixteen-row outlines */
FUNC(f49bb10)
{
    UNUSED;
    panel(h, v, 0, 0, 0x320, 0x45, 0x11166A);
    panel(h, v, 0, 0x45, 0x43, 0x258, 0x11166A);
    panel(h, v, 0x2DE, 0x45, 0x42, 0x213, 0x11166A);
    panel(h, v, 0x43, 0x215, 0x29B, 0x43, 0x11166A);
    panel(h, v, 0x18F, 0x45, 3, 0x1D0, 0x11166A);
    f49bc8a(v, h);
    f49bd3d(v, h);
    return FDS_OK;
}

/* ---- the best times page: 0x0049FE29 (PUSHAD) ---------------------------------------------------- */

/* 0x0040190A: the value as the display unit's (mph / kph) 0x3EDD / 10000, rounded */
static int f40190a(uint32_t *eax)
{
    uint64_t m = (uint64_t)*eax * 0x3EDDu + 0x1388u;
    if ((m >> 32) >= 0x2710u) return 0;              /* the DIV overflows: #DE */
    *eax = (uint32_t)(m / 0x2710u);
    return 1;
}

/* 0x00401AFC: EAX in tenths: "q.d", then (ECX != 0) the unit's word from the table 0x005186B0;
 * the display unit [0x00517954] when it is not 0 first converts the value.  0 when #DE. */
static int fmt_best(const fdb_host *h, vm *v, uint32_t eax, uint32_t ecx, uint32_t *ebx)
{
    uint32_t p = *ebx;
    if (R(0x00517954u) != 0 && !f40190a(&eax)) return 0;
    p = fel_fmt_num(v, eax / 10u, 0, p);
    p = dot(v, p);
    p = fel_fmt_num(v, eax % 10u, 0, p);
    if (ecx != 0) {
        uint32_t tk = (uint32_t)vm_rd8(v, 0x005186B0u + R(0x00517954u) * 2u)
                    | ((uint32_t)vm_rd8(v, 0x005186B0u + R(0x00517954u) * 2u + 1u) << 8);
        p = tokstr(h, v, tk, p);
    }
    *ebx = p;
    return 1;
}

FUNC(f49fe29)
{
    int rc;
    uint32_t edi, edx = 0xF0, ecx = 1, p;
    UNUSED;
    if ((rc = f49e101_(v, h)) != FDS_OK) return rc;
    edi = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    edi = R(0x00708040u) + R(edi + 0x3C) * 0x618u;
    do {
        uint32_t fa, fb, fc;
        W(0x00708318u, ecx);
        if (R(edi + 0x40) == 1) { fa = F_4C; fb = F_2C; fc = F_6C; }
        else                    { fa = F_AC; fb = F_8C; fc = F_CC; }
        text(h, v, fa, edi + 0x20, 0xF0, edx);
        p = timestr(v, R(edi + 0x14), BUF);
        nul(v, p);
        chars7(h, v, fb, BUF, 0x276, edx);
        p = dec(v, R(0x00708318u), BUF);
        nul(v, p);
        text(h, v, fc, BUF, 0xC8, edx);
        W(0x0070831Cu, edx);
        blit(h, v, 0x1Cu + R(edi + 8), 0xD2, R(0x0070831Cu));
        W(0x00708320u, 0x44u + R(edi + 0x10));
        blit(h, v, R(0x00708320u), 0x1CC, R(0x0070831Cu));
        edi += 0x9C;
        if (R(edi) == 0xFFFFFFFFu) return FDS_CRASH;
        edx += 0x19;
        ecx++;
    } while (ecx != 0xB);
    edi = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    tok(h, v, 0x0051820Cu, 0x1A2, 0x142, 0x1FB);
    blit(h, v, 0x1Cu + R(edi + 0x74), 0x14F, 0x1FB);
    text(h, v, F_AC, edi + 0x54, 0x172, 0x1FB);
    W(0x00708320u, 0x44u + R(edi + 0x78));
    blit(h, v, R(0x00708320u), 0x225, 0x1FB);
    p = BUF;
    if (!fmt_best(h, v, R(edi + 0x30), 1, &p)) return FDS_CRASH;
    nul(v, p);
    text(h, v, F_AC, BUF, 0x262, 0x1FB);
    return FDS_OK;
}

/* ---- the championship standings pages: 0x004A0A93, 0x004A1082 ------------------------------------- */
typedef struct {
    uint16_t col[7];            /* the row's seven score words                                   */
    int sign_last;              /* the last column tests the sign too                             */
    int by_class;               /* the row found by class and rank (0x0049E1F0), else by rank (0x004A16F4) */
} stcfg;

static uint32_t signed_num(vm *v, uint32_t val, uint32_t p)
{
    return (int32_t)val < 0 ? minus(v, p) : dec(v, val, p);
}

static int standings_page(vm *v, const fdb_host *h, const stcfg *c)
{
    static const uint16_t X[7] = { 0x19A, 0x1D0, 0x206, 0x23C, 0x272, 0x2A8, 0x2DE };
    static const uint16_t HX[6] = { 0x196, 0x1CC, 0x202, 0x238, 0x26E, 0x2A4 };
    uint32_t edx = 0xF0, edi, k, p;
    int rc;
    (void)fec_46ebbc(v, A_ITIN, 0, R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    tok(h, v, F_CC, 0x484, 0x73, 0xDC);
    tok(h, v, F_AC, 0x485, 0x9C, 0xDC);
    tok(h, v, F_AC, 0x4F9, 0x2DE, 0xDC);
    for (k = 0; k < 6; k++)
        blit(h, v, (R(0x00708038u) == 0x13 ? 0x90u : 0x8Au) + k, HX[k], 0x8C);
    W(0x00707EE5u, R(0x00708020u));
    W(0x00708024u, 10u + R(0x00708020u));
    do {
        uint32_t fa, fb;
        edi = R(A_ROWS2);
        if (c->by_class) { if (f49e1f0(v, R(0x00707EE5u), R(0x0070825Cu), &edi)) return FDS_FATAL; }
        else if (f4a16f4(v, R(0x00707EE5u), &edi)) return FDS_FATAL;
        if ((int32_t)R(edi + 4) > 0) { fa = F_6C; fb = F_4C; }
        else                         { fa = F_CC; fb = F_AC; }
        p = dec(v, R(edi + 0x70), BUF);
        nul(v, p);
        text(h, v, fa, BUF, 0x73, edx);
        text(h, v, fb, edi + 0x14, 0x9C, edx);
        for (k = 0; k < 7; k++) {
            uint32_t val = R(edi + c->col[k]);
            p = (k < 6 || c->sign_last) ? signed_num(v, val, BUF) : dec(v, val, BUF);
            nul(v, p);
            text(h, v, fb, BUF, X[k], edx);
        }
        blit(h, v, 0x1Cu + R(edi + 8), 0x7E, edx);
        W(0x00708320u, 0x44u + R(edi + 0xC));
        blit(h, v, R(0x00708320u), 0x154, edx);
        edx += 0x19;
        W(0x00707EE5u, R(0x00707EE5u) + 1u);
    } while (R(0x00707EE5u) != R(0x00708024u));
    return FDS_OK;
}
FUNC(f4a0a93)
{
    static const stcfg C = { { 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C }, 0, 1 };
    (void)r;
    return standings_page(v, h, &C);
}
FUNC(f4a1082)
{
    static const stcfg C = { { 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50 }, 1, 0 };
    (void)r;
    return standings_page(v, h, &C);
}

/* ---- the best times' record: 0x0049F85B and what it calls --------------------------------------
 * After a stage each player whose time beats the slowest of the stage's ten best ([0x00708040] +
 * the stage's block * 0x618, 0x9C a row) takes the tenth row and is bubbled up.  The row carries
 * a check code made by the original's DES-like cipher 0x00402614 (below). */

/* 0x004027E5: the bits named by the table at TBL (a count - 1, then a byte per bit: bit 5 picks
 * EAX over EDX as the source, the low five the bit) collected, first one first, into ESI:EDI */
static void perm(vm *v, uint32_t tbl, uint32_t eax, uint32_t edx, uint32_t *esi, uint32_t *edi)
{
    uint32_t lo = 0, hi = 0;
    int32_t n = (int32_t)B8(tbl);
    for (tbl++; n >= 0; n--) {
        uint32_t c = B8(tbl++), bit = (((c & 0x20u) ? eax : edx) >> (c & 0x1Fu)) & 1u;
        hi = (hi << 1) | (lo >> 31);
        lo = (lo << 1) | bit;
    }
    *esi = hi;
    *edi = lo;
}

/* BTR r, 27 ; ADC r, r : the 28-bit rotate of the key halves */
static uint32_t rol28(uint32_t x)
{
    uint32_t cf = (x >> 27) & 1u;
    x &= ~(1u << 27);
    return x * 2u + cf;
}

/* 0x004026D2 (PUSHAD): the 64-bit block [0x004DB530] enciphered under the key [0x004DB528] (a
 * sixteen-round Feistel network: the key schedule goes to 0x004DB4A8, sixteen pairs; the tables
 * are the image's) into [0x004DB538] */
static void f4026d2(vm *v)
{
    uint32_t ka = R(0x004DB528u), kb = R(0x004DB52Cu), esi, edi, c, d, k, j;
    perm(v, 0x004DB158u, ka, kb, &esi, &edi);
    c = edi;
    perm(v, 0x004DB175u, ka, kb, &esi, &edi);
    d = edi;
    for (k = 0; k < 16; k++) {
        c = rol28(c);
        d = rol28(d);
        if (B8(0x004DB1C3u + k) != 0) { c = rol28(c); d = rol28(d); }
        perm(v, 0x004DB192u, c, d, &esi, &edi);
        W(0x004DB4A8u + k * 8u, esi);
        W(0x004DB4ACu + k * 8u, edi);
    }
    perm(v, 0x004DB1D3u, R(0x004DB530u), R(0x004DB534u), &esi, &edi);
    for (k = 0; k < 16; k++) {
        uint32_t pe = edi, ps = esi, eax = 0, edx = 0, ebx = 0, sp = 0x004DB2A7u, t = 0x004DB255u, f = 0;
        int32_t n = (int32_t)B8(t);
        for (t++; n >= 0; n--) {                       /* the expansion of EDI, 48 bits in EAX:EDX */
            uint32_t bit = (pe >> (B8(t++) & 0x1Fu)) & 1u;
            eax = (eax << 1) | (edx >> 31);
            edx = (edx << 1) | bit;
        }
        eax ^= R(0x004DB4A8u + k * 8u);
        edx ^= R(0x004DB4ACu + k * 8u);
        for (j = 0; j < 8; j++) {                      /* the eight substitution boxes */
            eax = (eax << 6) | (edx >> 26);
            edx <<= 6;
            ebx = (ebx << 4) | B8(sp + ((eax >> 16) & 0x3Fu));
            sp += 0x40;
        }
        t = 0x004DB286u;
        n = (int32_t)B8(t);
        for (t++; n >= 0; n--)                         /* the permutation of the result */
            f = (f << 1) | ((ebx >> (B8(t++) & 0x1Fu)) & 1u);
        edi = ps ^ f;
        esi = pe;
    }
    perm(v, 0x004DB214u, edi, esi, &esi, &edi);
    W(0x004DB538u, esi);
    W(0x004DB53Cu, edi);
}

/* 0x00402614 (PUSHAD ... POPAD, then EAX / EBX loaded from the result): the check code of EAX
 * (negated when [0x0070AA78] has any of the bits 0xFFFFE003), BL, CL, DL and a checksum byte */
static void f402614(vm *v, uint32_t *eax, uint32_t *ebx, uint32_t ecx, uint32_t edx)
{
    uint32_t a = *eax, k;
    uint8_t al = 0x5A;
    if (R(0x0070AA78u) & 0xFFFFE003u) a = 0u - a;
    W(0x004DB530u, a);
    W8(0x004DB534u, *ebx);
    W8(0x004DB535u, ecx);
    W8(0x004DB536u, edx);
    for (k = 0; k < 7; k++) al = (uint8_t)(al + (uint8_t)~B8(0x004DB530u + k));
    W8(0x004DB537u, al);
    f4026d2(v);
    *eax = R(0x004DB538u);
    *ebx = R(0x004DB53Cu);
}
FUNC(f00402614)
{
    UNUSED;
    f402614(v, &r[VM_EAX], &r[VM_EBX], r[VM_ECX], r[VM_EDX]);
    return FDS_OK;
}

/* 0x0049F9B3 (EAX the stage's index; returns EAX, EBX, the rest kept): the check code of the
 * current player's time, car and the game's [0x0070CC40] for that stage; kept in 0x00708358 / 5C */
static void f49f9b3(vm *v, uint32_t *eax, uint32_t *ebx)
{
    uint32_t pl = R(A_PLAYERS + R(0x007082F8u) * 4u);
    uint32_t a = R(pl + 0x88), b = R(pl + 8);
    f402614(v, &a, &b, R(0x0070CC40u), *eax);
    W(0x00708358u, a);
    W(0x0070835Cu, b);
    *eax = a;
    *ebx = b;
}
FUNC(f0049f9b3)
{
    UNUSED;
    f49f9b3(v, &r[VM_EAX], &r[VM_EBX]);
    return FDS_OK;
}

/* the tenth row of the current stage's block of best times (0x9C a row, 0x57C = nine rows in) */
static int tenth_row(vm *v, uint32_t *row, uint32_t *entry)
{
    int rc;
    uint32_t e = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    if (entry) *entry = e;
    *row = R(0x00708040u) + R(e + 0x3C) * 0x618u + 0x57Cu;
    return FDS_OK;
}

/* 0x0046E68E (PUSHAD): the ten rows' +0x40 of the stage's block cleared */
FUNC(f0046e68e)
{
    uint32_t e, edi, k;
    int rc;
    UNUSED;
    e = fec_46ebbc(v, A_ITIN, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return FDS_RUNAWAY;
    edi = R(0x00708040u) + R(e + 0x3C) * 0x618u;
    for (k = 0; k < 10; k++, edi += 0x9C) W(edi + 0x40, 0);
    return FDS_OK;
}

/* 0x0049F89E (PUSHAD): [0x00708280] = whether the player [0x007082F8]'s time is not worse than the
 * tenth best */
FUNC(f0049f89e)
{
    uint32_t row, esi;
    int rc;
    UNUSED;
    W(0x00708280u, 0);
    if ((rc = tenth_row(v, &row, NULL)) != FDS_OK) return rc;
    esi = R(A_PLAYERS + R(0x007082F8u) * 4u);
    if (!((int32_t)R(esi + 0x88) > (int32_t)R(row + 0x14))) W(0x00708280u, 1);
    return FDS_OK;
}

/* 0x0049F9F0 (PUSHAD): the tenth row bubbled up past every row above it that it beats (or equals) */
FUNC(f0049f9f0)
{
    static const uint8_t F[] = { 0xC, 0x14, 4, 8, 0x1C, 0x10, 0x18, 0x40, 0x4C, 0x50, 0x54, 0x58 };
    uint32_t esi, edi, k, j;
    int rc;
    UNUSED;
    if ((rc = tenth_row(v, &esi, NULL)) != FDS_OK) return rc;
    edi = esi - 0x9C;
    for (k = 0; k < 9; k++, edi -= 0x9C, esi -= 0x9C) {
        if ((int32_t)R(esi + 0x14) > (int32_t)R(edi + 0x14)) continue;
        for (j = 0; j < sizeof F; j++) {
            uint32_t t = R(esi + F[j]);
            W(esi + F[j], R(edi + F[j]));
            W(edi + F[j], t);
        }
        for (j = 0; j < 0x1F; j++) W8(0x007081ECu + j, B8(edi + 0x20 + j));        /* the names, through the */
        for (j = 0; j < 0x1F; j++) W8(edi + 0x20 + j, B8(esi + 0x20 + j));         /* scratch 0x007081EC,   */
        for (j = 0; j < 0x1F; j++) W8(esi + 0x20 + j, B8(0x007081ECu + j));        /* which keeps the upper's */
    }
    return FDS_OK;
}

/* 0x0049F8FA (PUSHAD): the player [0x007082F8] takes the tenth row (its time, class, place, name's
 * pointer, car, the game's [0x0070AA78], the check code of 0x0049F9B3, the place + 1 and a mark),
 * the name copied in, and is bubbled up */
FUNC(f0049f8fa)
{
    uint32_t e, row, esi, a, b = 0, k, src;
    int rc;
    if ((rc = tenth_row(v, &row, &e)) != FDS_OK) return rc;
    W(0x00708358u, R(e + 0x3C));
    esi = R(A_PLAYERS + R(0x007082F8u) * 4u);
    W(row + 0x14, R(esi + 0x88));
    W(row + 0x1C, R(esi + 0x90));
    W(row + 8, R(esi + 4));
    W(row + 0xC, R(esi + 0x18));
    W(row + 0x10, R(esi + 8));
    W(row + 0x4C, R(esi + 0xD0));
    W(row + 0x50, R(0x0070AA78u));
    a = R(0x00708358u);
    f49f9b3(v, &a, &b);
    W(row + 0x54, a);
    W(row + 0x58, b);
    W(row + 4, R(0x007082F8u) + 1u);
    W(row + 0x40, 1);
    W(row + 0x20, R(esi + 0x18));
    src = R(row + 0xC);
    for (k = 0; k < 0x1F; k++) W8(row + 0x20 + k, B8(src + k));
    return f0049f9f0(v, h, r);
}

/* 0x0049F85B (PUSHAD): the stage's block of best times cleared of its marks, then each player in turn
 * ([0x007082F8]) that is not worse than the tenth best is put in ([0x00708288] = 1 once one was) */
FUNC(f0049f85b)
{
    uint32_t ecx = 0;
    int rc;
    if ((rc = f0046e68e(v, h, r)) != FDS_OK) return rc;
    W(0x00708288u, 0);
    do {
        W(0x007082F8u, ecx);
        if ((rc = f0049f89e(v, h, r)) != FDS_OK) return rc;
        if (R(0x00708280u) == 1) {
            W(0x00708288u, 1);
            if ((rc = f0049f8fa(v, h, r)) != FDS_OK) return rc;
        }
        ecx++;
    } while (ecx != R(0x007082F0u));
    return FDS_OK;
}

/* ---- the table ----------------------------------------------------------------------- */
static const struct { uint32_t va; fdb_fn fn; } TAB[] = {
    { 0x0049DC01, f49dc01 }, { 0x0049DC6C, f49dc6c }, { 0x0049EF98, f49ef98 },
    { 0x0049F81D, f49f81d }, { 0x004A074A, f4a074a }, { 0x004A16B6, f4a16b6 }, { 0x0049DCDC, f49dcdc },
    { 0x004A0712, f4a0712 }, { 0x0049F3B4, f49f3b4 }, { 0x0049F3DD, f49f3dd },
    { 0x0049F40A, f49f40a }, { 0x0049F433, f49f433 }, { 0x0049E101, f49e101 },
    { 0x0049DD98, f49dd98 }, { 0x0049EBCA, f49ebca }, { 0x0049EFF7, f49eff7 },
    { 0x0049F460, f49f460 },
    { 0x0049E7F9, f49e7f9 }, { 0x004A0115, f4a0115 }, { 0x004A4D6D, f4a4d6d },
    { 0x0049FB06, f49fb06 }, { 0x004A4998, f4a4998 }, { 0x004A4B78, f4a4b78 },
    { 0x0049B994, f49b994 }, { 0x0049BB10, f49bb10 }, { 0x0049FE29, f49fe29 },
    { 0x004A0A93, f4a0a93 }, { 0x004A1082, f4a1082 },
    { 0x00402614, f00402614 }, { 0x0049F9B3, f0049f9b3 }, { 0x0046E68E, f0046e68e },
    { 0x0049F89E, f0049f89e }, { 0x0049F9F0, f0049f9f0 }, { 0x0049F8FA, f0049f8fa },
    { 0x0049F85B, f0049f85b },
    { 0x004A4277, f4a4277 }, { 0x004A42F5, f4a42f5 },
};

fdb_fn fdb_lookup(uint32_t va)
{
    unsigned i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return TAB[i].fn;
    return NULL;
}
int fdb_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fdb_va(int i) { return TAB[i].va; }

/* the original's addresses the host answers: what the gate must hook */
static const uint32_t SVC[] = {
    0x00402311, 0x0040239C, 0x004066F9, 0x004495D3, 0x00401F8F,
    0x00462F22, 0x004631EB, 0x004634B4, 0x0046377D, 0x004A07E0, 0x00463D3B,
};
int fdb_svc_count(void) { return (int)(sizeof SVC / sizeof SVC[0]); }
uint32_t fdb_svc_va(int i) { return SVC[i]; }
