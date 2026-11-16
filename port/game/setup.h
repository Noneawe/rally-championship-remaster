/* setup.h - THE CAR SETUP SCREEN'S LOGIC (PLAYER n SERVICE AREA), ported.
 *
 * Screen 0x006FFBC0, body FUN_0046EEA9.  What the screen
 * KEEPS is the original's own globals, and it keeps them AT THEIR OWN
 * ADDRESSES, in the VM's data copy (x86vm.h): the drawing code (run by the VM,
 * frontend.c) reads them as it always did, and this file is what writes them.
 * Every function names the original it transcribes.  The screen's draw is NOT
 * here.
 *
 * THE VERIFICATION IS A DIFFERENTIAL: `setup_check` runs the original's own
 * machine code for each of these functions in the VM, on the same state and
 * the same keys, and compares every byte of the state after every step
 * (standing rule 11: the check has a mutant that must fail).
 *
 * THE MODEL:
 *   twelve component rows (items 2..13), the gearing picker (14), the tyre
 *   picker (15), two sliders edited live (steering 16, brake balance 17) and
 *   two pickers (suspension height 18, stiffness 19).  Touching a row or
 *   picker CHARGES its service time against a budget; the time trial's is
 *   0x0A8C s of which the last 0x384 is held back (TIME AVAILABLE is
 *   budget - 0x384).  CONTINUE commits: the touched parts are repaired
 *   (wear level 0) and the pickers' values written into the player record,
 *   which is where the launcher reads the car's setup.
*/
#ifndef RALLY_GAME_SETUP_H
#define RALLY_GAME_SETUP_H

#include <stdint.h>

#include "x86vm.h"

/* The key codes the panel handlers test: FUN_00407CDC's folded values. */
enum { SU_KEY_LEFT = 0x68, SU_KEY_RIGHT = 0x69, SU_KEY_ENTER = 0x1C };

/* The time trial's service budget, FUN_00489D64 (0x00489D65). */
enum { SU_TT_BUDGET = 0x0A8C };

/* The screen's player record base, [0x708DE8 + [0x7082F8]*4]. */
uint32_t setup_player(vm *v);

/* FUN_004A3047: the 14 parts' service times from the player's wear levels
 * and the car's repair record; the inspection shares; the four setup costs.
 * Also the panel working values (FUN_0046D371) when `first` is set. */
void setup_costs(vm *v);

/* FUN_0046D371: the pickers' working copies from the player record. */
void setup_panels_from_player(vm *v);

/* The state FUN_00489D64 / FUN_0046EEA9 leave before the first frame:
 * budget, spent, the touched / locked flags clear, [0x70CA08] = 0. */
void setup_begin(vm *v, uint32_t budget);

/* A SELECT on item `item` (1-based): 0x00468420 for the rows, the six
 * handlers 0x004682D4..0x004683D7 for the pickers.  Returns 1 when the
 * item leaves the screen (CONTINUE, item 1), 0 otherwise.  Items 20 / 21 /
 * 22 (LOAD, SAVE, VIEW STAGE) are not ported: returns -1. */
int  setup_select(vm *v, int item);

/* One frame of FUN_004684F0, the pre-draw handler: with the edit flag
 * [0x70CA08] set, the picker named by [0x70CA0C] takes `key` (0 = none). */
void setup_edit_frame(vm *v, unsigned key);

/* The four pickers' change handlers 0x0046857A (item 14), 0x004689A4 (15), 0x00468802 (18) and 0x004686CC (19)
 * entered with EBX = 1, as the car setup slot's load (0x0046BB78) calls them: the RETURN branch, no key.
 * Other items do nothing. */
void setup_picker_confirm(vm *v, int item);

/* FUN_00468DAD: CONTINUE's commit, minus its waiting (the service
 * animation): the touched parts are repaired, the pickers written into the
 * player record. */
void setup_commit(vm *v);

/* The words the next stage reads: the six sliders as the player record
 * holds them after the commit, `v` in menu units (0..100; tyre 0..5; gear
 * 0..4).  player+0x10C .. +0x120. */
typedef struct {
    int32_t tyre, bbal, seth, sets, stes, setg;
} setup_sliders;
void setup_get_sliders(vm *v, setup_sliders *o);

/* graham_to_shaun's scaling of those into the car record
 * four of them x 2.56 (round to nearest,
 * < 0 -> 0, >= 100 -> 0x100), brake balance then INVERTED (0x100 - x). */
typedef struct {
    int32_t tyretype, b_bal, set_g, set_h, set_s, ste_s;   /* car +0x2C +0x20 +0x28 +0x30 +0x34 +0x3C */
} setup_car_words;
void setup_car_words_from(const setup_sliders *s, setup_car_words *o);

#endif
