/* camview.h - the camera's RENDER half: from the car record's camera words
 * (port/sim/camera.h) to a stage_view.
 *
 * Once per rendered frame the original copies
 * the car's camera into the view globals (0x00436D04) and builds the view
 * matrix (0x00436E4C):
 *
 *   views 0-3   the rotation is the CAR'S OWN (car+0xB4, inverted), so they
 *               are bolted to the car; views 1-2 also lean with the steering
 *               (a filter at RENDER rate, +0x6A0)
 *   views 4-15  Ry(-yaw) Rx(-pitch) Rz(-roll) of the lagged angles, then the
 *               view's own Ry(-vyaw) Rx(-vpitch) Rz(-vroll)
 *   all         translate by -xcampos; focal = width x fishval x zoom 0x100 /
 *               163840 = 0.6 x width (fishval 384)
 *
 * and, when the HUD mask has bit 0x08 (F8), the REAR-VIEW MIRROR: a second
 * frame render into a 179 x 64 inset (at 800 x 600), from the car's origin
 * 0.5 m up, rotated by the car's matrix and a 180-degree yaw ([0x0059EC14]),
 * fishval 256, draw radius 2 (0x00452713, 0x00453E06, 0x00436D98).
 *
 * The rotation builders are FUN_00425D1C / CCA / D6D (FSINCOS of a 32-bit
 * binary angle) and the products FUN_00425E1D (ral_mat4_mul).  The inverse
 * of the car's matrix, FUN_0041FA3B (an adjugate and a Gram-Schmidt pass), is
 * taken as its transpose, as objscene_camera does for the view rotation. */
#ifndef RALLY_GAME_CAMVIEW_H
#define RALLY_GAME_CAMVIEW_H

#include "camera.h"
#include "stage.h"

/* The main view.  Runs views 1-2's lean filter (it writes cam->lean), so call
 * it once per rendered frame.  cam_yaw / cam_extra_yaw are [0x005BA608] and
 * [0x005BA614], for objscene's yaw billboards.  rot_out (16 floats, or NULL)
 * takes [0x0059EF94], the view's rotation - which the MIRROR's build writes
 * too (0x004370EB, with [0x005EC7FC] set), so after a frame with the mirror
 * the global holds the mirror's. */
void camview_main(stage_view *v, int screen_w, int screen_h,
                  sim_camera *cam, const car_state *car,
                  const sim_camera_consts *k,
                  uint32_t *cam_yaw, uint32_t *cam_extra_yaw, float *rot_out);

/* The mirror: the view, and the inset's rectangle in screen pixels (x, y,
 * w, h, top-down).  `slot` is the player, `layout` [0x005D6758], `panel` the
 * HUD's progress-bar bit (car+0x220 & 0x40). */
void camview_mirror(stage_view *v, int screen_w, int screen_h,
                    const car_state *car, const sim_camera_consts *k,
                    int32_t layout, int32_t slot, int panel, int rect[4],
                    float *rot_out);

#endif
