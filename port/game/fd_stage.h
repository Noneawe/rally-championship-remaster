/* fd_stage.h - the stage / level / arcade screens' draws and logic, in C (family C).
 *
 * The same idea as fe_logic.h and fe_champ.h: each function is the original's
 * code at the address in its name, over the VM's data copy at the original's
 * own addresses; fd_stage_check runs the original and these on the same random
 * state and compares all memory AND the calls made to the draw layer, in order.
 * Kept in its own files so the families' work merges cleanly.
 *
 * THE DRAW LAYER.  Every call a handler makes that leaves its own code goes
 * through ONE function, `call`, at the original's address with the registers
 * the original's call has set (an EAX..EDI array, vm order, in and out):
 *
 *   0x00402311  a token: EBX the font record, EAX the token, ECX x, EDX y
 *   0x0040239C  a string: EAX the font record, EBX the string, ECX x, EDX y
 *   0x004066F9  a sprite: EAX the sprite, ECX x, EDX y, ESI the bundle
 *   0x00401F8F  EAX a token's text written at EBX, EBX advanced past it (no NUL)
 *   0x004022D2  ECX = the width of the string at EBX in the record at EAX, EDX =
 *               that record's +8 (the line height)
 *   0x004065B0  a filled box: EAX the colour, EBX -> {x, y, w, h}
 *   0x00406457  a row: EAX colour, ECX x, EDX y, EBX length
 *   0x004064F6  a column: EAX colour, ECX x, EDX y, EBX length
 *   0x0047091E, 0x00470AFA   the best-time row: EDI the block (the host skips it
 *               when the block is not loaded)
 *
 * These are exactly the stand-ins frontend.c's vm_hook calls give the
 * interpreter, so the host's side of `call` is the same functions.  A handler
 * that draws is answered only while a layer is installed.
 *
 * Return codes: 0 done; FDSG_FATAL the original's own way out (0x0046F686 found
 * no such stage set, or would never end: fec_46f686's FEC_*): the memory is not
 * meaningful past one, the caller must treat it as the VM stopping. */
#ifndef RALLY_GAME_FD_STAGE_H
#define RALLY_GAME_FD_STAGE_H

#include <stdint.h>

#include "x86vm.h"

typedef void (*fdsg_call_fn)(void *ud, uint32_t va, uint32_t r[8]);
typedef struct { fdsg_call_fn call; void *ud; } fdsg_draw;

enum { FDSG_OK = 0, FDSG_FATAL = 1, FDSG_NOT_MINE = -1 };

/* Runs the original's function at `va` as C, with no register in (the host's
 * vm_run passes none).  FDSG_NOT_MINE: not answered here (run the original's
 * code); a handler that draws needs `d`. */
int fdsg_run(uint32_t va, vm *v, const fdsg_draw *d);

/* The two that take a register: EDI the stage's table row (0x00489D20, 0x00484207). */
int fdsg_run_edi(uint32_t va, vm *v, const fdsg_draw *d, uint32_t edi);

/* For the gate: every address fdsg_run answers, and the EDI ones. */
int fdsg_edi_count(void);
uint32_t fdsg_edi_va(int i);
int fdsg_count(void);
uint32_t fdsg_va(int i);

#endif
