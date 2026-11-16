/* fe_champ.c - see fe_champ.h.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps
 * as the original's does. */
#include "fe_champ.h"

#include "fe_logic.h"

#define R(a)    vm_rd32(v, (a))
#define W(a, x) vm_wr32(v, (a), (x))

enum {
    A_RALLIES = 0x006E7EE9,     /* the itinerary: 0xA4-byte entries, +0 the kind */
    A_SETS    = 0x006E7539,     /* the stage-set chain (FUN_0046F686)            */
    A_DRIVERS = 0x006EDFE8,     /* the 0x94-byte driver records, -1 ends         */
    A_RNG_MAX = 100000          /* a scan longer than this never ends in the original */
};

/* the original's fatal exit: two words, then 0x00407383 */
static void fatal(vm *v, uint32_t code, uint32_t arg)
{
    W(0x004F0274u, code);
    W(0x004F0278u, arg);
}

/* 0x0046EBBC (PUSH EAX/EBX/ECX ... POP: every register but EDI kept): with
 * EBX != 0, run on to the EBX-th kind-4 entry and one past it; then EAX more
 * entries.  The original scans memory until it finds one: no end is a hang
 * (or a wild read), reported as FEC_RUNAWAY. */
uint32_t fec_46ebbc(vm *v, uint32_t edi, uint32_t eax, uint32_t ebx, int *rc)
{
    *rc = FEC_OK;
    if (ebx != 0) {
        uint32_t ecx = 0, guard = 0;
        for (;;) {
            while (R(edi) != 4) {
                edi += 0xA4;
                if (++guard > A_RNG_MAX) { *rc = FEC_RUNAWAY; return edi; }
            }
            ecx++;
            if (ecx == ebx) { edi += 0xA4; break; }
            edi += 0xA4;
        }
    }
    return edi + eax * 0xA4u;
}

/* 0x0046F686 (no PUSHAD: EBX, ECX, EDX, ESI, EDI are all spent): the stage
 * record of an itinerary entry - the chain node at 0x006E7539 whose +4 is the
 * entry's +8, then its record `entry +4` (0x40 bytes each) from +0x34.
 *
 * THE ORIGINAL'S WALK NEVER LEAVES THE HEAD: its loop's back edge (0x0046F6A4,
 * `jmp 0x46F68C`) lands on the `lea esi, [0x6E7539]` that starts the function,
 * not on the compare after it.  So the head node either has the id, or - its
 * next pointer (+0) being 0 - the fatal exit 0x004D54FF is taken, or it has a
 * next and the loop never ends (FEC_RUNAWAY; the interpreter stops on its
 * budget).  A node past the head is never looked at. */
uint32_t fec_46f686(vm *v, uint32_t edi, uint32_t regs[8], int *rc)
{
    uint32_t ebx = R(edi + 4), ecx = R(edi + 8), esi = A_SETS;
    *rc = FEC_OK;
    if (R(esi + 4) != ecx) {
        if (R(esi) == 0) { fatal(v, 0x8070DA00u, 0x004DA5F7u); *rc = FEC_FATAL_LIST; }
        else *rc = FEC_RUNAWAY;
        return edi;
    }
    ebx *= 0x40u;
    esi += 0x34u + ebx;
    if (regs) {
        regs[VM_EBX] = ebx; regs[VM_ECX] = ecx; regs[VM_EDX] = ecx;
        regs[VM_ESI] = esi; regs[VM_EDI] = esi;
    }
    return esi;
}

/* FILD of the signed word `x`, FMUL by the float `fbits`, FISTP to 32 bits
 * under the default control word (round to nearest, ties to even; what does
 * not fit is the integer indefinite 0x80000000).  The x87's product is exact
 * (a 32-bit integer times a 24-bit mantissa fits its 64), so this is done in
 * integers: |x| * m * 2^sh rounded by hand, the same on a host whose long
 * double is no wider than a double. */
static uint32_t fild_fmul_fistp(uint32_t x, uint32_t fbits)
{
    uint32_t e = (fbits >> 23) & 0xFFu, m = fbits & 0x7FFFFFu;
    int neg = (int)(((fbits >> 31) ^ (x >> 31)) & 1u), sh;
    uint64_t a, q, limit;
    uint32_t ax = (x >> 31) ? 0u - x : x;           /* |x|, 2^31 for INT_MIN */
    if (e == 0xFFu) return 0x80000000u;             /* infinity, NaN */
    if (e) { m |= 0x800000u; sh = (int)e - 150; } else sh = -149;
    a = (uint64_t)ax * m;
    if (a == 0) return 0;
    limit = neg ? 0x80000000ull : 0x7FFFFFFFull;
    if (sh >= 0) {
        if (sh >= 32 || a > (limit >> sh)) return 0x80000000u;
        q = a << sh;
    } else {
        int s = -sh;
        if (s >= 64) q = 0;                         /* |value| < 2^-8 */
        else {
            uint64_t rem = a & ((1ull << s) - 1), half = 1ull << (s - 1);
            q = a >> s;
            if (rem > half || (rem == half && (q & 1u))) q++;
        }
        if (q > limit) return 0x80000000u;
    }
    return neg ? (uint32_t)(0u - (uint32_t)q) : (uint32_t)q;
}

/* FLD of a float and FSTP of it again: exact, but a signalling NaN comes back quiet */
static uint32_t quiet(uint32_t f)
{
    if ((f & 0x7F800000u) == 0x7F800000u && (f & 0x007FFFFFu)) f |= 0x00400000u;
    return f;
}

/* 0x00460EF7: index [0x7082C4] clamped to 0..5 (signed); the two float tables
 * at 0x005B8998 and 0x005B89B0 give the factors, kept in [0x006CEEB4] and
 * [0x006CEEB8]; the three words 0x70CA5C, 0x70CA58, 0x70CA54 scale by the
 * first, 0x70CA50 by the second.  (The log calls it makes are no-ops in the
 * game: the host hooks them.) */
void fec_460ef7(vm *v)
{
    int32_t i = (int32_t)R(0x007082C4u);
    uint32_t fa, fb;
    if (i < 0) i = 0;
    if (i >= 5) i = 5;
    fa = quiet(R(0x005B8998u + 4u * (uint32_t)i));
    fb = quiet(R(0x005B89B0u + 4u * (uint32_t)i));
    W(0x006CEEB4u, fa);
    W(0x006CEEB8u, fb);
    W(0x0070CA5Cu, fild_fmul_fistp(R(0x0070CA5Cu), fa));
    W(0x0070CA58u, fild_fmul_fistp(R(0x0070CA58u), fa));
    W(0x0070CA54u, fild_fmul_fistp(R(0x0070CA54u), fa));
    W(0x0070CA50u, fild_fmul_fistp(R(0x0070CA50u), fb));
}

static uint32_t rmod(vm *v, uint32_t n)
{
    uint32_t r = 0;
    fel_rng_mod(v, n, &r);
    return r;
}

/* 0x0046D5B5 (PUSHAD ... POPAD; EDI, EBX, EAX are its own): the rally's
 * [0x7082C0]'th entry (0x0046EBBC) gives four time words (+0x18..+0x24), scaled
 * by 0x00460EF7; the stage record (0x0046F686) gives +0xC, the base of the
 * rivals' pace: [0x70CA60] = it, [0x70CA64] = it * 100 / 0x96A.  Then for each
 * class ECX below [0x708260] (a do-while) every driver of that class (+8) -
 * 0x94-byte records from 0x6EDFE8, the do-while ending when the NEXT record's
 * first word is -1 - gets a random offset (3000, plus 1000 times a count that
 * grows once per call, the first draw after [0x7082AC] = 1 restarting it) added
 * to its +0xC and +0x10 when its +0x24 is 1, the pace's share EBP growing by
 * [0x70835C] a driver.
 * Draws (0x0040188E) in the original's order: 3000, then 1000 if [0x7082AC] != 1. */
int fec_46d5b5(vm *v)
{
    uint32_t edi = A_RALLIES, ecx, ebp, eax;
    int rc;
    edi = fec_46ebbc(v, edi, R(0x007082C0u), R(0x007082C4u), &rc);
    if (rc) return rc;
    W(0x0070CA50u, R(edi + 0x18) - 0);
    W(0x0070CA54u, R(edi + 0x1C) - 0);
    W(0x0070CA58u, R(edi + 0x20) - 0);
    W(0x0070CA5Cu, R(edi + 0x24) - 0);
    fec_460ef7(v);
    edi = fec_46f686(v, edi, NULL, &rc);
    if (rc) return rc;
    eax = R(edi + 0xC);
    W(0x0070CA60u, eax);
    eax *= 100u;
    if (eax == 0) { fatal(v, 0x80701010u, 0); return FEC_FATAL_ZERO; }
    W(0x0070CA64u, eax / 0x96Au);
    ecx = 0;
    do {
        edi = A_DRIVERS;
        ebp = 0;
        do {
            if (R(edi + 8) == ecx) {
                uint32_t edx = R(0x0070CA50u + ecx * 4u), ebx = R(0x0070CA68u + ecx * 4u), q;
                q = edx >> 1;
                if (q == 0) { fatal(v, 0x80701010u, 0); return FEC_FATAL_ZERO; }
                if (ebx == 0) return FEC_FATAL_DIV;
                W(0x0070835Cu, q / ebx);
                W(0x00708358u, rmod(v, 3000));
                if (R(0x007082ACu) == 1) W(0x007082B0u, 0);
                if (R(0x007082ACu) != 1) {
                    eax = rmod(v, 1000) * R(0x007082B0u);
                    W(0x00708358u, R(0x00708358u) + eax);
                    W(0x007082ACu, 0);
                    W(0x007082B0u, R(0x007082B0u) + 1u);
                }
                if (R(edi + 0x24) == 1) {
                    eax = 0u + R(0x00708358u) + ebp + edx;
                    W(edi + 0xC, eax);
                    W(edi + 0x10, R(edi + 0x10) + R(0x00708358u) + ebp + edx);
                }
                ebp += R(0x0070835Cu);
            }
            edi += 0x94;
        } while (R(edi) != 0xFFFFFFFFu);
        ecx++;
    } while (ecx != R(0x00708260u));
    return FEC_OK;
}

/* 0x0046E580 (PUSHAD ... POPAD): the rest of a rally after a retirement.  Each
 * pass takes the entry (0x0046EBBC) at [0x7082C4] / [0x7082C0]: kind 0 a stage
 * not yet run (0x0046D5B5), 1 the rally's start (0x0047A87F), 2 and anything
 * else nothing; then [0x7082C0] and [0x7082B8] step and 0x0046E617 resets the
 * players' times; kind 4 (the rally's end) or 3 (also sets [0x708290]) stops the
 * loop, and 0x0047A87F runs once more. */
int fec_46e580(vm *v)
{
    fel_fn f47a87f = fel_lookup(0x0047A87Fu), f46e617 = fel_lookup(0x0046E617u);
    uint32_t guard = 0;
    for (;;) {
        int rc;
        uint32_t edi = fec_46ebbc(v, A_RALLIES, R(0x007082C0u), R(0x007082C4u), &rc), kind;
        if (rc) return rc;
        kind = R(edi);
        if (kind == 0) {
            rc = fec_46d5b5(v);
            if (rc) return rc;
        } else if (kind == 1) {
            f47a87f(v);
        } else if (kind == 4) {
            break;
        } else if (kind == 3) {
            W(0x00708290u, 1);
            break;
        }
        W(0x007082C0u, R(0x007082C0u) + 1u);
        W(0x007082B8u, R(0x007082B8u) + 1u);
        f46e617(v);
        if (++guard > A_RNG_MAX) return FEC_RUNAWAY;
    }
    f47a87f(v);
    return FEC_OK;
}

/* ---- 0x0046F785, the driver that sets a rally's race up ---------------------
 *
 * Not PUSHAD'd, but it reads no register: everything it uses it loads itself
 * (the host runs it with none), and it leaves them all spent. */

static uint32_t svc(const fec_services *s, uint32_t va, uint32_t eax, uint32_t ebx)
{
    return s->svc(s->ud, va, eax, ebx, 0, 0);
}

/* 0x0046F737 (PUSHAD ... POPAD): the 200 stage-result records at [0x708054]
 * (0x9C bytes each) emptied: two -1 words, the +0x14 / +0x18 times at the "no
 * time" 0x36F272 */
void fec_46f737(vm *v)
{
    uint32_t esi = R(0x00708054u), k;
    for (k = 0; k < 0xC8; k++, esi += 0x9C) {
        W(esi, 0xFFFFFFFFu);
        W(esi + 4, 0xFFFFFFFFu);
        W(esi + 8, 0);
        W(esi + 0xC, 0);
        W(esi + 0x10, 0);
        W(esi + 0x14, 0x0036F272u);
        W(esi + 0x18, 0x0036F272u);
        W(esi + 0x1C, 0);
    }
}

/* 0x004A51D5 (PUSHAD ... POPAD): the stage-result records from the driver
 * records (a do-while, ended by the next driver's first word being -1): +0x14 =
 * +0xC, +4 = 0, +0x1C = +8, +0x18 = +0x10, +8 = +0x14, +0x10 = +0x18, +0 = +0,
 * +0x4C = +0x24, +0xC = the name token (table 0x6F47FC, 0x14 a name, indexed by
 * the driver's +0) and the token's text at +0x20 (0x00401F8F, then the NUL
 * of 0x00401F02). */
void fec_4a51d5(vm *v, const fec_services *s)
{
    uint32_t edi = A_DRIVERS, esi;
    fec_46f737(v);
    esi = R(0x00708054u);
    do {
        uint32_t tok = R(0x006F47FCu + R(edi) * 0x14u), n;
        W(esi + 0x14, R(edi + 0xC));
        W(esi + 4, 0);
        W(esi + 0x1C, R(edi + 8));
        W(esi + 0x18, R(edi + 0x10));
        W(esi + 8, R(edi + 0x14));
        W(esi + 0x10, R(edi + 0x18));
        W(esi, R(edi));
        W(esi + 0x4C, R(edi + 0x24));
        W(esi + 0xC, tok);
        n = svc(s, 0x00401F8Fu, tok, esi + 0x20);
        vm_wr8(v, esi + 0x20 + n, 0);
        edi += 0x94;
        esi += 0x9C;
    } while (R(edi) != 0xFFFFFFFFu);
}

/* 0x0046D0CB (PUSHAD ... POPAD): ninety of the stage-result records at
 * [0x708054] become the 0x50-byte opponents' records at [0x70805C] (+0xC =
 * +0x14, +4 = +8, +8 = +0x10, +0 = +0xC): with [0x70CAA8] == 2 the ones whose
 * +0x1C is not 0, else those whose +0x1C is 0 (each found by a scan on from the
 * last one taken). */
int fec_46d0cb(vm *v)
{
    uint32_t esi = R(0x0070805Cu), edi = R(0x00708054u), ecx, guard = 0;
    int two = R(0x0070CAA8u) == 2;
    for (ecx = 0x5A; ecx != 0; ecx--) {
        while ((R(edi + 0x1C) == 0) == two) {
            edi += 0x9C;
            if (++guard > A_RNG_MAX) return FEC_RUNAWAY;
        }
        W(esi + 0xC, R(edi + 0x14));
        W(esi + 4, R(edi + 8));
        W(esi + 8, R(edi + 0x10));
        W(esi, R(edi + 0xC));
        edi += 0x9C;
        esi += 0x50;
    }
    return FEC_OK;
}

/* 0x004A526C (PUSHAD ... POPAD): the four players' +0x288 = 0 */
void fec_4a526c(vm *v)
{
    uint32_t k;
    for (k = 0; k < 4; k++) W(R(0x00708DE8u + 4u * k) + 0x288u, 0);
}

/* 0x004A51BB: ECX a player; EAX stays, unless that player's +0xD0 is not 1,
 * then 0.  EDI is left the player's record. */
uint32_t fec_4a51bb(vm *v, uint32_t eax, uint32_t ecx, uint32_t *edi)
{
    *edi = R(0x00708DE8u + 4u * ecx);
    return R(*edi + 0xD0) == 1 ? eax : 0;
}

/* 0x0047D4A3 (PUSHAD ... POPAD): with [0x70AB50] == 1 the engine's clear,
 * set-up and clear again */
void fec_47d4a3(vm *v, const fec_services *s)
{
    if (R(0x0070AB50u) == 1) {
        svc(s, 0x00440F60u, 0, 0);
        svc(s, 0x004713BAu, 0, 0);
        svc(s, 0x00440F60u, 0, 0);
    }
}

/* the launches of modes 0xB / 0x10 (two players), 0xC / 0x11 (three) and 0xD /
 * 0x12 (four): each player 0..n-1 is made current ([0x7082F8]); one that is in
 * the race ([+0xD0] == 1) is launched (EAX 1 when the next player is in it too,
 * through 0x004A51BB, 0 for the last; EBX 1) and the outcomes are ANDed.  A
 * player not in the race is -1 (the first, only in the running AND; the others
 * also in [0x70CF94]).  An outcome of -2 ends it all (0x0047D46C, 0x0047D4A3).
 * Everyone -1 ends the rally's race: [0x708294] = 0, and with `out3` (mode
 * 0xD) the fourth player is put out too (+0x88, +0xD0 = 6).  Returns 1 when the
 * -2 ended it. */
static int launches(vm *v, const fec_services *s, uint32_t n, int out3)
{
    uint32_t acc = 0, k;
    for (k = 0; k < n; k++) {
        uint32_t P, eax, gof, edi;
        W(0x007082F8u, k);
        P = R(0x00708DE8u + 4u * k);
        if (R(P + 0xD0) != 1) {
            if (k == 0) acc = 0xFFFFFFFFu;
            else W(0x0070CF94u, 0xFFFFFFFFu);
            continue;
        }
        eax = k + 1 < n ? fec_4a51bb(v, 1, k + 1, &edi) : 0;
        svc(s, 0x00480461u, eax, 1);
        gof = R(0x0070CF94u);
        if (gof == 0xFFFFFFFEu) {
            fel_lookup(0x0047D46Cu)(v);
            fec_47d4a3(v, s);
            return 1;
        }
        acc = k == 0 ? gof : acc & gof;
    }
    if (acc == 0xFFFFFFFFu) {
        if (out3) {
            uint32_t P3 = R(0x00708DE8u + 4u * 3u);
            W(P3 + 0x88, 0x0036EE80u);
            W(P3 + 0xD0, 6);
        }
        W(0x00708294u, 0);
    }
    return 0;
}

/* 0x0046F785: [0x708294] = 1 (the rally goes on) and [0x70CC58] = bit 18 of
 * [0x70AA78]; the rivals' times (0x0046D5B5); eight random drivers (of 0xB2)
 * are slower by 0x4844 + a draw of 3000; one to four more, drivers whose +0x24
 * is 1, retire (+0x24 = 4, both times 0x36EE8A).  With [0x707BC4] == 1 (the
 * player retired) everyone is out (0x0047D46C) and that is all; otherwise the
 * stage-result records are made (0x004A51D5), sorted (0x004775CB), the
 * opponents' records taken from them (0x0046D0CB), and the race is launched
 * as [0x7082F4] says: 9 and 0xE the first player alone (-1 ends the race), 0xA
 * and 0xF so with [0x70CC4C] = [0x707E34] and [0x70D004] ANDed in, 0xB and
 * 0x10 two players, 0xC and 0x11 three, 0xD and 0x12 four (`launches`).  Every
 * way out ends in 0x004A526C.  The launcher (0x00480461) is the host's: it
 * writes the outcome in [0x70CF94]. */
int fec_46f785(vm *v, const fec_services *s)
{
    uint32_t ecx, edi, eax, mode, guard = 0;
    int rc;
    W(0x00708294u, 1);
    W(0x0070CC58u, 0);
    if (R(0x0070AA78u) & 0x00040000u) W(0x0070CC58u, 1);
    W(0x00707E34u, 0);
    rc = fec_46d5b5(v);
    if (rc) return rc;
    for (ecx = 8; ecx != 0; ecx--) {
        edi = A_DRIVERS + rmod(v, 0xB2) * 0x94u;
        eax = rmod(v, 3000) + 0x4844u;
        W(edi + 0xC, R(edi + 0xC) + eax);
        W(edi + 0x10, R(edi + 0x10) + eax);
    }
    ecx = rmod(v, 4) + 1u;
    do {
        for (;;) {
            edi = A_DRIVERS + rmod(v, 0xB2) * 0x94u;
            if (R(edi + 0x24) == 1) break;
            if (++guard > A_RNG_MAX) return FEC_RUNAWAY;
        }
        W(edi + 0x24, 4);
        W(edi + 0xC, 0x0036EE8Au);
        W(edi + 0x10, 0x0036EE8Au);
        ecx--;
    } while (ecx != 0);
    if (R(0x00707BC4u) == 1) {
        fel_lookup(0x0047D46Cu)(v);
    } else {
        fec_4a51d5(v, s);
        fel_lookup(0x004775CBu)(v);
        rc = fec_46d0cb(v);
        if (rc) return rc;
        W(0x0070CC38u, R(0x0070805Cu));
        edi = fec_46ebbc(v, A_RALLIES, R(0x007082C0u), R(0x007082C4u), &rc);
        if (rc) return rc;
        edi = fec_46f686(v, edi, NULL, &rc);
        if (rc) return rc;
        W(0x0070CC3Cu, R(edi + 0x34));
        mode = R(0x007082F4u);
        if (mode == 9 || mode == 0xE) {
            W(0x007082F8u, 0);
            svc(s, 0x00480461u, 0, 1);
            if (R(0x0070CF94u) == 0xFFFFFFFFu) W(0x00708294u, 0);
        } else if (mode == 0xA || mode == 0xF) {
            W(0x007082F8u, 0);
            W(0x0070CC4Cu, R(0x00707E34u));
            svc(s, 0x00480461u, 0, 1);
            if (mode == 0xA) W(0x0070CC4Cu, R(0x00707E34u));    /* only mode 0xA reloads it after the launch */
            if ((R(0x0070CF94u) & R(0x0070D004u)) == 0xFFFFFFFFu) W(0x00708294u, 0);
        } else if (mode == 0xB || mode == 0x10) {
            launches(v, s, 2, 0);
        } else if (mode == 0xC || mode == 0x11) {
            launches(v, s, 3, 0);
        } else if (mode == 0xD) {
            launches(v, s, 4, 1);
        } else if (mode == 0x12) {
            launches(v, s, 4, 0);
        }
    }
    fec_4a526c(v);
    return FEC_OK;
}

static const struct { uint32_t va; fec_fn fn; } TAB[] = {
    { 0x0046D5B5, fec_46d5b5 }, { 0x0046E580, fec_46e580 },
};
fec_fn fec_lookup(uint32_t va)
{
    unsigned i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return TAB[i].fn;
    return 0;
}
int fec_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fec_va(int i) { return TAB[i].va; }
