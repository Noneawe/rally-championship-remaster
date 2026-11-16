/* fireworks_draw.h - the fireworks' picture (fireworks.h is the rest).
 * The original draws each particle inside its
 * update, one quad through 0x0044CAA9; the port's fireworks_frame hands each
 * quad to a callback here.  The render state is the frame's at 0x004C1224:
 * the spray's texture, MODULATEALPHA, fog off (0x004C1125), SRCALPHA onto ONE
 * (0x004C1201's DESTBLEND), blending on, no depth writes (0x0044C46C). */
#ifndef RALLY_FIREWORKS_DRAW_H
#define RALLY_FIREWORKS_DRAW_H

#include "fireworks.h"
#include "stage.h"

/* [0x0059F014]: world -> camera from the object layer's two matrices, as
 * spray_view_matrix builds it (objscene.h: view_trans = minus the eye,
 * view_rot the rotation). */
void fireworks_view_matrix(float out[16], const float view_rot[16],
                           const float view_trans[16]);

/* 0x004BF796 with its picture: in->focal, cx, cy and zscale from the view,
 * the render states, the step and every quad.  `atlas` is the spray's
 * texture (0: stepped and not drawn); `uv` TEXTINFO record 88's eight
 * corner words (spray.aspect[88 * 9 + 1]). */
void fireworks_draw_frame(fireworks *w, fireworks_in *in, const stage_view *v,
                          uint32_t atlas, const float uv[8]);

#endif
