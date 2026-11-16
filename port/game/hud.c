/* hud.c - the in-race HUD.  hud.h says what is the
 * original's and what is not; is the reading, and section 14 is
 * this port of it.  Every address below is retail's. */

#include "hud.h"
#include "arcade.h"
#include "pcx.h"
#include "plate.h"
#include "sheet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the art: setplatesall_q's hard-wired ids (python tools/hud.py plates)
 * 0xC0 -> 1800, 0xC1 -> 1802, 0xC2 -> 1798, 0xDC..0xE1 -> 1804..1814.  Every
 * one is in the grade-exempt band 1798..1999, so no grade is passed. */
#define PLATE_C0 1800u
#define PLATE_C1 1802u
#define PLATE_C2 1798u
#define PLATE_GO 1804u                  /* + 2 x olaydraw                   */

#define RECTS_VA     0x008DD370u        /* 16-byte {u, v, w, h}             */
#define PARAMS_VA    0x0057CB8Cu        /* the handling records, 0xAC each  */

/* 0x008DD9A0 + state: the bar's section colours, read as a dword at a BYTE
 * offset (MOV EAX,[EAX + 0x008DD9A0]).  Only 4, 8 and 12 are ever used. */
static uint32_t bar_colour(int32_t state)
{
    switch (state) {
    case 4:  return 0x00E12626u;        /* red: position worse              */
    case 8:  return 0x0032A126u;        /* green: no worse                  */
    case 12: return 0x00227EDCu;        /* blue: not reached                */
    default: return 0x00212463u;        /* 0x008DD9A0 itself, the frame's   */
    }
}

/* 0x008DD9B0: the pacenote arrows' widths, for centring in a 64-px cell. */
static const int32_t pace_w[8] = { 32, 34, 45, 54, 56, 51, 50, 53 };

/* ---- x87 helpers ------------------------------------------------------- */

/* FISTP under the default control word: round to nearest, ties to even. */
static int32_t fistp(double v)
{
    return (int32_t)nearbyint(v);
}

static int32_t rd32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- the quad: FUN_004CE5ED -> FUN_004CE7D0 -> FUN_004CE912 ------------
 *
 * The draw state of one call: the texture id [0x008DDA64] (here the plat_tex
 * itself), the alpha byte [0x008DD9E0] and the colour [0x008DD9F4].
 * FUN_004CE7D0 builds four 32-byte vertices from integer x0/y0/x1/y1 and
 * 16.16 u/v, divides u and v by 256.0 and 65536.0 ([0x008DDA3C],
 * [0x008DDA40]), sz 0.01 and rhw 1.0 (FUN_004CE76B), diffuse
 * alpha << 24 | (colour & 0xFFFFFF), specular 0; FUN_004CE912 scales x by
 * the display width / 800.0 and y by the height / 600.0 ([0x008DDA5C],
 * [0x008DDA60]) and indexes (0, 1, 2) (0, 2, 3). */
typedef struct {
    plat_tex tex;
    uint32_t alpha, rgb;
    int      w, h;                      /* the display                      */
} pen;

/* the HUD's triangles are collected here and sent as one draw per run
 * of one texture.  Triangles of one draw rasterise in submission order, and
 * nothing the HUD draws changes the state between them (hud_draw sets it all
 * once, before the first), so the pixels are those of one draw per quad.  The
 * run ends at a texture change, at a full array, and at the end of hud_draw
 * (hud_flush) - before upload_canvas, which touches a texture. */
#define BATCH_VERTS 2048u               /* a HUD frame is ~400; uint16 idx  */
#define BATCH_IDX   (BATCH_VERTS / 4u * 6u + 64u)

static struct {
    plat_vertex v[BATCH_VERTS];
    uint16_t    idx[BATCH_IDX];
    unsigned    nv, ni;
    plat_tex    tex;
} g_batch;

static void hud_flush(void)
{
    if (g_batch.ni == 0) return;
    plat_gfx_tex_bind(g_batch.tex);
    plat_gfx_draw_indexed(PLAT_PRIM_TRIANGLES, g_batch.v, g_batch.nv,
                          g_batch.idx, g_batch.ni);
    g_batch.nv = 0;
    g_batch.ni = 0;
}

/* Append nv vertices and ni indices (relative to the first) on texture tex. */
static void batch_add(plat_tex tex, const plat_vertex *v, unsigned nv,
                      const uint16_t *idx, unsigned ni)
{
    unsigned k;
    if (g_batch.ni && (g_batch.tex != tex || g_batch.nv + nv > BATCH_VERTS ||
                       g_batch.ni + ni > BATCH_IDX))
        hud_flush();
    g_batch.tex = tex;
    memcpy(g_batch.v + g_batch.nv, v, nv * sizeof(plat_vertex));
    for (k = 0; k < ni; k++)
        g_batch.idx[g_batch.ni + k] = (uint16_t)(idx[k] + g_batch.nv);
    g_batch.nv += nv;
    g_batch.ni += ni;
}

static void quad(const pen *pn, const float x[4], const float y[4],
                 const float u[4], const float v[4])
{
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    plat_vertex q[4];
    int k;
    uint32_t col = ((pn->alpha & 0xFFu) << 24) | (pn->rgb & 0x00FFFFFFu);
    for (k = 0; k < 4; k++) {
        /* THE PORT'S HALF PIXEL (hud.h): Direct3D samples a pre-transformed
         * quad at pixel corners, GL at centres. */
        q[k].sx  = (float)((double)x[k] * pn->w / 800.0) + 0.5f;
        q[k].sy  = (float)((double)y[k] * pn->h / 600.0) + 0.5f;
        q[k].sz  = 0.01f;
        q[k].rhw = 1.0f;
        q[k].diffuse  = col;
        q[k].specular = 0;
        q[k].tu = u[k];
        q[k].tv = v[k];
    }
    batch_add(pn->tex, q, 4, idx, 6);
}

/* Corners in FUN_004CE7D0's order: (x0,y0) (x1,y0) (x1,y1) (x0,y1). */
static void rect_uv(const pen *pn, int32_t x0, int32_t y0, int32_t x1,
                    int32_t y1, float u0, float v0, float u1, float v1)
{
    float x[4] = { (float)x0, (float)x1, (float)x1, (float)x0 };
    float y[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    float u[4] = { u0, u1, u1, u0 };
    float v[4] = { v0, v0, v1, v1 };
    quad(pn, x, y, u, v);
}

/* FUN_004CE5ED: ESI -> {u, v, w, h}, EBX/ECX = x/y.  x1 = x + |w|,
 * y1 = y + |h|; u1 = u + w, so a negative w mirrors. */
static void sprite(const hud *h, const pen *pn, int32_t i, int32_t x,
                   int32_t y)
{
    const int32_t *r;
    if (i < 0 || i >= HUD_RECTS) return;   /* the original reads on: it never
                                            * indexes past 0x60 (race-hud 1) */
    r = h->rect[i];
    rect_uv(pn, x, y, x + (r[2] < 0 ? -r[2] : r[2]),
            y + (r[3] < 0 ? -r[3] : r[3]),
            (float)r[0] / 256.0f, (float)r[1] / 256.0f,
            (float)(r[0] + r[2]) / 256.0f, (float)(r[1] + r[3]) / 256.0f);
}

/* FUN_004CE715, "untextured": every u = 0xF80000, every v = 0x80000 - one
 * point, (248, 8), of WHATEVER texture is bound, modulated by the colour.
 * x1/y1 exclusive, as the callers' INCs make them. */
static void flat(const pen *pn, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    rect_uv(pn, x0, y0, x1, y1, 248.0f / 256.0f, 8.0f / 256.0f,
            248.0f / 256.0f, 8.0f / 256.0f);
}

/* ---- loading ------------------------------------------------------------- */

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* the plate on the SURFACE its texture id is registered with.
 * setplatesall_q's FUN_00421118 calls pass the surface's size level in EAX
 * (0x00564F5C: 0 = 256, 1 = 128 - 0xC0 is 0, the 128 surfaces 0xC4..0xC7
 * are 1), and 0xC2 is 0 (0x00423C0A): tx1798, a 128x128 file, goes onto a
 * 256x256 surface at its own size, top left, the rest left as it was
 * (FUN_004199ED never upscales).  The rects
 * are in 1/256ths of the SURFACE (FUN_004CE7D0 divides by 256.0), so
 * rect 0x60 {0, 0, 128, 128} is the whole plate.  Uploaded at 128, the
 * port showed its top-left quarter at twice the size: the 4WD dial "4x
 * and clipped".  One level, as upload_canvas's surfaces: the HUD is drawn
 * at its own size. */
static plat_tex upload_on_surface(const plate *p, unsigned surf)
{
    static uint32_t out[512 * 512];
    const uint32_t *lv[1];
    unsigned x, y;

    surf *= p->scale;                  /* an "hd" override, 2x both */
    if (surf <= p->w || surf <= p->h || surf > 512u) return plate_upload(p);
    memset(out, 0, sizeof(uint32_t) * surf * surf);
    for (y = 0; y < p->h; y++)
        for (x = 0; x < p->w; x++) {
            uint32_t c = p->level[0][y * p->w + x];   /* B,G,R,A -> R,G,B,A */
            out[y * surf + x] = (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16)
                              | ((c & 0x000000ffu) << 16);
        }
    lv[0] = out;
    return plat_gfx_tex_create(surf, surf, lv, 1);
}

static int load_plate_on(plate *pl, const char *files_root, unsigned id,
                         unsigned surf, plat_tex *out, char *err, size_t errlen)
{
    if (plate_load(pl, files_root, id, PLATE_ALPHA_FORCED, NULL, err,
                   errlen) != 0)
        return -1;
    if (pl->fallback != PLATE_FALLBACK_NONE) {
        snprintf(err, errlen, "files\\b\\tx%04u.pcx is missing", id);
        return -1;
    }
    *out = surf ? upload_on_surface(pl, surf) : plate_upload(pl);
    return *out ? 0 : -1;
}

static int load_plate(plate *pl, const char *files_root, unsigned id,
                      plat_tex *out, char *err, size_t errlen)
{
    return load_plate_on(pl, files_root, id, 0, out, err, errlen);
}

/* ---- the player's car picture: FUN_00424297, called once by the level
 * load (0x00424277) after the HUD's plates.  Car c = [0x006C2134]
 * (shauncarnum1), remapped 6 -> 0x19 by FUN_00454CF7; below 0x12 it is cut
 * from files\b\tx1822.pcx (+ tx1823, the inverted alpha), else from
 * tx1824 (+ tx1825), at the tables 0x00591AE4 (x) / 0x00591B74 (y), i.e.
 * x = (c % 3) * 82, y = ((c % 18) / 3) * 42.  FUN_0041A626 copies the 82 x 42
 * block, texels as they are, into the 0xC1 plate's surface at (0xAD, 0x68):
 * sprite 0x00.  Player 2's car goes to (0xAD, 0x94), sprite 0x01 - the
 * port has no second player.  The coordinates are shifted right by
 * FUN_004051A6's texture reduction, 0 at the port's full size. */
#define PIC_W 0x52
#define PIC_H 0x2A

static int car_picture(plate *hudp, const char *files_root, int32_t car,
                       char *err, size_t errlen)
{
    plate   *src;
    int32_t  c = car == 6 ? 0x19 : car;                   /* FUN_00454CF7 */
    unsigned sx, sy, x, y;

    if (c < 0 || c >= 0x24) return 0;          /* past the tables' 36 rows */
    src = (plate *)malloc(sizeof(*src));
    if (!src) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    if (plate_load(src, files_root, c < 0x12 ? 1822u : 1824u,
                   PLATE_ALPHA_FORCED, NULL, err, errlen) != 0
        || src->fallback != PLATE_FALLBACK_NONE) {
        if (src->fallback != PLATE_FALLBACK_NONE)
            snprintf(err, errlen, "files\\b\\tx%04u.pcx is missing",
                     c < 0x12 ? 1822u : 1824u);
        free(src);
        return -1;
    }
    /* THE PORT'S: the rectangles are in SHIPPED texels, and either
     * plate may be an "hd" override at twice that (plate.scale).  Each
     * destination texel takes the box of source texels it covers: a 2x
     * picture into a 1x HUD averages 2x2, the same scale copies as the
     * original does, and a 1x picture into a 2x HUD repeats. */
    {
        unsigned ss = src->scale, ds = hudp->scale;
        unsigned n = ss > ds ? ss / ds : 1u;

        sx = (unsigned)(c % 3) * PIC_W * ss;
        sy = (unsigned)((c % 18) / 3) * PIC_H * ss;
        for (y = 0; y < PIC_H * ds; y++)
            for (x = 0; x < PIC_W * ds; x++) {
                unsigned bx = sx + x * ss / ds, by = sy + y * ss / ds;
                unsigned dx = 0xADu * ds + x, dy = 0x68u * ds + y;
                unsigned sum[4] = { 0, 0, 0, 0 }, i, j, k;
                uint32_t o = 0;

                if (bx + n > src->w || by + n > src->h
                    || dx >= hudp->w || dy >= hudp->h)
                    continue;
                for (j = 0; j < n; j++)
                    for (i = 0; i < n; i++)
                        for (k = 0; k < 4u; k++)
                            sum[k] += (src->level[0][(by + j) * src->w + bx + i]
                                       >> (8u * k)) & 0xffu;
                for (k = 0; k < 4u; k++)
                    o |= (uint32_t)((sum[k] + n * n / 2u) / (n * n)) << (8u * k);
                hudp->level[0][dy * hudp->w + dx] = o;
            }
    }
    free(src);
    plate_rechain(hudp);                 /* THE PORT'S: plate.h says why */
    return 0;
}

static int load_c1(plate *pl, const char *files_root, int32_t carnum,
                   plat_tex *out, char *err, size_t errlen)
{
    if (plate_load(pl, files_root, PLATE_C1, PLATE_ALPHA_FORCED, NULL, err,
                   errlen) != 0)
        return -1;
    if (pl->fallback != PLATE_FALLBACK_NONE) {
        snprintf(err, errlen, "files\\b\\tx%04u.pcx is missing", PLATE_C1);
        return -1;
    }
    if (car_picture(pl, files_root, carnum, err, errlen) != 0)
        return -1;
    *out = plate_upload(pl);
    return *out ? 0 : -1;
}

/* B,G,R,A (the 2D layer's canvas order) -> R,G,B,A (plat.h's) */
static void upload_canvas(plat_tex *t, const uint32_t *px, int w, int hh)
{
    static uint32_t tmp[128 * 128];
    const uint32_t *lv[1];
    int i;
    for (i = 0; i < w * hh; i++) {
        uint32_t c = px[i];
        tmp[i] = (c & 0xff00ff00u) | ((c & 0x00ff0000u) >> 16)
               | ((c & 0x000000ffu) << 16);
    }
    lv[0] = tmp;
    if (*t) plat_gfx_tex_update(*t, tmp);
    else    *t = plat_gfx_tex_create((unsigned)w, (unsigned)hh, lv, 1);
}

/* FUN_00443C7D: the route's cumulative x/z distance, 16.16, at each index:
 * dist[i] = sum over j < i of FISTP(sqrt(dx^2 + dz^2)), dist[n-1] = total.
 * The squares are FILD x87 extended; double holds them exactly for any
 * stage's distances (|d| < 2^26). */
static void build_dist(hud *h, const sim_route *r, const sim_ground_geom *g)
{
    int32_t i, acc = 0;
    free(h->dist);
    h->dist = NULL;
    h->ndist = 0;
    if (!r || !g || r->n < 2) return;
    h->dist = (uint32_t *)calloc((size_t)r->n, sizeof(uint32_t));
    if (!h->dist) return;
    for (i = 0; i + 1 < r->n; i++) {
        const int32_t *a = NULL, *b = NULL;
        int32_t pa = r->pol[i], pb = r->pol[i + 1];
        if (pa >= 0 && pa < g->npols && pb >= 0 && pb < g->npols) {
            int32_t na = rd32(g->pols + (size_t)pa * 0x40u + 4);
            int32_t nb = rd32(g->pols + (size_t)pb * 0x40u + 4);
            if (na >= 0 && na < g->npnts && nb >= 0 && nb < g->npnts) {
                a = (const int32_t *)(const void *)(g->pnts + (size_t)na * 12u);
                b = (const int32_t *)(const void *)(g->pnts + (size_t)nb * 12u);
            }
        }
        h->dist[i] = (uint32_t)acc;
        if (a && b) {
            double dx = (double)b[0] - (double)a[0];
            double dz = (double)b[2] - (double)a[2];
            acc = (int32_t)((uint32_t)acc + (uint32_t)fistp(sqrt(dx * dx + dz * dz)));
        }
    }
    h->dist[i] = (uint32_t)acc;
    h->ndist = r->n;
}

static uint32_t dist_at(const hud *h, int32_t i)
{
    return (h->dist && i >= 0 && i < h->ndist) ? h->dist[i] : 0u;
}

/* (a - b) SHR 1, FILD, / 65536.0 / 1609.3457 ([0x0057B01C], [0x0057AF74]),
 * FADD ST0 - miles, as initoverlay_q and FUN_004588E5 both write it. */
static float miles(uint32_t diff)
{
    int32_t half = (int32_t)(diff >> 1);
    return (float)((double)half / 65536.0 / 1609.345703125 * 2.0);
}

int hud_init(hud *h, const char *files_root, const ral_image *im,
             const char *jade_path, const car_params *p,
             const float *sin_table, const sim_route *route,
             const sim_ground_geom *geom, int32_t carnum, int video,
             char *err, size_t errlen)
{
    const unsigned char *t;
    plate *pl;
    int i;

    memset(h, 0, sizeof(*h));
    h->sin_table = sin_table;
    h->video = video;
    t = ral_image_at(im, RECTS_VA, HUD_RECTS * 16u);
    if (!t) {
        snprintf(err, errlen, "the sprite table 0x%08X is not in the image",
                 RECTS_VA);
        return -1;
    }
    for (i = 0; i < HUD_RECTS; i++) {
        int k;
        for (k = 0; k < 4; k++) h->rect[i][k] = ral_image_i32(t + i * 16 + k * 4);
    }
    /* +0xCC, FUN_00444EBA: [0x0057CBA4 + (car & 0x1F) * 0xAC] - the handling
     * record's +0x18, drive_class: a 4WD car gets the 7,000 rpm dial. */
    h->b.dial2 = p ? p->drive_class : 0;
    build_dist(h, route, geom);
    h->route = route;
    h->geom = geom;
    h->shown_id = -1;

    if (video) {
        pl = (plate *)malloc(sizeof(*pl));
        if (!pl) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        if (load_plate(pl, files_root, PLATE_C0, &h->tex_c0, err, errlen) != 0
            || load_c1(pl, files_root, carnum, &h->tex_c1, err, errlen) != 0
            || load_plate_on(pl, files_root, PLATE_C2, 256u, &h->tex_c2,
                             err, errlen) != 0) {
            free(pl);
            return -1;
        }
        for (i = 0; i < 6; i++)
            if (load_plate(pl, files_root, PLATE_GO + 2u * (unsigned)i,
                           &h->tex_go[i], err, errlen) != 0) {
                free(pl);
                return -1;
            }
        free(pl);
        h->have = 1;
    }

    /* THE RACE-OVER PROMPT'S STAND-IN FONT - JADE's left-aligned record,
     * 0x00517978, as hud_standin.c had it.  The banner is FNT22's
     * (hud_banner_load). */
    h->jade = jade_path ? slurp(jade_path, &h->jade_len) : NULL;
    if (h->jade) {
        h->font.first = 0x20;
        h->font.last = 0x80;
        h->font.align = GFX2D_ALIGN_LEFT;
        h->font.gap = 1;
        h->font.line_h = 9;
        h->font.esc_gap = 6;
        h->font.bundle = h->jade;
        h->font.bundle_len = h->jade_len;
        h->have_font = 1;
    }
    gfx2d_surface_init(&h->sb, h->banner_px, 128, 128);
    gfx2d_surface_init(&h->s2, h->canvas2, 256, 16);
    return 0;
}

/* loadgamefonts_q, 0x0041A4AC - the banner's half:
 *   [0x00512C90] = 0x21 (trim; 0x20: MERGE), FUN_00411D4C(FNT22_00.PCX):
 *     0x0041210D  file 00 decoded (indices) and kept aside, counter 1
 *     0x004121B0  file 01 decoded; every byte |= (kept & 0x0F) << 4
 *     the cut (sheet.h) of the merged picture; counter 2: no file, stop
 *   0x004121EF  palette[0] = 0; palette[i] = palette[i & 0x0F], its alpha
 *               byte = (i & 0xF0) | (i >> 4)
 *   FUN_00411C8F  the palette into the LUT [0x00517E00] - the banner's
 *   (0x0041A586 makes a brightened second LUT, [0x00517E60], for records
 *   0x00517E4C.. - the corner labels', built below.)
 * The palette is the last file's, 01's (0x004116C7 loads it each time). */
int hud_banner_load(hud *h, const ral_image *im, const char *p00,
                    const char *p01, char *err, size_t errlen)
{
    const char *path[2];
    uint8_t *idx[2] = { NULL, NULL };
    uint32_t pal[256], *px = NULL;
    unsigned w[2] = { 0, 0 }, hh[2] = { 0, 0 }, i;
    const unsigned char *r;
    sheet_builder b;
    int k, got, rc = -1;

    path[0] = p00;
    path[1] = p01;
    h->have_banner = 0;
    r = ral_image_at(im, 0x00517E2Cu, 0x20);
    if (!r) {
        snprintf(err, errlen, "the font record 0x00517E2C is not in the image");
        return -1;
    }
    for (k = 0; k < 2; k++) {
        size_t len;
        uint8_t *buf = path[k] ? (uint8_t *)slurp(path[k], &len) : NULL;
        if (!buf) {
            snprintf(err, errlen, "FNT22_0%d.PCX is missing", k);
            goto out;
        }
        idx[k] = (uint8_t *)malloc(1024u * 1024u);
        if (!idx[k] || pcx_decode_index(buf, len, idx[k], 1024u * 1024u, pal,
                                        &w[k], &hh[k], err, errlen) != 0) {
            free(buf);
            if (idx[k]) snprintf(err + strlen(err), errlen - strlen(err),
                                 " (FNT22_0%d.PCX)", k);
            goto out;
        }
        free(buf);
    }
    if (w[0] != w[1] || hh[0] != hh[1]) {
        snprintf(err, errlen, "FNT22_00 is %ux%u, FNT22_01 %ux%u", w[0], hh[0],
                 w[1], hh[1]);
        goto out;
    }
    px = (uint32_t *)malloc((size_t)w[1] * hh[1] * sizeof *px);
    if (!px) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    for (i = 0; i < w[1] * hh[1]; i++)
        px[i] = (uint8_t)(idx[1][i] | ((idx[0][i] & 0x0Fu) << 4));
    sheet_builder_init(&b);
    got = sheet_cut(&b, px, (int)w[1], (int)hh[1], pal);
    if (got == r[1] - r[0] + 1)
        h->fnt22 = sheet_bundle(&b, &h->fnt22_len);
    sheet_builder_free(&b);
    if (got != r[1] - r[0] + 1) {
        snprintf(err, errlen, "FNT22 cuts to %d glyphs, not %d", got,
                 r[1] - r[0] + 1);
        goto out;
    }
    if (!h->fnt22) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    /* 0x004121EF.  The LUT is the 2D layer's 0xAARRGGBB. */
    pal[0] = 0;
    for (i = 0; i < 256; i++)
        h->fnt22_lut[i] = (pal[i & 0x0Fu] & 0x00FFFFFFu)
                        | (uint32_t)((i & 0xF0u) | (i >> 4)) << 24;
    h->bfont.first    = r[0];
    h->bfont.last     = r[1];
    h->bfont.align    = r[2];
    h->bfont.flags    = r[3];
    h->bfont.gap      = ral_image_i32(r + 0x04);
    h->bfont.line_h   = ral_image_i32(r + 0x08);
    h->bfont.y_origin = ral_image_i32(r + 0x0C);
    h->bfont.esc_gap  = ral_image_i32(r + 0x10);
    h->bfont.lut      = h->fnt22_lut;
    h->bfont.bundle   = h->fnt22;
    h->bfont.bundle_len = h->fnt22_len;
    /* 0x0041A586: the brightened copy - byte 0 + 0x40, byte 1 + 0x20, byte 2
     * + 0x20, each saturating at 0xFF, the alpha byte left (B, G, R of the
     * LUT's 0xAARRGGBB) - is records 0x00517E4C.. 's LUT [0x00517E60].  The
     * record 0x00517E4C: the banner's but left aligned (+0x02 = 0), with its
     * own LUT and the same bundle (0x0041A5BC). */
    for (i = 0; i < 256; i++) {
        uint32_t c = h->fnt22_lut[i];
        uint32_t bl = (c & 0xFFu) + 0x40u, gr = ((c >> 8) & 0xFFu) + 0x20u,
                 rd = ((c >> 16) & 0xFFu) + 0x20u;
        h->fnt22_lut_lit[i] = (c & 0xFF000000u) | (bl > 0xFFu ? 0xFFu : bl)
                            | (gr > 0xFFu ? 0xFFu : gr) << 8
                            | (rd > 0xFFu ? 0xFFu : rd) << 16;
    }
    r = ral_image_at(im, 0x00517E4Cu, 0x20);
    if (!r) {
        snprintf(err, errlen, "the font record 0x00517E4C is not in the image");
        goto out;
    }
    h->lfont = h->bfont;
    h->lfont.first    = r[0];
    h->lfont.last     = r[1];
    h->lfont.align    = r[2];
    h->lfont.flags    = r[3];
    h->lfont.gap      = ral_image_i32(r + 0x04);
    h->lfont.line_h   = ral_image_i32(r + 0x08);
    h->lfont.y_origin = ral_image_i32(r + 0x0C);
    h->lfont.esc_gap  = ral_image_i32(r + 0x10);
    h->lfont.lut      = h->fnt22_lut_lit;
    h->lfont.field_w  = 0x80;           /* [0x00517E68] = 0x80 (0x004594EF) */
    h->label_valid = 0;
    {
        const char *e = getenv("RALLY_GAP_NAMES");
        long nb, na;
        char c;
        h->diag_names = 0;
        if (e && sscanf(e, "%ld:%ld%c", &nb, &na, &c) == 2) {
            h->diag_names = 1;
            h->diag_id[0] = (int32_t)nb;
            h->diag_id[1] = (int32_t)na;
            printf("hud: RALLY_GAP_NAMES - both gap panels and the corner "
                   "labels with lexicon ids %ld / %ld (a DIAGNOSTIC)\n", nb, na);
        }
    }
    memset(h->label, ' ', sizeof h->label);
    h->label[0][8] = h->label[1][8] = 0;
    h->have_banner = 1;
    rc = 0;
out:
    free(idx[0]);
    free(idx[1]);
    free(px);
    return rc;
}

void hud_free(hud *h)
{
    int i;
    if (h->tex_c0) plat_gfx_tex_destroy(h->tex_c0);
    if (h->tex_c1) plat_gfx_tex_destroy(h->tex_c1);
    if (h->tex_c2) plat_gfx_tex_destroy(h->tex_c2);
    for (i = 0; i < 6; i++) if (h->tex_go[i]) plat_gfx_tex_destroy(h->tex_go[i]);
    if (h->tex_banner) plat_gfx_tex_destroy(h->tex_banner);
    free(h->fnt22);
    if (h->tex_prompt) plat_gfx_tex_destroy(h->tex_prompt);
    free(h->dist);
    free(h->jade);
    if (h->tex_map) plat_gfx_tex_destroy(h->tex_map);
    free(h->map);
    free(h->line_x);
    free(h->line_z);
    memset(h, 0, sizeof(*h));
}

/* ---- the map disc --------------- */

#define HUD_PI 3.14159265358979323846   /* FLDPI */

/* 0x0041EA8B -> 0x0041EC71: idx = (angle + 0x80000) >>> 20 into the
 * 4,096-step sine table, x' = x c + z s, z' = z c - x s, each FSTP to a
 * float and then FISTP. */
static void map_rot(const float *sn, int32_t x, int32_t z, int32_t angle,
                    int32_t *ox, int32_t *oz)
{
    uint32_t i = ((uint32_t)angle + 0x80000u) >> 20;
    double s = sn[i], c = sn[i + SIM_SIN_QUARTER];
    float fx = (float)((double)x * c + (double)z * s);
    float fz = (float)((double)z * c - (double)x * s);
    *ox = fistp(fx);
    *oz = fistp(fz);
}

static const int32_t *map_anchor(const hud *h, int32_t ri)
{
    const sim_ground_geom *g = h->geom;
    int32_t p = h->route->pol[ri], n;
    if (p < 0 || p >= g->npols) return NULL;
    n = rd32(g->pols + (size_t)p * 0x40u + 4);
    if (n < 0 || n >= g->npnts) return NULL;
    return (const int32_t *)(const void *)(g->pnts + (size_t)n * 12u);
}

static uint32_t map_code(const hud *h, int32_t ri)      /* pols +0x2C */
{
    int32_t p = h->route->pol[ri];
    if (p < 0 || p >= h->geom->npols) return 0;
    return (uint32_t)rd32(h->geom->pols + (size_t)p * 0x40u + 0x2C);
}

/* FUN_004441C2: route indices a..b inclusive carried by a similarity
 * transform onto the segment between the two markers' encoded points
 **/
static void map_span(hud *h, int32_t a, int32_t b)
{
    const int32_t *qa = map_anchor(h, a), *qb = map_anchor(h, b);
    uint32_t ca = map_code(h, a), cb = map_code(h, b);
    int32_t pax = (int32_t)((ca & 0xFFFFu) << 14);
    int32_t paz = -(int32_t)((ca >> 16) << 14);
    int32_t pbx = (int32_t)((cb & 0xFFFFu) << 14);
    int32_t pbz = -(int32_t)((cb >> 16) << 14);
    double dpx, dpz, dqx, dqz, lq;
    float scale;
    int32_t angp, angq, i;

    if (!qa || !qb) return;
    dpx = (double)(int32_t)((uint32_t)pbx - (uint32_t)pax);
    dpz = (double)(int32_t)((uint32_t)pbz - (uint32_t)paz);
    dqx = (double)(int32_t)((uint32_t)qb[0] - (uint32_t)qa[0]);
    dqz = (double)(int32_t)((uint32_t)qb[2] - (uint32_t)qa[2]);
    lq = sqrt(dqx * dqx + dqz * dqz);
    scale = lq > 0.0 ? (float)(sqrt(dpx * dpx + dpz * dpz) / lq) : 0.0f;
    angp = (int32_t)((uint32_t)fistp(atan2(dpx, dpz) / HUD_PI * 0.125 * 4294967296.0) << 2);
    angq = (int32_t)((uint32_t)fistp(atan2(dqx, dqz) / HUD_PI * 0.125 * 4294967296.0) << 2);
    for (i = a; i <= b; i++) {
        const int32_t *r = map_anchor(h, i);
        int32_t x, z;
        if (!r) continue;
        map_rot(h->sin_table, (int32_t)((uint32_t)r[0] - (uint32_t)qa[0]),
                (int32_t)((uint32_t)r[2] - (uint32_t)qa[2]),
                (int32_t)(0u - (uint32_t)angq), &x, &z);
        x = fistp((double)x * (double)scale);
        z = fistp((double)z * (double)scale);
        map_rot(h->sin_table, x, z, angp, &x, &z);
        h->line_x[i] = (int32_t)((uint32_t)x + (uint32_t)pax);
        h->line_z[i] = (int32_t)(0u - ((uint32_t)z + (uint32_t)paz));
    }
}

int hud_map_load(hud *h, const char *files_root, int level, char *err,
                 size_t errlen)
{
    char path[1024];
    size_t len = 0;
    int32_t a, b, n;

    h->have_map = 0;
    if (!h->route || !h->geom || h->route->n < 2 || !h->sin_table) {
        snprintf(err, errlen, "no route: no map");
        return -1;
    }
    snprintf(path, sizeof path, "%s/maps/MAP%02d.SQ", files_root, level);
    h->map = (uint8_t *)slurp(path, &len);
    if (!h->map) {
        snprintf(path, sizeof path, "%s/maps/map%02d.sq", files_root, level);
        h->map = (uint8_t *)slurp(path, &len);
    }
    if (!h->map) {
        snprintf(err, errlen, "files\\maps\\map%02d.sq will not open", level);
        return -1;
    }
    h->map_tiles = len >= 16 ? rd32(h->map + 12) : -1;
    if (h->map_tiles < 0 || len != 16u + 32768u + (size_t)h->map_tiles * 768u) {
        snprintf(err, errlen, "map%02d.sq: %lu bytes for %ld tiles", level,
                 (unsigned long)len, (long)h->map_tiles);
        free(h->map);
        h->map = NULL;
        return -1;
    }
    /* FUN_00444140: from index 0, each span up to the next marker with a
     * non-zero +0x2C; the tail after the last is not filled (zero). */
    n = h->route->n;
    h->line_x = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    h->line_z = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    if (!h->line_x || !h->line_z) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (a = 0;;) {
        for (b = a + 1; b < n && map_code(h, b) == 0; b++) {}
        if (b >= n) break;
        map_span(h, a, b);
        a = b;
    }
    /* DIAGNOSTIC: RALLY_MAP_DUMP=<file> writes each route index's offset
     * line point and anchor (x, z) - tools/map_check.py's input */
    if (getenv("RALLY_MAP_DUMP")) {
        FILE *f = fopen(getenv("RALLY_MAP_DUMP"), "w");
        if (f) {
            for (a = 0; a < n; a++) {
                const int32_t *v = map_anchor(h, a);
                fprintf(f, "%ld %ld %ld %ld %ld\n", (long)a, (long)h->line_x[a],
                        (long)h->line_z[a], v ? (long)v[0] : 0L, v ? (long)v[2] : 0L);
            }
            fclose(f);
        }
    }
    h->have_map = 1;
    return 0;
}

/* FUN_0041AC65: the 128x128 window, 8x8 tiles of the 128x128 grid from
 * (win_x, win_y) / 16; outside the grid or an empty cell, opaque black
 * ([0x574275]).  A texel is B, G, R in the file. */
static void map_fill(hud *h)
{
    const uint8_t *cell = h->map + 16, *tiles = h->map + 16 + 32768;
    int32_t tx0 = h->win_x >> 4, ty0 = h->win_y >> 4, r, c, x, y;

    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            uint32_t tx = (uint32_t)(tx0 + c), ty = (uint32_t)(ty0 + r);
            const uint8_t *t = NULL;
            if (tx < 128u && ty < 128u) {
                unsigned k = (unsigned)cell[(ty * 128u + tx) * 2u]
                           | ((unsigned)cell[(ty * 128u + tx) * 2u + 1u] << 8);
                if (k && (int32_t)k <= h->map_tiles)
                    t = tiles + (size_t)(k - 1u) * 768u;
            }
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    uint32_t px = 0xFF000000u;
                    if (t) {
                        const uint8_t *q = t + (y * 16 + x) * 3;
                        px |= ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
                    }
                    h->map_px[(r * 16 + y) * 128 + c * 16 + x] = px;
                }
        }
}

void hud_map_frame(hud *h, const car_state *car, int32_t heading)
{
    const int32_t *v;
    int32_t nb, mx, my, dx, dy;
    uint32_t t;

    if (!h->have_map) return;
    /* FUN_004592B8 */
    nb = car->nearestb;
    if (h->route->n < 2 || nb < 0 || nb >= h->route->n) return;
    v = map_anchor(h, nb);
    if (!v) return;
    mx = (int32_t)((uint32_t)h->line_x[nb] + (uint32_t)(int32_t)
         (((int64_t)(int32_t)((uint32_t)car->pos[0] - (uint32_t)v[0]) * 96) / 1000));
    my = (int32_t)((uint32_t)h->line_z[nb] + (uint32_t)(int32_t)
         (((int64_t)(int32_t)(0u - ((uint32_t)car->pos[2] - (uint32_t)v[2])) * 96) / 1000));
    dx = (mx >> 16) - h->win_x;
    dy = (my >> 16) - h->win_y;
    if (!h->win_valid || !(dx >= 0x34 && dx < 0x4C && dy >= 0x34 && dy < 0x4C)) {
        h->win_x = (int32_t)(((uint32_t)(mx >> 16) + 8u - 64u) & ~15u);
        h->win_y = (int32_t)(((uint32_t)(my >> 16) + 8u - 64u) & ~15u);
        map_fill(h);
        h->win_valid = 1;
        /* FUN_00458FAA: re-uploaded only on a refill (+0xADC) */
        if (h->video) upload_canvas(&h->tex_map, h->map_px, 128, 128);
    }
    h->car_wx = (int32_t)(((uint32_t)mx - ((uint32_t)h->win_x << 16) - 0x300000u) << 1);
    h->car_wy = (int32_t)(((uint32_t)my - ((uint32_t)h->win_y << 16) - 0x300000u) << 1);
    /* FUN_00458BA8: +0x34 / +0x38 the car's place in the window less 48,
     * +0x3C the heading in radians (FLDPI, x 0.5, x 2^-16) */
    h->map_x = (float)((double)h->car_wx * 0.5 * (1.0 / 65536.0));
    h->map_y = (float)((double)h->car_wy * 0.5 * (1.0 / 65536.0));
    t = (0u - (uint32_t)heading) >> 14;
    h->map_head = (float)((double)t * HUD_PI * 0.5 * (1.0 / 65536.0));
}

/* initoverlay_q, 0x00458764 (player 1's half):
 *   if (route n - 2 < 0 (JS) or numcheckposits < 1 (JS)):
 *       sections = 1 ; len[0] = total = 1.0
 *   else: sections = numcheckposits ; total = 0
 *       for i < sections: len[i] = miles(dist[cp[i+1]] - dist[cp[i]]) ;
 *                         total += len[i] ; state[i] = 0
 *   siz / alp / top = 0x40 / 0x80 / 0x50 ; pacenotes cleared ; done = 0 */
void hud_race_start(hud *h, const sim_race *rc)
{
    hud_block *b = &h->b;
    int32_t i, n = rc ? rc->numcheckposits : 0;
    if (!rc || !rc->route || rc->route->n - 2 < 0 || n < 1) {
        b->sections = 1;
        b->len[0] = b->total = 1.0f;
        b->state[0] = 0;
    } else {
        if (n > HUD_SECTIONS) n = HUD_SECTIONS;  /* the original writes on   */
        b->sections = n;
        b->total = 0.0f;
        for (i = 0; i < n; i++) {
            int32_t a = rc->cp[i + 1 < SIM_RACE_CHECKPOINTS ? i + 1 : i];
            b->len[i] = miles(dist_at(h, a) - dist_at(h, rc->cp[i]));
            b->total = b->total + b->len[i];
            b->state[i] = 0;
        }
    }
    b->siz = 0x40;
    b->alp = 0x80;
    b->top = 0x50;
    memset(b->pace, 0, sizeof(b->pace));
    b->pace_x = 0x320 / 2;
    b->done = 0.0f;
    b->olaydraw = -1;
    h->shown_id = -1;
}

/* ---- the frame ----------------------------------------------------------- */

/* FUN_0045941D for player 1, after FUN_0041A608 has made the id's text
 * (str_emit_value_q - here the lexicon, lexicon.h):
 *   clear 3 x [0x00517DF4] (= 25) rows of the 0xC4 surface to 0
 *   [0x00517E48] = 0x168 (the record's field, 360)
 *   draw_text(0x00517E2C, x 4, y 0); x -= 0x78, y += 25; twice more
 * The record is centred (+0x02 = 3), so the line is laid out once in 360 px
 * and each draw leaves a different 120-px slice of it in x 4..123. */
static void banner_text(hud *h, const char *str)
{
    int line, x = 4, y = 0;
    int rows = 3 * h->bfont.line_h;
    if (rows > h->sb.h) rows = h->sb.h;
    memset(h->banner_px, 0, (size_t)rows * (size_t)h->sb.w * sizeof(uint32_t));
    h->bfont.field_w = 0x168;
    for (line = 0; line < 3; line++) {
        gfx2d_text(&h->sb, &h->bfont, str, x, y);
        x -= 0x78;
        y += h->bfont.line_h;
    }
}

/* FUN_00459492 (called by FUN_00459163 when the strings differ from the
 * copies at 0x006845A4..): clear 2 x [0x00517DF4] + 3 rows from row 3 x 25
 * (to the surface's last), then draw [0x0068458C] at (4, 75) and [0x00684598]
 * at (4, 100) with record 0x00517E4C (left aligned, brightened LUT). */
static void label_text(hud *h)
{
    int y0 = 3 * h->bfont.line_h, rows = 2 * h->bfont.line_h + 3, k;
    if (y0 > h->sb.h) y0 = h->sb.h;
    if (y0 + rows > h->sb.h) rows = h->sb.h - y0;
    memset(h->banner_px + (size_t)y0 * (size_t)h->sb.w, 0,
           (size_t)rows * (size_t)h->sb.w * sizeof(uint32_t));
    for (k = 0; k < 2; k++)
        gfx2d_text(&h->sb, &h->lfont, h->label[k], 4,
                   y0 + k * h->bfont.line_h);
}

/* What the two buffers hold, by game mode.
 *  gmode 1: FUN_00435688 (0x004356BF..0x00435837) fills both with spaces,
 *    then, for the car ahead (-> [0x00684598], when currpos != 1) and the car
 *    behind (-> [0x0068458C], when != 90), formats the roster's name id
 *    through 0x00401F8F into the 8 spaces at 0x008E80D0 and copies the two
 *    dwords: the lexicon's text, TRUNCATED to 8 characters.
 *  gmode 2: FUN_00458AC1 (0x00458B2F / 0x00458B89) -> 0x00458F61: " CAR nn "
 *    with nn = the start list's car number ([0x005B9394 + 4 p] behind,
 *    [0x005B9390 + 4 p] = entry p - 1 ahead), divided by 10 into two digits;
 *    spaces when the panel's make is -1.
 *  every other mode: messagesystem_init's spaces. */
static void label_strings(hud *h, const sim_race *rc, int32_t p)
{
    const hud_block *b = &h->b;
    int k;
    memset(h->label, ' ', sizeof h->label);
    h->label[0][8] = h->label[1][8] = 0;
    if (!rc && !h->diag_names) return;
    if (h->diag_names || rc->mode.gmode == 1) {
        for (k = 0; k < 2; k++) {
            int32_t make = k ? b->make_ahead : b->make_behind;
            int32_t id = k ? b->name_ahead : b->name_behind;
            const char *s;
            size_t n;
            if (make == -1 || !h->lex) continue;
            if (h->diag_names && id < 0) {   /* the diagnostic's " CAR nn " */
                char *o = h->label[k];
                o[1] = 'C'; o[2] = 'A'; o[3] = 'R';
                o[5] = (char)('0' + (-id) / 10);
                o[6] = (char)('0' + (-id) % 10);
                continue;
            }
            s = lexicon_text(h->lex, id);
            n = strlen(s);
            memcpy(h->label[k], s, n > 8 ? 8 : n);
        }
    } else if (rc->mode.gmode == 2 && rc->arcade) {
        const sim_arcade *a = rc->arcade;
        for (k = 0; k < 2; k++) {
            int32_t make = k ? b->make_ahead : b->make_behind;
            int32_t e = k ? p - 1 : p, v;
            char *o = h->label[k];
            if (make == -1 || e < 0 || e >= SIM_ARCADE_LIST) continue;
            v = a->carno[e];
            o[1] = 'C'; o[2] = 'A'; o[3] = 'R';
            o[5] = (char)('0' + v / 10);
            o[6] = (char)('0' + v % 10);
        }
    }
}

static int text_width(hud *h, gfx2d_surface *s, const char *str)
{
    gfx2d_font m = h->font;
    m.align = GFX2D_ALIGN_MEASURE;
    m.field_w = 0;
    m.lut = h->ink;
    gfx2d_text(s, &m, str, 0, 0);
    return m.field_w;
}

/* THE PORT'S race-over prompt (hud.h): one line of JADE, centred in the
 * 180 x 12.5 texels its quad shows. */
static void canvas_text(hud *h, gfx2d_surface *s, const char *str)
{
    int x;
    gfx2d_clear(s, 0x00000000u);
    if (!h->have_font || !*str) return;
    x = (180 - text_width(h, s, str)) / 2;
    h->shadow[0] = 0xFF000000u;
    h->font.lut = h->shadow;
    gfx2d_text(s, &h->font, str, x + 1, 3);
    h->ink[0] = 0xFFFFFFFFu;
    h->font.lut = h->ink;
    gfx2d_text(s, &h->font, str, x, 2);
}

/* the replay viewer's strip text - the race-over prompt's font, a
 * shadowed line at (x, y) on a caller's surface, and its width.  Draws nothing
 * when the font would not load (hud_init says so). */
void hud_text(hud *h, gfx2d_surface *s, const char *str, int x, int y)
{
    if (!h->have_font || !*str) return;
    h->shadow[0] = 0xFF000000u;
    h->font.lut = h->shadow;
    gfx2d_text(s, &h->font, str, x + 1, y + 1);
    h->ink[0] = 0xFFFFFFFFu;
    h->font.lut = h->ink;
    gfx2d_text(s, &h->font, str, x, y);
}

int hud_text_width(hud *h, gfx2d_surface *s, const char *str)
{
    return h->have_font ? text_width(h, s, str) : 0;
}

/* FUN_00458D93 for one player, the frame's countdown:
 *   siz = 0 ; (alp: SUB 0, unchanged) ; top = 0
 *   fs count != 0 -> ret ; u = alivet r - 300, JS -> ret ; clamp 0..1500
 *   d = u / 100, f = u % 100 (DIV) ; [0x00684564] = min(d, 5)
 *   olaydraw = d < 6 ? d : -1
 *   e = (f == 0 && olaydraw != -1) ? 0 : 2f        (the first: an upload)
 *   GO! ([0x00684564] == 5): siz = clamp(256 - e, 0, 256) ; alp = 256 - e ;
 *       top = (siz - siz*128/48) SAR 1 ; siz = siz*128/48 (IMUL / IDIV)
 *   else: siz = alp = 256 - e ; top = 0 */
static void countdown_frame(hud_block *b, const car_state *car)
{
    int32_t u, e, kind, s0;
    uint32_t d, f;
    b->siz = 0;
    b->top = 0;
    if (car->fs_count != 0) return;
    u = (int32_t)((uint32_t)car->alivet_r - 300u);
    if (u < 0) return;
    if (u > 0x5DC) u = 0x5DC;
    d = (uint32_t)u / 100u;
    f = (uint32_t)u % 100u;
    kind = d < 5u ? (int32_t)d : 5;
    b->olaydraw = d < 6u ? (int32_t)d : -1;
    e = (f == 0 && b->olaydraw != -1) ? 0 : (int32_t)(2u * f);
    if (kind == 5) {
        s0 = 0x100 - e;
        if (s0 < 0) s0 = 0;
        if (s0 >= 0x100) s0 = 0x100;
        b->alp = 0x100 - e;
        b->siz = s0 * 0x80 / 0x30;
        b->top = (s0 - b->siz) >> 1;
    } else {
        b->siz = 0x100 - e;
        b->alp = 0x100 - e;
    }
}

void hud_frame(hud *h, const car_state *car, sim_race *rc, uint32_t slot)
{
    hud_block *b = &h->b;
    const uint8_t *raw = (const uint8_t *)car;
    float tacho;
    double sp;
    int32_t i;

    /* FUN_004521B6: [0x0068424A] = car+0x220 (0x004521D8); the panel bit
     * [0x00684256] = flags & 0x40. */
    b->flags = rd32(raw + 0x220);
    b->panel = b->flags & 0x40;

    /* FUN_00459163, the message pickup: a new msgtopre is rendered
     * (FUN_0041A608), then +0x724 = id, msgtopre = 0, +0x720 = 1,
     * +0x728 = 1. */
    if (rc) {
        sim_race_msg *m = &rc->msg[slot & 7u];
        if (m->topre != 0) {
            int32_t id = m->topre;
            if (h->video && h->have_banner)
                banner_text(h, h->lex ? lexicon_text(h->lex, id) : "");
            h->shown_id = id;
            h->pickups++;
            m->w724 = id;
            m->topre = 0;
            m->w720 = 1;
            m->w728 = 1;
            if (h->video && h->have_banner)
                upload_canvas(&h->tex_banner, h->banner_px, 128, 128);
        }
    }

    /* 0x004523BF: the block is filled only while [0x005FE9D8] is 0. */
    if (rc && rc->mode.no_route_q != 0) return;

    /* FUN_004588E5.  +0x10: FISTP of car+0x718, which FUN_0045E872 sets to
     * revs, or 0.0 if below ([0x005C6E30]). */
    tacho = car->revs;
    if (tacho < 0.0f) tacho = 0.0f;
    b->tacho = fistp(tacho);
    /* +0x14: car+0xAB0 (0x0043C14D) = FISTP(czspd x 2.25, FSTP float, then
     * x 1.6093457 if car+0x220 & 0x100). */
    sp = (double)(float)(car->czspd * 2.25f);
    if (b->flags & 0x100) sp *= 1.6093456745147705;
    b->speed = fistp(sp);
    b->gear = car->gear;
    /* +0x6C: (dist[nearestb] - dist[cp[0]]), JAE else 0, in miles */
    if (rc) {
        uint32_t a = dist_at(h, car->nearestb), z = dist_at(h, rc->cp[0]);
        b->done = miles(a >= z ? a - z : 0u);
    }
    b->clock_ms = car->catitken * 10;
    /* +0x1C: car+0xA0C (currpos); FUN_00458A66 then writes -1 in gmode 0.
     * +0x2C = -(car+0xA10), +0x24 = car+0xA14, +0x30 = car+0xA18, +0x28 =
     * car+0xA1C (0x0045896F..0x0045899F): FUN_00435688's in gmode 1
     * messagesystem_init's -1 until it runs and in every other
     * mode but 2.  The name ids, +0x04 / +0x00 = car+0xA20 / +0xA24, feed
     * only the corner labels (label_strings). */
    b->position = rc ? rc->currpos[slot & 7u] : -1;
    if (!rc || rc->mode.gmode == 0) b->position = -1;
    if (rc) {
        b->gap_ahead = (int32_t)(0u - (uint32_t)rc->gap_ahead[slot & 7u]);
        b->gap_behind = rc->gap_behind[slot & 7u];
        b->make_ahead = rc->type_ahead[slot & 7u];
        b->make_behind = rc->type_behind[slot & 7u];
        b->name_ahead = rc->name_ahead[slot & 7u];
        b->name_behind = rc->name_behind[slot & 7u];
    } else {
        b->gap_behind = b->gap_ahead = -1;
        b->make_behind = b->make_ahead = -1;
    }
    /* FUN_00458AC1, gmode 2: the clock is the TIME LEFT, catileft x
     * 10 (0x00458AE0); +0x1C is car+0x654 arcadepo (0x00458AD4), the
     * streamer's position.  Then the two panels: behind, the gap
     * car+0x634 and the make (+0x24) of the slot the start list names at
     * the position, none at the field's last; ahead, MINUS car+0x630 and
     * the slot at position - 1, none in first.  The " CAR nn" strings
     * 0x00458F61 builds beside them are the corner labels' (label_strings). */
    if (rc && rc->mode.gmode == 2) {
        b->clock_ms = car->catileft * 10;
        b->position = car->arcadeposition;
        if (rc->arcade) {
            const sim_arcade *a = rc->arcade;
            int32_t p = car->arcadeposition;
            const car_state *w;
            b->gap_behind = car->arc_gap_behind_ms;
            if (p >= 0 && p < SIM_ARCADE_LIST
                && (w = rc->rec[a->slot[p] & 7]) != NULL)
                b->make_behind = w->carnum1;
            if (p == a->n) b->make_behind = -1;
            b->gap_ahead = (int32_t)(0u - (uint32_t)car->arc_gap_ahead_ms);
            if (p >= 1 && p <= SIM_ARCADE_LIST
                && (w = rc->rec[a->slot[p - 1] & 7]) != NULL)
                b->make_ahead = w->carnum1;
            if (p == 1) b->make_ahead = -1;
        }
    }
    /* FUN_00458C89: pacenotes - car+0xF98 (the count) is not modelled, 0. */
    memset(b->pace, 0, sizeof(b->pace));
    b->pace_x = 0x320 / 2;
    /* FUN_00458D93 */
    countdown_frame(b, car);
    /* FUN_00458EF5: every section 12 (blue); then for each section passed,
     * 8 (green) if checkpt[i] <= the previous one (99 before the first,
     * JNS), else 4 (red). */
    if (rc) {
        int32_t n = rc->numcheckposits, prev = 0x63;
        if (n > HUD_SECTIONS) n = HUD_SECTIONS;
        for (i = 0; i < n; i++) b->state[i] = 12;
        if (car->currsegment > 0) {
            for (i = 0; i < car->currsegment && i < HUD_SECTIONS; i++) {
                /* car+0x140 + 4i: checkpt0..F, and on past them in the
                 * record, as the original reads it */
                int32_t cp = (0x140 + 4 * i + 4 <= (int32_t)sizeof(*car))
                             ? rd32(raw + 0x140 + 4 * i) : 0;
                b->state[i] = ((int32_t)((uint32_t)prev - (uint32_t)cp) >= 0)
                              ? 8 : 4;
                prev = cp;
            }
        }
    }

    /* RALLY_GAP_NAMES=BEHIND:AHEAD, a DIAGNOSTIC (not the
     * original's): shows both gap panels (+0:00.22 behind, -0:00.41 ahead)
     * and the two corner labels with those lexicon ids, whatever the race
     * says - so a short run can be set against a snapshot that has them.  A
     * NEGATIVE id -n gives the arcade's " CAR nn " (0x00458F61). */
    if (h->diag_names) {
        b->make_behind = b->make_ahead = 0;
        b->gap_behind = 220;
        b->gap_ahead = -410;
        b->name_behind = h->diag_id[0];
        b->name_ahead = h->diag_id[1];
    }
    /* FUN_00459163 (0x00459163..0x004591C4): the strings are compared with
     * the copies, and FUN_00459492 draws the labels again when they differ.
     * 0x006845A4.. start as zero, so the first frame draws. */
    label_strings(h, rc, car->arcadeposition);
    if (!h->label_valid || memcmp(h->label, h->label_shown, sizeof h->label)) {
        memcpy(h->label_shown, h->label, sizeof h->label);
        h->label_valid = 1;
        if (h->video && h->have_banner) {
            label_text(h);
            upload_canvas(&h->tex_banner, h->banner_px, 128, 128);
        }
    }
}

/* ---- the draw ------------------------------------------------------------ */

/* The DIV ladder at 0x004C94A3 and its five copies: |v| mod 360000, then
 * 60000, 6000, 1000, 100, 10, each plus the digit set's base. */
static void ladder(int32_t v, int32_t base, int32_t d[6])
{
    uint32_t a = (uint32_t)(v < 0 ? -v : v), r;
    r = a % 360000u;
    d[0] = (int32_t)(r / 60000u) + base; r %= 60000u;
    d[1] = (int32_t)(r / 6000u) + base;  r %= 6000u;
    d[2] = (int32_t)(r / 1000u) + base;  r %= 1000u;
    d[3] = (int32_t)(r / 100u) + base;   r %= 100u;
    d[4] = (int32_t)(r / 10u) + base;
    d[5] = (int32_t)(r % 10u) + base;
}

/* FUN_004CCF86, the progress bar.  All of it through FUN_004CE715 on
 * texture 0xC0. */
static void progress_bar(const hud *h, pen *pn)
{
    const hud_block *b = &h->b;
    int32_t i, n = b->sections;
    float prev = 0.0f, cum = 0.0f;

    pn->alpha = 0xE0;
    pn->rgb = 0x212463;
    flat(pn, 0, 0, 0x89, 0x20);
    flat(pn, 0x2AF, 0, 0x320, 0x20);
    flat(pn, 0x89, 0, 0x2AF, 3);
    flat(pn, 0x89, 0x1D, 0x2AF, 0x20);
    pn->rgb = 0;
    flat(pn, 0, 0x1F, 0x320, 0x20);
    if (n == 0) return;
    for (i = 0; i < n && i < HUD_SECTIONS; i++) {
        uint32_t col = bar_colour(b->state[i]);
        int32_t ea, eb;
        cum = cum + b->len[i];
        if (n - i == 1) cum = b->total;
        ea = fistp(553.0 * (double)prev / (double)b->total) + 0x89;
        eb = fistp(553.0 * (double)cum / (double)b->total) + 0x86;
        pn->alpha = 0xE0;
        pn->rgb = 0;
        flat(pn, ea, 3, eb, 4);
        flat(pn, eb, 3, eb + 1, 0x1C);
        flat(pn, ea, 4, ea + 1, 0x1D);
        flat(pn, ea + 1, 0x1C, eb + 1, 0x1D);
        pn->alpha = 0x78;
        pn->rgb = col;
        flat(pn, ea + 1, 4, eb, 0x1C);
        if (n - i != 1) {
            pn->alpha = 0xE0;
            pn->rgb = 0x212463;
            flat(pn, eb + 1, 3, eb + 3, 0x1D);
        }
        prev = cum;
    }
}

/* FUN_004CCD7E, from FUN_004CCDD8 with scale [0x008DDAE8] = 1.0, x offset 0,
 * y 200: a siz-pixel square centred on x = 400. */
static void countdown_draw(const hud *h, pen *pn, int32_t plate)
{
    const hud_block *b = &h->b;
    int32_t sz, tp, x0, x1, y0;
    if (b->olaydraw == -1) return;
    if (plate < 0 || plate > 5) return;
    sz = fistp((double)b->siz * 1.0);
    tp = fistp((double)b->top * 1.0);
    y0 = 200 + tp;
    x0 = (int32_t)((uint32_t)(0x320 - sz) >> 1);
    x1 = (int32_t)((uint32_t)(0x320 + sz) >> 1);
    pn->tex = h->tex_go[plate];
    pn->alpha = (uint32_t)b->alp;          /* 0x100 << 24 is 0: f = 0 draws
                                            * at alpha 0   */
    pn->rgb = 0xFFFFFF;
    rect_uv(pn, x0, y0, x1, y0 + sz, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* FUN_004CE2B8 -> FUN_004CE3CC: the rev needle, a quad of tx1802 rotated
 * about (96, 503). */
static void needle(const hud *h, pen *pn)
{
    static const float cx[4] = { 2.5f, -2.5f, -2.5f, 2.5f };
    static const float cy[4] = { 13.5f, 13.5f, -41.5f, -41.5f };
    const hud_block *b = &h->b;
    float r = (float)b->tacho, t;
    float x[4], y[4], u[4], v[4];
    int32_t n;
    uint32_t i;
    int k;

    if (b->dial2) {                         /* [0x008DD9DC] = +0xCC          */
        if (r < 0.0f) r = 0.0f;
        if (!(r < 7000.0f)) r = 7000.0f;
        t = r / 8000.0f + 0.375f;
    } else {
        if (r < 0.0f) r = 0.0f;
        if (!(r < 9500.0f)) r = 9500.0f;
        t = (r < 4000.0f) ? r / 64000.0f + 0.375f
                          : (r - 4000.0f) / 8000.0f + 0.4375f;
    }
    n = fistp((double)t * 65536.0 * 4096.0);
    i = ((uint32_t)(-(int32_t)((uint32_t)n << 4)) + 0x80000u) >> 20;
    for (k = 0; k < 4; k++) {
        float s = h->sin_table[i], c = h->sin_table[i + SIM_SIN_QUARTER];
        u[k] = (cx[k] + 253.5f) / 256.0f;
        v[k] = (cy[k] + 242.5f) / 256.0f;
        /* FUN_0041EC71: x' = x c + y s, y' = y c - x s */
        x[k] = (cx[k] * c + cy[k] * s) * 1.0f + 96.0f;
        y[k] = (cy[k] * c - cx[k] * s) * 1.0f + 503.0f;
    }
    quad(pn, x, y, u, v);
}

/* The gap panel of the car behind (x 20..) or ahead (x 701..). */
static void gap_panel(const hud *h, pen *pn, int32_t make, int32_t gap,
                      int32_t tx, const int32_t xs[8])
{
    int32_t d[6], m = make, v = gap / 10;
    if (m < 0) m = 0;
    if (m >= 0x16) m = 0x16;
    sprite(h, pn, 0x29 + m, tx, 1);
    ladder(v, 0x18, d);
    sprite(h, pn, v < 0 ? 0x23 : 0x22, xs[0], 19);
    sprite(h, pn, d[1], xs[1], 19);
    sprite(h, pn, 0x24, xs[2], 19);
    sprite(h, pn, d[2], xs[3], 19);
    sprite(h, pn, d[3], xs[4], 19);
    sprite(h, pn, 0x25, xs[5], 19);
    sprite(h, pn, d[4], xs[6], 19);
    sprite(h, pn, d[5], xs[7], 19);
}

/* FUN_004C928C, layout 0. */
/* FUN_004CE14D: sixteen triangles about (706, 504), radius 62
 * ([0x8DDBA8..0x8DDBB4]); the texture turned by the heading, 0.75 of the
 * radius ([0x8DDBAC]) in window texels about the car's place (+48, /128 -
 * [0x8DDA4C], [0x8DDA50]: these UVs are 0..1 of the 128 surface, not
 * 1/256ths).  Vertex -1 reads the zero dword before each array: the centre. */
static void map_disc(const hud *h, const pen *pn)
{
    static const uint16_t idx[48] = {
        0, 1, 2,  0, 2, 3,  0, 3, 4,  0, 4, 5,  0, 5, 6,  0, 6, 7,
        0, 7, 8,  0, 8, 9,  0, 9, 10, 0, 10, 11, 0, 11, 12, 0, 12, 13,
        0, 13, 14, 0, 14, 15, 0, 15, 16, 0, 16, 1 };
    plat_vertex q[17];
    uint32_t col = ((pn->alpha & 0xFFu) << 24) | (pn->rgb & 0x00FFFFFFu);
    int32_t rot = fistp((double)(float)(-h->map_head * 0.63661977f) * 1024.0);
    int k;

    for (k = 0; k < 17; k++) {
        double s = 0, c = 0, s2 = 0, c2 = 0, x, y, u, v;
        if (k) {
            uint32_t a = (uint32_t)(k - 1) * 256u, b2 = (a + (uint32_t)rot) & 0xFFFu;
            s = h->sin_table[a];  c = h->sin_table[a + SIM_SIN_QUARTER];
            s2 = h->sin_table[b2]; c2 = h->sin_table[b2 + SIM_SIN_QUARTER];
        }
        x = s * 62.0 + 706.0;
        y = -c * 62.0 + 504.0;
        u = (s2 * 62.0 * 0.75 + 48.0 + h->map_x) / 128.0;
        v = (-c2 * 62.0 * 0.75 + 48.0 + h->map_y) / 128.0;
        /* FUN_004CE912's scale, and THE PORT'S HALF PIXEL (quad, above) */
        q[k].sx  = (float)(x * pn->w / 800.0) + 0.5f;
        q[k].sy  = (float)(y * pn->h / 600.0) + 0.5f;
        q[k].sz  = 0.01f;
        q[k].rhw = 1.0f;
        q[k].diffuse  = col;
        q[k].specular = 0;
        q[k].tu = (float)u;
        q[k].tv = (float)v;
    }
    batch_add(pn->tex, q, 17, idx, 48);
}

static void layout0(const hud *h, const sim_race *rc, uint32_t slot, int sw,
                    int shh)
{
    static const int32_t xb[8] = { 20, 34, 47, 54, 68, 81, 88, 102 };
    static const int32_t xa[8] = { 701, 715, 728, 735, 749, 762, 769, 783 };
    const hud_block *b = &h->b;
    int32_t f = b->flags, dy = b->panel ? 0 : 32, d[6], i;
    pen pn;

    pn.w = sw;
    pn.h = shh;
    pn.alpha = 0xE0;
    pn.rgb = 0xFFFFFF;
    /* 0x10: the map disc (FUN_004CE0FC) from 0xC6 - */
    if ((f & 0x10) && h->have_map && h->tex_map) {
        pn.tex = h->tex_map;
        map_disc(h, &pn);
    }
    pn.tex = h->tex_c0;
    if (f & 0x40) progress_bar(h, &pn);
    pn.rgb = 0xFFFFFF;
    pn.alpha = 0xFF;
    /* 0x08: the rear-view mirror's outline, FUN_004CDF6C -> FUN_004CDE5B
     * Four one-pixel flat strips in black ([0x008DD9F4] = 0,
     * set by 0x004CDEB8; the alpha is the 0xFF of 0x004C92D6) round the inset
     * the mirror pass draws: the table at 0x008DDAF4 gives the
     * top-left, (605, 48) with the panel bit 0x40 ([0x00684256]) and (605, 16)
     * without; x0 = x - 1, x1 = x0 + 179, y1 = y + 64.  Exclusive ends, as
     * 0x004CDEE0..0x004CDF64 INC them: top (x0, y), x1, y+1; right (x1, y), x1+1, y1;
     * left (x0, y+1), x0+1, y1+1; bottom (x0+1, y1), x1+1, y1+1.  The same
     * texture as the bar before it (0xC0): the flat strips sample one point. */
    if (f & 0x08) {
        int32_t x0 = 604, y = (f & 0x40) ? 48 : 16;
        int32_t x1 = x0 + 179, y1 = y + 64;
        pn.rgb = 0x000000;
        flat(&pn, x0, y, x1, y + 1);
        flat(&pn, x1, y, x1 + 1, y1);
        flat(&pn, x0, y + 1, x0 + 1, y1 + 1);
        flat(&pn, x0 + 1, y1, x1 + 1, y1 + 1);
        pn.rgb = 0xFFFFFF;
    }
    countdown_draw(h, &pn, rc ? rc->olaydraw[slot & 1u] : b->olaydraw);
    pn.rgb = 0xFFFFFF;
    pn.alpha = 0xFF;
    pn.tex = h->tex_c1;
    if (f & 0x20) {
        sprite(h, &pn, 0x49, 9, 533);
        sprite(h, &pn, b->gear + 0x41, 17, 541);
    }
    pn.tex = b->dial2 ? h->tex_c2 : h->tex_c0;
    if (f & 0x01) sprite(h, &pn, b->dial2 ? 0x60 : 0x5E, 33, 440);
    pn.tex = h->tex_c0;
    if (f & 0x10) sprite(h, &pn, 0x5F, 642, 440);
    if (f & 0x40) {
        /* FUN_004CCAAB: the marker, colour [0x008DD980] */
        pn.rgb = 0xFFFFFF;
        sprite(h, &pn, 0x5C,
               fistp((double)b->done / (double)b->total * 550.0 + 119.0), 0);
    }
    pn.rgb = 0xFFFFFF;
    if (f & 0x80) {
        /* FUN_004CC42B: up to four arrows in 64-px cells from +0xB4 */
        int32_t x = b->pace_x;
        for (i = 0; i < 4; i++, x += 0x40) {
            int32_t id = b->pace[i];
            if (id == 0) continue;
            sprite(h, &pn, 0x4C + ((id - 1) & 15),
                   ((0x40 - pace_w[(id - 1) & 7]) >> 1) + x, 0x36 - dy);
        }
    }
    pn.tex = h->tex_c1;
    if (f & 0x01) needle(h, &pn);
    if (f & 0x40) {
        if (b->make_behind != -1)
            gap_panel(h, &pn, b->make_behind, b->gap_behind, 76, xb);
        if (b->make_ahead != -1)
            gap_panel(h, &pn, b->make_ahead, b->gap_ahead, 760, xa);
    }
    if (f & 0x02) {
        if (b->position != -1) {
            ladder(b->position, 0x02, d);
            sprite(h, &pn, d[4], 0x60, 0x36 - dy);
            sprite(h, &pn, d[5], 0x8C, 0x36 - dy);
        }
        sprite(h, &pn, 0x00, 0x0E, 0x33 - dy);   /* the car, car_picture  */
        sprite(h, &pn, 0x4A, 0x0F, 0x58 - dy);
        ladder(b->clock_ms / 10, 0x0C, d);
        sprite(h, &pn, d[0], 0x2E, 0x63 - dy);
        sprite(h, &pn, d[1], 0x42, 0x63 - dy);
        sprite(h, &pn, 0x16, 0x56, 0x63 - dy);
        sprite(h, &pn, d[2], 0x5E, 0x63 - dy);
        sprite(h, &pn, d[3], 0x72, 0x63 - dy);
        sprite(h, &pn, 0x17, 0x86, 0x63 - dy);
        sprite(h, &pn, d[4], 0x8E, 0x63 - dy);
        sprite(h, &pn, d[5], 0xA2, 0x63 - dy);
    }
    if (f & 0x20) {
        /* [0x008DDA6C], the leading-zero blank: the hundreds and tens draw
         * only once a non-zero digit has been seen; the units always. */
        int lead = 1;
        ladder(b->speed, 0x0C, d);
        if (!(lead && d[3] == 0x0C)) {
            sprite(h, &pn, d[3], 0x43, 0x23F);
            lead = 0;
        }
        if (!(lead && d[4] == 0x0C)) {
            sprite(h, &pn, d[4], 0x57, 0x23F);
            lead = 0;
        }
        sprite(h, &pn, d[5], 0x6B, 0x23F);
    }
}

/* FUN_00459539 for player 1: unless [0x00574558] or car+0xFA8, while +0x728
 * and (msgflash <= 0 (JLE) or msgtimer mod msgflash < msgflash SHR 1, an
 * unsigned DIV): the banner at x [0x00684604 + layout*4] = 220, y
 * [0x006845D4 + layout*4 (+0x18 without the panel)] = 128 | 96, 360 x 25:
 * three 120 x 25 quads side by side (FUN_00449020, alpha 0xFF, colour -1),
 * band b at u 8..248, v 50b..50b+50 in 1/256ths of the 0xC4 surface. */
static void banner(const hud *h, const sim_race *rc, uint32_t slot, pen *pn)
{
    const sim_race_msg *m;
    int32_t y, b;
    if (!rc || rc->quiet_q != 0 || !h->tex_banner) return;
    m = &rc->msg[slot & 7u];
    if (m->w728 == 0) return;
    if (m->flash > 0) {
        uint32_t fl = (uint32_t)m->flash;
        if (!((uint32_t)m->timer % fl < (fl >> 1))) return;
    }
    y = h->b.panel ? 128 : 96;
    pn->tex = h->tex_banner;
    pn->alpha = 0xFF;
    pn->rgb = 0xFFFFFF;
    for (b = 0; b < 3; b++)
        rect_uv(pn, 220 + 120 * b, y, 220 + 120 * (b + 1), y + 25,
                8.0f / 256.0f, (float)(50 * b) / 256.0f,
                248.0f / 256.0f, (float)(50 * b + 50) / 256.0f);
}

/* 0x00459732..0x00459980, the end of FUN_00459539 (reached whether or not
 * the banner drew - 0x004595F1 jumps to it): with the panel element (mask
 * bit 0x40) in layout 0, bands 3 and 4 of the 0xC4 surface - u 8..248 (120
 * texels), v 150..200 and 200..250 in 1/256ths (25 rows) - as 68 x 15 quads
 * at (4, 2) and (692, 2) (0x2B4), alpha 0xFF, colour -1: each 120 x 25 band
 * is shown at 68 x 15, so the text is minified by 0.57 / 0.6. */
static void labels(const hud *h, pen *pn)
{
    if (!(h->b.flags & 0x40) || !h->tex_banner) return;
    pn->tex = h->tex_banner;
    pn->alpha = 0xFF;
    pn->rgb = 0xFFFFFF;
    rect_uv(pn, 4, 2, 4 + 0x44, 2 + 15, 8.0f / 256.0f, 150.0f / 256.0f,
            248.0f / 256.0f, 200.0f / 256.0f);
    rect_uv(pn, 0x2B4, 2, 0x2B4 + 0x44, 2 + 15, 8.0f / 256.0f,
            200.0f / 256.0f, 248.0f / 256.0f, 250.0f / 256.0f);
}

void hud_draw(hud *h, const sim_race *rc, uint32_t slot, int race_over,
              int screen_w, int screen_h)
{
    pen pn;
    if (!h->have) return;
    /* 0x0044C48C: ZENABLE 0, ZWRITEENABLE 0 (ZFUNC 4).  THE REST IS THE
     * ORIGINAL'S 0x004586C9 (blended, MODULATEALPHA, no fog - hud.h); the
     * alpha test and the filters are inherited there, so the port's choice. */
    plat_gfx_set_state(PLAT_RS_ZTEST, 0u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 0u);
    plat_gfx_set_state(PLAT_RS_ZFUNC, (uint32_t)PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, (uint32_t)PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, (uint32_t)PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    plat_gfx_set_state(PLAT_RS_ALPHATEST, 1u);
    plat_gfx_set_state(PLAT_RS_ALPHAREF, 0u);
    plat_gfx_set_state(PLAT_RS_ALPHAFUNC, (uint32_t)PLAT_CMP_GREATER);
    plat_gfx_set_state(PLAT_RS_TEXADDRESS, 3u);
    plat_gfx_set_state(PLAT_RS_TEXMAG, 2u);
    plat_gfx_set_state(PLAT_RS_TEXMIN, 2u);
    layout0(h, rc, slot, screen_w, screen_h);
    pn.w = screen_w;
    pn.h = screen_h;
    banner(h, rc, slot, &pn);
    labels(h, &pn);
    hud_flush();
    /* THE PORT'S, not the original's (hud.h): the original goes to its
     * results screen. */
    if (race_over && h->have_font) {
        if (!h->tex_prompt) {
            canvas_text(h, &h->s2, "HOME: RACE AGAIN   ESC: QUIT");
            upload_canvas(&h->tex_prompt, h->canvas2, 256, 16);
        }
        pn.tex = h->tex_prompt;
        pn.alpha = 0xFF;
        pn.rgb = 0xFFFFFF;
        rect_uv(&pn, 220, (h->b.panel ? 128 : 96) + 32, 580,
                (h->b.panel ? 128 : 96) + 57, 0.0f, 0.0f,
                180.0f / 256.0f, 12.5f / 16.0f);
        hud_flush();
    }
}
