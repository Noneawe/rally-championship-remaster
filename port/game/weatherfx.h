/* weatherfx.h - what the render path draws for rain and snow,
 * is the reading; every address
 * below is retail's.
 *
 * The weather words car 0 +0x900 (rain) and +0x904 (snow) reach the picture
 * through the render snapshot FUN_00454419 (0x004546A6..0x0045476F), which
 * scales them into the engine's block at [0x0070FD98] = 0x005787E0, and four
 * of the engine's own modules read them there:
 *
 *   rain3d  0x004B12E9  falling streaks, every view   (block +0x2764 = rain*2)
 *   snow3d  0x004B2204  falling flakes, every view    (block +0x2760 = snow*8)
 *   screen snow   0x004AF22C  flakes stuck on the glass, inside views
 *                                                  (+0x2740 = snow>>3 inside)
 *   screen rain   0x004AA895  drops running on the glass, inside views
 *                                                  (+0x2738 = rain*4 inside)
 *
 * The first two run in the engine's draw pass 0x004C1100, after the world and
 * before the cockpit; the last two in 0x004B7106, which 0x004539C7 runs after
 * the cockpit.  The mirror's pass (0x00452713) reaches none of it.
 *
 * WHAT IS NOT HERE, and each is named where it would plug in:
 *  the WIPERS are wipers.c's: the phase and the pose (item 1),
 *     the masks, the flow field's draws and the two wipe-offs 0x004B0280 /
 *     0x004AC6ED (item 2, 2026-10-26), which this module calls through
 *     `wipe` / `wp` - set by the caller; without them only the drops'
 *     evaporation runs.
 *  the windscreen's dirt layer 0x004B8BA2 / 0x004B9233:.
 *  0x004A7AD9 (the screen snow's caller runs it behind [0x0070AA78] &
 *     0x4000): unread.
 *  [0x00711676..7E], which 0x004B0D67 copies into the screen modules'
 *     acceleration every engine update: unread, so the screen SNOW's spawn
 *     sees 0 there (the screen RAIN overwrites it from the camera itself,
 *     0x004AC806, before it reads it).  A STAND-IN.
 *  the shared generator [0x006CF334]'s other ~70 users (the spray, the
 *     wiper's flow field at load): the port's sequence is the weather's own.
*/

#ifndef RALLY_WEATHERFX_H
#define RALLY_WEATHERFX_H

#include <stdint.h>

#include "plat.h"
#include "stage.h"
#include "wipers.h"

struct ral_image;

#define WFX_CARS        23          /* every per-car table: 23 rows          */
#define WFX_RAIN3D_N    256         /* 0x0085CDC9..                           */
#define WFX_SNOW3D_N    2048        /* 0x008668C0..                           */
#define WFX_DROPS_N     2048        /* 0x007BE18E, stride 0x30                */
#define WFX_FLAKES_N    2048        /* 0x0083EA42, stride 0x3C                */

/* A windscreen raindrop, 0x007BE18E + k*0x30 (the fields read). */
typedef struct {
    int32_t active;                 /* +0x00 */
    int32_t size;                   /* +0x04  0..7                            */
    float   x, y;                   /* +0x14, +0x18  0..2047, 0..1023         */
    float   vx, vy;                 /* +0x1C, +0x20                           */
} wfx_drop;

/* A flake on the glass, 0x0083EA42 + k*0x3C. */
typedef struct {
    int32_t alive;                  /* +0x00 */
    int32_t age;                    /* +0x08  ms                              */
    float   u, v;                   /* +0x0C, +0x10  the atlas cell's corner  */
    float   x, y;                   /* +0x20, +0x24                           */
} wfx_flake;

typedef struct {
    /* ---- out of the image (wfx_load) ---- */
    int32_t  wscreen[WFX_CARS][6];  /* 0x0057E144 + id*0x12C: two points, mm */
    int32_t  yoff;                  /* [0x005C6CF8] = 0xB333 (0.7)           */
    float    zbias[WFX_CARS];       /* 0x0070D5B0, floats (FADD)              */
    float    tilt_deg[WFX_CARS];    /* 0x007BA1C7, before 0x004B0C09's 90 - x */
    float    drop_rad[8];           /* 0x007BCB12                             */
    float    deg2rad;               /* [0x0085CCB1]                           */
    int      have_tables;

    /* ---- the files ---- */
    uint8_t  edge[256 * 128];       /* 0x007B1D4F, files\m\edgeNN.pcx        */
    plat_tex atlas;                 /* texture id 0xCA = plate 900, files\m  */
    plat_tex copy[2];               /* plate 1999's slot: the frame's copy   */
    unsigned copy_w, copy_h;        /* their (power-of-two) size             */
    int      copy_cur;              /* which one this frame's Blt fills      */
    int      copy_valid;            /* the other one holds a frame           */

    int      model;                 /* FUN_00454CEC(car+0x24): [0x007115A2]  */

    /* ---- the engine's clock, 0x004C27F0 ---- */
    uint32_t last_ms;
    int32_t  dt;                    /* [0x0070D340]                           */

    /* ---- the shared generator [0x006CF334] ---- */
    uint32_t lcg;

    /* ---- the wipe-off: the caller's, read only ---- */
    const wiper_masks *wipe;        /* the masks; NULL or !have: no wipe     */
    const wipers  *wp;              /* the phase [0x007EC55C], its table     */
    unsigned long  wipe_draws;      /* the flow field's draws, made between
                                     * the seed and rain3d's table          */
    int32_t        wipe_cur;        /* [0x007BC664], both wipe-offs' cursor:
                                     * set once from wipe->cur0, never reset */

    /* ---- rain3d ---- */
    int32_t  r_x[WFX_RAIN3D_N], r_y[WFX_RAIN3D_N], r_z[WFX_RAIN3D_N];
    int32_t  r_primed;              /* [0x0085CCBD] */
    int32_t  r_off[3];              /* [0x0085E635..] */
    float    r_vel[3];              /* [0x0085CD41] (view 0) */
    int32_t  r_prev[3];             /* [0x0085CD7D] (view 0) */
    int32_t  r_fall;                /* [0x0085CDBD] */
    uint32_t r_gust_t, r_gust_p;    /* [0x0085CCC1], [0x0085CCC5] */
    float    r_tgt_x, r_tgt_z;      /* [0x0085CCC9], [0x0085CCD1] */
    float    r_wind_x, r_wind_z;    /* [0x0085CCD5], [0x0085CCDD] */
    int32_t  r_jit;                 /* [0x0085CCE1] */

    /* ---- snow3d ---- */
    int32_t  s_x[WFX_SNOW3D_N], s_y[WFX_SNOW3D_N], s_z[WFX_SNOW3D_N];
    int32_t  s_ox[WFX_SNOW3D_N], s_oz[WFX_SNOW3D_N];
    float    s_u[WFX_SNOW3D_N];
    int32_t  s_t[4096];             /* 0x00872BD0 */
    int32_t  s_drift;               /* [0x00872A54] */
    uint32_t s_last;                /* [0x00872A10] */
    int32_t  s_off[3];              /* [0x00872AD4..] */
    uint32_t s_light[17];           /* 0x0087BD24 */

    /* ---- the screen modules ---- */
    wfx_drop  drop[WFX_DROPS_N];
    int32_t   drop_cur;             /* [0x007BD0CE] / 0x30 */
    float     curve[256];           /* 0x007BC712 */
    float     hex[8][6][2];         /* 0x007BCE8E */
    int32_t   acc_first, acc_second; /* [0x007BE0FE], [0x007BE102] */
    float     acc_prev_p[3];        /* 0x007BE126 */
    float     acc_prev_v[3];        /* 0x007BE132 */
    float     acc[3];               /* [0x007B9D6B/6F/73] */
    uint32_t  evap;                 /* [0x007D6206] */
    uint32_t  drop_gate;            /* (the frac gate draws [0x006CF334]) */
    wfx_flake flake[WFX_FLAKES_N];
    int32_t   flake_cur;            /* [0x0083DA1E] / 0x3C */
    uint32_t  flake_seed_a;         /* [0x0085CB0A] */
    uint32_t  flake_seed_b;         /* [0x0085CB0E] */

    /* DIAGNOSTIC, weather-drawn's: the glass's modules run in full - their
     * draws of the shared generator [0x006CF334] included, so the falling
     * rain is the same - and only their draw calls are left out */
    int       diag_no_glass_draw;
} wfx;

/* What one frame hands the engine: the snapshot's inputs, the port's names
 * for the globals FUN_00454419 and 0x004541B4 copy. */
typedef struct {
    int32_t  rain, snow;            /* car 0 +0x900 / +0x904                 */
    int      inside;                /* [0x005F44A0]: car+0x6F0, when the
                                     * level has rain words (0x0045228C)    */
    int      advance;               /* block +0x10: 0 while paused           */
    uint32_t now_ms;                /* [0x005C6568]                          */
    int      lights;                /* block +0x50: car+0xABC >= 16          */
    uint32_t tint;                  /* [0x005F44A4]                          */
    const stage_view *v;            /* F, the centre, the far value          */
    const float *view_rot;          /* [0x0059EF94], 4x4 row-major           */
    int32_t  cam[3];                /* [0x005BA5E8..F0], the eye, 16.16      */
    const float *car_mv;            /* [0x0059F054], the car's model->view   */
    int32_t  car_pos[3];            /* car+0x134.. (block +0x26AC)           */
    const float *car_rot;           /* [0x0059F114] (see weatherfx.c)        */
    int      screen_w, screen_h;    /* the frame, for the copy and the lens  */
} wfx_frame;

/* The tables from the image, the atlas (plate 900, graded by `g` as the
 * level's plates are) and the edge mask for `model` from the files.  0 on
 * success; `err` otherwise, and the weather is then not drawn. */
int  wfx_load(wfx *w, const struct ral_image *im, const char *files_root,
              const void *grade, int model, int video, char *err,
              size_t errlen);

/* 0x004B0C61: files\m\edgeNN.pcx (NN = the model) into the edge mask. */
int  wfx_load_edge(wfx *w, const char *path, char *err, size_t errlen);

/* 0x004C0F44 at a race's (re)start: the generator seeded with `model`, the
 * rain3d and snow3d tables, the screen modules' pools. */
void wfx_race_start(wfx *w, int model);

/* 0x004C106B's clock half, once a rendered frame before anything below. */
void wfx_engine_tick(wfx *w, const wfx_frame *f);

/* 0x00453C27: after the main view's world, the frame copied for the
 * raindrops - into the copy the NEXT frame's drops sample (0x004590B5
 * uploads it after this frame's drops are drawn). */
void wfx_scene_copy(wfx *w, const wfx_frame *f);

/* 0x004C1100's weather: rain3d then snow3d. */
void wfx_draw_falling(wfx *w, const wfx_frame *f);

/* 0x004B7106's weather, after the cockpit: screen snow, screen rain. */
void wfx_draw_screen(wfx *w, const wfx_frame *f);

/* the end of the frame: this frame's copy becomes the one sampled */
void wfx_frame_end(wfx *w);

#endif /* RALLY_WEATHERFX_H */
