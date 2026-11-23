/* camview.c - the camera's render half.  See camview.h;. */

#include "camview.h"
#include "objxform.h"

#include <math.h>
#include <string.h>

/* FUN_00425D1C / FUN_00425CCA / FUN_00425D6D: FILD the 32-bit binary angle,
 * / 65536 / 65536 / [0x0057B058] (1/2pi), FSINCOS, both stored float32. */
static void sincos32(const sim_camera_consts *k, int32_t a, float *s, float *c)
{
    double r = (double)a / 65536.0 / 65536.0 / (double)k->inv2pi;
    *s = (float)sin(r);
    *c = (float)cos(r);
}

static void ident(ral_mat4 *m)
{
    memset(m, 0, sizeof *m);
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

/* out = out x R(a), with R the builder's layout. */
static void mul_rot(ral_mat4 *out, const sim_camera_consts *k, int axis,
                    int32_t a)
{
    ral_mat4 r, t;
    float s, c;
    ident(&r);
    sincos32(k, a, &s, &c);
    if (axis == 1) {            /* FUN_00425D1C -> [0x0059ED14], about y */
        r.m[0] = c;  r.m[2] = -s;  r.m[8] = s;  r.m[10] = c;
    } else if (axis == 0) {     /* FUN_00425CCA -> [0x0059ECD4], about x */
        r.m[5] = c;  r.m[6] = s;   r.m[9] = -s; r.m[10] = c;
    } else {                    /* FUN_00425D6D -> [0x0059ED54], about z */
        r.m[0] = c;  r.m[1] = s;   r.m[4] = -s; r.m[5] = c;
    }
    ral_mat4_mul(&t, out, &r);
    *out = t;
}

static int32_t neg(int32_t a) { return (int32_t)((uint32_t)0 - (uint32_t)a); }

/* FUN_0041FA3B of a rotation: its transpose (objscene_camera's rule). */
static void inverse_rot(ral_mat4 *out, const float *m)
{
    int r, c;
    ident(out);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            out->m[r * 4 + c] = m[c * 4 + r];
}

/* [0x0059F014] = T(-eye) x R, into the stage view. */
static void finish(stage_view *v, const ral_mat4 *rot, const int32_t eye[3],
                   int screen_w, int screen_h, float fishval,
                   const sim_camera_consts *k, float *rot_out)
{
    ral_mat4 t, m;
    /* [0x0059EF94], the rotation alone (0x00437080 / 0x004370EB): the car's
     * sounds are placed by it (soundfeed.h) */
    if (rot_out) memcpy(rot_out, rot->m, sizeof rot->m);
    ident(&t);
    t.m[12] = (float)neg(eye[0]);         /* FUN_00425DBE, 0x0043708D       */
    t.m[13] = (float)neg(eye[1]);
    t.m[14] = (float)neg(eye[2]);
    ral_mat4_mul(&m, &t, rot);
    stage_view_default(v, screen_w, screen_h, eye[0], eye[1], eye[2], 0.0f);
    memcpy(v->m, m.m, sizeof v->m);
    v->eye_y = eye[1];
    v->has_eye_y = 1;
    /* FUN_0044C516 at 0x0044C5B5: width x fishval x zoom x 1/163840. */
    /* Wider than 4:3 keeps the 4:3 picture's vertical view and shows more to
     * the sides (Hor+), not the same width with the top and bottom cropped. */
    if (screen_w * 3 > screen_h * 4)
        v->wedge_k = (float)screen_w / (float)(screen_h * 4 / 3);
    v->focal = (float)((double)(screen_w * 3 > screen_h * 4 ? screen_h * 4 / 3
                                                              : screen_w)
                       * fishval * 256.0 * k->focal_k);
}

void camview_main(stage_view *v, int screen_w, int screen_h,
                  sim_camera *cam, const car_state *car,
                  const sim_camera_consts *k,
                  uint32_t *cam_yaw, uint32_t *cam_extra_yaw, float *rot_out)
{
    ral_mat4 rot;
    int32_t view = cam->view;

    if (view >= 0 && view <= 3) {
        /* 0x00436F7A: the car's own rotation. */
        inverse_rot(&rot, car->matrix);
        if (view == 1 || view == 2) {
            /* 0x00436FA1: the lean - czspd / 10 clamped to [0, 1], times the
             * steering (+0x648) as radians, times 0.1; half the gap closed
             * per FRAME; the view rolled by it. */
            float s = (float)((double)car->czspd / k->lean_div);
            double target;
            int32_t a;
            if (s < 0.0f || s != s) s = 0.0f;
            if (!(s < k->one)) s = k->one;
            target = (double)car->stangv / 65536.0 / 65536.0 / k->inv2pi
                     * s * k->lean_k;
            cam->lean = (float)((target - cam->lean) * k->half + cam->lean);
            a = (int32_t)((uint32_t)(int32_t)lrint((double)cam->lean * 65536.0
                                                   * 4096.0 * k->inv2pi) << 4);
            mul_rot(&rot, k, 2, a);
        }
    } else {
        /* 0x00436E8D */
        ident(&rot);
        mul_rot(&rot, k, 1, neg(cam->yaw));
        mul_rot(&rot, k, 0, neg(cam->pitch));
        mul_rot(&rot, k, 2, neg(cam->roll));
        mul_rot(&rot, k, 1, neg(cam->vyaw));
        mul_rot(&rot, k, 0, neg(cam->vpitch));
        mul_rot(&rot, k, 2, neg(cam->vroll));
    }
    finish(v, &rot, cam->pos, screen_w, screen_h, cam->fishval, k, rot_out);
    if (cam_yaw) *cam_yaw = (uint32_t)cam->yaw;              /* [0x005BA608] */
    if (cam_extra_yaw) *cam_extra_yaw = (uint32_t)cam->vyaw; /* [0x005BA614] */
}

void camview_mirror(stage_view *v, int screen_w, int screen_h,
                    const car_state *car, const sim_camera_consts *k,
                    int32_t layout, int32_t slot, int panel, int rect[4],
                    float *rot_out)
{
    ral_mat4 mr, cm, both, rot;
    int32_t eye[3];
    int idx = (int)layout + 3 * (int)slot + (panel ? 0 : 6);

    /* 0x00453E06: the inset, scaled from 800 x 600 by MUL / DIV. */
    if (idx < 0 || idx >= 12) idx = 0;
    rect[0] = (int)((uint32_t)k->mirror_at[idx][0] * (uint32_t)screen_w / 800u);
    rect[1] = (int)((uint32_t)k->mirror_at[idx][1] * (uint32_t)screen_h / 600u);
    rect[2] = (int)(0xB3u * (uint32_t)screen_w / 800u);
    rect[3] = (int)(0x40u * (uint32_t)screen_h / 600u);
    if (screen_w * 3 > screen_h * 4) {      /* wider than 4:3: the HUD's layout (hud.c hud_x) */
        double sc = screen_h / 600.0;
        rect[0] = (int)(k->mirror_at[idx][0] * sc + (screen_w - 800.0 * sc));
        rect[2] = (int)(0xB3 * sc);
    }

    /* 0x00436D98: [0x0059EC14] x the car's matrix, inverted. */
    memcpy(mr.m, k->mirror_rot, sizeof mr.m);
    memcpy(cm.m, car->matrix, sizeof cm.m);
    ral_mat4_mul(&both, &mr, &cm);
    inverse_rot(&rot, both.m);
    /* The eye: the car's origin, 0.5 m up (0x00436DCF). */
    eye[0] = car->pos[0];
    eye[1] = (int32_t)((uint32_t)car->pos[1] + 0x8000u);
    eye[2] = car->pos[2];
    finish(v, &rot, eye, screen_w, screen_h, k->mirror_fish2, k, rot_out);
    /* FUN_0044C516's centre is half the viewport's size; the original's D3D
     * viewport puts it at the inset, which here is the inset's origin. */
    v->centre_x = (float)(rect[0] + (rect[2] >> 1));
    v->centre_y = (float)(rect[1] + (rect[3] >> 1));
}
