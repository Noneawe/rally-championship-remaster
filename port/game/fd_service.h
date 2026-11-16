/* fd_service.h - the SERVICE AREA's handlers in C (family A).
 *
 * Each function is the original's code at the address in its name, written as
 * C over the same globals in the VM's data copy (the way fe_logic.c is), so
 * frontend.c can ask fds_lookup(va) in `fe_vmcall` / `vm_run` and the
 * interpreter never sees it.  The platform calls the handlers end in are the
 * hooks the host already has in the VM (0x004495D3 the translucent quad,
 * 0x0048C4AC the menu model draw); they are reached with `vm_invoke`, exactly
 * where the original CALLs them, so a handler answers only where the hook is
 * there.  fd_service_check runs the original and these over one random state
 * and compares the calls in order and the whole data and heap.
 *
 *   0x0048C1A2  the carousel kind's set-up ([0x70C9E0]); ESI = a car list (kinds
 *               13 and 14)
 *   0x0048BD34  REVIEW OPTIONS' model draw  (-> 0x0048BD44)
 *   0x0048BDF1  the carousels' model draw   (-> 0x0048C30E, 3-4 x 0x0048C39A)
 *   0x0048FD81  the service area's quads (the margins, the part panels) and the
 *               four lists it ends in: 0x0048CAE7, 0x0048D64C, 0x0048E1EF,
 *               0x0048EF99 (also answered on their own)
 *
 * A handler takes the registers a caller sets (EAX..EDI order, NULL = zero) and
 * returns 0, or -1 when the original would not have come back (a loop that
 * never ends, a wild read): the host treats it as the VM stopping. */
#ifndef RALLY_GAME_FD_SERVICE_H
#define RALLY_GAME_FD_SERVICE_H

#include <stdint.h>

#include "x86vm.h"

typedef int (*fds_fn)(vm *v, const uint32_t r[8]);

/* The native twin of the original's function at `va`, or NULL. */
fds_fn fds_lookup(uint32_t va);

/* For the gate: every address fds_lookup answers. */
int fds_count(void);
uint32_t fds_va(int i);

/* For the gate's controls: the Nth call that reaches a service is perturbed
 * (a coordinate off by one); 0 = off.  Never set by the game. */
void fds_break(int which);
int fds_break_armed(void);     /* 1: armed and not yet reached */

#endif
