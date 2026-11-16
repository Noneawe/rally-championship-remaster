/* fe_logic.h - the menus' small state handlers, in C.
 *
 * Each function here is the original's code at the address in its name, read
 * and written as C over the same globals in the VM's data copy (the way
 * setup.c does).  frontend.c's vm_run asks fel_lookup first: a handler found
 * here runs natively and the interpreter never sees it.  fe_logic_check runs
 * the original and these on the same random state and compares all memory. */
#ifndef RALLY_GAME_FE_LOGIC_H
#define RALLY_GAME_FE_LOGIC_H

#include <stdint.h>

#include "x86vm.h"

typedef void (*fel_fn)(vm *v);

/* The draw layer a handler calls (batch 5): the two original functions every
 * screen's draw code reaches the pixels through.  The handlers pass the
 * registers' values as the original's call does and the owner draws.
 *   tok:  0x00402311  EBX the font record, EAX the token, ECX x, EDX y
 *   text: 0x0040239C  EAX the font record, EBX the string's VM address,
 *                     ECX x, EDX y */
typedef struct {
    void (*tok)(void *ud, uint32_t font, int32_t token, int x, int y);
    void (*text)(void *ud, uint32_t font, uint32_t str, int x, int y);
    /* batch 7: a platform service the handler ends in, at the original's
     * own address `va` with EAX and ECX as the call passes them (0x00409A8B the
     * overall volume, 0x0040F651 the music volume, 0x0040EE7C a sound request,
     * 0x0040F566 the narration - EAX the path, ECX the volume, 0x00410E0C the
     * clock - returns EAX, 0x00465011 the stage map, 0x00401F8F the token
     * EAX's text written at EBX - returns its length, the NUL is the handler's).
     * The registers are the ones the original's call has set.  Returns EAX. */
    uint32_t (*svc)(void *ud, uint32_t va, uint32_t eax, uint32_t ebx, uint32_t ecx,
                    uint32_t edx, uint32_t ebp);
    void *ud;
} fel_draw;

/* Installs the draw layer (kept by pointer; NULL removes it).  A handler that
 * draws is answered by fel_lookup only while one is installed, so a screen
 * can never silently lose its pictures. */
void fel_set_draw(const fel_draw *d);

/* The native twin of the original's function at `va`, or NULL. */
fel_fn fel_lookup(uint32_t va);

/* 0x004112A4, var\ral.cfg's keys parsed into the words of the table at
 * 0x00512991 from the text at `text` (the original's ESI).  Not in fel_lookup:
 * it takes an argument in a register, so frontend.c's hook passes it; it leaves
 * every register as it was (PUSHAD ... POPAD), which the gate verifies. */
void fel_cfg_parse_keys(vm *v, uint32_t text);
uint32_t fel_fmt_num(vm *v, uint32_t val, uint32_t flags, uint32_t dest);   /* 0x00402011 */
uint32_t fel_rng_next(vm *v);                                /* 0x004018A0 */
int fel_rng_mod(vm *v, uint32_t n, uint32_t *out);             /* 0x0040188E */
int fel_numbered_path(vm *v, uint32_t tmpl, uint32_t idx);   /* 0x00403071 */

/* 0x00478937's best-times seeding, the seven PUSHAD ... POPAD
 * routines fel_seed_va lists.  `ts` stands in for 0x00401F8F (the token's text
 * written at `dest`, no NUL) and is called where the original calls it.
 * `edi` is 0x00478A8E's table (the others take none).  Returns the ZF the
 * original leaves (0 or 1), or -1 for an address that is not one of them. */
typedef void (*fel_tokstr)(void *ud, vm *v, int32_t tok, uint32_t dest);
int fel_seed(vm *v, uint32_t va, uint32_t edi, fel_tokstr ts, void *ud);
int fel_seed_count(void);
uint32_t fel_seed_va(int i);

/* the boot's two entry points whole - 0x00411013 (ral.dat, hst.dat,
 * ral.cfg, the default names, the 300 file probes) and 0x00478937 (the main
 * menu's entry: names, the tables, the seeding) - with every platform service
 * reached through the hook the VM has for it (vm_invoke).  0 done; -1 the
 * boot ended: the original's fatal exit ([0x4F0274] says which) or a service
 * with no hook (fel_boot_error() is its address, else 0). */
int fel_boot_411013(vm *v);
int fel_boot_478937(vm *v);
int fel_boot_error(void);

/* For the gate: every address fel_lookup answers. */
int fel_count(void);
uint32_t fel_va(int i);
int fel_uses_draw(int i);       /* 1: the handler draws, so it needs fel_set_draw */

#endif
