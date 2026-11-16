/* car_input.h - the live source of the car's four device words.
 *
 * The original polls DirectInput on its own thread (INPUTTDB, 0x00407621) and
 * FUN_00408D37 turns each control scheme's twelve bindings into four words,
 * dannyeax..dannyedx, which FUN_00438880 hands to the tick's input stage
 * (sections 9.1-9.3).  This module is the KEYBOARD path
 * of FUN_00408D37 over the original's bindings (car_keys.h): no
 * joystick, no axis, no POV.  ~~What it produces is the binary's; which key
 * is which is not.~~  Both are the binary's since: the keys are a player
 * record's twelve bytes (+0x1C..+0x27), the image's player 1 until a launch
 * from the menus hands over the chosen player's.
 *
 * It also replays a KEY SCRIPT, so a live run can be driven without a human:
 * the same words from a file, step by step, for a measurement that repeats. */
#ifndef RALLY_GAME_CAR_INPUT_H
#define RALLY_GAME_CAR_INPUT_H

#include <stdint.h>

/* Slot bits, one per binding slot: bit s set = slot s's key is down. */
typedef uint32_t car_slots;

/* FUN_00408D37's keyboard path: the four words for the slots that are down.
 * words[0..3] = dannyeax, dannyebx, dannyecx, dannyedx. */
void car_input_words(car_slots down, uint32_t words[4]);

/* THE JOYSTICK HALF.  A scheme's device state as 0x004086D2 leaves
 * it after a poll: */
typedef struct car_dev {
    int      present;   /* [esi+0x20] == 1 and [esi+4] != 0: a device      */
    int      jstype;    /* [esi+8]: 1 forces every arm digital (0x7000)    */
    int32_t  x;         /* +0x28: X, DIPROP_RANGE -256..256                */
    int32_t  ydown;     /* +0x2C: Y's down half, or pedal axis 1, >= 0     */
    int32_t  yup;       /* +0x30: Y's up half, or pedal axis 2, >= 0       */
    uint32_t buttons;   /* +0x34: bit n-1 = DirectInput button n held      */
    uint32_t pov;       /* rgdwPOV[0], 0x4F1A10: hundredths of a degree,
                           low word 0xFFFF centred                         */
} car_dev;

/* FUN_00408D37 whole: the keys that are down, the device, and each slot's
 * mapping dword (car_joy_image's shape, decoded as 0x00408CD5 does).  With
 * d->present == 0 it is car_input_words. */
void car_input_device_words(car_slots down, const car_dev *d,
                            const uint32_t map[12], uint32_t words[4]);

/* THE PAD (a remaster, named): plat_pad_read through section 12's dead zone
 * and saturation into a car_dev - the stick's X as +0x28, the RIGHT trigger
 * as +0x30 (the accelerator's axis), the LEFT as +0x2C (the brake's), the
 * buttons renumbered (car_input.c, pad_button) and the d-pad as the POV.
 * Returns d->present. */
int car_input_pad(car_dev *d);

/* SETUP CONTROLLER's axis mode, player +0x34, which 0x00408A1B stores
 * as the scheme's jstype [esi+8] (0 -> 2; 0x00408A5E..0x00408A6E).  The
 * port's jstype for it: 1 is all-digital (0x00408F61), anything else the
 * analogue shape (jstype 2 and 3 differ only in where the pedal axes come
 * from, which the pad's triggers fix).  The image's own record holds 2. */
int  car_input_jstype(int axis_mode);
void car_input_set_axis_mode(int axis_mode);

/* the steering stick's response curve (rally.cfg pad_curve),
 * an exponent in percent clamped to 100..300; 100, the default, is linear.
 * car_input_curve maps a 0..256 magnitude through it. */
void car_input_set_curve(int pct);
int  car_input_curve(int v);

/* The current scheme's twelve mapping dwords, by slot: car_joy_image
 * overlaid with the port's additions for the slots the image leaves unbound
 * (pause, reset car), until car_input_set_joymap. */
const uint32_t *car_input_joymap(void);

/* A player record's eleven mapping dwords, +0x50..+0x78, as the launcher
 * copies them (0x00480D4F). */
void car_input_set_joymap(const uint32_t rec[11]);

/* joylowerdz / joyupperdz, 0..100 percent (section 12).  The image's default
 * is -1, "leave the driver's" - none.  The port's default is its own. */
void car_input_set_deadzone(int lower, int upper);
void car_input_get_deadzone(int *lower, int *upper);

/* Section 12's two DIPROP values from joylowerdz / joyupperdz, in
 * DirectInput's 0..10000; and one raw axis reading (0..32767 magnitude)
 * through them onto 0..256, as a driver applying them does. */
void car_input_dz_props(int lower, int upper, int *deadzone, int *saturation);
int  car_input_dz_apply(int raw, int deadzone, int saturation);

/* The slots whose bound key plat_input_key reports down. */
car_slots car_input_keyboard(void);

/* A player record's twelve key bytes, +0x1C..+0x27 in record order (folded
 * codes, car_keys.h), as the launcher copies them (0x00480DA7).  Until it is
 * called the binding is car_key_default, the image's player 1. */
void car_input_set_keys(const unsigned char rec[12]);

/* The SDL scancode bound to `slot`, or 0. */
int car_input_slot_key(int slot);

/* 1 while the binding is the image's player 1 (car_key_default). */
int car_input_keys_are_default(void);

/* A key script: lines "STEP KEYS", the keys held from that step (counted
 * from the run's start) until the next line.  KEYS is any of
 *     L R  steer left/right     T B  throttle/brake    U D  gear up/down
 *     H    handbrake            X    rescue            -    nothing
 *     V    the camera view (binding slot 9)
 *     P    pause (binding slot 10: PAUSED, - a script cannot
 *          resume it: any key on the keyboard does)
 * and '#' starts a comment.  Returns 0, or -1 (unreadable or malformed),
 * with the line number in *bad_line. */
typedef struct car_script {
    long      n;
    long     *step;
    car_slots *keys;
} car_script;

int       car_script_load(car_script *s, const char *path, long *bad_line);
car_slots car_script_at(const car_script *s, long step);
void      car_script_free(car_script *s);

#endif
