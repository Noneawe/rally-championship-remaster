/* vmstore.c - the data copy and the hook table of x86vm.h, WITHOUT the interpreter.
 *
 * The menus' state lives at the original's own addresses in a plain array (this file's `struct vm`: the image's
 * data section copied, a heap, a stack); `vm_rd32` / `vm_wr32` / `vm_rd8` / `vm_wr8` / `vm_cstr` read and write it,
 * `vm_hook` / `vm_invoke` are the table of C functions that stand in for the original's services, and `vm_set_cf`
 * / `vm_cf` the flags a service leaves for the caller that tests them.  Nothing here decodes an instruction: the
 * shipped game (`rally`, `rally.exe`, the aarch64 build) links THIS file and not x86vm.c, so it cannot run the
 * original's code and cannot print "stopped the VM".  The gates keep x86vm.c, which includes this file first
 * (its interpreter is the oracle they compare the C against). */
#include "x86vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_LO   0x004DB000u
#define DATA_HI   0x00A2D000u
/* a heap for the blocks the original ALLOCATES - there is no static address
 * for them: frontend.c's VirtualAlloc hook
 * (0x004028FF) hands it out; 0x004787D0's nine blocks are ~0.8 MB */
#define HEAP_LO   0x00C00000u
#define HEAP_HI   0x00E00000u
#define STACK_LO  0x00B00000u
#define STACK_HI  0x00B10000u
#define SENTINEL  0xDEAD0000u
#define BUDGET    64000000L         /* the standings' bubble sorts
                                       (FUN_004A1776: 180 x 180 rows, the
                                       names swapped a byte at a time) need
                                       ~10 M; a runaway still ends */
#define MAXHOOK   128               /* the championship's take ~50 */

typedef struct { uint32_t va; vm_hook_fn fn; void *ud; } hook;

struct vm {
    const ral_image *im;
    uint8_t  *data;                 /* [DATA_LO, DATA_HI)                    */
    uint8_t  *heap;                 /* [HEAP_LO, HEAP_HI)                    */
    uint8_t   stack[STACK_HI - STACK_LO];
    uint32_t  r[8];
    uint32_t  eip;
    int       cf, zf, sf, of;
    long double fs[8];              /* the x87 stack, ST0 = fs[fsp-1]        */
    int       fsp;
    uint32_t  fsw_cc;               /* C0 C2 C3 (0x100 0x400 0x4000)  */
    uint32_t  fcw;                  /* the control word, kept across
                                     * calls as the process keeps it         */
    uint32_t  last_call, last_site;  /* the newest CALL, for the error message */
    uint32_t  ring[8][2];            /* the last eight: target, site            */
    unsigned  nring;
    hook      hk[MAXHOOK];
    int       nhk;
    char      err[480];
    char      strbuf[512];
    uint64_t  steps;                /* every instruction run, ever     */
    /* the image's raw-backed span below DATA_LO (the code), found
     * once - every fetch went through ral_image_at's section walk */
    uint32_t  code_lo, code_hi;
    const uint8_t *code;
};

static void fail(vm *v, const char *fmt, uint32_t a)
{
    if (!v->err[0]) {
        size_t n;
        snprintf(v->err, sizeof v->err, fmt, a);
        n = strlen(v->err);
        {
            unsigned k;
            snprintf(v->err + n, sizeof v->err - n, " (calls, newest first:");
            for (k = 0; k < 6 && k < v->nring; k++) {
                n = strlen(v->err);
                snprintf(v->err + n, sizeof v->err - n, " 0x%08X<-0x%08X",
                         v->ring[(v->nring - 1 - k) & 7][0], v->ring[(v->nring - 1 - k) & 7][1]);
            }
            n = strlen(v->err);
            snprintf(v->err + n, sizeof v->err - n, ")");
        }
    }
}

/* ---- memory ------------------------------------------------------------- */

static uint8_t *ptr(vm *v, uint32_t va, int write)
{
    if (va >= DATA_LO && va < DATA_HI) return v->data + (va - DATA_LO);
    if (va >= HEAP_LO && va < HEAP_HI) return v->heap + (va - HEAP_LO);
    if (va >= STACK_LO && va < STACK_HI) return v->stack + (va - STACK_LO);
    if (!write) {
        const unsigned char *p;
        if (va - v->code_lo < v->code_hi - v->code_lo)
            return (uint8_t *)(v->code + (va - v->code_lo));
        p = ral_image_at(v->im, va, 1);
        if (p) return (uint8_t *)p;
    }
    fail(v, write ? "write to unmapped/read-only 0x%08X"
                  : "read from unmapped 0x%08X", va);
    return NULL;
}

uint8_t vm_rd8(vm *v, uint32_t va)
{
    uint8_t *p = ptr(v, va, 0);
    return p ? *p : 0;
}

/* the four bytes at va when they lie in ONE region, else NULL - the
 * caller then takes the byte path, which fails at the same byte it always did */
static uint8_t *span4(vm *v, uint32_t va, int write)
{
    if (va - DATA_LO <= DATA_HI - DATA_LO - 4) return v->data + (va - DATA_LO);
    if (va - STACK_LO <= STACK_HI - STACK_LO - 4) return v->stack + (va - STACK_LO);
    if (va - HEAP_LO <= HEAP_HI - HEAP_LO - 4) return v->heap + (va - HEAP_LO);
    if (!write && v->code_hi - v->code_lo >= 4 && va - v->code_lo <= v->code_hi - v->code_lo - 4)
        return (uint8_t *)(v->code + (va - v->code_lo));
    return NULL;
}

uint32_t vm_rd32(vm *v, uint32_t va)
{
    uint32_t x = 0;
    int i;
    const uint8_t *p = span4(v, va, 0);
    if (p)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
             | ((uint32_t)p[3] << 24);
    for (i = 3; i >= 0; i--) x = (x << 8) | vm_rd8(v, va + (uint32_t)i);
    return x;
}

void vm_wr8(vm *v, uint32_t va, uint8_t x)
{
    uint8_t *p = ptr(v, va, 1);
    if (p) *p = x;
}

void vm_wr32(vm *v, uint32_t va, uint32_t x)
{
    int i;
    uint8_t *p = span4(v, va, 1);
    if (p) {
        p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
        p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
        return;
    }
    for (i = 0; i < 4; i++) vm_wr8(v, va + (uint32_t)i, (uint8_t)(x >> (8 * i)));
}

const char *vm_cstr(vm *v, uint32_t va)
{
    size_t n = 0;
    while (n + 1 < sizeof v->strbuf) {
        char c = (char)vm_rd8(v, va + (uint32_t)n);
        v->strbuf[n++] = c;
        if (!c) break;
    }
    v->strbuf[n < sizeof v->strbuf ? n : sizeof v->strbuf - 1] = 0;
    return v->strbuf;
}

vm *vm_new(const ral_image *im)
{
    vm *v = (vm *)calloc(1, sizeof *v);
    uint32_t a;
    if (!v) return NULL;
    v->im = im;
    v->fcw = 0x037Fu;               /* FNINIT's: extended, to nearest */
    v->data = (uint8_t *)calloc(1, DATA_HI - DATA_LO);
    v->heap = (uint8_t *)calloc(1, HEAP_HI - HEAP_LO);
    if (!v->data || !v->heap) { free(v->data); free(v->heap); free(v); return NULL; }
    for (a = DATA_LO; a < DATA_HI; a += 4096) {
        const unsigned char *p = ral_image_at(im, a, 4096);
        if (p) { memcpy(v->data + (a - DATA_LO), p, 4096); continue; }
        {   /* a section's tail: copy what is backed, a dword at a time */
            uint32_t b;
            for (b = a; b < a + 4096; b += 4) {
                p = ral_image_at(im, b, 4);
                if (p) memcpy(v->data + (b - DATA_LO), p, 4);
            }
        }
    }
    {   /* the code window - from .text's start, as far as the image's
         * raw bytes run on contiguously; every byte in it is the pointer
         * ral_image_at would have returned, and anything outside still asks */
        const unsigned char *p0 = ral_image_at(im, 0x00401000u, 1);
        uint32_t hi = 0x00401000u;
        if (p0) {
            while (hi + 512u <= DATA_LO && ral_image_at(im, hi, 512) == p0 + (hi - 0x00401000u))
                hi += 512u;
            while (hi < DATA_LO && ral_image_at(im, hi, 1) == p0 + (hi - 0x00401000u))
                hi++;
            v->code = p0;
            v->code_lo = 0x00401000u;
            v->code_hi = hi;
        }
    }
    return v;
}

void vm_free(vm *v)
{
    if (!v) return;
    free(v->data);
    free(v->heap);
    free(v);
}

void vm_unhook(vm *v, uint32_t va)
{
    int i;
    for (i = 0; i < v->nhk; i++)
        if (v->hk[i].va == va) {
            v->hk[i] = v->hk[--v->nhk];
            return;
        }
}

void vm_hook(vm *v, uint32_t va, vm_hook_fn fn, void *ud)
{
    int i;
    /* hooking an address again replaces its hook - the arcade's NEXT
     * STAGE hooks its best-time row every frame, which filled the table */
    for (i = 0; i < v->nhk; i++)
        if (v->hk[i].va == va) { v->hk[i].fn = fn; v->hk[i].ud = ud; return; }
    if (v->nhk < MAXHOOK) {
        v->hk[v->nhk].va = va;
        v->hk[v->nhk].fn = fn;
        v->hk[v->nhk].ud = ud;
        v->nhk++;
    }
}

void vm_set_zf(vm *v, int zf)
{
    v->zf = zf != 0;
}

void vm_set_cf(vm *v, int cf)
{
    v->cf = cf != 0;
}

uint32_t vm_last_call(const vm *v)
{
    return v->last_call;
}

uint32_t vm_last_site(const vm *v)
{
    return v->last_site;
}

void vm_mem_copy(vm *dst, const vm *src)
{
    memcpy(dst->data, src->data, DATA_HI - DATA_LO);
    memcpy(dst->heap, src->heap, HEAP_HI - HEAP_LO);
}

uint32_t vm_mem_diff(const vm *a, const vm *b)
{
    size_t i;
    for (i = 0; i < DATA_HI - DATA_LO; i++)
        if (a->data[i] != b->data[i]) return DATA_LO + (uint32_t)i;
    for (i = 0; i < HEAP_HI - HEAP_LO; i++)
        if (a->heap[i] != b->heap[i]) return HEAP_LO + (uint32_t)i;
    return 0;
}

/* RALLY_HOOK_TRACE=1: every hook the port answers, by the interpreter's CALL or
 * by a native twin (vm_invoke), with the registers a hook may read - the diff
 * of two runs is what a twin changed. */
static void hook_trace(vm *v, uint32_t va, const uint32_t r[8])
{
    static int on = -1;
    if (on < 0) { const char *t = getenv("RALLY_HOOK_TRACE"); on = t && *t ? 1 : 0; }
    if (!on) return;
    /* the registers a draw hook reads (EAX, ECX, EDX), and a text draw's string:
     * the pointer in EBX is noise, what it points at is the picture */
    fprintf(stderr, "HOOK %08X a=%08X c=%08X d=%08X", va, r[VM_EAX], r[VM_ECX], r[VM_EDX]);
    if (va == 0x0040239Cu) fprintf(stderr, " \"%s\"", vm_cstr(v, r[VM_EBX]));
    fprintf(stderr, "\n");
}

int vm_invoke(vm *v, uint32_t va, uint32_t r[8])
{
    int i;
    for (i = 0; i < v->nhk; i++)
        if (v->hk[i].va == va) {
            uint32_t esp = r[VM_ESP];
            hook_trace(v, va, r);
            v->last_call = va;
            v->last_site = 0;
            v->hk[i].fn(v, r, v->hk[i].ud);
            r[VM_ESP] = esp;
            return 1;
        }
    return 0;
}

int vm_cf(const vm *v) { return v->cf != 0; }
int vm_zf(const vm *v) { return v->zf != 0; }

