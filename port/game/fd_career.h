/* fd_career.h - the career, slot-screen, narration and pre-draw handlers in C (family D).
 *
 * The same idea as fe_logic.h / fe_champ.h: each function is the original's
 * code at the address in its name, over the VM's data copy at the original's
 * own addresses; fd_career_check runs the original and these on the same
 * random state and compares all memory, every platform service the handler
 * reaches (in order, with the registers the original's call has set) and the
 * flags it leaves.
 *
 * Kept apart from fe_logic.c so the two sessions' work merges cleanly.  The
 * host (frontend.c) answers `fdc_lookup(va)` after `fel_lookup(va)` in vm_run /
 * fe_vmcall, with the host's services installed by fdc_set_host: a handler
 * that reaches a service is answered ONLY while a host is installed (a screen
 * can never silently lose its pictures or its narration).
 *
 * Return value of an entry point: FDC_FATAL (< 0) when the original would
 * have stopped the VM (a divide by zero, a scan that never ends); else the
 * carry flag the original leaves (0 / 1), which only 0x0046C40F's callers read. */
#ifndef RALLY_GAME_FD_CAREER_H
#define RALLY_GAME_FD_CAREER_H

#include <stdint.h>

#include "x86vm.h"

enum { FDC_FATAL = -1 };

/* the flags a service sets (the return of fdc_host.svc) */
enum { FDC_CF = 1, FDC_ZF = 2 };

/* A platform service at the original's own address `va` (the hooks frontend.c
 * installs: 0x00402311 / 0x0040239C / 0x004066F9 the draw layer, 0x00401F8F the
 * token's text at EBX, 0x0040F566 the narration, 0x0040C37C speech busy,
 * 0x00407CDC the key, 0x00465011 the stage map ...).  `r` is EAX..EDI as the
 * original's call sets them, in and out (0x00401F8F advances EBX); the return
 * is the flags the service sets.  A host that reaches the hook through the
 * VM is `vm_invoke(v, va, r)` plus `vm_cf`. */
typedef struct {
    uint32_t (*svc)(void *ud, vm *v, uint32_t va, uint32_t r[8]);
    void *ud;
} fdc_host;

void fdc_set_host(const fdc_host *h);

typedef int (*fdc_fn)(vm *v);

/* 0x004680B9 takes EDI (the item record the engine hands it): the caller's
 * register, so not in fdc_lookup.  Needs a host (a device service).  PUSHAD ... POPAD. */
int fdc_4680b9(vm *v, uint32_t edi);

/* 0x0046A2E2 / 0x0046BE55: a slot's name typed, once a frame.  They read the caller's ECX and EDI
 * (`r`, EAX..EDI as the loop holds them), so they are not in fdc_lookup.  fdc_is_typing: the address is one of
 * them and a host is installed.  The carry the original leaves (ESCAPE: set) is read by no caller. */
int fdc_is_typing(uint32_t va);
int fdc_typing(vm *v, uint32_t va, const uint32_t r[8]);

/* The slot files: 0x004A7041 / 0x004A6E82 (the reads, EBX the slot; CF = no such slot or a bad file),
 * 0x004A709B / 0x004A6EDC (the writers, EBX the slot; CF = not written).  Services the loads and the typers
 * reach (the host answers them with this), run over the file layer's own services (0x00402BCD open, 0x00402C22
 * read, 0x00402C3C write, 0x00402C08 close, the clock 0x00410D81), which stay the platform's.  PUSHAD ... POPAD:
 * every register comes back as it was given; the return is the flags (FDC_ZF on success, else FDC_CF). */
int fdc_is_slot_file(uint32_t va);
uint32_t fdc_slot_file(vm *v, uint32_t va, const uint32_t r[8]);

/* 0x00407B8A: the scan code EAX's character (AL; 0 = none), by the layout
 * [0x4EB7A8], the shift states and the dead key [0x4F1A98].  Not an entry the host
 * asks for: the keyboard's translation, for the handlers that type a name. */
uint32_t fdc_keychar(vm *v, uint32_t scan);

/* The native twin of the original's function at `va`, or NULL (not ported, or
 * it reaches a service and no host is installed). */
fdc_fn fdc_lookup(uint32_t va);

/* For the gate */
int fdc_count(void);
uint32_t fdc_va(int i);
int fdc_uses_host(int i);

#endif
