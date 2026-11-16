/* fd_cspecs.h - INFORMATION > CAR SPECIFICATIONS, in C (family G).
 *
 * The same idea as fd_stage.h: each function is the original's code at the
 * address in its name, over the VM's data copy at the original's own
 * addresses; fd_cspecs_check runs the original and these on the same state and
 * compares all memory AND the calls made to the draw layer, in order.
 *
 * THE SCREEN.  [0x7082FC] is the car (an index into the list [0x70C5C8], the
 * 23 cars' records at 0x70C06A), [0x708304] the page (0..3).  Page 0 is the 3D
 * car and its description; pages 1..3 are the spec sheets.  Each page's art is
 * a picture by car (the table [0x70BDE2 + page * 4], indexed by the car's id).
 *
 *   0x004650C4  the page's art (-> 0x0047B0C2, a service)
 *   0x00466869  starts the car's narration (-> 0x0040F566, a service)
 *   0x00466837 / 0x004668BB  the car row's LEFT / RIGHT: the car, then the two above
 *   0x0046696C / 0x0046698F  the page row's LEFT / RIGHT: the page, then the art
 *   0x004667D6 / 0x004667FC  the pre-draws: the car's name, the page's name, as
 *               text at 0x00A2C254 / 0x00A2C354 (the two rows' own strings)
 *   0x00478F77  the post-draw: the page's draw, EDI the car's record
 *   0x004669FE / 0x00466CA1 / 0x00466F11 / 0x00466A2F  the four pages' draws
 *
 * THE DRAW LAYER (a function that leaves its own code goes through ONE call,
 * at the original's address, with the registers the original's call set; the
 * same type fd_stage.h defines):
 *
 *   0x00402311  a token: EBX the font record, EAX the token, ECX x, EDX y
 *   0x0040239C  a string: EAX the font record, EBX the string, ECX x, EDX y
 *   0x00401F8F  EAX a token's text written at EBX, EBX advanced past it
 *   0x0047B0C2  the screen's art: EAX the path (the host loads it)
 *   0x0040F566  the narration: EAX the path, ECX the volume
 *
 * Returns FDSG_OK, FDSG_FATAL (a division the original would fault on), or
 * FDSG_NOT_MINE. */
#ifndef RALLY_GAME_FD_CSPECS_H
#define RALLY_GAME_FD_CSPECS_H

#include <stdint.h>

#include "fd_stage.h"
#include "x86vm.h"

/* Runs the original's function at `va` as C; `r` the registers in (EDI for the
 * page draws), NULL = none. */
int fdq_run(uint32_t va, vm *v, const fdsg_draw *d, const uint32_t r[8]);

/* For the gate: every address fdq_run answers. */
int fdq_count(void);
uint32_t fdq_va(int i);

#endif
