/* fe_champ.h - the championship's logic, in C.
 *
 * The same idea as fe_logic.h: each function is the original's code at the
 * address in its name, over the VM's data copy at the original's own
 * addresses; fe_champ_check runs the original and these on the same random
 * state and compares all memory.  Kept apart from fe_logic.c so the two
 * sessions' work merges cleanly.
 *
 * A function that is a register-taking helper (0x0046EBBC, 0x0046F686) is
 * written as a C function with the registers as arguments, and is gated with
 * them as inputs; the entry points (0x0046D5B5, 0x0046E580) take none (PUSHAD
 * ... POPAD, which the gate verifies) and are what a host's vm_run would ask
 * fec_lookup for.
 *
 * Return codes: 0 done; FEC_FATAL_* the original's own way out (a fatal exit
 * through 0x00407383, or a #DE the interpreter stops on): the memory is not
 * meaningful past one, the caller must treat it as the VM stopping. */
#ifndef RALLY_GAME_FE_CHAMP_H
#define RALLY_GAME_FE_CHAMP_H

#include <stdint.h>

#include "x86vm.h"

enum {
    FEC_OK = 0,
    FEC_FATAL_ZERO = 1,     /* 0x004D5469: a quantity that must not be 0 is */
    FEC_FATAL_LIST = 2,     /* 0x004D54FF: the stage-set chain has no such id */
    FEC_FATAL_DIV  = 3,     /* a DIV by 0: #DE */
    FEC_RUNAWAY    = 4      /* a scan the original would never end (no kind-4 entry...) */
};

/* 0x0046EBBC: EDI the itinerary's start, EAX the entries ahead, EBX the
 * rallies to skip (each ends at a kind-4 entry); returns EDI, every other
 * register is kept.  *rc is FEC_RUNAWAY for a scan that would never end. */
uint32_t fec_46ebbc(vm *v, uint32_t edi, uint32_t eax, uint32_t ebx, int *rc);

/* 0x0046F686: EDI an itinerary entry; returns EDI the stage record.  The
 * original leaves EBX = entry +4 times 0x40, ECX = entry +8, EDX = ECX and ESI
 * = EDI (the record) behind; `regs` (may be NULL) gets them in vm order. */
uint32_t fec_46f686(vm *v, uint32_t edi, uint32_t regs[8], int *rc);

/* 0x00460EF7: the rally's four time words [0x70CA50..0x70CA5C] scaled by the
 * float tables (x87: FILD, FMUL, FISTP to nearest) - takes no register but
 * reads [0x7082C4]. */
void fec_460ef7(vm *v);

/* 0x0046D5B5: the rivals' times; 0x0046E580: the loop after a retirement. */
int fec_46d5b5(vm *v);
int fec_46e580(vm *v);

/* ---- 0x0046F785, the driver that sets a rally's race up ----
 *
 * It reads no register (the host runs it with none) and leaves them all
 * spent.  The services it reaches through the platform / host, at the
 * original's own addresses, with the registers the call has set (EAX, EBX;
 * ECX and EDX are passed as 0 and are not meaningful):
 *   0x00401F8F  the token EAX's text written at EBX; returns its length (the
 *               NUL after it is the caller's)
 *   0x00480461  the launcher: EAX the player (or the next one's entry), EBX the
 *               game mode (1: the championship); the host decides what happens,
 *               and writes the race's outcome in [0x0070CF94] / [0x0070D004]
 *   0x00440F60, 0x004713BA  the engine's screen clear / set-up, which 0x0047D4A3
 *               calls when [0x0070AB50] == 1
 * It returns EAX (only 0x00401F8F's length is used).  A host that has no
 * use for one returns 0. */
typedef uint32_t (*fec_svc_fn)(void *ud, uint32_t va, uint32_t eax, uint32_t ebx,
                               uint32_t ecx, uint32_t edx);
typedef struct { fec_svc_fn svc; void *ud; } fec_services;

int fec_46f785(vm *v, const fec_services *s);

/* its helpers, each gated alone: 0x0046F737 the stage-result records emptied,
 * 0x004A51D5 made from the drivers, 0x0046D0CB the opponents' records taken
 * from them (FEC_RUNAWAY: the scan never ends), 0x004A526C each player's +0x288
 * zeroed, 0x0047D4A3 the engine calls, 0x004A51BB (ECX a player, EAX in and out;
 * *edi the player's record). */
void fec_46f737(vm *v);
void fec_4a51d5(vm *v, const fec_services *s);
int fec_46d0cb(vm *v);
void fec_4a526c(vm *v);
void fec_47d4a3(vm *v, const fec_services *s);
uint32_t fec_4a51bb(vm *v, uint32_t eax, uint32_t ecx, uint32_t *edi);

/* For the gate: the entry points with no register. */
typedef int (*fec_fn)(vm *v);
fec_fn fec_lookup(uint32_t va);
int fec_count(void);
uint32_t fec_va(int i);

#endif
