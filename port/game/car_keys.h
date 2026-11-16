/* car_keys.h - THE CAR'S KEYS: the original's binding tables.
 *
 * replacing the stand-in (arrows, A/Z, SPACE, R).
 * has the reading; tools/keys.py check
 * compares every table below with the image (the car-keys gate).
 *
 * WHERE THE ORIGINAL KEEPS THEM.  Each player record holds twelve key bytes
 * at +0x1C..+0x27, in the REDEFINE KEYBOARD screen's order (its action index
 * [0x708018], 0..11).  The image ships them initialised: player 1's at
 * 0x007083D4, and var\ral.dat, which the original loads over the records,
 * carries the same twelve bytes per player.  The race launcher copies the
 * current player's into the menu block (0x00480DA7: +0x1C..+0x27 ->
 * 0x0070CDE8..F3), INITCONTROLDATA (0x00408A1B) reads them from there into
 * the scheme's twelve binding records, and FUN_00408D37 tests
 * [code + 0x004F1AA4] & 0x80 for each - so a byte is a FOLDED scancode (the
 * filler 0x00407809's numbering, car_key_sdl below), not a DIK code.
 *
 * THE ORDER IS NOT THE SLOT ORDER.  The loader takes the record's bytes in
 * the order +0x54, +0x55, +0x56, +0x57, +0x59, +0x5A, +0x58, +0x5B.. for slots
 * 0..11 (0x00408B23..), so record bytes 4, 5, 6 (handbrake, gear up, gear
 * down) feed slots 6, 4, 5.  car_key_slot is that permutation.
 *
 * The values are the binary's; the SDL scancode for each folded code is this
 * port's (the fold is DirectInput set-1 numbering, and SDL's scancodes are USB
 * usages, so the map is a fact about keyboards, not about the game). */
#ifndef RALLY_GAME_CAR_KEYS_H
#define RALLY_GAME_CAR_KEYS_H

#define CAR_SLOTS 12

/* Player 1's twelve key bytes as the image holds them (0x007083D4), in record
 * order, with the action's name on the REDEFINE KEYBOARD screen (0x6FE4D4) and
 * the key's in the install's BRITISH.KIM.  Players 3 and 4 ship the same
 * bytes; player 2 ships the numeric keypad (0x00708660). */
static const unsigned char car_key_default[CAR_SLOTS] = {
    0x68,   /*  0 turn left           left         -> slot 0  (ECX -0x100)  */
    0x69,   /*  1 turn right          right        -> slot 1  (ECX +0x100)  */
    0x66,   /*  2 accelerator         up           -> slot 2  (EAX, 0x4000) */
    0x6B,   /*  3 brake               down         -> slot 3  (EDX, 0x2000) */
    0x28,   /*  4 handbrake           '            -> slot 6  (0x04)        */
    0x2B,   /*  5 gear up/change      #            -> slot 4  (0x01)        */
    0x36,   /*  6 gear down           right shift  -> slot 5  (0x02)        */
    0x1B,   /*  7 lights              ]            -> slot 7  (0x08, the
             *                                       car+0x084 latch)        */
    0x35,   /*  8 wipers (no item)    /            -> slot 8  (0x10, no
             *    reader): the demo's menu lists it, retail's skips it - the
             *    stub 0x00464FBD and the record byte stay              */
    0x1C,   /*  9 cycle camera views  enter        -> slot 9  (0x20,
             *                                       FUN_00439024)           */
    0x19,   /* 10 pause               p            -> slot 10 (0x40, held:
             *                                       doplaygame's PAUSED)     */
    0x27    /* 11 reset car           ;            -> slot 11 (0x80, held 1 s
             *                                       while slow: the rescue)  */
};

/* THE JOYSTICK HALF.  Player 1's mapping dwords as the image holds
 * them (player record +0x50..+0x78, 0x00708408; player 2's, 0x00708694, are
 * the same), by BINDING SLOT.  The launcher copies eleven of them
 * (0x00480D4F: record +0x50 +0x54 +0x58 +0x5C +0x60 +0x64 +0x68 +0x6C +0x70
 * +0x74 +0x78 -> 0x70CDB8 .. 0x70CDD8, 0x70CDE0, 0x70CDE4, skipping 0x70CDDC,
 * slot 8's) and INITCONTROLDATA decodes each through 0x00408CD5:
 *
 *   bit 0x400: axis +0x28 (X), scale -1     bit 0x800: axis +0x28, scale +1
 *   bit 0x200: axis +0x30 (Y's up half)     bit 0x100: axis +0x2C (Y's down)
 *   (tested in that order; none: no axis)
 *   low byte n != 0: button mask 1 << (n - 1); 0: no button
 *
 * So the image's joystick is WHOLLY ANALOGUE: the stick's X steers and its Y
 * is the two pedals, none masked - every one takes FUN_00408D37's
 * proportional arm - and buttons 1-4 are gear up, gear down,
 * handbrake and the camera.  Lights, pause and reset car are unbound. */
static const unsigned int car_joy_image[CAR_SLOTS] = {
    0x400,  /* slot 0  turn left    record +0x58: X, -1          */
    0x800,  /* slot 1  turn right   record +0x5C: X, +1          */
    0x200,  /* slot 2  accelerator  record +0x50: +0x30          */
    0x100,  /* slot 3  brake        record +0x54: +0x2C          */
    1,      /* slot 4  gear up      record +0x60: button 1       */
    2,      /* slot 5  gear down    record +0x64: button 2       */
    3,      /* slot 6  handbrake    record +0x68: button 3       */
    0,      /* slot 7  lights       record +0x74: none           */
    0,      /* slot 8  wipers       not copied (0x70CDDC)        */
    4,      /* slot 9  camera       record +0x6C: button 4       */
    0,      /* slot 10 pause        record +0x70: none           */
    0       /* slot 11 reset car    record +0x78: none           */
};

/* Record dword offset -> binding slot, for a record's own mapping (the
 * launcher's copy and the loader's order, composed). */
static const unsigned char car_joy_rec_slot[11] = {
    /* +0x50 +0x54 +0x58 +0x5C +0x60 +0x64 +0x68 +0x6C +0x70 +0x74 +0x78 */
         2,    3,    0,    1,    4,    5,    6,    9,   10,    7,   11
};

/* Record byte i -> binding slot (INITCONTROLDATA's key-byte order). */
static const unsigned char car_key_slot[CAR_SLOTS] = {
    0, 1, 2, 3, 6, 4, 5, 7, 8, 9, 10, 11
};

/* Folded code -> SDL scancode; 0 = a code the fold never produces.  The fold
 * (0x00407809) keeps DirectInput codes 0x01..0x58 as they are and moves 17
 * extended ones down: 0x9C->0x60 0x9D->0x61 0xB5->0x62 0xB8->0x64 0xC7->0x65
 * 0xC8->0x66 0xC9->0x67 0xCB->0x68 0xCD->0x69 0xCF->0x6A 0xD0->0x6B
 * 0xD1->0x6C 0xD2->0x6D 0xD3->0x6E 0xDB->0x78 0xDC->0x79 0xDD->0x7A.
 * Nothing else is written, so 0x59..0x5F, 0x63, 0x6F..0x77 stay 0 (the KIM
 * names 0x63 prtscn, 0x70 pause, 0x71 break: keys the game cannot see).
 * 0x54 is the KIM's "sysreq"; 0x55 has no key and no name. */
static const unsigned char car_key_sdl[0x80] = {
    /* 0x00 */   0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
    /* 0x10 */  20, 26,  8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40,224,  4, 22,
    /* 0x20 */   7,  9, 10, 11, 13, 14, 15, 51, 52, 53,225, 49, 29, 27,  6, 25,
    /* 0x30 */   5, 17, 16, 54, 55, 56,229, 85,226, 44, 57, 58, 59, 60, 61, 62,
    /* 0x40 */  63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,
    /* 0x50 */  90, 91, 98, 99,154,  0,100, 68, 69,  0,  0,  0,  0,  0,  0,  0,
    /* 0x60 */  88,228, 84,  0,230, 74, 82, 75, 80, 79, 77, 81, 78, 73, 76,  0,
    /* 0x70 */   0,  0,  0,  0,  0,  0,  0,  0,227,231,101,  0,  0,  0,  0,  0
};

#endif
