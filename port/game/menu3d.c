/* menu3d.c - see menu3d.h. */
#include "menu3d.h"

#include "plat.h"
#include "platecache.h"
#include "stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FE_W 800
#define FE_H 600

enum {
    /* FUN_004713BA's MOV run into the type table [0x00581BC4 + type*4] */
    TT_RUN_LO   = 0x004715ECu, TT_RUN_HI = 0x004717AEu,
    TT_BASE     = 0x00581BC4u,
    /* 0x004717B3: for edi = 0 .. 0x16, [edi*4 + 0x00581C78] = FUN_0046102B(edi) */
    TT_CARS     = 0x00581C78u, TT_NCARS = 0x17,
    CAR_MODELS  = 0x006CEF1Cu,       /* FUN_0046102B's table, clamped 0..0x17 */
    /* 0x004717D6: the manifest's texture id 0x87 -> plate 0x82A */
    MANIFEST    = 0x0058C6B0u, ENV_ID = 0x87
};

struct menu3d {
    objscene      os;
    char          files[1024];
    uint32_t      model[256];         /* [0x00581BC4 + type*4]              */
    int           ntypes;
    uint32_t      env_plate;          /* the manifest's entry for id 0x87   */
    int           tex_ready;
    plate_cache   pc;
    stage_view    view;
    objlit_env    env;
    objlit_stats  st;
    int32_t       parts[256];         /* [0x005823D0], as 0x0045FB45 leaves it */
    menu3d_rec    rec[MENU3D_MAXDRAW];
    int           nrec;
};

/* 0x004715EC..: `C7 05 <addr> <imm>` (MOV dword [addr], imm), ten bytes each,
 * read out of the user's image; the run is the whole table but for the cars. */
static int parse_types(menu3d *m, const ral_image *im, char *err, size_t n)
{
    uint32_t a = TT_RUN_LO;
    int stores = 0, i;
    while (a < TT_RUN_HI) {
        const unsigned char *p = ral_image_at(im, a, 10);
        uint32_t dst, imm;
        if (!p || p[0] != 0xC7 || p[1] != 0x05) {
            snprintf(err, n, "menu3d: no MOV dword [m],imm at 0x%08X - not the "
                     "retail image this was read from", a);
            return -1;
        }
        dst = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
        imm = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16 | (uint32_t)p[9] << 24;
        if (dst < TT_BASE || dst >= TT_BASE + 256u * 4u || (dst - TT_BASE) % 4u) {
            snprintf(err, n, "menu3d: the store at 0x%08X is not into the type table", a);
            return -1;
        }
        m->model[(dst - TT_BASE) / 4u] = imm;
        stores++;
        a += 10;
    }
    for (i = 0; i < TT_NCARS; i++) {
        const unsigned char *p = ral_image_at(im, CAR_MODELS + 4u * (uint32_t)i, 4);
        if (!p) { snprintf(err, n, "menu3d: no car model table"); return -1; }
        m->model[(TT_CARS - TT_BASE) / 4u + (uint32_t)i] =
            (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    {
        const unsigned char *p = ral_image_at(im, 0x004717D6u, 10);
        if (!p || p[0] != 0xC7 || p[1] != 0x05) {
            snprintf(err, n, "menu3d: no manifest store at 0x004717D6");
            return -1;
        }
        if (((uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 |
             (uint32_t)p[5] << 24) != MANIFEST + 4u * ENV_ID) {
            snprintf(err, n, "menu3d: 0x004717D6 does not write texture id 0x87");
            return -1;
        }
        m->env_plate = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16 |
                       (uint32_t)p[9] << 24;
    }
    m->ntypes = (int)((TT_CARS - TT_BASE) / 4u) + TT_NCARS;
    return stores;
}

menu3d *menu3d_open(const ral_image *im, const char *files_root,
                    char *err, size_t n)
{
    menu3d *m = (menu3d *)calloc(1, sizeof *m);
    uint32_t load[256];
    int32_t lo[3] = { 0, 0, 0x7F }, hi[3] = { 0xFF, 0xFF, 0xFF };
    if (!m) { snprintf(err, n, "out of memory"); return NULL; }
    snprintf(m->files, sizeof m->files, "%s", files_root);
    if (parse_types(m, im, err, n) < 0) { free(m); return NULL; }
    objscene_init(&m->os);
    /* FUN_004248B9's sweep over the table.  Type 0's model 1 ([0x00581BC4] at
     * 0x004715EC) is left out: FUN_00420F8A searches from type 1, so nothing
     * in the menus can reach it. */
    memcpy(load, m->model, sizeof load);
    load[0] = 0;
    objscene_load_types(&m->os, load, m->ntypes, m->files);

    /* 0x0046C4B4: the menus' light, set by every 0x0046C45F */
    m->env.lo = 0x50; m->env.hi = 0xFF;
    m->env.ambient = 0xB4; m->env.diffuse = 0xB4;
    objlit_light(m->env.light, 0x6E, 0x1E, NULL);
    /* FUN_004713BA's last ramp, 0x00471ED6..0x00471F1C: (0,0,0x7F) -> white,
     * through FUN_0044D0EF's LOCALDAT arm ([0x005D41B0] = 1) */
    objlit_ramp(m->env.lut, lo, hi);
    m->env.strength = 0x200;               /* 0x0048C4AC                    */
    m->env.reflect  = 1;                   /* [0x005D6760], 0x00471F21      */
    m->env.context  = 5;                   /* 0x0048C4BB                    */
    m->env.fog0     = 0xFF;
    /* FUN_0044E351, outside a level ([0x0057AF70] = 0): before EVERY model
     * draw, 0x0045FB45 zeroes the part table and sets parts 6, 7 and 9 to
     * 999 - which no polygon's byte +0x0B can equal, so a car's race-number
     * digits are never drawn in the menus.  The type gates are skipped. */
    m->env.in_level = 0;
    m->parts[6] = m->parts[7] = m->parts[9] = 0x3E7;
    m->env.part_state = m->parts;
    /* the port's half pixel (objlit.h); RALLY_M3D_BIAS overrides it for the
     * measurement in */
    m->env.bias = 0.5f;
    if (getenv("RALLY_M3D_BIAS")) m->env.bias = (float)atof(getenv("RALLY_M3D_BIAS"));
    menu3d_view(m, 0, 0, 0x100);
    objlit_stats_init(&m->st);
    return m;
}

void menu3d_close(menu3d *m)
{
    if (!m) return;
    if (m->tex_ready) plate_cache_free(&m->pc);
    objscene_free(&m->os);
    free(m);
}

const uint32_t *menu3d_models(const menu3d *m, int *ntypes)
{
    if (ntypes) *ntypes = m->ntypes;
    return m->model;
}

void menu3d_view(menu3d *m, int32_t off_x, int32_t off_y, int32_t zoom)
{
    stage_view *v = &m->view;
    stage_view_default(v, FE_W, FE_H, 0, 0, 0, 0.0f);
    /* FUN_0044C516 with [0x005D6F20] = 800, [0x005D6F24] = 600 (0x0046C49B):
     * the centre is half of each (SHR 1, FILD), and the focal length is
     * [0x004EF920] * [0x005BA5E4] * [0x005D6F14] * [0x005D6C60] - the width,
     * 384.0f, the zoom and 1/163840 - so 0.6 x the width at zoom 0x100 and
     * 0.75 x at 0x140. */
    v->centre_x = (float)(FE_W >> 1);
    v->centre_y = (float)(FE_H >> 1);
    {
        uint32_t b = 0x36CCCCCDu;                     /* [0x005D6C60]          */
        float k;
        memcpy(&k, &b, 4);
        v->focal = (float)((long double)FE_W * 384.0L * (long double)zoom * (long double)k);
    }
    /* [0x005BD538] is 6 in the image and only a level load changes it; the
     * detail [0x005BD93C] is 0.  Both feed only the depth scale here. */
    v->radius = 6;
    v->detail = 0;
    /* [0x0057AF70] is 0 outside a level (0x00442E8E), so the offsets apply:
     * [0x005D7088] / [0x005D708C], added at the vertex output. */
    m->env.off_x = (float)off_x;
    m->env.off_y = (float)off_y;
}

static void ensure_tex(menu3d *m)
{
    static uint16_t tid[OBJ_TYPES * OBJ_TEXIDS_MAX];
    plate_grade g;
    unsigned i, k;
    char err[256];
    plat_tex t;
    if (m->tex_ready) return;
    if (plate_cache_init(&m->pc) != 0) return;
    m->tex_ready = 1;
    /* FUN_004713BA writes LOCALDAT +0x64C / +0x650 = 0x100 (0x00471574): the
     * neutral grade */
    g.saturation = PLATE_GRADE_NEUTRAL;
    g.brightness = PLATE_GRADE_NEUTRAL;
    k = objscene_texids(&m->os, tid, (unsigned)(sizeof tid / sizeof tid[0]));
    for (i = 0; i < k; i++) {
        t = 0;
        if (plate_cache_get(&m->pc, m->files, tid[i], &g, &t, err, sizeof err) != 0)
            fprintf(stderr, "menu3d: plate %u: %s\n", tid[i], err);
        objscene_set_texture(&m->os, tid[i], t);
    }
    t = 0;
    if (plate_cache_get(&m->pc, m->files, m->env_plate, &g, &t, err, sizeof err) != 0)
        fprintf(stderr, "menu3d: plate %u: %s\n", m->env_plate, err);
    m->env.tex87 = t;
    /* Not "menu3d: ...": menu_models.sh reads those as draws */
    printf("car selector: %u plates from graphics overrides\n",
           plate_overrides_taken());
}

void menu3d_begin(menu3d *m)
{
    ensure_tex(m);
    /* FUN_0044C942: GOTO3D, then 0x0044C99C (MAG 2, MIN 2 - [0x005D674C],
     * shaunmipmapping, is 0 - RANGEFOGENABLE 1), MODULATEALPHA, FOG on,
     * SRCALPHA / INVSRCALPHA, blending on */
    plat_video_scene_begin();
    plat_gfx_set_state(PLAT_RS_TEXMAG, 2u);
    plat_gfx_set_state(PLAT_RS_TEXMIN, 2u);
    plat_gfx_set_state(PLAT_RS_RANGEFOG, 1u);
    plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
    plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
    plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_INVSRCALPHA);
    plat_gfx_set_state(PLAT_RS_BLEND, 1u);
    plat_gfx_set_state(PLAT_RS_ZTEST, 1u);
    plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
}

void menu3d_end(menu3d *m)
{
    (void)m;
    plat_video_scene_end();
}

void menu3d_draw(menu3d *m, int type, const objlit_place *p, int win_w, int win_h)
{
    ral_mat4 mv;
    objlit_stats one;
    objlit_matrix(&mv, p);                         /* 0x0048C4B6 */
    m->env.scale_x = (float)win_w / (float)FE_W;
    m->env.scale_y = (float)win_h / (float)FE_H;
    objlit_stats_init(&one);
    objlit_draw(&m->os, type, &mv, &m->view, &m->env, &one);
    if (m->nrec < MENU3D_MAXDRAW) {
        menu3d_rec *r = &m->rec[m->nrec++];
        r->type  = type;
        r->model = (type >= 0 && type < 256) ? m->model[type] : 0u;
        r->place = *p;
        r->st    = one;
    }
    m->st.polys += one.polys;   m->st.gated += one.gated;
    m->st.tris  += one.tris;    m->st.drawn += one.drawn;
    m->st.lit   += one.lit;     m->st.white += one.white;
    m->st.reflect += one.reflect; m->st.tex_none += one.tex_none;
    m->st.behind += one.behind;
    m->st.clamp_lo += one.clamp_lo; m->st.clamp_hi += one.clamp_hi;
}

int menu3d_records(const menu3d *m, const menu3d_rec **out)
{
    *out = m->rec;
    return m->nrec;
}

const objlit_stats *menu3d_stats(const menu3d *m)
{
    return &m->st;
}

void menu3d_stats_reset(menu3d *m)
{
    objlit_stats_init(&m->st);
    m->nrec = 0;
}
