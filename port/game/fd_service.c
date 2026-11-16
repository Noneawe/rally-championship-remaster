/* fd_service.c - see fd_service.h. family A, 2026-11-09. */
#include "fd_service.h"

#include <stddef.h>

#define R(a)    vm_rd32(v, (uint32_t)(a))
#define W(a, x) vm_wr32(v, (uint32_t)(a), (uint32_t)(x))

/* the gate's control: `brk` names a site, the first pass through it is perturbed */
static int brk;
void fds_break(int which) { brk = which; }
int fds_break_armed(void) { return brk != 0; }
static int hit(int site)
{
    if (brk != site) return 0;
    brk = 0;
    return 1;
}

/* a walk that the original would never leave (a list with no -1 in reach) */
#define WALK_LIMIT 0x100000

/* ------------------------------------------------------------------------- */
/* 0x0048C1A2: the carousel kinds                                              */
/* ------------------------------------------------------------------------- */

enum { KO_STORE, KO_FILL, KO_HAND, KO_END };
#include "fd_service_kinds.h"

/* 0x0048BB9E / 0x00049C570 / 0x0049C614's loop: the model of each car in the
 * list at `list` (a pointer to a record whose first word indexes the table
 * 0x70C24E) copied to 0x70C5E0..; returns the count, or -1 (no end) */
static int32_t list_models(vm *v, uint32_t list)
{
    uint32_t n = 0;
    while (R(list) != 0xFFFFFFFFu) {
        uint32_t rec = R(list), idx = R(rec);
        if (n >= WALK_LIMIT) return -1;
        W(0x70C5E0u + 4u * n, R(0x70C24Eu + 4u * idx));
        n++;
        list += 4;
    }
    return (int32_t)n;
}

/* the constants every list-driven kind ends with (the same stores, in the same
 * order, in 0x0048BB9E, 0x0049C570 and 0x0049C614) */
static void list_tail(vm *v, uint32_t n, uint32_t scale)
{
    W(0x70C9E4u, n);
    W(0x708E08u, n - 1u);
    W(0x5E7EC8u, scale);
    W(0x70C9E8u, 0);
    W(0x70C9ECu, 0);
    W(0x70C5CCu, 0);
    W(0x70C5D0u, 0x36BC0u);
    W(0x70C5D4u, 0xFFFF4000u);
    W(0x70C5D8u, 0x5F000u);
    W(0x70C5DCu, 0x20000u);
    W(0x5E7ECCu, 0x10FFu);
    W(0x5E7EB0u, 0xFFFE6000u);
}

static int h48C1A2(vm *v, const uint32_t r[8])
{
    uint32_t kind = R(0x70C9E0u), esi = r ? r[VM_ESI] : 0, k;
    int32_t n;
    if (kind > 0x14u) return 0;                          /* none of the 21: a bare RET */
    k = (uint32_t)FDS_KSTART[kind];
    if (FDS_KOPS[k].k == KO_HAND) {
        switch (kind) {
        case 11:                                          /* 0x0048BB9E */
            W(0x70C9F8u, 1);
            W(0x70C5C8u, 0x70C06Au);
            n = list_models(v, 0x70C06Au);
            if (n < 0) return -1;
            list_tail(v, (uint32_t)n, 0xF);
            return 0;
        case 13:                                          /* 0x0049C570: ESI the list */
            W(0x70C9F8u, 1);
            W(0x70C5C8u, esi);
            n = list_models(v, esi);
            if (n < 0) return -1;
            list_tail(v, (uint32_t)n, 0xC);
            return 0;
        default: {                                        /* 14, 0x0049C614 */
            uint32_t cnt = 0;
            W(0x70C9F8u, 1);
            W(0x70C5C8u, esi);
            do {                                          /* count the entries after the first */
                cnt++;
                esi += 4;
                if (cnt >= WALK_LIMIT) return -1;
            } while (R(esi) != 0xFFFFFFFFu);
            if (cnt == 1) {                               /* one entry: it is put in twice */
                esi = R(0x70C5C8u);
                W(esi + 4u, R(esi));
            }
            n = list_models(v, R(0x70C5C8u));
            if (n < 0) return -1;
            list_tail(v, (uint32_t)n, 0xC);
            return 0;
        }
        }
    }
    for (;; k++) {
        switch (FDS_KOPS[k].k) {
        case KO_STORE:
            W(FDS_KOPS[k].a, FDS_KOPS[k].b + (uint32_t)hit(2));
            break;
        case KO_FILL: {
            uint32_t i;
            for (i = 0; i < FDS_KOPS[k].b; i++) W(FDS_KOPS[k].a + 4u * i, FDS_KOPS[k].c);
            break;
        }
        default:
            return 0;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* the menu models' draws                                                      */
/* ------------------------------------------------------------------------- */

/* 0x00420F8A: the type whose model is `model` (the table at 0x581FC4, from 1),
 * 0 when there is none */
static uint32_t model_type(vm *v, uint32_t model)
{
    uint32_t t;
    for (t = 1; t != 0x100; t++)
        if (R(0x581FC4u + 4u * t) == model) return t;
    return 0;
}

/* the model draw the host hooks (0x0048C4AC): every word it reads is set */
static int model_draw(vm *v)
{
    uint32_t rr[8] = { 0 };
    vm_invoke(v, 0x0048C4ACu, rr);
    return 0;
}

/* the part of both draws that picks a model's overrides: the table at
 * 0x70B832, twelve bytes a model above 0x258.  `checked`: the first (0x0048BD44)
 * leaves a word alone when its entry is -1, the second (0x0048C39A) copies all */
static void model_overrides(vm *v, uint32_t model, int checked)
{
    uint32_t edi;
    if ((int32_t)model <= 0x258) return;
    edi = 0x70B832u + (model - 0x259u) * 0xCu;
    W(0x5E7EC8u, R(edi));
    if (!checked || R(edi + 4u) != 0xFFFFFFFFu) W(0x5E7EB0u, R(edi + 4u));
    if (!checked || R(edi + 8u) != 0xFFFFFFFFu) W(0x5E7ECCu, R(edi + 8u));
}

/* 0x0048BD44 */
static int f48BD44(vm *v)
{
    uint32_t eax, ebx;
    W(0x5E7ED0u, (R(0x5C6568u) << 3) & 0xFFFFu);
    W(0x5E7EB4u, R(0x70C5D8u));
    ebx = R(0x707E91u) - 1u;
    eax = R(0x70C5E0u + 4u * ebx);
    W(0x5E7EACu, 0);
    model_overrides(v, eax, 1);
    W(0x70C5C4u, eax);
    W(0x5D422Cu, model_type(v, eax));
    return model_draw(v);
}

static int h48BD34(vm *v, const uint32_t r[8])
{
    (void)r;
    W(0x5E9488u, 0);
    return f48BD44(v);
}

/* 0x0048C0D2: CAR SPECIFICATIONS' one car, turning: the model of the car
 * [0x7082FC] in the list kind 11 built, at the depth [0x70C5D8].  Its
 * x, height, scale and pitch are the kind's stores and the last draw's, which
 * is what the original draws with. */
static int f48C0D2(vm *v)
{
    uint32_t eax;
    W(0x5E7ED0u, (R(0x5C6568u) << 3) & 0xFFFFu);
    W(0x5E7EB4u, R(0x70C5D8u));
    eax = R(0x70C5E0u + (R(0x7082FCu) << 2));
    W(0x70C5C4u, eax);
    W(0x5D422Cu, model_type(v, eax));
    return model_draw(v);
}

/* 0x0048BE68: the tail of that screen's 3D draw list 0x0048BE40, entered past its
 * Z clear and residency as the others are (the host draws it on page 0 only: the
 * list's own first test, [0x708304] == 0; the pages after it are flat art) */
static int h48BE68(vm *v, const uint32_t r[8])
{
    (void)r;
    W(0x5E9488u, 0);
    return f48C0D2(v);
}

static int h48C0D2(vm *v, const uint32_t r[8]) { (void)r; return f48C0D2(v); }

/* 0x0048C39A: one model of the carousel; EBX its slot, ECX its place (-1, 0, 1, 2) */
static int f48C39A(vm *v, uint32_t ebx, uint32_t ecx)
{
    uint32_t eax, edx, ebp, hi;
    uint64_t p;
    W(0x5E7ECCu, 0);
    edx = ecx - 1u;
    eax = R(0x70C5D0u);
    p = (uint64_t)((int64_t)(int32_t)eax * (int64_t)(int32_t)edx);   /* imul edx: edx:eax */
    ebp = (uint32_t)p;
    p = (uint64_t)R(0x70C5D0u) * (uint64_t)(R(0x70C9ECu) ^ 0xFFFFFFFFu);    /* mul edx */
    hi = (uint32_t)(p >> 32);
    W(0x5E7EACu, hi + ebp);
    W(0x5E7ED0u, ((R(0x5C6568u) & 0xFFFFu) << 3) & 0xFFFFu);
    W(0x5E7EB0u, R(0x70C5D4u));
    W(0x5E7EB4u, R(0x70C5D8u));
    if (ecx == 0 || ecx == 1) {
        eax = ecx == 0 ? R(0x70C9ECu) ^ 0xFFFFFFFFu : R(0x70C9ECu);
        p = (uint64_t)eax * (uint64_t)R(0x70C5DCu);
        edx = (uint32_t)(p >> 32);
        W(0x5E7EB4u, R(0x5E7EB4u) - edx - (uint32_t)hit(4));
    }
    W(0x5D6E44u, 0);
    W(0x5D6E48u, 0);
    eax = R(0x70C5E0u + 4u * ebx);
    if ((int32_t)eax > 0x258) {
        uint32_t edi = 0x70B832u + (eax - 0x259u) * 0xCu;
        W(0x5E7EC8u, R(edi));
        W(0x5E7EB0u, R(edi + 4u));
        W(0x5E7ECCu, R(edi + 8u));
    }
    W(0x70C5C4u, eax);
    W(0x5D422Cu, model_type(v, eax));
    return model_draw(v);
}

/* 0x0048C30E: the carousel's three or four models.  `ecx` is the caller's ECX:
 * the one-model branch (0x0048C389) calls 0x0048C39A without setting it */
static int f48C30E(vm *v, uint32_t ecx)
{
    uint32_t n = R(0x70C9E4u), ebx;
    if (n == 1) return f48C39A(v, 0, ecx);
    if ((n - 1u) >> 31) return 0;                         /* js: a negative count */
    ebx = R(0x70C9E8u);
    if (ebx == 0) ebx = n;
    ebx--;
    if (f48C39A(v, ebx, 0xFFFFFFFFu)) return -1;
    ebx++;
    if (ebx == R(0x70C9E4u)) ebx = 0;
    if (f48C39A(v, ebx, 0)) return -1;
    ebx++;
    if (ebx == R(0x70C9E4u)) ebx = 0;
    if (f48C39A(v, ebx, 1)) return -1;
    if (R(0x70C9ECu) != 0) {
        ebx++;
        if (ebx == R(0x70C9E4u)) ebx = 0;
        if (f48C39A(v, ebx, 2)) return -1;
    }
    return 0;
}

static int h48BDF1(vm *v, const uint32_t r[8])
{
    W(0x5E9488u, 0);
    return f48C30E(v, r ? r[VM_ECX] : 0);
}

static int h48C30E(vm *v, const uint32_t r[8]) { return f48C30E(v, r ? r[VM_ECX] : 0); }
static int h48BD44(vm *v, const uint32_t r[8]) { (void)r; return f48BD44(v); }
static int h48C39A(vm *v, const uint32_t r[8])
{
    return f48C39A(v, r ? r[VM_EBX] : 0, r ? r[VM_ECX] : 0);
}

/* ------------------------------------------------------------------------- */
/* 0x0048FD81 and its lists: the translucent quads                              */
/* ------------------------------------------------------------------------- */

enum { K_RECT, K_STORE, K_JNE, K_JE, K_JNEM, K_JEM, K_JMP, K_CALL, K_END };
typedef struct {
    uint8_t  k;
    uint16_t t;                   /* a jump's op, or the list a CALL runs      */
    int16_t  x0, y0, x1, y1;      /* a quad's corners                          */
    uint32_t a, b;                /* a quad: colour and alpha; else address, value */
} fds_op;
#include "fd_service_rects.h"

/* a quad's colour or alpha: a literal, or (0xFnnnnnnn) the word at nnnnnnn */
static uint32_t src(vm *v, uint32_t x)
{
    return (x & 0xF0000000u) == 0xF0000000u ? R(x & 0x0FFFFFFFu) : x;
}

static int run_list(vm *v, int list, int depth)
{
    int i = FDS_START[list];
    uint32_t rr[8] = { 0 };
    for (;;) {
        const fds_op *o = &FDS_OPS[i];
        switch (o->k) {
        case K_RECT:
            W(0x5D6CA4u, (uint32_t)(int32_t)o->x0 + (uint32_t)hit(1));
            W(0x5D6CB4u, (int32_t)o->y0);
            W(0x5D6CA8u, (int32_t)o->x1);
            W(0x5D6CB8u, (int32_t)o->y1);
            W(0x5D6C7Cu, src(v, o->a));
            W(0x5D6C80u, src(v, o->b));
            vm_invoke(v, 0x004495D3u, rr);
            i++;
            break;
        case K_STORE: W(o->a, o->b); i++; break;
        case K_JNE:  i = R(o->a) != o->b ? o->t : i + 1; break;
        case K_JE:   i = R(o->a) == o->b ? o->t : i + 1; break;
        case K_JNEM: i = R(o->a) != R(o->b) ? o->t : i + 1; break;
        case K_JEM:  i = R(o->a) == R(o->b) ? o->t : i + 1; break;
        case K_JMP:  i = o->t; break;
        case K_CALL:
            if (depth) return -1;
            if (run_list(v, o->t, depth + 1)) return -1;
            i++;
            break;
        default:
            return 0;
        }
    }
}

static int h48FD81(vm *v, const uint32_t r[8]) { (void)r; return run_list(v, 0, 0); }
static int h48CAE7(vm *v, const uint32_t r[8]) { (void)r; return run_list(v, 1, 1); }
static int h48D64C(vm *v, const uint32_t r[8]) { (void)r; return run_list(v, 2, 1); }
static int h48E1EF(vm *v, const uint32_t r[8]) { (void)r; return run_list(v, 3, 1); }
static int h48EF99(vm *v, const uint32_t r[8]) { (void)r; return run_list(v, 4, 1); }

/* ------------------------------------------------------------------------- */

static const struct { uint32_t va; fds_fn fn; } TAB[] = {
    { 0x0048C1A2u, h48C1A2 }, { 0x0048BD34u, h48BD34 }, { 0x0048BDF1u, h48BDF1 },
    { 0x0048BD44u, h48BD44 }, { 0x0048C30Eu, h48C30E }, { 0x0048C39Au, h48C39A },
    { 0x0048FD81u, h48FD81 }, { 0x0048CAE7u, h48CAE7 }, { 0x0048D64Cu, h48D64C },
    { 0x0048E1EFu, h48E1EF }, { 0x0048EF99u, h48EF99 },
    { 0x0048C0D2u, h48C0D2 }, { 0x0048BE68u, h48BE68 },       
};

fds_fn fds_lookup(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return TAB[i].fn;
    return NULL;
}

int fds_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fds_va(int i) { return TAB[i].va; }
