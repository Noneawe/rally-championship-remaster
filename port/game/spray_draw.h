/* spray_draw.h - the spray's picture (spray.h is the rest),
 * The original draws each particle inside its update; the
 * port's spray_update calls back here for the quad (spray.c computes the
 * point, the near test and [0x008D5760] as the original does). */
#ifndef RALLY_SPRAY_DRAW_H
#define RALLY_SPRAY_DRAW_H

#include "spray.h"
#include "stage.h"

struct spray_view {
    const stage_view *v;        /* F, the centre, the far value, the fog    */
};

/* 0x004BACFA's texture half: files\m\surfdata.pcx and surfmask.pcx, 256 x
 * 256, into one texture whose alpha is 255 - the mask's red; and the draw's
 * callback.  0 on success, `err` otherwise (the spray then steps undrawn). */
int  spray_load_atlas(spray *s, const char *p_data, const char *p_mask,
                      char *err, size_t errlen);

/* [0x0059F014]: world -> camera from the object layer's two matrices
 * (objscene.h: view_trans = minus the eye, view_rot the rotation). */
void spray_view_matrix(spray *s, const float view_rot[16],
                       const float view_trans[16]);

/* 0x004BB1F7 with its picture: the module's render states (texture blend
 * MODULATEALPHA, colour key, SRCALPHA / INVSRCALPHA, fog, blending, the
 * atlas, no depth writes), every particle's step and draw, then the sparks'
 * pass under DESTBLEND ONE. */
void spray_draw_frame(spray *s, const spray_view *v);

#endif
