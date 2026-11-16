/* wear.h - THE CAR'S WEAR BETWEEN RACES;
 *
 *
 * The original carries the car's 44 damage figures and its eight dents from
 * race to race through the player record, in every mode:
 *
 *   the launcher       0x00480A49..0x00480C5D  record -> the launcher block
 *                      (FUN_00483559's twin copy is the same)
 *   graham_to_shaun    0x004455B1..0x004457E7  block -> car 1's record, FILD
 *   the race           wears the car (sim_damage_accumulate_q - NOT PORTED)
 *   shaun_to_graham    0x004467FE..0x00446A5C  car -> block, FISTP
 *   the launcher's     0x0048121A..0x004814D7  block -> record, then five
 *   return                                     summaries recomputed
 *
 * The block is 49 dwords at 0x0070CCA0 (wear_block.l[k] is 0x0070CCA0 +
 * 4k) and the dents' 32 bytes at 0x0070CFD8.  Six of the dwords are not
 * loaded into the car; they pass through the race untouched, so a caller
 * keeps the block from the launch and hands the same one back.
 *
 * The tables are transcribed by hand here; tools/wearmap.py reads the same
 * four out of the code, and wear_check diffs the two (wear_print_maps). */
#ifndef RALLY_GAME_WEAR_H
#define RALLY_GAME_WEAR_H

#include "car.h"

#include <stdint.h>
#include <stdio.h>

#define WEAR_L 49                     /* 0x0070CCA0 .. 0x0070CD60          */

typedef struct wear_block {
    int32_t  l[WEAR_L];
    uint32_t oct[8];                  /* 0x0070CFD8: caroctdamage's bits   */
} wear_block;

/* The record's memory, as the caller has it (the menus' interpreter). */
typedef uint32_t (*wear_rd_fn)(void *ud, uint32_t addr);
typedef void     (*wear_wr_fn)(void *ud, uint32_t addr, uint32_t v);

/* The launcher's copy out of player record P. */
void wear_from_record(wear_block *w, wear_rd_fn rd, void *ud, uint32_t P);
/* graham_to_shaun's: the 43 loaded fields FILD'd into car->dam[], the dents'
 * bits into car->caroctdamage[]. */
void wear_into_car(car_state *car, const wear_block *w);
/* shaun_to_graham's: the 43 fields FISTP'd back (nearest-even, as FNINIT
 * leaves the x87), the dents' bits; the other six dwords untouched. */
void wear_from_car(wear_block *w, const car_state *car);
/* The launcher's return into player record P: every copied dword, the
 * dents, then P+0xD8 = (whl1..4)/10 + bdywrk x 6/10 and P+0xF8, 0x108,
 * 0x100, 0xE8 = susp, tyres, brakes, lights. */
void wear_into_record(const wear_block *w, wear_wr_fn wr, void *ud, uint32_t P);

/* The four maps, one line a store, in tools/wearmap.py's form and order. */
void wear_print_maps(FILE *out);

#endif
