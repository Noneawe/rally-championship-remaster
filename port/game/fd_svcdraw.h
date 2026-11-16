/* fd_svcdraw.h - the service area's post-draw 0x004A1C11 and the helpers it
 * draws with, in C.
 *
 * The same idea as fd_stage.h, and the same draw layer (`fdsg_draw`, `call`
 * answering 0x00402311 / 0x0040239C / 0x004066F9 / 0x00401F8F at the
 * original's own address with its registers).  fd_svcdraw_check runs the
 * original's code and these over one random state and compares the calls in
 * order and the whole data and heap.
 *
 *   0x004A1C11  the post-draw: the title line, the twelve rows' pickers, the
 *               labels, the sprites, the clock, the inspection bar
 *   0x004A1A56  the clock's three lines (time used / left / the penalty)
 *   0x004681D4  a two-part bar: [0x707BAC] percent of [0x707BA0] wide
 *   0x0046825C  the same, one sprite when the value is not above 0
 *
 * The rest of 0x004A1C11's helpers (0x004A2D46 the sixteen figures, 0x004A2CEE,
 * 0x004A2C96, 0x004A2B26, 0x004A2B67, 0x004A2BA8, 0x004A2C1F the cursor's
 * bars, 0x004A27C3 the inspection total) take EDI or leave registers behind,
 * so they are not entry points here: they are reached from 0x004A1C11 only.
 *
 * A divide by zero in the original ends in 0x00407383 (the runtime's error
 * exit): the C returns FDSG_FATAL there. */
#ifndef RALLY_GAME_FD_SVCDRAW_H
#define RALLY_GAME_FD_SVCDRAW_H

#include <stdint.h>

#include "fd_stage.h"
#include "x86vm.h"

/* FDSG_OK, FDSG_FATAL, or FDSG_NOT_MINE (run the original's code). */
int fdv_run(uint32_t va, vm *v, const fdsg_draw *d);

/* The two that take a register: EDI the hst.dat row (the best-time rows 0x0047091E and
 * 0x00470AFA: the services the stage panels draw through).  Nothing is returned in a
 * register: the original's PUSHAD / POPAD keeps them all. */
int fdv_run_edi(uint32_t va, vm *v, const fdsg_draw *d, uint32_t edi);

/* For the gate: every address fdv_run answers, and the EDI ones. */
int fdv_count(void);
uint32_t fdv_va(int i);
int fdv_edi_count(void);
uint32_t fdv_edi_va(int i);

#endif
