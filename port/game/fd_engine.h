/* fd_engine.h - the engine-facing menu entry points, in C (family E).
 *
 * The same idea as fe_logic.h / fe_champ.h: each function is the original's
 * code at the address in its name, over the VM's data copy at the original's
 * own addresses; fd_engine_check runs the original and these on the same
 * random state and compares all memory, the registers and the calls made to
 * the services.
 *
 * ENTRY POINTS (fde_lookup): every one takes the registers the caller set (r,
 * EAX..EDI in vm order) and gives back the registers
 * the original leaves (the PUSHAD ... POPAD ones leave them as they were), and
 * the carry flag where the caller tests it (*cf, -1 when the original leaves
 * it to whatever the last instruction was).
 *
 * SERVICES: what the original calls and the port answers itself (DirectInput's
 * device calls, the engine's set-up / tear-down, the sound's stop and cue, the
 * podium scene, a draw routine the box is, the file layer): svc(ud, va, r) is
 * the call at `va` with the registers the original has set (EAX..EBP as the
 * call finds them; only those the service reads are meaningful and the gate
 * compares only those).  A service may change r, as a hook does.  A host with
 * no use for one returns at once.
 *
 * Return codes: 0 done; FDE_RUNAWAY a scan the original would never end.  `r`
 * is never NULL (the host passes zeros). */
#ifndef RALLY_GAME_FD_ENGINE_H
#define RALLY_GAME_FD_ENGINE_H

#include <stdint.h>

#include "x86vm.h"

/* FDE_FATAL: the original's fatal exit (0x00407383): the two words [0x004F0274] / [0x004F0278] are set as it sets them;
 * the data past one is not meaningful */
enum { FDE_OK = 0, FDE_FATAL = 1, FDE_RUNAWAY = 2 };

/* returns the carry flag the service leaves (the file layer's failure flag;
 * 0 for the rest) */
typedef int (*fde_svc_fn)(void *ud, uint32_t va, uint32_t r[8]);
typedef struct { fde_svc_fn svc; void *ud; } fde_services;

typedef int (*fde_fn)(vm *v, uint32_t r[8], int *cf, const fde_services *s);

/* NULL when `va` is not one of the entry points below. */
fde_fn fde_lookup(uint32_t va);
int fde_count(void);
uint32_t fde_va(int i);

/* The finish routines' engine calls, in the order the original makes them, as
 * services (the host answers them with nothing except the three that are
 * heard / seen): 0x004157F9 0x0040EE51 0x00478F6B 0x00441843 0x0044187C
 * 0x0044188F 0x00441832 0x004264D7 (the podium scene) 0x0040F639 (music stop)
 * 0x0040456A 0x00478F6B 0x00440F60 0x004713BA 0x00440F60 0x0040F639
 * 0x0040F5A3 (track 3), then 0x00407CDC with EAX = 0. */

#endif
