/* fireworks_draw.c - the fireworks' picture;
 * fireworks_draw.h. */

#include "fireworks_draw.h"

#include "plat.h"

#include <string.h>

static void vtx(plat_vertex *p, float x, float y, float z, uint32_t col,
                float u, float v)
{
    p->sx = x; p->sy = y; p->sz = z; p->rhw = 256.0f;
    p->diffuse = col; p->specular = 0u;          /* fog is off here          */
    p->tu = u; p->tv = v;
}

/* The quads of a frame, as triangles, drawn in one call (or one a full
 * buffer): the original draws each as its own polygon, but a frame holds up
 * to ~4,000 and a draw each costs the handheld its frame (the lesson).
 * The blend is additive, so the order does not change the picture. */
enum { BATCH_QUADS = 2048 };
static plat_vertex batch[BATCH_QUADS * 6];
static unsigned    nbatch;

static void flush(void)
{
    if (nbatch) plat_gfx_draw(PLAT_PRIM_TRIANGLES, batch, nbatch);
    nbatch = 0;
}

static void quad(void *ud, const fw_quad *q)
{
    plat_vertex p[4];
    int k;
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    (void)ud;
    for (k = 0; k < 4; k++)
        vtx(&p[k], q->x[k], q->y[k], q->z, q->col, q->uv[2 * k], q->uv[2 * k + 1]);
    if (nbatch + 6 > BATCH_QUADS * 6) flush();
    for (k = 0; k < 6; k++) batch[nbatch++] = p[tri[k]];
}

void fireworks_view_matrix(float out[16], const float vr[16], const float vt[16])
{
    int r, c;
    memset(out, 0, 16 * sizeof(float));
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            out[r * 4 + c] = vr[r * 4 + c];
    for (c = 0; c < 3; c++)
        out[12 + c] = vt[12] * vr[c] + vt[13] * vr[4 + c]
                    + vt[14] * vr[8 + c] + vr[12 + c];
    out[15] = 1.0f;
}

void fireworks_draw_frame(fireworks *w, fireworks_in *in, const stage_view *v,
                          uint32_t atlas, const float uv[8])
{
    if (!w->have) return;
    if (v) {
        in->focal = v->focal;
        in->cx = v->centre_x;
        in->cy = v->centre_y;
        in->zscale = stage_depth_scale(v->radius, v->detail);
    }
    if (uv) memcpy(w->uv, uv, sizeof w->uv);
    w->quad = (atlas && v && in->on) ? quad : NULL;
    if (w->quad) {
        plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);       /* MODULATEALPHA     */
        plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
        plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_ONE);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
        plat_gfx_set_state(PLAT_RS_BLEND, 1u);
        plat_gfx_tex_bind(atlas);
        plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
        plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
        plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);          /* 0x0044C46C        */
    }
    nbatch = 0;
    fireworks_frame(w, in);
    if (w->quad) flush();
}
