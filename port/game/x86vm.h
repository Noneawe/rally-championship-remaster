/* x86vm.h - a small interpreter for the original's STRAIGHT-LINE DRAW CODE.
 *
 * The car setup screen (and RESULTS) are drawn by
 * hundreds of instructions that are nothing but constants: `MOV ECX,x / MOV
 * EDX,y / MOV EAX,token / LEA EBX,[font] / CALL draw_tok`, sprite blits, and
 * the arithmetic that places a slider's marker.  Typing 150 such calls into
 * the port would copy the original's layout by hand and get it wrong in the
 * one place nobody diffs; reading them out of the user's own RAL.EXE at run
 * time is what `frontend.c` already does for the tables and the font wiring
 * (load_fonts parses FUN_004150C9's own code).  This is the same idea, taken
 * one step further: the draw routines are RUN, over a mutable copy of the
 * image's data, and every call they make that leaves the draw layer is a HOOK
 * that the port answers natively.
 *
 * WHAT THIS IS NOT.  It is not an emulator of the game.  It runs only the
 * code the port names (the post-draw handlers of a screen), only the integer
 * subset those use, and it STOPS WITH AN ERROR on the first instruction it
 * does not know - never a guess.  The screens' LOGIC (what a key does, what
 * a panel costs) is ported in C, writing the same globals at the same
 * addresses, so the interpreted draw code reads the state the port keeps.
 * Nothing the interpreter runs is copied into the repo.
 *
 * THE TWO HALVES.  vmstore.c is the data copy (vm_new / vm_rd* / vm_wr* / vm_cstr / vm_mem_*) and the hook
 * table (vm_hook / vm_invoke, the flags a service leaves: vm_cf / vm_set_cf) - no instruction is decoded there.  x86vm.c
 * is the interpreter (vm_call / vm_error / vm_steps / vm_regs, RALLY_VM_STATS, RALLY_VM_TRAP) and includes vmstore.c.
 * The shipped game (rally, rally.exe, the aarch64 build) links vmstore.c ONLY; the gates link x86vm.c.  Code that is
 * built into the shipped game must not call vm_call, vm_error, vm_steps or vm_regs (frontend.c goes through its
 * orig_call / orig_error / orig_regs, the interpreter's own under -DRALLY_INTERP, a refusal without).
*/
#ifndef RALLY_GAME_X86VM_H
#define RALLY_GAME_X86VM_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"

typedef struct vm vm;

/* A hook stands in for a function at `va`.  It reads its arguments from
 * `r` (EAX..EDI order: eax ecx edx ebx esp ebp esi edi), may write them back
 * (a function that returns a value in EAX, or advances EBX), and the VM then
 * behaves as if the function had returned. */
typedef void (*vm_hook_fn)(vm *v, uint32_t r[8], void *ud);

enum { VM_EAX, VM_ECX, VM_EDX, VM_EBX, VM_ESP, VM_EBP, VM_ESI, VM_EDI };

vm  *vm_new(const ral_image *im);
void vm_free(vm *v);

/* The mutable data.  Reads outside the image and the data copy return 0 and
 * set the error. */
uint32_t vm_rd32(vm *v, uint32_t va);
uint8_t  vm_rd8(vm *v, uint32_t va);
void     vm_wr32(vm *v, uint32_t va, uint32_t x);
void     vm_wr8(vm *v, uint32_t va, uint8_t x);
/* A NUL-terminated string from VM memory. */
const char *vm_cstr(vm *v, uint32_t va);

void vm_hook(vm *v, uint32_t va, vm_hook_fn fn, void *ud);
/* the function at `va` runs as the original's code again. */
void vm_unhook(vm *v, uint32_t va);

/* the hook standing in for `va` run directly, as a CALL to it would
 * (vm_last_call is `va` inside it); `r` in and out, the flags it sets are
 * the VM's (vm_cf).  0 when nothing is hooked there (nothing runs).  For
 * the C that replaces an interpreted caller of a hooked service. */
int vm_invoke(vm *v, uint32_t va, uint32_t r[8]);
int vm_cf(const vm *v);
int vm_zf(const vm *v);   /* the zero flag, for a native twin of a service that sets it */

/* The newest CALL's target - inside a hook, the address it stands in for
 * (one hook for several screen bodies). */
uint32_t vm_last_call(const vm *v);
/* The newest CALL's own address (a hook shared by several callers
 * tells them apart). */
uint32_t vm_last_site(const vm *v);
/* The registers as the last vm_call left them (a leaf that returns in ECX). */
void vm_regs(const vm *v, uint32_t out[8]);
/* instructions interpreted since vm_new (hooks are not counted) */
uint64_t vm_steps(const vm *v);
/* the first address where two VMs' data or heap differ (the gates'
 * whole-memory compare); 0 when they are equal. */
uint32_t vm_mem_diff(const vm *a, const vm *b);
/* dst's data and heap become src's. */
void vm_mem_copy(vm *dst, const vm *src);

/* For a hook that stands in for a function whose caller tests the flags it
 * returns (FUN_00407CDC: ZF = no key). */
void vm_set_zf(vm *v, int zf);
void vm_set_cf(vm *v, int cf);   /* the file layer: CF = 1 is FAILURE (0x0040299B) */

/* Run the function at `va` to its RET with the given registers (NULL = all
 * zero).  0 on success; -1 on an unknown instruction, a wild access or the
 * step budget - vm_error() says which. */
int  vm_call(vm *v, uint32_t va, const uint32_t regs[8]);
const char *vm_error(const vm *v);

#endif
