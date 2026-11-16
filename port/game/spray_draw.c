/* spray_draw.c - the spray's picture; spray_draw.h. */

#include "spray_draw.h"

#include "pcx.h"
#include "plat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void vtx(plat_vertex *p, float x, float y, float z, float rhw,
                uint32_t col, uint32_t spec, float u, float v)
{
    p->sx = x; p->sy = y; p->sz = z; p->rhw = rhw;
    p->diffuse = col; p->specular = spec;
    p->tu = u; p->tv = v;
}

/* The polygon buffer's specular words 1..3 as the last quad left them: the
 * original's streak writes only corner 0's (0x004BF6AE), so the other three
 * keep the last full quad's fog.  The spray's own quads are modelled; what
 * another module's quads (the car's, 0x00453883) leave between is not. */
static uint32_t last_spec;

static float uv_at(spray *s, uint32_t va)
{
    float f;
    memcpy(&f, spray_at(s, va), 4);
    return f;
}

/* 0x004BB3E3 / 0x004BBB94's quad from the camera point: the scale F / z,
 * the fog 0x0044C2FB's (255 - ramp) << 24 in the specular.  A rotated quad
 * is w = size * scale wide and w * aspect high, turned by the angle through
 * the sine table (cos = +0x400): its corners -A,-B / +B,-A / +A,+B / -B,+A
 * with A = w sin + h cos, B = w cos - h sin.  A sprite is a square of half
 * side size * scale on its sub-cell's UVs. */
static void quad(struct spray_s *s, const void *vv, int sprite,
                 const float cam[3], uint32_t cell, uint32_t angle,
                 uint32_t col, float size, float aspect)
{
    const stage_view *sv = ((const spray_view *)vv)->v;
    float sc = sv->focal / cam[2];
    float SX = cam[0] * sc + sv->centre_x, SY = sv->centre_y - cam[1] * sc;
    float sz = cam[2] * stage_depth_scale(sv->radius, sv->detail);
    uint32_t spec = (uint32_t)stage_fog_byte(sv->fog, cam[2]) << 24;
    uint32_t e = sprite ? 0x008D5C06u + cell * 16u : 0x008D5806u + cell * 16u;
    float u0 = uv_at(s, e), v0 = uv_at(s, e + 4);
    float u1 = uv_at(s, e + 8), v1 = uv_at(s, e + 12);
    plat_vertex q[4];
    static int dbg = -1;
    last_spec = spec;
    if (dbg < 0) dbg = getenv("RALLY_SPRAY_DEBUG") ? atoi(getenv("RALLY_SPRAY_DEBUG")) : 0;
    if (dbg == 1) { size *= 20.0f; col = 0xFFFF00FFu; }
    if (dbg == 2) col |= 0x00FFFFFFu;
    if (dbg == 3 && sprite) { static int n; if (n++ < 40) printf("spray quad: sprite %d cell %u col %08X size %g half %.1f px at %.0f,%.0f cam z %g\n", sprite, cell, col, size, size * sc, SX, SY, cam[2]); }

    if (sprite) {
        float w = size * sc;
        vtx(&q[0], SX - w, SY - w, sz, 256.0f, col, spec, u0, v0);
        vtx(&q[1], SX + w, SY - w, sz, 256.0f, col, spec, u1, v0);
        vtx(&q[2], SX + w, SY + w, sz, 256.0f, col, spec, u1, v1);
        vtx(&q[3], SX - w, SY + w, sz, 256.0f, col, spec, u0, v1);
    } else {
        float w = size * sc, h = w * aspect;
        float sn = s->sin_t[angle], cs = s->sin_t[angle + 0x400u];
        float A = w * sn + h * cs, B = w * cs - h * sn;
        vtx(&q[0], SX - A, SY - B, sz, 256.0f, col, spec, u0, v0);
        vtx(&q[1], SX + B, SY - A, sz, 256.0f, col, spec, u1, v0);
        vtx(&q[2], SX + A, SY + B, sz, 256.0f, col, spec, u1, v1);
        vtx(&q[3], SX - B, SY + A, sz, 256.0f, col, spec, u0, v1);
    }
    plat_gfx_draw(PLAT_PRIM_TRIANGLE_FAN, q, 4);
}

/* 0x004B5A8F's polygon: spray.c made the four corners; the fog of corner 0
 * is 0x0044C2FB at the camera z the original passes. */
static void streak(struct spray_s *s, const void *vv,
                   const spray_streak *o)
{
    const stage_view *sv = ((const spray_view *)vv)->v;
    plat_vertex q[4];
    float z;
    int k;
    static int dbg = -1;
    (void)s;
    if (dbg < 0) dbg = getenv("RALLY_SPRAY_DEBUG") ? atoi(getenv("RALLY_SPRAY_DEBUG")) : 0;
    if (dbg == 3) {
        static int n;
        if (n++ < 40)
            printf("spray streak: (%.1f,%.1f)-(%.1f,%.1f) half %.2f px col %08X z %g\n",
                   (double)(o->x[0] + o->x[1]) * 0.5, (double)(o->y[0] + o->y[1]) * 0.5,
                   (double)(o->x[2] + o->x[3]) * 0.5, (double)(o->y[2] + o->y[3]) * 0.5,
                   (double)o->w1x, o->col[0], (double)o->z[0]);
    }
    memcpy(&z, &o->fogz, 4);
    for (k = 0; k < 4; k++)
        if (!isfinite(o->x[k]) || !isfinite(o->y[k])) return;
    for (k = 0; k < 4; k++)
        vtx(&q[k], o->x[k], o->y[k], o->z[k], 256.0f, o->col[k],
            k == 0 ? (uint32_t)stage_fog_byte(sv->fog, z) << 24 : last_spec,
            o->u[k], o->v[k]);
    plat_gfx_draw(PLAT_PRIM_TRIANGLE_FAN, q, 4);
}

/* 0x004BB285: the sparks' pass blends additively */
static void second_pass(struct spray_s *s)
{
    (void)s;
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_ONE);
}

static int load_pcx(const char *path, uint32_t *px, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf;
    unsigned dw = 0, dh = 0;
    int rc;
    if (!f) {
        snprintf(err, errlen, "%s: cannot open", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)(n > 0 ? n : 1));
    if (!buf || n <= 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(err, errlen, "%s: cannot read", path);
        return -1;
    }
    fclose(f);
    memset(px, 0, 256u * 256u * 4u);
    rc = pcx_decode_request(buf, (size_t)n, px, 256u * 256u, 256u, 256u,
                            &dw, &dh, err, errlen);
    free(buf);
    return rc;
}

int spray_load_atlas(spray *s, const char *p_data, const char *p_mask,
                     char *err, size_t errlen)
{
    static uint32_t data[256 * 256], mask[256 * 256];
    const uint32_t *lv[1];
    unsigned i;

    if (load_pcx(p_data, data, err, errlen) != 0) return -1;
    if (load_pcx(p_mask, mask, err, errlen) != 0) return -1;
    /* 0x004BAE15: (data & 0xFFFFFF) | ~(mask << 8) & 0xFF000000, as
     * 0xAARRGGBB; the upload wants R, G, B, A in memory */
    for (i = 0; i < 256u * 256u; i++) {
        uint32_t a = ((mask[i] << 8) & 0xFF000000u) ^ 0xFF000000u;
        uint32_t c = (data[i] & 0x00FFFFFFu) | a;
        data[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
    lv[0] = data;
    s->atlas = plat_gfx_tex_create(256u, 256u, lv, 1);
    if (!s->atlas) {
        snprintf(err, errlen, "the spray's texture would not upload");
        return -1;
    }
    s->quad = quad;
    s->streak = streak;
    s->second_pass = second_pass;
    return 0;
}

void spray_view_matrix(spray *s, const float vr[16], const float vt[16])
{
    int r, c;
    memset(s->view, 0, sizeof s->view);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            s->view[r * 4 + c] = vr[r * 4 + c];
    for (c = 0; c < 3; c++)
        s->view[12 + c] = vt[12] * vr[c] + vt[13] * vr[4 + c]
                        + vt[14] * vr[8 + c] + vr[12 + c];
    s->view[15] = 1.0f;
}

void spray_draw_frame(spray *s, const spray_view *v)
{
    if (!s->have) return;
    if (s->atlas && v && v->v) {
        s->proj.focal = v->v->focal;
        s->proj.cx = v->v->centre_x;
        s->proj.cy = v->v->centre_y;
        s->proj.zscale = stage_depth_scale(v->v->radius, v->v->detail);
    }
    if (s->atlas) {
        plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);       /* MODULATEALPHA     */
        plat_gfx_set_state(PLAT_RS_COLORKEY, 1u);
        plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
        plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
        plat_gfx_set_state(PLAT_RS_BLEND, 1u);
        plat_gfx_tex_bind(s->atlas);
        plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
        plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
        plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);          /* 0x0044C46C        */
    }
    spray_update(s, s->atlas ? v : NULL);
    if (s->atlas) {
        plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_ONE);
        plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
    }
}
