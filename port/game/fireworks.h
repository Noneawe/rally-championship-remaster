/* fireworks.h - the championship finish's fireworks,
 * Read from retail's bytes; every
 * address in fireworks.c is retail's.
 *
 * The original's module is 0x004BF712..0x004C0209, run once a rendered frame
 * from 0x004C1224 (after the spray, 0x004BB1F7): the pool of 0x1000 records of
 * 0x4C bytes in a ring (init 0x004BF712), the update-and-draw 0x004BF7B9, a
 * rocket spawn (0x004BFBA4), a trail spark spawn (0x004BFC37), a burst
 * (0x004BFD93 / 0x004BFE01) and the quad (0x004BFA22).  It runs only while
 * [0x005787E4] is set, which the finish routine does from 32 s with the
 * championship won ([0x0070D074]).
 *
 * THE PORT'S, not the original's: the pool's addresses (FW_POOL_VA; the
 * original's is a heap block, the records' links hold these), the quad's
 * hand-off to a callback, and the two sound flags, which the audio thread
 * reads and clears (worldsound.c).  The arithmetic is `ext` (ext.h), the
 * x87's 80 bits on x86. */
#ifndef RALLY_FIREWORKS_H
#define RALLY_FIREWORKS_H

#include <stddef.h>
#include <stdint.h>

struct ral_image;

#define FW_REC      0x4Cu               /* a particle                       */
#define FW_N        0x1000u             /* records; 0xFFF live at most      */
#define FW_POOL_VA  0x00D00000u         /* the port's, see above (x86vm.c's heap) */

/* the record's fields */
enum {
    FWR_PREV = 0x00, FWR_NEXT = 0x04, FWR_TYPE = 0x08,
    FWR_X = 0x0C, FWR_Y = 0x10, FWR_Z = 0x14,
    FWR_PX = 0x18, FWR_PY = 0x1C, FWR_PZ = 0x20,
    FWR_CEIL = 0x24,
    FWR_VX = 0x28, FWR_VY = 0x2C, FWR_AY = 0x30, FWR_VZ = 0x34,
    FWR_SIZE = 0x38, FWR_COL = 0x3C, FWR_RATE = 0x40, FWR_LIFE = 0x44,
    FWR_RAMP = 0x48
};

/* One quad as 0x004BFA22 builds it in the polygon buffer [0x009089A0]: four
 * corners (0 top left, 1 top right, 2 bottom right, 3 bottom left), one
 * depth, one colour, and record 88 of TEXTINFO's corners as the UVs. */
typedef struct {
    float    x[4], y[4], z;
    uint32_t col;
    float    uv[8];
} fw_quad;

/* What one rendered frame needs (0x004C106B..0x004C1224's reads). */
typedef struct {
    int      on;            /* [0x005787E4]: the fireworks are on           */
    int32_t  dt;            /* [0x0070D340], ms (0 while paused)            */
    float    view[16];      /* [0x0059F014]: world -> camera, row-major     */
    float    car[3];        /* [0x0059F144..4C]: the view car's position,
                             * car+0x8C0's translation as floats            */
    float    focal, cx, cy, zscale;     /* [0x005D6F1C / 30 / 34], [0x0088C22D] */
} fireworks_in;

typedef struct fireworks_s {
    uint8_t  pool[FW_N * FW_REC];
    uint32_t head;              /* [0x008DA660]                             */
    int32_t  count;             /* [0x008DA68C]                             */
    uint32_t frame;             /* [0x0070D4AC]: frames since the level     */
    int32_t  whoosh, boom;      /* [0x0051037C], [0x00510380]               */
    uint32_t *lcg;              /* [0x006CF334], shared with the weather    */
    uint32_t own_lcg;
    /* out of the image */
    float    k[0x2A];           /* 0x008DC6B8..0x008DC75C                   */
    uint32_t near_bits;         /* [0x0088C221]                             */
    float    inv65536;          /* [0x0088C229]                             */
    float    sin_t[0x1400];     /* 0x009C3EE4, the cos table is +0x400      */
    uint32_t ramp[0x900];       /* 0x008DA6B8, eight ramps of 256 + spill   */
    uint32_t spark[0x400];      /* 0x008C33F8                               */
    float    uv[8];             /* TEXTINFO record 88's corners (spray's)   */
    int      have;
    /* the draw: one call a quad (NULL: not drawn) */
    void   (*quad)(void *ud, const fw_quad *q);
    void    *ud;
    /* counted, not the original's */
    unsigned long rockets, sparks, bursts, burst_particles, died, drawn, full,
                  behind;
    unsigned long born[16], by_type[16];
} fireworks;

/* Copies the tables out of the image.  0 on success; `err` otherwise. */
int  fireworks_load(fireworks *w, const struct ral_image *im, char *err,
                    size_t errlen);

/* 0x004BF712: the pool linked and emptied; the frame counter 0 (0x004C0FC5).
 * The generator is left as it is. */
void fireworks_reset(fireworks *w);

/* 0x004C10D3's increment, then 0x004BF796 (nothing while !in->on). */
void fireworks_frame(fireworks *w, const fireworks_in *in);

/* For the gate: the pool's record at an address (NULL outside it). */
uint8_t *fireworks_at(fireworks *w, uint32_t va);

#endif
