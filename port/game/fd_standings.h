/* fd_standings.h - the championship's results / standings screens' code, in C
 * (family B: the stage / rally / championship result pages, their rows,
 * the page arrows, the results builders).
 *
 * The same idea as fe_logic.h / fe_champ.h: each function is the original's code
 * at the address in its name, over the VM's data copy at the original's own
 * addresses.  fd_standings_check runs the original (the VM) and these on the same
 * random state and compares the data and heap, the registers a function leaves,
 * and every call it makes to a PLATFORM SERVICE (below), in order.
 *
 * THE SERVICES.  A screen's code leaves the data copy through a few of the
 * original's functions that the host answers (frontend.c's vm_hook): the draw
 * calls 0x00402311 / 0x0040239C / 0x004066F9 / 0x004495D3, the token text
 * 0x00401F8F, and the bodies of the screens it opens (0x00462F22 ..).  `svc`
 * is called at the original's own address with the registers the call has set
 * (EAX..EDI in vm order), and may change them as the hook does (0x00401F8F
 * advances EBX).  The host that still has a VM answers with vm_invoke.
 *
 * REGISTERS.  An entry point takes and returns the eight registers in `r`
 * (vm order; ESP is not used).  A function written PUSHAD ... POPAD leaves `r`
 * as it found it; the others (0x0049DCDC and the like) leave what the
 * original leaves.  Flags are not modelled.
 *
 * FATAL.  The original's own ways out (a scan that finds no row: a fatal exit
 * through 0x00407383) return FDS_FATAL with the two words [0x004F0274] /
 * [0x004F0278] set as the original sets them; the data past one is not
 * meaningful and the host must treat it as the VM stopping. */
#ifndef RALLY_GAME_FD_STANDINGS_H
#define RALLY_GAME_FD_STANDINGS_H

#include <stdint.h>

#include "x86vm.h"

/* FDS_CRASH: the original's own way out that is not a fatal exit: a stack one word out whose RET
 * goes to garbage (a page whose list ends early), or a divide error (#DE) */
enum { FDS_OK = 0, FDS_FATAL = 1, FDS_RUNAWAY = 2, FDS_CRASH = 3 };

typedef struct {
    void (*svc)(void *ud, vm *v, uint32_t va, uint32_t r[8]);
    void *ud;
} fdb_host;

typedef int (*fdb_fn)(vm *v, const fdb_host *h, uint32_t r[8]);

/* the function at `va`, or NULL */
fdb_fn fdb_lookup(uint32_t va);

/* for the gate */
int fdb_count(void);
uint32_t fdb_va(int i);

/* the services the gate must hook */
int fdb_svc_count(void);
uint32_t fdb_svc_va(int i);

#endif
