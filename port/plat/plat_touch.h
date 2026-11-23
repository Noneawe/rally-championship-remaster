/* plat_touch.h - the on-screen controls of a touch screen: layout, hit
 * testing and the virtual pad they make.  Pure code, no SDL and no GL, so the
 * same layout is what plat_sdl2.c tests fingers against and plat_gl.c draws,
 * and plat_touch_check.c can test it.
 *
 * Two layouts.  MENU: a d-pad on the left, OK and BACK on the right.  RACE:
 * steering is a drag anywhere in the left part (the finger's start is the
 * stick's centre), GAS and BRAKE on the right, gear up / down, handbrake,
 * camera and pause around them.  All of it in window pixels, sized by the
 * window's height so a phone's thumbs reach it. */
#ifndef RALLY_PLAT_TOUCH_H
#define RALLY_PLAT_TOUCH_H

#include "plat.h"

#define TOUCH_MAX_BTN 12
#define TOUCH_MAX_FINGERS 10

typedef struct {
    float    x, y, w, h;      /* window pixels, y down */
    unsigned pad;             /* PLAT_PAD_* bit it presses, 0 for none  */
    int      axis;            /* 1 gas (rt), 2 brake (lt), 0 none       */
    char     label[4];
} touch_btn;

typedef struct {
    int   down;
    float x, y;               /* now */
    float x0, y0;             /* where it went down */
} touch_finger;

/* the buttons for a window of w x h; returns how many */
int plat_touch_layout(int w, int h, int race, touch_btn out[TOUCH_MAX_BTN]);

/* the pad the fingers make; lit gets a bit per button (index in the layout)
 * that is held, *steer the steering in -1..1 (for drawing the knob) */
void plat_touch_eval(const touch_finger *f, int nf, int w, int h, int race,
                     plat_pad *p, unsigned *lit, float *steer);

#endif
