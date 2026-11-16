/* spray.h - the wheels' spray: dust, snow clouds, grass, tyre marks, wet and
 * dry tarmac, the body's sparks and the cars' smoke;
 * is the reading, every address retail's.
 *
 * The original's splash module (0x004B9734..0x004BF711) runs in the engine's
 * draw pass, after rain3d and snow3d:
 *
 *   emit    0x004B9734, once a rendered frame while block +0x10 == 1: per
 *           car (block +0x5C of them, the last first) the speed history, the
 *           wheels' axes and positions (0x004B9A6C), the dispatch 0x004B9CC3
 *           for the four wheels and 0x004B9D79 for the body contact, then
 *           three car-level emitters (exhaust 0xD, smoke 0x13 and 0x16);
 *   update  0x004BB1F7 -> 0x004BB29A: every live particle stepped by its
 *           type and drawn before it moves, a dead one returned to the pool;
 *           then 0x004BB367, the sparks (type 0x12) in an additive pass.
 *
 * THE MEMORY MODEL.  This is a C transcription, not an interpreter, but it
 * keeps the module's state where the original keeps it: the globals and
 * constants of 0x008D5400..0x008DA700 and 0x0070D300..0x0070D600 are one
 * copy of the image's bytes (every constant read from the image, every
 * per-site generator starting as the image has it), the pool's records are
 * the original's 0x94-byte layout at a fixed base with their links stored as
 * addresses, and each car's render snapshot is the original's 0x4C8-byte
 * engine object at 0x005787E0 + 0x60 + car*0x4C8.  So the gate (spray_vm_
 * check.c) runs the original's own code in x86vm.c on the same bytes and
 * compares memory whole.
 *
 * WHAT THE CALLER FILLS (spray_car, FUN_004547E1's per-car copy): the body
 * matrix car+0x8C0 (S+0x24 points at it), the front wheels' matrix car+0x740
 * (S+0x28; only row 0 is read), and the snapshot's fields listed there.
 *
 * THE SPARKS' STREAK 0x004B5A8F: spray.c transcribes it down to the
 * four screen points, colours and UVs it hands the polygon clip (spray_streak
 * below); spray_draw.c draws them.  NOT HERE: 0x004BB95F (no caller).
 *
 * THE PORT'S, not the original's: a pool that fills during a spray or mud
 * throw makes the original return through a misaligned stack (0x004BD38E /
 * 0x004BD8BB drop one dword of the two the loop pushed, so RET pops the
 * caller's EAX: a crash); the port returns from the throw.  The
 * arithmetic is `ext` (ext.h): long double, the x87's 80 bits, on x86; `double`
 * elsewhere, so the handheld's spray can differ in the last bits. */
#ifndef RALLY_SPRAY_H
#define RALLY_SPRAY_H

#include <stddef.h>
#include <stdint.h>

struct ral_image;

/* the draw's view: spray_draw.h (the gate steps without one) */
typedef struct spray_view spray_view;

#define SPRAY_CARS      8               /* FUN_004542EF's loop              */
#define SPRAY_REC       0x94u           /* a particle                       */
#define SPRAY_POOL_N    0x400u          /* records; 0x3FE live at most      */

/* The address ranges the module keeps (see the top). */
#define SPRAY_D0        0x008D5400u     /* globals, generators, tables      */
#define SPRAY_D1        0x008DA700u
#define SPRAY_K0        0x0070D300u     /* the engine's clock and constants */
#define SPRAY_K1        0x0070D600u
#define SPRAY_BLOCK     0x005787E0u     /* [0x0070FD98], the object block   */
#define SPRAY_SNAP      0x4C8u          /* one engine object (a car's S)    */
#define SPRAY_BLOCK_LEN (0x60u + SPRAY_CARS * SPRAY_SNAP)
#define SPRAY_POOL_VA   0x00C00000u     /* the pool's base: the port's, the
                                         * original's is a heap block       */
#define SPRAY_T0        0x00711600u
#define SPRAY_T1        0x00711900u
#define SPRAY_VIEW_VA   0x0059F014u
#define SPRAY_MAT_VA    0x00D00000u     /* car+0x8C0 / car+0x740 blocks: the
                                         * port's addresses for the cars'
                                         * records, 0x80 bytes a car        */

/* The projection constants the draws read from the engine ([0x005D6F1C],
 * [0x005D6F30], [0x005D6F34], [0x0088C22D]); the caller fills them each
 * frame (spray_draw_frame, or the gate). */
typedef struct {
    float focal, cx, cy, zscale;
} spray_proj;

/* One spark's streak as 0x004B5A8F leaves it in the polygon buffer
 * [0x009089A0] (stride 0x20): the corners across the current end (0, 1) and
 * across the previous end (2, 3).  `fogz` is the camera z whose fog the
 * original puts in corner 0's specular (the other three keep what the last
 * quad left: spray_draw.c). */
typedef struct {
    float    x[4], y[4], z[4];
    uint32_t col[4];
    float    u[4], v[4];
    uint32_t fogz;
    /* the terms the corners are sums of (the original's [0x00894245..],
     * [0x0088C18D], [0x0088C191]), for the gate: a corner is x +- w with w
     * about a pixel, which hides most of an error in the unit perpendicular */
    float    px, py, w1x, w1y, w0x, w0y, s1, s0;
} spray_streak;

/* What one car's snapshot needs this frame (FUN_004547E1 and the wheels'
 * block 0x00454909..0x00454B2D).  The caller fills it from the car record
 * and the sim's surface solve; spray_car copies it into S. */
typedef struct {
    int      live;              /* S+0x00: the engine object is drawn       */
    int32_t  model;             /* S+0x04: FUN_00454CEC(car+0x24)           */
    float    revs;              /* S+0x08: car+0x1D8                        */
    uint32_t ground_col;        /* S+0x1C: car+0x240                        */
    float    body[16];          /* car+0x8C0: the body's 4x4, row-major,
                                 * the position at [12..14]                 */
    float    front[16];         /* car+0x740: the front wheels' 4x4         */
    int32_t  reset;             /* car+0xFAC: S+0x30 = 1 while it counts;
                                 * the caller decrements it as 0x004547F3   */
    float    smoke_a, smoke_b;  /* car+0x2B4 (CTMP), car+0x2B8 (dam[0])     */
    float    fwhlspd, rwhlspd;  /* car+0x1B8, +0x1BC                        */
    float    cxspd, czspd;      /* car+0x1A4, +0x1AC                        */
    float    vel_x, vel_z;      /* car+0x198, +0x1A0                        */
    float    susp[4];           /* car+0x600..0x60C                         */
    int32_t  partsleft;         /* car+0x298                                */
    /* the surface solve's arrays, car+0x920..0x9BC, 5 points each          */
    int32_t  cls_a[5], cls_b[5], aux18_a[5], aux18_b[5];
    int32_t  aux14_a[5], aux14_b[5];
    float    weight[5];
    /* the body contact, car+0xA28..0xA38 */
    int      body_contact;      /* car+0xA34                                */
    int32_t  contact[3];        /* car+0xA28..0xA30, 16.16                  */
    float    contact_vt;        /* car+0xA38                                */
} spray_car_in;

typedef struct spray_s {
    /* the module's memory (see the top) */
    uint8_t  d[SPRAY_D1 - SPRAY_D0];
    uint8_t  k[SPRAY_K1 - SPRAY_K0];
    uint8_t  block[SPRAY_BLOCK_LEN];
    uint8_t  pool[SPRAY_POOL_N * SPRAY_REC];
    uint8_t  mats[SPRAY_CARS * 0x80];
    uint8_t  t[SPRAY_T1 - SPRAY_T0];    /* the per-model tables 0x711682 /
                                         * 0x711796, out of the image       */
    float    view[16];          /* [0x0059F014]: world -> camera, 4x4 row-
                                 * major, the translation in [12..14]       */
    uint32_t *lcg;              /* [0x006CF334], the generator shared with
                                 * the weather (weatherfx.h's wfx.lcg)       */
    uint32_t own_lcg;           /* lcg's target when the caller gives none   */
    uint32_t jit;               /* [0x008C38C4], 0x004B5DD7's own            */
    float    k8dccb4;           /* [0x008DCCB4], 0x004C2859's factor         */

    /* out of the image */
    float    sin_t[0x1400];     /* 0x009C3EE4, the cos table is +0x400      */
    float    aspect[256 * 9];   /* 0x008C0FF4, 0x004B604C's from TEXTINFO   */
    int32_t  geom[32][4];       /* 0x0057E10C + model*0x12C, +0x24..+0x30   */
    int      have;
    uint32_t near_bits;         /* [0x0088C221], the draw's near plane       */
    float    smoke_k0, smoke_k1;/* [0x005F0A1C], [0x0057AFC8]: S+0x4C0's     */
    int32_t  rain85;            /* [0x0085CDC5] as spray_clock left it       */
    uint32_t atlas;             /* plat_tex: surfdata.pcx + surfmask.pcx     */
    /* the draw (spray_draw.c; NULL in the gate): one quad, camera space */
    void   (*quad)(struct spray_s *s, const void *view, int sprite,
                   const float cam[3], uint32_t cell, uint32_t angle,
                   uint32_t col, float size, float aspect);
    /* a spark's streak (NULL: not drawn, not even computed) */
    void   (*streak)(struct spray_s *s, const void *view,
                     const spray_streak *out);
    /* between the two passes: the sparks' pass is additive (0x004BB285);
     * called only when there is a view */
    void   (*second_pass)(struct spray_s *s);
    spray_proj proj;

    /* counted, not the original's */
    unsigned long emitted[0x20], died, full;
    unsigned long arm[2][16], swaps, drawn;  /* the dispatch (wheel, body),
                                              * the second layer taken, the
                                              * draws past the near plane  */
    unsigned long streaks, streak_oob;       /* streaks handed on; colour
                                              * reads outside the table    */
} spray;

/* Copies the module's regions and tables out of the image and builds the
 * TEXTINFO-derived table.  0 on success; `err` otherwise. */
int  spray_load(spray *s, const struct ral_image *im, const uint8_t *textinfo,
                size_t textinfo_len, char *err, size_t errlen);

/* 0x004BAE77: the pool emptied and linked, the generators left as they are
 * (the original reloads at a race's start; its generators carry). */
void spray_race_start(spray *s);

/* The engine's clock for this frame (0x004C27F0): dt in ms (0 while paused)
 * into [0x0070D340], 1/dt [0x0070D350], [0x0070D344]; `rain` is block
 * +0x2764 (rain3d's copy, [0x0085CDC5]). */
void spray_clock(spray *s, int32_t dt, int32_t rain, int advance);

/* FUN_004547E1's copy of car `car`'s inputs into its S (call for every car
 * slot each rendered frame; live = 0 for a slot with no car). */
void spray_car(spray *s, int car, const spray_car_in *in);

/* 0x004B9734: the emitters, for `ncars` cars (block +0x5C), when the engine
 * advances. */
void spray_emit(spray *s, int ncars, int advance);

/* 0x004BB1F7: every particle's step and draw (view NULL: step only), then
 * the sparks' pass. */
void spray_update(spray *s, const spray_view *view);

/* For the gate: the module's memory at an address (NULL outside it). */
uint8_t *spray_at(spray *s, uint32_t va);

#endif
