/* x86vm.c - see x86vm.h.
 *
 * Every opcode here is one the draw paths of the car setup screen use; an
 * opcode not listed ends the run with its address (vm_error), because a
 * silent wrong answer is the only way this could mislead. */
#include "x86vm.h"
#include "x87.h"

#include <stdio.h>
#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vmstore.c"          /* the data copy and the hook table; x86vm.c is the interpreter over them */

const char *vm_error(const vm *v)
{
    return v->err;
}

uint64_t vm_steps(const vm *v)
{
    return v->steps;
}

void vm_regs(const vm *v, uint32_t out[8])
{
    memcpy(out, v->r, sizeof v->r);
}

/* ---- flags and the ALU -------------------------------------------------- */

static void szf(vm *v, uint32_t r, int bits)
{
    uint32_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    v->zf = (r & m) == 0;
    v->sf = (int)((r >> (bits - 1)) & 1);
}

/* op: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp; returns the result
 * (the caller discards it for cmp). */
static uint32_t alu(vm *v, int op, uint32_t a, uint32_t b, int bits)
{
    uint64_t m = bits == 8 ? 0xFFu : bits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t top = bits == 8 ? 0x80u : bits == 16 ? 0x8000u : 0x80000000u;
    uint32_t r;
    a &= (uint32_t)m; b &= (uint32_t)m;
    switch (op) {
    case 0: r = (uint32_t)((a + (uint64_t)b) & m);
            v->cf = ((uint64_t)a + b) > m;
            v->of = (((a ^ r) & (b ^ r)) & top) != 0; break;
    case 2: { uint32_t c = (uint32_t)v->cf;
            r = (uint32_t)((a + (uint64_t)b + c) & m);
            v->cf = ((uint64_t)a + b + c) > m;
            v->of = (((a ^ r) & (b ^ r)) & top) != 0; break; }
    case 3: { uint32_t c = (uint32_t)v->cf;
            r = (uint32_t)((a - (uint64_t)b - c) & m);
            v->cf = (uint64_t)a < (uint64_t)b + c;
            v->of = (((a ^ b) & (a ^ r)) & top) != 0; break; }
    case 5: case 7:
            r = (uint32_t)((a - (uint64_t)b) & m);
            v->cf = a < b;
            v->of = (((a ^ b) & (a ^ r)) & top) != 0; break;
    case 1: r = a | b; v->cf = v->of = 0; break;
    case 4: r = a & b; v->cf = v->of = 0; break;
    default: r = a ^ b; v->cf = v->of = 0; break;
    }
    szf(v, r, bits);
    return r;
}

static int cond(const vm *v, int cc)
{
    int r;
    switch (cc >> 1) {
    case 0: r = v->of; break;
    case 1: r = v->cf; break;
    case 2: r = v->zf; break;
    case 3: r = v->cf || v->zf; break;
    case 4: r = v->sf; break;
    case 5: r = 0; break;                       /* parity: unused here        */
    case 6: r = v->sf != v->of; break;
    default: r = v->zf || (v->sf != v->of); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---- decode ------------------------------------------------------------- */

typedef struct { int reg, is_reg, rm; uint32_t ea; } modrm;

static uint32_t fetch8(vm *v)  { return vm_rd8(v, v->eip++); }
static uint32_t fetch32(vm *v)
{
    uint32_t x = vm_rd32(v, v->eip);
    v->eip += 4;
    return x;
}

static void decode(vm *v, modrm *m)
{
    uint32_t b = fetch8(v);
    int mod = (int)(b >> 6), rm = (int)(b & 7);
    m->reg = (int)((b >> 3) & 7);
    m->is_reg = mod == 3;
    m->rm = rm;
    m->ea = 0;
    if (mod == 3) return;
    if (rm == 4) {
        uint32_t s = fetch8(v);
        int sc = (int)(s >> 6), ix = (int)((s >> 3) & 7), bs = (int)(s & 7);
        uint32_t ea = 0;
        if (bs == 5 && mod == 0) ea = fetch32(v);
        else ea = v->r[bs];
        if (ix != 4) ea += v->r[ix] << sc;
        m->ea = ea;
    } else if (rm == 5 && mod == 0) {
        m->ea = fetch32(v);
        return;
    } else {
        m->ea = v->r[rm];
    }
    if (mod == 1) m->ea += (uint32_t)(int32_t)(int8_t)fetch8(v);
    else if (mod == 2) m->ea += fetch32(v);
}

static uint32_t get(vm *v, const modrm *m, int bits)
{
    if (bits == 32) return m->is_reg ? v->r[m->rm] : vm_rd32(v, m->ea);
    if (m->is_reg)
        return m->rm < 4 ? v->r[m->rm] & 0xFFu : (v->r[m->rm - 4] >> 8) & 0xFFu;
    return vm_rd8(v, m->ea);
}

static void put(vm *v, const modrm *m, int bits, uint32_t x)
{
    if (bits == 32) {
        if (m->is_reg) v->r[m->rm] = x; else vm_wr32(v, m->ea, x);
        return;
    }
    if (m->is_reg) {
        if (m->rm < 4) v->r[m->rm] = (v->r[m->rm] & ~0xFFu) | (x & 0xFFu);
        else v->r[m->rm - 4] = (v->r[m->rm - 4] & ~0xFF00u) | ((x & 0xFFu) << 8);
    } else vm_wr8(v, m->ea, (uint8_t)x);
}

static void push(vm *v, uint32_t x)
{
    v->r[VM_ESP] -= 4;
    vm_wr32(v, v->r[VM_ESP], x);
}

static uint32_t pop(vm *v)
{
    uint32_t x = vm_rd32(v, v->r[VM_ESP]);
    v->r[VM_ESP] += 4;
    return x;
}

static void do_call(vm *v, uint32_t target)
{
    int i;
    v->last_call = target;
    v->last_site = v->eip;
    v->ring[v->nring & 7][0] = target;
    v->ring[v->nring & 7][1] = v->eip;
    v->nring++;
    for (i = 0; i < v->nhk; i++)
        if (v->hk[i].va == target) {
            uint32_t r[8];
            memcpy(r, v->r, sizeof r);
            hook_trace(v, target, r);
            v->hk[i].fn(v, r, v->hk[i].ud);
            r[VM_ESP] = v->r[VM_ESP];
            memcpy(v->r, r, sizeof r);
            return;
        }
    push(v, v->eip);
    v->eip = target;
}

/* ---- x87 ----------------------------------------------------------------- */

/* / (the session brief's x87 slice, 2026-10-25): the whole
 * D8..DF map the blades' load and the wheels' spray use - the register
 * forms, the 64- and 80-bit memory forms, the integer arithmetic, the
 * constants and transcendentals, and the control word.  Arithmetic is
 * long double (the x87's 80 bits on an x86 host); the control word's
 * ROUNDING field is honoured by every integer store and FRNDINT, and (2026-10-26) by every arithmetic result and float store: fpu() sets the
 * host's rounding mode from it around each instruction, as the x87 rounds
 * them all by RC (the wipers' scan conversion runs under chop; x87_vm_check's
 * "/ rc" cases failed 5 of 6 before).  Its
 * PRECISION field is stored but not applied: which one the game runs
 * under is still open.  The exceptions are
 * masked, as FNINIT leaves them: an out-of-range integer store writes the
 * integer indefinite. */

#define ST(i) v->fs[v->fsp - 1 - (i)]

/* The transcendentals and FPREM.  On an x86 host they are the hardware's own
 * instructions, which is what the original ran: libm's long double routines
 * differ from the x87's microcode in the last bits (x87_vm_check measured it,
 * 2026-10-25).  Elsewhere (the handheld) libm stands in - close, NOT exact.
 * FSIN and FCOS are x87.h's, shared with the ports this VM gates. */
#ifdef X87_NATIVE
static long double x_tan(long double x) { __asm__("fptan; fstp %%st(0)" : "+t"(x)); return x; }
static long double x_f2xm1(long double x) { __asm__("f2xm1" : "+t"(x)); return x; }
static long double x_yl2x(long double y, long double x)
{
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}
/* one FPREM (prem1 = 0) or FPREM1, as the hardware leaves ST0 and the codes */
static long double x_prem(long double a, long double b, int prem1, uint32_t *cc)
{
    unsigned short sw;
    if (prem1) __asm__("fprem1; fnstsw %1" : "+t"(a), "=a"(sw) : "u"(b));
    else       __asm__("fprem; fnstsw %1" : "+t"(a), "=a"(sw) : "u"(b));
    *cc = sw & 0x4700u;
    return a;
}
#else
static long double x_tan(long double x) { return tanl(x); }
static long double x_f2xm1(long double x) { return exp2l(x) - 1.0L; }
static long double x_yl2x(long double y, long double x) { return y * log2l(x); }
static long double x_prem(long double a, long double b, int prem1, uint32_t *cc)
{
    /* complete (C2 clear); C0 C3 C1 = the quotient's Q2 Q1 Q0, from the
     * exact fmod against 8b and whether FPREM1 rounded up */
    long double rt = fmodl(a, b), r = prem1 ? remainderl(a, b) : rt;
    uint32_t q = (uint32_t)truncl(fmodl(fabsl(a), 8.0L * fabsl(b)) / fabsl(b));
    if (r != rt) q++;
    *cc = ((q & 4) ? 0x100u : 0) | ((q & 2) ? 0x4000u : 0) | ((q & 1) ? 0x200u : 0);
    return r;
}
#endif

static int fpush(vm *v, long double x, uint32_t at)
{
    if (v->fsp >= 8) { fail(v, "x87 stack overflow at 0x%08X", at); return -1; }
    v->fs[v->fsp++] = x;
    return 0;
}

static int fneed(vm *v, int n, uint32_t at)
{
    if (v->fsp < n) { fail(v, "x87 stack underflow at 0x%08X", at); return -1; }
    return 0;
}

static long double fround(const vm *v, long double x)
{
    switch ((v->fcw >> 10) & 3) {
    case 1:  return floorl(x);
    case 2:  return ceill(x);
    case 3:  return truncl(x);
    default: {                               /* to nearest, ties to even */
        long double f = floorl(x), d = x - f;
        if (d > 0.5L || (d == 0.5L && fmodl(f, 2.0L) != 0)) f += 1.0L;
        return f == 0 ? copysignl(0.0L, x) : f;      /* -0.3 rounds to -0 */
    }
    }
}

/* FCOM's condition codes: C0 below, C3 equal, all three unordered */
static void fcmp(vm *v, long double a, long double b)
{
    if (isnan(a) || isnan(b)) v->fsw_cc = 0x4500u;
    else if (a < b)  v->fsw_cc = 0x0100u;
    else if (a == b) v->fsw_cc = 0x4000u;
    else             v->fsw_cc = 0;
}

/* /0../7 of D8 DA DC DE: ST0 = ST0 op x (or x op ST0 for the reversed) */
static long double farith(int r, long double a, long double b)
{
    switch (r) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6: return a / b;
    default: return b / a;
    }
}

static long double rd_f32(vm *v, uint32_t ea)
{
    uint32_t w = vm_rd32(v, ea);
    float f;
    memcpy(&f, &w, 4);
    return (long double)f;
}

static long double rd_f64(vm *v, uint32_t ea)
{
    uint64_t w = (uint64_t)vm_rd32(v, ea) | ((uint64_t)vm_rd32(v, ea + 4) << 32);
    double d;
    memcpy(&d, &w, 8);
    return (long double)d;
}

static void wr_f64(vm *v, uint32_t ea, long double x)
{
    double d = (double)x;
    uint64_t w;
    memcpy(&w, &d, 8);
    vm_wr32(v, ea, (uint32_t)w);
    vm_wr32(v, ea + 4, (uint32_t)(w >> 32));
}

/* the 80-bit format by its fields, so an aarch64 host reads it too */
static long double rd_f80(vm *v, uint32_t ea)
{
    uint64_t mant = (uint64_t)vm_rd32(v, ea) | ((uint64_t)vm_rd32(v, ea + 4) << 32);
    uint32_t se = vm_rd8(v, ea + 8) | ((uint32_t)vm_rd8(v, ea + 9) << 8);
    int e = (int)(se & 0x7FFF);
    long double x;
    if (e == 0x7FFF) x = (mant << 1) ? NAN : INFINITY;
    else x = ldexpl((long double)mant, (e ? e : 1) - 16383 - 63);
    return (se & 0x8000u) ? -x : x;
}

static void wr_f80(vm *v, uint32_t ea, long double x)
{
    uint32_t se = signbit(x) ? 0x8000u : 0;
    uint64_t mant = 0;
    int i;
    x = fabsl(x);
    if (isnan(x))      { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(x)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (x != 0) {
        int e;
        long double f = frexpl(x, &e);       /* x = f 2^e, f in [0.5, 1) */
        int be = e - 1 + 16383;
        if (be <= 0) { mant = (uint64_t)ldexpl(x, 16382 + 63); be = 0; }
        else mant = (uint64_t)ldexpl(f, 64);
        se |= (uint32_t)be;
    }
    for (i = 0; i < 8; i++) vm_wr8(v, ea + (uint32_t)i, (uint8_t)(mant >> (8 * i)));
    vm_wr8(v, ea + 8, (uint8_t)se);
    vm_wr8(v, ea + 9, (uint8_t)(se >> 8));
}

/* FIST: the rounded value, or the integer indefinite when it will not fit */
static int64_t fist(const vm *v, long double x, int bits)
{
    long double lim = ldexpl(1.0L, bits - 1);
    long double r = fround(v, x);
    if (isnan(r) || r < -lim || r >= lim) return bits == 64 ? INT64_MIN : -(int64_t)(lim);
    return (int64_t)r;
}

static int fpu_op(vm *v, uint32_t op, uint32_t at)
{
    modrm m;
    long double a, b;
    int r, i;
    decode(v, &m);
    r = m.reg;
    i = m.rm;
    if (!m.is_reg) {
        uint32_t ea = m.ea;
        if (op == 0xD8 || op == 0xDA || op == 0xDC || op == 0xDE) {
            /* the arithmetic against memory: f32, i32, f64, i16 */
            if (fneed(v, 1, at)) return -1;
            b = op == 0xD8 ? rd_f32(v, ea)
              : op == 0xDC ? rd_f64(v, ea)
              : op == 0xDA ? (long double)(int32_t)vm_rd32(v, ea)
              : (long double)(int16_t)(vm_rd8(v, ea) | (vm_rd8(v, ea + 1) << 8));
            if (r == 2 || r == 3) {
                fcmp(v, ST(0), b);
                if (r == 3) v->fsp--;
            } else {
                ST(0) = farith(r, ST(0), b);
            }
            return 0;
        }
        switch (op << 4 | (uint32_t)r) {
        case 0xD90: return fpush(v, rd_f32(v, ea), at);
        case 0xDD0: return fpush(v, rd_f64(v, ea), at);
        case 0xDB5: return fpush(v, rd_f80(v, ea), at);
        case 0xDB0: return fpush(v, (long double)(int32_t)vm_rd32(v, ea), at);
        case 0xDF0: return fpush(v, (long double)(int16_t)(vm_rd8(v, ea) | (vm_rd8(v, ea + 1) << 8)), at);
        case 0xDF5: return fpush(v, (long double)(int64_t)((uint64_t)vm_rd32(v, ea)
                                  | ((uint64_t)vm_rd32(v, ea + 4) << 32)), at);
        case 0xD92: case 0xD93: {
            float f;
            uint32_t w;
            if (fneed(v, 1, at)) return -1;
            f = (float)ST(0);
            memcpy(&w, &f, 4);
            vm_wr32(v, ea, w);
            if (r == 3) v->fsp--;
            return 0;
        }
        case 0xDD2: case 0xDD3:
            if (fneed(v, 1, at)) return -1;
            wr_f64(v, ea, ST(0));
            if (r == 3) v->fsp--;
            return 0;
        case 0xDB7:
            if (fneed(v, 1, at)) return -1;
            wr_f80(v, ea, ST(0));
            v->fsp--;
            return 0;
        case 0xDB2: case 0xDB3:
            if (fneed(v, 1, at)) return -1;
            vm_wr32(v, ea, (uint32_t)fist(v, ST(0), 32));
            if (r == 3) v->fsp--;
            return 0;
        case 0xDF2: case 0xDF3: {
            uint32_t w;
            if (fneed(v, 1, at)) return -1;
            w = (uint32_t)fist(v, ST(0), 16);
            vm_wr8(v, ea, (uint8_t)w);
            vm_wr8(v, ea + 1, (uint8_t)(w >> 8));
            if (r == 3) v->fsp--;
            return 0;
        }
        case 0xDF7: {
            uint64_t w;
            if (fneed(v, 1, at)) return -1;
            w = (uint64_t)fist(v, ST(0), 64);
            vm_wr32(v, ea, (uint32_t)w);
            vm_wr32(v, ea + 4, (uint32_t)(w >> 32));
            v->fsp--;
            return 0;
        }
        case 0xD95:                                          /* fldcw */
            v->fcw = vm_rd8(v, ea) | ((uint32_t)vm_rd8(v, ea + 1) << 8);
            return 0;
        case 0xD97:                                          /* fnstcw */
            vm_wr8(v, ea, (uint8_t)v->fcw);
            vm_wr8(v, ea + 1, (uint8_t)(v->fcw >> 8));
            return 0;
        case 0xDD7: {                                        /* fnstsw m16 */
            uint32_t w = v->fsw_cc | ((uint32_t)((8 - v->fsp) & 7) << 11);
            vm_wr8(v, ea, (uint8_t)w);
            vm_wr8(v, ea + 1, (uint8_t)(w >> 8));
            return 0;
        }
        }
        fail(v, "unsupported x87 memory form at 0x%08X", at);
        return -1;
    }
    switch (op) {
    case 0xD8:                                   /* ST0 = ST0 op STi */
        if (fneed(v, i + 1, at)) return -1;
        if (r == 2 || r == 3) {
            fcmp(v, ST(0), ST(i));
            if (r == 3) v->fsp--;
        } else {
            ST(0) = farith(r, ST(0), ST(i));
        }
        return 0;
    case 0xDC: case 0xDE:                        /* STi = STi op ST0 [pop] */
        if (op == 0xDE && r == 3 && i == 1) {    /* fcompp */
            if (fneed(v, 2, at)) return -1;
            fcmp(v, ST(0), ST(1));
            v->fsp -= 2;
            return 0;
        }
        if (r == 2 || r == 3 || fneed(v, i + 1, at)) break;
        /* the DC/DE encodings name the reversed forms the other way round:
         * E0 is ST(i) = ST0 - ST(i), E8 ST(i) - ST0, F0 ST0 / ST(i), F8 ST(i) / ST0 */
        ST(i) = farith(r < 4 ? r : r ^ 1, ST(i), ST(0));
        if (op == 0xDE) v->fsp--;
        return 0;
    case 0xDD:
        if (r == 0) return 0;                    /* ffree: the tag only */
        if (fneed(v, i + 1, at)) return -1;
        if (r == 2 || r == 3) {                  /* fst / fstp ST(i) */
            ST(i) = ST(0);
            if (r == 3) v->fsp--;
            return 0;
        }
        if (r == 4 || r == 5) {                  /* fucom / fucomp */
            fcmp(v, ST(0), ST(i));
            if (r == 5) v->fsp--;
            return 0;
        }
        break;
    case 0xDA:
        if (r == 5 && i == 1) {                  /* fucompp */
            if (fneed(v, 2, at)) return -1;
            fcmp(v, ST(0), ST(1));
            v->fsp -= 2;
            return 0;
        }
        break;
    case 0xDB:
        if (r == 4 && i == 2) { v->fsw_cc = 0; return 0; }               /* fnclex */
        if (r == 4 && i == 3) { v->fsp = 0; v->fsw_cc = 0; v->fcw = 0x037Fu; return 0; } /* fninit */
        break;
    case 0xDF:
        if (r == 4 && i == 0) {                  /* fnstsw ax */
            v->r[VM_EAX] = (v->r[VM_EAX] & 0xFFFF0000u) | v->fsw_cc
                         | ((uint32_t)((8 - v->fsp) & 7) << 11);
            return 0;
        }
        break;
    case 0xD9:
        if (r == 0) {                            /* fld ST(i) */
            if (fneed(v, i + 1, at)) return -1;
            return fpush(v, ST(i), at);
        }
        if (r == 1) {                            /* fxch */
            if (fneed(v, i + 1, at)) return -1;
            a = ST(0); ST(0) = ST(i); ST(i) = a;
            return 0;
        }
        if (r == 2 && i == 0) return 0;          /* fnop */
        switch (0xE0 + (r - 4) * 8 + i) {
        case 0xE8: return fpush(v, 1.0L, at);
        case 0xE9: return fpush(v, 3.3219280948873623478703194294894L, at);   /* l2t */
        case 0xEA: return fpush(v, 1.4426950408889634073599246810019L, at);   /* l2e */
        case 0xEB: return fpush(v, 3.1415926535897932384626433832795L, at);   /* pi  */
        case 0xEC: return fpush(v, 0.30102999566398119521373889472449L, at);  /* lg2 */
        case 0xED: return fpush(v, 0.69314718055994530941723212145818L, at);  /* ln2 */
        case 0xEE: return fpush(v, 0.0L, at);
        }
        if (fneed(v, 1, at)) return -1;
        switch (0xE0 + (r - 4) * 8 + i) {
        case 0xF7: v->fsp--; return 0;  /* fincstp: after FFREE ST0
                                         * (0x004AED57) it is a pop - a
                                         * bare rotation is not modelled */
        case 0xE0: ST(0) = -ST(0); return 0;
        case 0xE1: ST(0) = fabsl(ST(0)); return 0;
        case 0xE4: fcmp(v, ST(0), 0.0L); return 0;            /* ftst */
        case 0xF0: ST(0) = x_f2xm1(ST(0)); return 0;          /* f2xm1 */
        case 0xFA: ST(0) = sqrtl(ST(0)); return 0;
        case 0xFC: ST(0) = fround(v, ST(0)); return 0;        /* frndint */
        case 0xFE: ST(0) = x87_sin(ST(0)); v->fsw_cc &= ~0x400u; return 0;
        case 0xFF: ST(0) = x87_cos(ST(0)); v->fsw_cc &= ~0x400u; return 0;
        case 0xF2:                                            /* fptan */
            ST(0) = x_tan(ST(0));
            v->fsw_cc &= ~0x400u;
            return fpush(v, 1.0L, at);
        case 0xFB:                                            /* fsincos */
            a = ST(0);
            ST(0) = x87_sin(a);
            v->fsw_cc &= ~0x400u;
            return fpush(v, x87_cos(a), at);
        }
        if (fneed(v, 2, at)) return -1;
        switch (0xE0 + (r - 4) * 8 + i) {
        case 0xF1: ST(1) = x_yl2x(ST(1), ST(0)); v->fsp--; return 0;  /* fyl2x */
        case 0xF3: ST(1) = atan2l(ST(1), ST(0)); v->fsp--; return 0;  /* fpatan */
        case 0xFD: ST(0) = ldexpl(ST(0), (int)truncl(ST(1))); return 0; /* fscale */
        case 0xF8: case 0xF5:                   /* fprem / fprem1 */
            ST(0) = x_prem(ST(0), ST(1), r == 6, &v->fsw_cc);
            return 0;
        }
        break;
    }
    fail(v, "unsupported x87 register form at 0x%08X", at);
    return -1;
}

/* fpu_op under the control word's rounding: the host's long double
 * arithmetic, its float and double conversions and its native fsin are
 * rounded by the host's mode, so it is set for the one instruction and put
 * back - a hook runs under the port's own mode, never the game's. */
static int fpu(vm *v, uint32_t op, uint32_t at)
{
    static const int mode[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD,
                                 FE_TOWARDZERO };
    int rc = (int)((v->fcw >> 10) & 3), was, r;
    if (rc == 0) return fpu_op(v, op, at);
    was = fegetround();
    fesetround(mode[rc]);
    r = fpu_op(v, op, at);
    fesetround(was);
    return r;
}

#undef ST

/* One instruction.  Returns 0, or -1 with the error set. */
static int step(vm *v)
{
    uint32_t at = v->eip, op = fetch8(v), x, y;
    modrm m;
    if (op >= 0x50 && op <= 0x57) { push(v, v->r[op - 0x50]); return 0; }
    if (op >= 0x58 && op <= 0x5F) { v->r[op - 0x58] = pop(v); return 0; }
    if (op >= 0x40 && op <= 0x4F) {                      /* inc / dec r32 */
        int c = v->cf, r = (int)(op & 7);
        v->r[r] = alu(v, op < 0x48 ? 0 : 5, v->r[r], 1, 32);
        v->cf = c;
        return 0;
    }
    if (op >= 0xB8 && op <= 0xBF) { v->r[op - 0xB8] = fetch32(v); return 0; }
    if (op >= 0x91 && op <= 0x97) {                      /* xchg eax, r32 */
        x = v->r[VM_EAX]; v->r[VM_EAX] = v->r[op - 0x90]; v->r[op - 0x90] = x;
        return 0;
    }
    if (op >= 0xB0 && op <= 0xB7) {                      /* mov r8, imm8 */
        modrm d = { (int)(op - 0xB0), 1, (int)(op - 0xB0), 0 };
        put(v, &d, 8, fetch8(v));
        return 0;
    }
    if ((op & 0xC7) == 0x02) {                           /* op r8, r/m8 */
        decode(v, &m); { modrm d = { m.reg, 1, m.reg, 0 };
        x = alu(v, (int)(op >> 3), get(v, &d, 8), get(v, &m, 8), 8);
        if (op != 0x3A) put(v, &d, 8, x); } return 0;
    }
    if (op >= 0x70 && op <= 0x7F) {
        int8_t d = (int8_t)fetch8(v);
        if (cond(v, (int)(op & 15))) v->eip += (uint32_t)(int32_t)d;
        return 0;
    }
    switch (op) {
    case 0x60: {                                         /* pushal */
        uint32_t sp = v->r[VM_ESP];
        int i;
        for (i = 0; i < 8; i++) push(v, i == VM_ESP ? sp : v->r[i]);
        return 0;
    }
    case 0x61: {                                         /* popal */
        int i;
        for (i = 7; i >= 0; i--) { x = pop(v); if (i != VM_ESP) v->r[i] = x; }
        return 0;
    }
    case 0x68: push(v, fetch32(v)); return 0;
    case 0x6A: push(v, (uint32_t)(int32_t)(int8_t)fetch8(v)); return 0;
    case 0x69: case 0x6B: {                              /* imul r, r/m, imm */
        int64_t p;
        decode(v, &m);
        y = op == 0x69 ? fetch32(v) : (uint32_t)(int32_t)(int8_t)fetch8(v);
        p = (int64_t)(int32_t)get(v, &m, 32) * (int32_t)y;
        v->r[m.reg] = (uint32_t)p;
        v->cf = v->of = p != (int64_t)(int32_t)p;
        return 0;
    }
    case 0xE8: x = fetch32(v); do_call(v, v->eip + x); return 0;
    case 0xE9: x = fetch32(v); v->eip += x; return 0;
    case 0xEB: v->eip += (uint32_t)(int32_t)(int8_t)fetch8(v); return 0;
    case 0xC3: v->eip = pop(v); return 0;
    case 0xD7:                                           /* xlatb */
        v->r[VM_EAX] = (v->r[VM_EAX] & ~0xFFu) |
                       vm_rd8(v, v->r[VM_EBX] + (v->r[VM_EAX] & 0xFFu));
        return 0;
    case 0xF8: v->cf = 0; return 0;
    case 0xF9: v->cf = 1; return 0;
    case 0xA1: v->r[VM_EAX] = vm_rd32(v, fetch32(v)); return 0;
    case 0xA3: vm_wr32(v, fetch32(v), v->r[VM_EAX]); return 0;
    case 0xA0: x = fetch32(v);                          /* AL moffs */
        v->r[VM_EAX] = (v->r[VM_EAX] & ~0xFFu) | vm_rd8(v, x); return 0;
    case 0xA2: vm_wr8(v, fetch32(v), (uint8_t)v->r[VM_EAX]); return 0;
    case 0xFC: return 0;               /* CLD: the string ops run DF = 0 */
    case 0x9B: return 0;               /* WAIT: the exceptions are masked */
    case 0x90: return 0;               /* NOP (0x004B0349)                */
    case 0x8B: decode(v, &m); v->r[m.reg] = get(v, &m, 32); return 0;
    case 0x89: decode(v, &m); put(v, &m, 32, v->r[m.reg]); return 0;
    case 0x8A: decode(v, &m); { modrm d = { m.reg, 1, m.reg, 0 };
               put(v, &d, 8, get(v, &m, 8)); } return 0;
    case 0x88: decode(v, &m); { modrm s = { m.reg, 1, m.reg, 0 };
               put(v, &m, 8, get(v, &s, 8)); } return 0;
    case 0x8D: decode(v, &m); v->r[m.reg] = m.ea; return 0;
    case 0x87: decode(v, &m); x = get(v, &m, 32);       /* xchg */
               put(v, &m, 32, v->r[m.reg]); v->r[m.reg] = x; return 0;
    case 0xC7: decode(v, &m); put(v, &m, 32, fetch32(v)); return 0;
    case 0xC6: decode(v, &m); put(v, &m, 8, fetch8(v)); return 0;
    case 0x8F: decode(v, &m); put(v, &m, 32, pop(v)); return 0;
    case 0x81: case 0x83: case 0x80:
        decode(v, &m);
        y = op == 0x81 ? fetch32(v)
          : op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(v) : fetch8(v);
        x = alu(v, m.reg, get(v, &m, op == 0x80 ? 8 : 32), y, op == 0x80 ? 8 : 32);
        if (m.reg != 7) put(v, &m, op == 0x80 ? 8 : 32, x);
        return 0;
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
    case 0x30: case 0x38:                                /* op r/m8, r8  */
        decode(v, &m); { modrm s = { m.reg, 1, m.reg, 0 };
        x = alu(v, (int)(op >> 3), get(v, &m, 8), get(v, &s, 8), 8);
        if (op != 0x38) put(v, &m, 8, x); } return 0;
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29:
    case 0x31: case 0x39:                                /* op r/m, r    */
        decode(v, &m);
        x = alu(v, (int)(op >> 3), get(v, &m, 32), v->r[m.reg], 32);
        if (op != 0x39) put(v, &m, 32, x);
        return 0;
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B:
    case 0x33: case 0x3B:                                /* op r, r/m    */
        decode(v, &m);
        x = alu(v, (int)(op >> 3), v->r[m.reg], get(v, &m, 32), 32);
        if (op != 0x3B) v->r[m.reg] = x;
        return 0;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D:
    case 0x35: case 0x3D:                                /* op eax, imm32 */
        x = alu(v, (int)(op >> 3), v->r[VM_EAX], fetch32(v), 32);
        if (op != 0x3D) v->r[VM_EAX] = x;
        return 0;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C:
    case 0x34: case 0x3C:                                /* op al, imm8   */
        x = alu(v, (int)(op >> 3), v->r[VM_EAX] & 0xFFu, fetch8(v), 8);
        if (op != 0x3C) v->r[VM_EAX] = (v->r[VM_EAX] & ~0xFFu) | (x & 0xFFu);
        return 0;
    case 0x84: decode(v, &m); { modrm s = { m.reg, 1, m.reg, 0 };
               alu(v, 4, get(v, &m, 8), get(v, &s, 8), 8); } return 0;
    case 0x85: decode(v, &m); alu(v, 4, get(v, &m, 32), v->r[m.reg], 32); return 0;
    case 0xA4:                                           /* MOVSB, DF = 0 */
        vm_wr8(v, v->r[VM_EDI], vm_rd8(v, v->r[VM_ESI]));
        v->r[VM_ESI]++; v->r[VM_EDI]++;
        return 0;
    case 0xA5:                                           /* MOVSD, DF = 0 */
        vm_wr32(v, v->r[VM_EDI], vm_rd32(v, v->r[VM_ESI]));
        v->r[VM_ESI] += 4; v->r[VM_EDI] += 4;
        return 0;
    case 0xA8: alu(v, 4, v->r[VM_EAX] & 0xFF, fetch8(v), 8); return 0;
    case 0xA9: alu(v, 4, v->r[VM_EAX], fetch32(v), 32); return 0;   
    case 0xF6: case 0xF7: {
        int bits = op == 0xF6 ? 8 : 32;
        decode(v, &m);
        switch (m.reg) {
        case 0: y = bits == 8 ? fetch8(v) : fetch32(v);
                alu(v, 4, get(v, &m, bits), y, bits); return 0;
        case 2: put(v, &m, bits, ~get(v, &m, bits)); return 0;
        case 3: x = get(v, &m, bits);
                put(v, &m, bits, alu(v, 5, 0, x, bits)); v->cf = x != 0; return 0;
        case 4: if (bits != 32) break;
                { uint64_t p = (uint64_t)v->r[VM_EAX] * get(v, &m, 32);
                  v->r[VM_EAX] = (uint32_t)p; v->r[VM_EDX] = (uint32_t)(p >> 32);
                  v->cf = v->of = v->r[VM_EDX] != 0; } return 0;
        case 5: if (bits != 32) break;
                { int64_t p = (int64_t)(int32_t)v->r[VM_EAX] * (int32_t)get(v, &m, 32);
                  v->r[VM_EAX] = (uint32_t)p; v->r[VM_EDX] = (uint32_t)((uint64_t)p >> 32);
                  v->cf = v->of = p != (int64_t)(int32_t)p; } return 0;
        case 6: if (bits != 32) break;
                y = get(v, &m, 32);
                if (!y) { fail(v, "divide by zero at 0x%08X", at); return -1; }
                { uint64_t n = ((uint64_t)v->r[VM_EDX] << 32) | v->r[VM_EAX];
                  v->r[VM_EAX] = (uint32_t)(n / y);
                  v->r[VM_EDX] = (uint32_t)(n % y); } return 0;
        case 7: if (bits != 32) break;
                y = get(v, &m, 32);
                if (!y) { fail(v, "divide by zero at 0x%08X", at); return -1; }
                { int64_t n = (int64_t)(((uint64_t)v->r[VM_EDX] << 32) | v->r[VM_EAX]);
                  v->r[VM_EAX] = (uint32_t)(n / (int32_t)y);
                  v->r[VM_EDX] = (uint32_t)(n % (int32_t)y); } return 0;
        }
        break;
    }
    case 0xFE:
        /* INC / DEC r/m8 - 0x004020E4's `DEC DL` (the hex digits' loop, run by
         * 0x00402666: the best-times page's key string).  CF is kept. */
        decode(v, &m);
        if (m.reg == 0 || m.reg == 1) {
            int c = v->cf;
            put(v, &m, 8, alu(v, m.reg ? 5 : 0, get(v, &m, 8), 1, 8));
            v->cf = c;
            return 0;
        }
        break;
    case 0xFF:
        decode(v, &m);
        if (m.reg == 0 || m.reg == 1) {                  /* inc/dec r/m  */
            int c = v->cf;
            put(v, &m, 32, alu(v, m.reg ? 5 : 0, get(v, &m, 32), 1, 32));
            v->cf = c;
            return 0;
        }
        if (m.reg == 6) { push(v, get(v, &m, 32)); return 0; }
        if (m.reg == 2) { do_call(v, get(v, &m, 32)); return 0; }
        break;
    case 0xD0:
        /* SHR r/m8,1 - the warnings' `SHR AH,1` after FNSTSW, which
         * puts C0 in CF */
        decode(v, &m);
        if (m.reg != 5) break;
        x = get(v, &m, 8);
        v->cf = (int)(x & 1u);
        v->of = (int)((x >> 7) & 1u);
        x >>= 1;
        v->zf = x == 0;
        v->sf = 0;
        put(v, &m, 8, x);
        return 0;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        return fpu(v, op, at);
    case 0xC1: case 0xD1: case 0xD3: {
        /* D3, the count in CL - FUN_0043F7C3's ROL EAX,CL (the parts'
         * roll) and 0x0043F7A8's SHL EAX,CL */
        int n;
        decode(v, &m);
        n = op == 0xC1 ? (int)(fetch8(v) & 31)
          : op == 0xD3 ? (int)(v->r[VM_ECX] & 31u) : 1;
        x = get(v, &m, 32);
        if (n && (m.reg == 0 || m.reg == 1)) {
            /* ROL / ROR (the RNG 0x004018B4's ROR EAX,1) - CF is the
             * bit rotated round, OF for a count of 1; ZF and SF unchanged */
            if (m.reg == 0) { x = (x << n) | (x >> (32 - n)); v->cf = (int)(x & 1u);
                              v->of = (int)((x >> 31) ^ (x & 1u)); }
            else            { x = (x >> n) | (x << (32 - n)); v->cf = (int)(x >> 31);
                              v->of = (int)(((x >> 31) ^ (x >> 30)) & 1u); }
            put(v, &m, 32, x);
            return 0;
        }
        if (n && (m.reg == 2 || m.reg == 3)) {
            /* RCL / RCR through CF, a bit at a time (program
             * controller's 0x004666B6: CMP / RCL dword [m],1 packs each
             * axis's test into a bit); OF for a count of 1, ZF and SF
             * unchanged */
            int i;
            for (i = 0; i < n; i++) {
                uint32_t c = (uint32_t)v->cf;
                if (m.reg == 2) { v->cf = (int)(x >> 31); x = (x << 1) | c; }
                else            { v->cf = (int)(x & 1u); x = (x >> 1) | (c << 31); }
            }
            v->of = m.reg == 2 ? (int)((x >> 31) ^ (uint32_t)v->cf)
                               : (int)(((x >> 31) ^ (x >> 30)) & 1u);
            put(v, &m, 32, x);
            return 0;
        }
        if (n) {
            switch (m.reg) {
            case 4: v->cf = (int)((x >> (32 - n)) & 1); x <<= n; break;
            case 5: v->cf = (int)((x >> (n - 1)) & 1); x >>= n; break;
            case 7: v->cf = (int)(((int32_t)x >> (n - 1)) & 1);
                    x = (uint32_t)((int32_t)x >> n); break;
            default: fail(v, "shift form at 0x%08X", at); return -1;
            }
            szf(v, x, 32);
            put(v, &m, 32, x);
        }
        return 0;
    }

    case 0x66:                                           /* operand size  */
        /* the one 16-bit form the carousel draw uses, 0x0048C3D6's
         * `SHL AX,3` (66 C1 E0 03) - the yaw is the clock << 3 in a WORD, so
         * EAX's upper half is left as it was.  Register operands only. */
        op = fetch8(v);
        if (op == 0x81 || op == 0x83) {
            /* the ALU group on a WORD - the config parser's
             * `CMP word [EDX],0x7830` ("0x", 0x004113E8) */
            decode(v, &m);
            if (op == 0x81) { y = fetch8(v); y |= fetch8(v) << 8; }
            else y = (uint32_t)(int32_t)(int8_t)fetch8(v) & 0xFFFFu;
            x = m.is_reg ? v->r[m.rm] & 0xFFFFu
                         : (uint32_t)vm_rd8(v, m.ea) | ((uint32_t)vm_rd8(v, m.ea + 1) << 8);
            x = alu(v, m.reg, x, y, 16);
            if (m.reg != 7) {
                if (m.is_reg) v->r[m.rm] = (v->r[m.rm] & 0xFFFF0000u) | (x & 0xFFFFu);
                else { vm_wr8(v, m.ea, (uint8_t)x); vm_wr8(v, m.ea + 1, (uint8_t)(x >> 8)); }
            }
            return 0;
        }
        if (op == 0xC7) {
            /* `MOV word [r/m], imm16` - CAR SPECIFICATIONS' line break,
             * 0x00401F0C (66 C7 03 0D 0A: CR LF at EBX) */
            decode(v, &m);
            if (m.reg != 0) { fail(v, "66 C7 form at 0x%08X", at); return -1; }
            y = fetch8(v); y |= fetch8(v) << 8;
            if (m.is_reg) v->r[m.rm] = (v->r[m.rm] & 0xFFFF0000u) | (y & 0xFFFFu);
            else { vm_wr8(v, m.ea, (uint8_t)y); vm_wr8(v, m.ea + 1, (uint8_t)(y >> 8)); }
            return 0;
        }
        if (op != 0xC1) break;
        decode(v, &m);
        if (!m.is_reg || m.reg != 4) break;
        {
            int n = (int)(fetch8(v) & 31);
            x = v->r[m.rm] & 0xFFFFu;
            if (n) {
                v->cf = n <= 16 ? (int)((x >> (16 - n)) & 1) : 0;
                x = (x << n) & 0xFFFFu;
                v->zf = x == 0;
                v->sf = (int)((x >> 15) & 1);
                v->r[m.rm] = (v->r[m.rm] & 0xFFFF0000u) | x;
            }
        }
        return 0;
    case 0xF3: {                                         /* rep stos / movs */
        uint32_t o2 = fetch8(v);
        if (o2 == 0xAB) {
            while (v->r[VM_ECX]) {
                vm_wr32(v, v->r[VM_EDI], v->r[VM_EAX]);
                v->r[VM_EDI] += 4; v->r[VM_ECX]--;
            }
            return 0;
        }
        if (o2 == 0xAA) {                                /* rep stosb */
            while (v->r[VM_ECX]) {
                vm_wr8(v, v->r[VM_EDI], (uint8_t)v->r[VM_EAX]);
                v->r[VM_EDI]++; v->r[VM_ECX]--;
            }
            return 0;
        }
        if (o2 == 0xA5) {                                /* rep movsd */
            while (v->r[VM_ECX]) {
                vm_wr32(v, v->r[VM_EDI], vm_rd32(v, v->r[VM_ESI]));
                v->r[VM_EDI] += 4; v->r[VM_ESI] += 4; v->r[VM_ECX]--;
            }
            return 0;
        }
        if (o2 == 0xA4) {
            while (v->r[VM_ECX]) {
                vm_wr8(v, v->r[VM_EDI], vm_rd8(v, v->r[VM_ESI]));
                v->r[VM_EDI]++; v->r[VM_ESI]++; v->r[VM_ECX]--;
            }
            return 0;
        }
        fail(v, "unsupported rep opcode at 0x%08X", at);
        return -1;
    }
    case 0x0F:
        op = fetch8(v);
        if (op >= 0x80 && op <= 0x8F) {
            x = fetch32(v);
            if (cond(v, (int)(op & 15))) v->eip += x;
            return 0;
        }
        if (op >= 0x90 && op <= 0x9F) {
            decode(v, &m); put(v, &m, 8, cond(v, (int)(op & 15))); return 0;
        }
        if (op == 0xAF) {
            int64_t p;
            decode(v, &m);
            p = (int64_t)(int32_t)v->r[m.reg] * (int32_t)get(v, &m, 32);
            v->r[m.reg] = (uint32_t)p;
            v->cf = v->of = p != (int64_t)(int32_t)p;
            return 0;
        }
        if (op == 0xB6) {
            decode(v, &m); v->r[m.reg] = get(v, &m, 8); return 0;
        }
        /* SHLD / SHRD r/m32, r32, imm8 (the best-times cipher) */
        if (op == 0xA4 || op == 0xAC) {
            uint32_t d, s, c;
            decode(v, &m);
            c = fetch8(v) & 31u;
            if (!c) return 0;
            d = get(v, &m, 32);
            s = v->r[m.reg];
            if (op == 0xA4) {
                v->cf = (int)((d >> (32 - c)) & 1u);
                d = (d << c) | (s >> (32 - c));
            } else {
                v->cf = (int)((d >> (c - 1)) & 1u);
                d = (d >> c) | (s << (32 - c));
            }
            v->zf = d == 0;
            v->sf = (int)(d >> 31);
            put(v, &m, 32, d);
            return 0;
        }
        /* BT / BTS / BTR / BTC - 0F BA /4../7 ib, and 0F A3 / AB / B3
         * / BB with the bit in a register (a bit string: a memory operand
         * moves by whole dwords, signed) */
        if (op == 0xBA || op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB) {
            int kind;
            uint32_t bit, w;
            decode(v, &m);
            if (op == 0xBA) {
                if (m.reg < 4) { fail(v, "unsupported 0F BA /0../3 at 0x%08X", at); return -1; }
                kind = m.reg - 4;
                bit = fetch8(v) & 31u;
            } else {
                int32_t off = (int32_t)v->r[m.reg];
                kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
                if (!m.is_reg) m.ea += (uint32_t)((off >> 5) * 4);
                bit = (uint32_t)off & 31u;
            }
            w = get(v, &m, 32);
            v->cf = (int)((w >> bit) & 1u);
            if (kind == 1) put(v, &m, 32, w | (1u << bit));
            else if (kind == 2) put(v, &m, 32, w & ~(1u << bit));
            else if (kind == 3) put(v, &m, 32, w ^ (1u << bit));
            return 0;
        }
        if (op == 0xB7) {                                /* movzx r32, r/m16 */
            decode(v, &m);
            v->r[m.reg] = m.is_reg ? v->r[m.rm] & 0xFFFFu
                                   : (uint32_t)vm_rd8(v, m.ea) | ((uint32_t)vm_rd8(v, m.ea + 1) << 8);
            return 0;
        }
        fail(v, "unsupported opcode 0F xx at 0x%08X", at);
        return -1;
    }
    fail(v, "unsupported opcode at 0x%08X", at);
    return -1;
}

/* RALLY_VM_STATS=<file> appends, at exit, every entry point the game
 * ran (address, calls, instructions) - the inventory of what is still
 * interpreted. */
static struct { uint32_t va; unsigned long calls; uint64_t steps; } vstat[256];
static int nvstat;

static void vstat_dump(void)
{
    const char *path = getenv("RALLY_VM_STATS");
    FILE *f = path ? fopen(path, "a") : NULL;
    int i;
    if (!f) return;
    for (i = 0; i < nvstat; i++)
        fprintf(f, "0x%08X %lu %llu\n", vstat[i].va, vstat[i].calls,
                (unsigned long long)vstat[i].steps);
    fclose(f);
}

static void vstat_add(uint32_t va, uint64_t steps)
{
    int i;
    for (i = 0; i < nvstat; i++)
        if (vstat[i].va == va) break;
    if (i == nvstat) {
        if (nvstat == 256) return;
        if (nvstat == 0 && getenv("RALLY_VM_STATS")) atexit(vstat_dump);
        vstat[nvstat++].va = va;
    }
    vstat[i].calls++;
    vstat[i].steps += steps;
}

int vm_call(vm *v, uint32_t va, const uint32_t regs[8])
{
    long n = 0;
    int rc = 0;
    /* RALLY_VM_TRAP names the original's code the game is about
     * to INTERPRET.  "abort" stops the process at the first one (the proof that
     * nothing runs interpreted); any other value logs each address to stderr
     * and goes on.  Hooks never reach here, so a handler answered in C is not
     * named. */
    {
        static int trap = -1;
        if (trap < 0) {
            const char *t = getenv("RALLY_VM_TRAP");
            trap = !t || !*t ? 0 : !strcmp(t, "abort") ? 2 : 1;
        }
        if (trap) {
            fprintf(stderr, "VM-TRAP: interpreting 0x%08X\n", va);
            fflush(stderr);
            if (trap == 2) exit(97);
        }
    }
    v->err[0] = 0;
    if (regs) memcpy(v->r, regs, sizeof v->r); else memset(v->r, 0, sizeof v->r);
    v->r[VM_ESP] = STACK_HI - 64;
    push(v, SENTINEL);
    v->eip = va;
    v->cf = v->zf = v->sf = v->of = 0;
    v->fsp = 0;
    while (v->eip != SENTINEL) {
        if (++n > BUDGET) { fail(v, "step budget exhausted near 0x%08X", v->eip); rc = -1; break; }        if (step(v) != 0 || v->err[0]) { rc = -1; break; }
    }
    v->steps += (uint64_t)n;
    vstat_add(va, (uint64_t)n);
    return rc;
}
