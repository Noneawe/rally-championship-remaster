/* viewer_keys.h - THE REPLAY VIEWER'S DEBUG KEYS.  A STAND-IN TABLE, NOT THE
 * GAME'S BINDINGS.
 *
 * These drive the viewer (pause, step, seek, quit) and are
 * this port's own choice.  They are not the original's controls and do not
 * claim to be: the game's bindable actions and their defaults are the
 * (read 2026-10-10: car_keys.h), and driving the car from the keyboard is
 * The original's own
 * replay editor has keys of its own (its speed cycle sets [0x5f0a44],
 * and none of them is transcribed here.
 *
 * The values are SDL scancodes, which is what plat_input_key takes
 * (the game's folded-scancode-to-SDL map is
 * car_keys.h's car_key_sdl). */
#ifndef RALLY_GAME_VIEWER_KEYS_H
#define RALLY_GAME_VIEWER_KEYS_H

enum {
    VK_PAUSE      = 44,   /* SPACE     pause / resume                         */
    VK_STEP       = 55,   /* PERIOD    one 10 ms step, while paused           */
    VK_BACK_1S    = 80,   /* LEFT      seek back 100 steps                    */
    VK_FWD_1S     = 79,   /* RIGHT     seek forward 100 steps                 */
    VK_BACK_10S   = 81,   /* DOWN      seek back 1,000 steps                  */
    VK_FWD_10S    = 82,   /* UP        seek forward 1,000 steps               */
    VK_RESTART    = 74,   /* HOME      back to the start frame                */
    VK_GHOST      = 10,   /* G         also draw the GAME'S car (the capture) */
    VK_SHOT       = 69,   /* F12       write the frame to rally_<frame>.ppm    */
    VK_QUIT       = 41,   /* ESCAPE                                           */
    /* --live: the arrows are the CAR'S keys (car_keys.h; ~~and
     * SPACE~~, not since the defaults), so a live run pauses on P - the
     * game's own pause key, binding slot 10 - and has no seek; HOME starts
     * it again. */
    VK_LIVE_PAUSE = 19,   /* P - UNUSED since: a live run's P is the
                           * game's PAUSED (binding slot 10, rally.c)        */
    /* two of the GAME'S OWN in-race keys, FUN_00438C42 - not this
     * table's choice: DIK_F1 (the just-pressed byte [0x004F1CDF]) cycles the
     * view, DIK_F8 ([0x004F1CE6]) toggles the rear-view mirror
     * F12 stays the viewer's PPM key above: in the
     * original it is the local map's toggle, not ported. */
    VK_VIEW       = 58,   /* F1        next camera view (the game's key)      */
    VK_MIRROR     = 65,   /* F8        rear-view mirror (the game's key)      */
    /* FUN_00438C42's F3 / F4 ([0x004F1CE1] / [0x004F1CE2]): the
     * overall volume, with LEFT Ctrl ([0x004F1B41], DIK_LCONTROL held) the
     * in-game music. */
    VK_VOL_DOWN   = 60,   /* F3        volume down (the game's key)           */
    VK_VOL_UP     = 61,   /* F4        volume up (the game's key)             */
    VK_LCTRL      = 224   /* left Ctrl, held: not in keys[], read directly   */
};

#define VK_COUNT 15

#endif
