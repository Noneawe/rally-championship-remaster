/* stage.c - the stage renderer.
 *
 * Read stage.h first: it carries the field maps and the address of the
 * instruction every constant here came from.  The transcription itself, with
 * the disassembly beside it, is in
 *
 *
 * THE SHAPE OF THE ORIGINAL, IN ONE PARAGRAPH.  A level is a flat grid of
 * 32-world-unit cells; each cell owns one contiguous run of polygons.  Once
 * per frame the engine rebuilds a (2R+1)^2 array of the cell indices within R
 * cells of the eye, walks it, and for each polygon in each cell transforms
 * corner 0 into a 4,096-slot direct-mapped cache and rejects the polygon if
 * that corner is outside a 90-degree horizontal wedge.  Survivors are appended
 * to ONE OF TWO LISTS - opaque ones keyed by texture, translucent ones keyed
 * by depth - each list is quicksorted by its key, and only then is anything
 * drawn.  The port does the same thing in the same order.
 *
 * THE EMITTER IS NOW TRANSCRIBED TOO -.  It is the
 * pipeline at FUN_0045026e, reached through FUN_00452eee and FUN_0044d9bc,
 * and reads all of it.  FUN_0045172d, which
 * sessions 32-33 named "the per-quad emitter", is NOT one: it is a 1-to-4
 * midpoint SUBDIVIDER that re-lights a triangle whose corners carry a dynamic
 * light, it is reached only from FUN_00450c37, and nothing in it clips.
 *
 *
 * WHAT IS STILL THE PORT'S OWN is down to TWO things and each is marked
 * PORT'S CHOICE at the line: the two ends of the shade ramp (the level colour
 * pair is not loaded), and the second texture layer as a second pass.
 *
 * TOOK THE UVs OFF THAT LIST.  stage_set_textinfo loads the four
 * base pairs from TEXTINFO record bytes +0x00..+0x0f and quad_uv permutes
 * those rather than a unit square.  What that needed was FUN_0044c396, which
 * nobody had opened: the stored u16 is a 0.16 FIXED-POINT coordinate, and
 * plates 400..799 have all eight values DOUBLED (0x004512a0).  See quad_uv,
 * sections 6.1-6.2.
*/

#include "stage.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ utils */

/* FISTP rounds under the current x87 mode, which is round-to-nearest-even on
 * entry and is never changed on this path.  C's cast truncates, so it is the
 * wrong instruction to transcribe with.  This matters: FUN_00452c90 compares
 * the two ROUNDED integers, so a polygon exactly on the wedge is decided by
 * the rounding and not by the float compare. */
static int32_t fistp(float f)
{
    return (int32_t)nearbyintf(f);
}

/* `SAR EAX,0x15` at 0x0045289c.  C99 leaves >> on a negative signed value
 * implementation-defined, so do not rely on the compiler being sane about a
 * transcription whose whole point is the sign behaviour. */
static int32_t sar21(int32_t v)
{
    return (v < 0) ? ~((~v) >> CDAT_CELL_SHIFT) : (v >> CDAT_CELL_SHIFT);
}

uint32_t stage_cell_key(int32_t world_x, int32_t world_z)
{
    /* FUN_00440424 - `SHR`, logical, so a negative coordinate wraps into the
     * top of an 11-bit index.  cdat_cell_of is the same instruction. */
    return (cdat_cell_of(world_x) & CDAT_CELL_MASK)
         | ((cdat_cell_of(world_z) & CDAT_CELL_MASK) << 11);
}

/* ------------------------------------------------------------------- load */

int stage_load(stage *s,
               const void *cdat_buf, size_t cdat_len,
               const void *localdat_buf, size_t localdat_len,
               char *err, size_t errlen)
{
    unsigned i;

    memset(s, 0, sizeof *s);

    /* 0 is SUCCESS and -1 is failure - cdat.h did not say so until this
     * ticket asked, and the first draft of this line tested the truth of the
     * return value and refused every valid level.  The convention is now in
     * the header. */
    if (cdat_open_buffer(cdat_buf, cdat_len, &s->cdat, err, errlen) != 0) {
        return -1;
    }

    /* cdat.h's accessor contract, honoured ONCE so that no inner loop below
     * has to.  A payload that does not fit is refused rather than clamped:
     * the accessors would read past the allocation and the only reason the
     * shipped corpus never does is that it is valid. */
    if (!s->cdat.fits) {
        if (err && errlen) {
            const char *m = "cdat: declared sections do not fit the payload";
            size_t n = strlen(m);
            if (n >= errlen) { n = errlen - 1; }
            memcpy(err, m, n);
            err[n] = '\0';
        }
        cdat_free(&s->cdat);
        return -1;
    }

    for (i = 0; i < STAGE_VCACHE_SLOTS; i++) {
        s->vcache[i].tag = 0xffffffffu;      /* the invalidation the game uses */
    }

    if (localdat_buf && localdat_len) {
        ld_parse(localdat_buf, localdat_len, &s->ld);
        s->have_ld = 1;
        /* initgametexinfbuf_q 0x00437b3a: each of the 256 dwords at
         * LOCALDAT +0x0100 is a TEXTINFO record index; +0x20 of the slot is
         * `index >> 4`, the PLATE, and that is what the draw batches on. */
        for (i = 0; i < LD_TEXSLOTS; i++) {
            uint32_t rec = ld_dw(&s->ld, LD_TEXINFO_OFF + i * 4);
            s->texinfo_rec[i] = rec;
            s->plate[i] = (uint16_t)((rec >> 4) & 0xffffu);
            /* FUN_0044d9bc 0x0044d9c6, the per-slot diffuse tint. */
            s->slot_tint[i] = ld_dw(&s->ld, LD_SLOT_TINT_OFF + i * 4);
        }
    } else {
        for (i = 0; i < LD_TEXSLOTS; i++) {
            s->plate[i] = (uint16_t)i;       /* no level texture set: slot == plate */
        }
    }
    return 0;
}

void stage_free(stage *s)
{
    cdat_free(&s->cdat);
}

void stage_set_texture(stage *s, unsigned slot, plat_tex t)
{
    if (slot < LD_TEXSLOTS) {
        s->tex[slot] = t;
    }
}

void stage_set_object_hook(stage *s,
                           void (*fn)(void *ctx, const stage_view *v,
                                      int32_t poly),
                           void *ctx)
{
    s->obj_fn  = fn;
    s->obj_ctx = ctx;
}

void stage_set_textinfo(stage *s, const void *buf, size_t len)
{
    const uint8_t *ti = (const uint8_t *)buf;
    unsigned i;

    /* FUN_00453161 builds the dispatch tables from byte +0x2e of the texinfo
     * slot, which initgametexinfbuf_q copied from TEXTINFO record byte +0x26
     * (`MOV AL,[EDI+0x26]` / `MOV [ESI+0x2e],AL`, 0x00437b8d).  So the
     * material class of a polygon is a property of its TEXTURE and it is a
     * byte in a shipped file. */
    for (i = 0; i < LD_TEXSLOTS; i++) {
        size_t rec = (size_t)s->texinfo_rec[i] * LD_TEXTINFO_STRIDE;
        size_t off = rec + 0x26;
        unsigned k;

        s->texflags[i] = (buf && off < len) ? ti[off] : 0u;

        /* Three instructions earlier in the same copy loop, 0x00437b81:
         * record +0x24 -> slot +0x2c, the SECOND LAYER'S mode selector.
         * Same guard as +0x26 above, and the same fallback: a slot whose
         * record lies past the end of the file selects table entry 0, which
         * is mode 3 - the block the other two are variations on. */
        s->texmode[i] = (buf && rec + 0x24 < len) ? ti[rec + 0x24] : 0u;

        /* The four base uv pairs, TEXTINFO record bytes +0x00..+0x0f, copied
         * to the texinfo slot verbatim at 0x00437b93-0x00437ba8.  Little
         * endian u16 pairs; FUN_004511ab reads them as
         * `MOV AX,word [ESI + EBX]` and `... + 0x2`, ESI being the corner
         * selector 0/4/8/12 out of the symmetry table. */
        for (k = 0; k < 4u; k++) {
            size_t p = rec + k * 4u;

            if (buf && p + 4u <= len) {
                s->uv[i][k][0] = (uint16_t)(ti[p]     | (ti[p + 1] << 8));
                s->uv[i][k][1] = (uint16_t)(ti[p + 2] | (ti[p + 3] << 8));
            } else {
                s->uv[i][k][0] = 0u;
                s->uv[i][k][1] = 0u;
            }
        }
    }
}

/* ===================================================================== *
 *  VERTEX FOG -,.  THE HORIZON.                        *
 * ===================================================================== *
 *
 * stage.h carries the chain and the three addresses that make byte +0x20 a
 * TRANSCRIPTION rather than an inference.  This is the arithmetic.
*/

/* FUN_0044DD01, 226 bytes: a BILINEAR sample of that 64x64 map at the world
 * position, returned shifted left 8.
 *
 *   0044dd0c  EAX = pos_x >> 24, ECX = pos_z >> 24     ; the tile
 *   0044dd13  AND 0x3f each, ECX << 6, add             ; wrap PER AXIS
 *   0044dd1e  the four taps, at (x,z) (x+1,z) (x,z+1) (x+1,z+1)
 *   0044dd7e  EBX = (pos_x >> 8) & 0xffff              ; the 0.16 fraction
 *   0044dd90  three lerps, each IMUL / SHRD ...,16
 *   0044dddf  SHL EAX,8
 *
 * The +1 is applied BEFORE the mask, so a tap at tile 63 wraps to tile 0.
 * The shifts are SHR - logical - so a negative coordinate is treated as its
 * unsigned bit pattern, which the mask makes irrelevant for the tile and
 * exact for the fraction. */
static int32_t fog_noise_sample(int32_t x, int32_t z)
{
    const uint8_t *n = stage_fog_noise();
    uint32_t ix = (uint32_t)x >> 24, iz = (uint32_t)z >> 24;
    int32_t  fx = (int32_t)(((uint32_t)x >> 8) & 0xffffu);
    int32_t  fz = (int32_t)(((uint32_t)z >> 8) & 0xffffu);
    int32_t  t00, t01, t10, t11, a, b;

#define FOG_TAP(TX, TZ) \
    ((int32_t)n[((((uint32_t)(TZ)) & 63u) << 6) | (((uint32_t)(TX)) & 63u)])

    t00 = FOG_TAP(ix,      iz);
    t01 = FOG_TAP(ix + 1u, iz);
    t10 = FOG_TAP(ix,      iz + 1u);
    t11 = FOG_TAP(ix + 1u, iz + 1u);
#undef FOG_TAP

    a = t00 + (int32_t)(((int64_t)(t01 - t00) * fx) >> 16);
    b = t10 + (int32_t)(((int64_t)(t11 - t10) * fx) >> 16);
    return (a + (int32_t)(((int64_t)(b - a) * fz) >> 16)) << 8;
}

/* FUN_0044DBFC, transcribed to the end of the basic block - standing rule 3,
 * and the tail matters: the four instructions after the loop build the mode-4
 * flat override out of the accumulator's FINAL value, with LOCALDAT +0x658
 * where the loop body uses nothing, and with the two clamps in a DIFFERENT
 * order from the loop's.
 *
 *   0044dc0c  EBX = [+0x66c] - [+0x654]
 *   0044dc18  IMUL EBX / SHRD EAX,EDX,8          ; blend * (far - near) >> 8
 *   0044dc1e  EBP = ([+0x654] << 8) + EAX        ; the per-step increment
 *   0044dc29  [0x005e0980] = EBP                 ; write-only: ONE reference
 *                                                ; image-wide, this store
 *   0044dc67  [0x005ea4e0] = [0x005f0b18] << 3   ; length = (R * 32) * 8
 *   0044dc80  EBX += EBP
 *             EAX = (EBX >> 16) + [+0x6a4] + [0x005e94b4]
 *             clamp 0..255 -> ramp[i]
 *   0044dcb8  EAX = (EBX >> 16) + [+0x6a4] + [+0x658], clamp,
 *             += [0x005e94b4], clamp -> [0x005e94d4]
 *
 * WHAT IS NOT PORTED, AND IT IS MEASURED RATHER THAN GUESSED.  [0x005E94B4]
 * is a SUN-GLARE bias: 0x0044DC2F builds it as
 * (([0x005F449C] * [0x005F4498]) >> 4) * LOCALDAT +0x65c >> 8, and
 * [0x005F449C] is set to 0 at 0x0045350B and only raised again if
 * +0x65c + +0x6ac is non-zero (0x00453515) AND a sun direction, built from
 * LOCALDAT +0x680..+0x688 through FSINCOS and multiplied by the view matrix
 * at 0x004535A1, clears a horizon threshold (0x004535CA).  Over the 48
 * shipped levels LOCALDAT +0x65c is ZERO on 47 - LEVEL55 carries 1 - so the
 * bias this term feeds is provably 0 on 47 of 48 levels and at most a
 * fraction of one ramp step on the 48th.  It is therefore held at 0 here and
 * filed as an open question with an owner rather than invented.
 *
 * `blend` is the noise sample, which the four levels whose +0x654 and +0x66c
 * DIFFER (LEVEL44, 64, 65 and 66) are the only ones to notice: on the other
 * 44 the lerp's two endpoints are equal and the map cancels out. */
void stage_fog_build(stage_fog *f, const ld_file *ld, int have_ld,
                     int32_t radius, int32_t eye_x, int32_t eye_z)
{
    int32_t idx, near_e, far_e, base, flat658, blend, step, acc = 0, x;
    int32_t i;

    memset(f, 0, sizeof *f);
    if (!have_ld || !ld) {
        f->len = 1;                     /* ramp[0] = 0, i.e. no fog at all */
        f->color = 0xff000000u;
        return;
    }

    /* FUN_00437AA7's clamp, 0x00437AB0 / 0x00437ABA.  The index is LOCALDAT
     * +0x1fd0 and has no writer image-wide - see localdat.h. */
    idx = (int32_t)ld_dw(ld, 0x1fd0);
    if (idx < 0) { idx = 0; }
    if (idx > 4) { idx = 4; }

    near_e  = (int32_t)ld_dw(ld, 0x2010 + 4 * (unsigned)idx);   /* -> +0x654 */
    far_e   = (int32_t)ld_dw(ld, 0x2024 + 4 * (unsigned)idx);   /* -> +0x66c */
    base    = (int32_t)ld_dw(ld, 0x1ffc + 4 * (unsigned)idx);   /* -> +0x6a4 */
    flat658 = (int32_t)ld_dw(ld, 0x1fe8 + 4 * (unsigned)idx);   /* -> +0x658 */

    blend = fog_noise_sample(eye_x, eye_z);
    step  = (near_e << 8)
          + (int32_t)(((int64_t)blend * (int64_t)(far_e - near_e)) >> 8);

    /* [0x005F0B18] = [0x005BD538] * 0x20 at 0x0044C51B - the same zfar the
     * depth scale is built from - and the length is that times 8, which is
     * exactly the scale SHR EAX,0xd applies to a raw 16.16 view z. */
    f->len = radius * 32 * 8;
    if (f->len < 1)               { f->len = 1; }
    if (f->len > STAGE_FOG_STORE) { f->len = STAGE_FOG_STORE; }  /* port's */

    for (i = 0; i < f->len; i++) {
        acc += step;
        x = (acc >> 16) + base;                 /* + [0x005e94b4], held 0 */
        if (x < 0)   { x = 0; }
        if (x > 255) { x = 255; }
        f->ramp[i] = (uint8_t)x;
    }

    /* 0x0044DCB8, and note it is the SAME accumulator, after the loop. */
    x = (acc >> 16) + base + flat658;
    if (x < 0)   { x = 0; }
    if (x > 255) { x = 255; }
    /* + [0x005e94b4] here, then clamp again - held 0, so the second clamp
     * cannot fire.  Transcribed so the shape survives if the bias lands. */
    if (x < 0)   { x = 0; }
    if (x > 255) { x = 255; }
    f->flat = x;
    f->mode = 0;

    /* 0x0044DBA2's pack, then 0x0044DBD2's OR - stage.h and localdat.h. */
    f->color = 0xff000000u
             | ((ld_dw(ld, LD_FOGCOLOR_OFF)     & 0xffu) << 16)
             | ((ld_dw(ld, LD_FOGCOLOR_OFF + 4) & 0xffu) <<  8)
             |  (ld_dw(ld, LD_FOGCOLOR_OFF + 8) & 0xffu);
}

/* FUN_0044C2FB and FUN_0044C321's tail, for one corner.
 *
 *   0044c33a  FLD [EDX+0x18] / FISTP [0x005d6ea0]   ; view z -> int, ROUNDED
 *   0044c2fb  TEST EAX,EAX / JNS / XOR EAX,EAX      ; negative clamps to 0
 *   0044c301  SHR EAX,0xd                           ; world * 8
 *   0044c304  CMP EAX,[0x005ea4e0] / JS / clamp to len - 1
 *   0044c312  MOV AL,byte [EAX + 0x005e94e0]
 *   0044c355  CMP [0x005e948c],4 / JNE -> keep the lookup
 *   0044c35e  EAX = [0x005e94d4]                    ; the flat override
 *   0044c363  XOR AL,0xff                           ; 255 - value
 *
 * The SHR is logical, but EAX is already non-negative by the line above, so
 * an arithmetic shift would give the same answer; the clamp is what makes
 * that true and it is why it is transcribed before the shift rather than
 * folded into the index clamp. */
unsigned stage_fog_byte(const stage_fog *f, float view_z)
{
    int32_t z, idx;

    if (!f || f->len <= 0) {
        return 255u;                    /* "no fog": specular alpha 1.0 */
    }
    if (f->mode == 4) {
        return (unsigned)((f->flat ^ 0xff) & 0xff);
    }
    z = fistp(view_z);
    if (z < 0) { z = 0; }
    idx = (int32_t)((uint32_t)z >> 13);
    if (idx >= f->len) { idx = f->len - 1; }
    return (unsigned)(f->ramp[idx] ^ 0xffu);
}

/* ------------------------------------------------- the texture animation */

unsigned stage_texslot(const stage *s, unsigned slot, int32_t time_base)
{
    unsigned rate, phase;

    if (!s->have_ld || slot >= LD_TEXSLOTS) {
        return slot;
    }
    /* FUN_004505a0.  `MOVZX EAX,byte [EBX + 0x5bc908]` is LOCALDAT +0x0f98 +
     * slot; zero means "use the slot as it stands". */
    rate = ld_byte(&s->ld, LD_TEXRATE_OFF + slot);
    if (rate == 0) {
        return slot;
    }
    /* `FILD [0x006c2118]` / `FMUL [ECX*4 + 0x0057bbd0]`, and the table at
     * 0x0057bbd0 is filled by 0x0041e48f with 1/i (and 1.0 at index 0).  The
     * rate byte is therefore a DIVISOR - a larger rate animates SLOWER.  That
     * is the opposite of what the name suggests and it is why it is written
     * down here rather than left to the reader. */
    phase = (unsigned)fistp((float)time_base / (float)rate) & 7u;
    return ld_byte(&s->ld, LD_TEXANIM_OFF + slot * LD_TEXANIM_PHASES + phase);
}

/* -------------------------------------------- the transformed-vertex cache */

static stage_vtx *vtx_get(stage *s, const stage_view *v, uint32_t idx,
                          stage_stats *st)
{
    stage_vtx *slot = &s->vcache[idx & STAGE_VCACHE_MASK];
    int32_t p[3];
    float x, y, z, sc;

    /* FUN_0044afce: direct-mapped on the low 12 bits, tagged with the FULL
     * index, so two points 4,096 apart evict each other every time. */
    if (slot->tag == idx) {
        if (st) { st->vtx_hits++; }
        return slot;
    }
    if (st) { st->vtx_transformed++; }
    slot->tag = idx;

    /* FUN_0044afe9's three FILDs take the RAW 16.16 integers.  The /65536 is
     * carried in the matrix; a port that scales here scales twice. */
    cdat_pnt(&s->cdat, (int32_t)idx, p);
    x = (float)p[0];
    y = (float)p[1];
    z = (float)p[2];

    /* FUN_0044b1ab: four rows of four floats, basis in the columns,
     * translation in row 3.  v' = v * M. */
    slot->vx = v->m[0] * x + v->m[4] * y + v->m[ 8] * z + v->m[12];
    slot->vy = v->m[1] * x + v->m[5] * y + v->m[ 9] * z + v->m[13];
    slot->vz = v->m[2] * x + v->m[6] * y + v->m[10] * z + v->m[14];

    /* 0x0044b01d / 0x0044b024 zero both screen coordinates BEFORE the near
     * test, so a point behind the near plane keeps (0,0) rather than a NaN. */
    slot->sx = 0.0f;
    slot->sy = 0.0f;

    /* `FCOMP [0x0057b09c]` - 4096.0f in 16.16, i.e. 0.0625 world units. */
    if (!(slot->vz < v->near_z)) {
        sc = v->focal / slot->vz;
        slot->sx =  slot->vx * sc + v->centre_x;
        slot->sy = -slot->vy * sc + v->centre_y;   /* FCHS, 0x0044b06a */
    }
    return slot;
}

/* ------------------------------------------------------- the visible grid */

static void grid_rebuild(stage *s, const stage_view *v)
{
    int32_t r     = v->radius;
    int32_t two_r = r * 2;                    /* [0x005f1020] = R + R        */
    int32_t ox    = sar21(v->eye_x) - r;      /* [0x005f1034]                */
    int32_t oz    = sar21(v->eye_z) - r;      /* [0x005f1038]                */
    int32_t i;

    /* 0x004528b4: the whole rebuild is skipped while the eye stays in the
     * same cell, which is what makes a full sweep of the zube array per
     * rebuild affordable. */
    if (s->grid_valid && ox == s->grid_x && oz == s->grid_z) {
        return;
    }

    /* the other view's grid, if it is the one wanted: swap it in.  The
     * grid is a pure function of (ox, oz, r) and the zube array, so this is
     * the grid the sweep below would build; the skip above still tests (ox,
     * oz) alone, as it always has. */
    if (s->prev_valid && s->prev_x == ox && s->prev_z == oz
        && s->prev_r == r) {
        int32_t tmp[STAGE_GRID_DWORDS];
        int32_t tx = s->grid_x, tz = s->grid_z, tr = s->grid_r;
        int     tv = s->grid_valid;

        memcpy(tmp, s->grid, sizeof tmp);
        memcpy(s->grid, s->prev_grid, sizeof s->grid);
        memcpy(s->prev_grid, tmp, sizeof tmp);
        s->grid_x = ox; s->grid_z = oz; s->grid_r = r; s->grid_valid = 1;
        s->prev_x = tx; s->prev_z = tz; s->prev_r = tr; s->prev_valid = tv;
        return;
    }
    if (s->grid_valid) {                        /* keep the one being left */
        memcpy(s->prev_grid, s->grid, sizeof s->grid);
        s->prev_x = s->grid_x;
        s->prev_z = s->grid_z;
        s->prev_r = s->grid_r;
        s->prev_valid = 1;
    }
    s->grid_x = ox;
    s->grid_z = oz;
    s->grid_r = r;
    s->grid_valid = 1;

    for (i = 0; i < STAGE_GRID_DWORDS; i++) {
        s->grid[i] = (int32_t)STAGE_GRID_EMPTY;
    }

    /* 0x004528e2: a linear sweep of EVERY zube record, in whatever order the
     * array is in.  This is why the port does not have to reproduce
     * FUN_00420b34's load-time selection sort - the sweep
     * writes into a grid keyed by the cell, so file order cannot matter. */
    for (i = 0; i < s->cdat.nzube; i++) {
        uint32_t key = cdat_zube_key(&s->cdat, i);
        uint32_t cx, cz, dx, dz;

        if (key == CDAT_ZUBE_KEY_END) {
            continue;
        }
        cx = key & CDAT_CELL_MASK;
        cz = (key >> 11) & CDAT_CELL_MASK;

        /* The subtraction is masked back into 11 bits, which is what lets the
         * SAR above and FUN_00440424's SHR disagree about sign without
         * consequence: both are correct modulo 2048. */
        dx = ((uint32_t)cx - (uint32_t)ox) & CDAT_CELL_MASK;
        if (dx > (uint32_t)two_r) {
            continue;
        }
        dz = ((uint32_t)cz - (uint32_t)oz) & CDAT_CELL_MASK;
        if (dz > (uint32_t)two_r) {
            continue;
        }
        s->grid[(int32_t)(dz * STAGE_GRID_STRIDE + dx)] = i;
    }
}

void stage_grid_build(stage *s, const stage_view *v)
{
    grid_rebuild(s, v);
}

/* ------------------------------------------------------------- the reject */

static int poly_reject(stage *s, const stage_view *v, const uint8_t *pol,
                       stage_stats *st)
{
    const stage_vtx *slot;
    int32_t xi, zi;

    /* FUN_00452c90's first half.  The game gates it on two globals; the port
     * exposes the mask and treats zero as "no flag test", which is what the
     * game's own `JZ 0x00452cb9` does. */
    if (v->flag_mask != 0
        && (cdat_u16(pol, CDAT_POL_FLAGS) & v->flag_mask) != 0) {
        if (st) { st->polys_culled_flag++; }
        return 1;
    }

    slot = vtx_get(s, v, cdat_u32(pol, CDAT_POL_CORNER0), st);

    /* `FLD [EDX+0x10]` / `FABS` / `FISTP`, then `FLD [EDX+0x18]` /
     * `FADD [0x005f2048]` / `FISTP`, then `CMP` / `JNS`.
     *
     * [0x005f2048] HAS EXACTLY ONE ABSOLUTE REFERENCE IMAGE-WIDE - this FADD
     * - and no writer, and the image holds 0.0f there
     * (`python tools/ralimg.py refs 0x5f2048`).  So the addend is dead and
     * the test is exactly `round(z) >= round(|x|)`: the two side planes of a
     * 90-degree horizontal wedge, applied to CORNER 0 ONLY.
     *
     * There is no far plane here.  The draw radius IS the far plane, and it
     * is the grid's, not the frustum's. */
    if (v->cull_all_corners) {
        /* the patch: out only when every corner is past the same side
         * plane - right (z < x) or left (z < -x).  A quad that straddles
         * the wedge, or wraps behind the eye, goes on to the near clip. */
        int c, right = 1, left = 1;
        float wk = v->wedge_k > 1.0f ? v->wedge_k : 1.0f;
        for (c = 0; c < 4 && (right || left); c++) {
            const stage_vtx *p = c == 0 ? slot : vtx_get(s, v,
                cdat_u32(pol, (unsigned)(CDAT_POL_CORNER0 + c * 4)), st);
            if (!(p->vz * wk <  p->vx)) right = 0;
            if (!(p->vz * wk < -p->vx)) left  = 0;
        }
        if (right || left) {
            if (st) { st->polys_culled_geom++; }
            return 1;
        }
        return 0;
    }

    xi = fistp(fabsf(slot->vx));
    zi = fistp(v->wedge_k > 1.0f ? slot->vz * v->wedge_k : slot->vz);
    if (zi < xi) {
        if (st) { st->polys_culled_geom++; }
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------- the depth key */

static uint32_t depth_key(stage *s, const stage_view *v, const uint8_t *pol,
                          stage_stats *st)
{
    unsigned tex = pol[CDAT_POL_TEX0];
    int32_t  key;

    /* FUN_004531d6.  Two ways to reduce four corner depths to one, chosen by
     * bit 1 of the texture's flag byte: the MINIMUM when the bit is set,
     * otherwise the mean (four adds then `SAR ...,2`). */
    if (s->texflags[tex] & STAGE_TEXFLAG_MINZ) {
        int c;
        key = fistp(vtx_get(s, v, cdat_u32(pol, CDAT_POL_CORNER0), st)->vz);
        for (c = 1; c < 4; c++) {
            int32_t z = fistp(vtx_get(s, v,
                        cdat_u32(pol, (unsigned)(CDAT_POL_CORNER0 + c * 4)),
                        st)->vz);
            if (z < key) {          /* `JS` on (z - key), i.e. keep the min */
                key = z;
            }
        }
    } else {
        float sum = 0.0f;
        int c;
        for (c = 0; c < 4; c++) {
            sum += vtx_get(s, v,
                       cdat_u32(pol, (unsigned)(CDAT_POL_CORNER0 + c * 4)),
                       st)->vz;
        }
        key = fistp(sum) >> 2;      /* `FISTP` then `SAR dword ...,0x2` */
    }
    /* `ADD ...,0x2000000` / `SHR ...,0xa` - a bias that makes the key
     * unsigned for anything within 512 world units behind the eye, then a
     * 1,024-unit quantisation so the sort has few distinct keys. */
    return ((uint32_t)key + 0x2000000u) >> 10;
}

/* ------------------------------------------------------------ the two lists */

static void list_push(stage_item *list, int *n, int *overflow,
                      uint32_t key, int32_t poly)
{
    if (*n >= STAGE_LIST_MAX) {
        /* The original does not do this.  Neither FUN_00452c55 nor
         * FUN_00452c28 bounds-checks its count; the port refuses to walk off
         * the end of its own array and says how often it had to. */
        (*overflow)++;
        return;
    }
    list[*n].key  = key;
    list[*n].poly = poly;
    (*n)++;
}

static int item_cmp(const void *a, const void *b)
{
    uint32_t ka = ((const stage_item *)a)->key;
    uint32_t kb = ((const stage_item *)b)->key;
    /* The original's two quicksorts (0x00452e83, 0x00452df5) compare the key
     * dword with `CMP` / `JC` / `JA`, i.e. UNSIGNED and ASCENDING. */
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

/* A STABLE sort, not the libc's qsort.  The depth list's key is an
 * integer view z, so ties are common, and qsort leaves their order to the C
 * library.  Measured 2026-10-05 on LEVEL33 (eye +4): glibc 2.39 and the
 * Windows .exe's msvcrt ordered the same tied items differently, giving 122
 * and 127 draw calls for one frame; with this sort both give 122, and the
 * Linux log and frame are byte-identical to qsort's (glibc's qsort was
 * already stable here).  Ties go in insertion order, i.e. the walk's order.
 * The ORIGINAL's tie order, its own quicksorts at 0x00452e83 / 0x00452df5,
 * is NOT transcribed:.  Falls back to qsort only if the
 * scratch allocation fails. */
static void item_sort(stage_item *a, size_t n)
{
    /* the scratch lives across frames and the passes swap buffers
     * instead of copying back after each one; the merges are the same, so the
     * order is. */
    static stage_item *scratch;
    static size_t      scratch_cap;
    stage_item *src = a, *dst, *t;
    size_t w, i;
    if (n < 2) { return; }
    if (n > scratch_cap) {
        stage_item *bigger = (stage_item *)realloc(scratch, n * sizeof *bigger);
        if (!bigger) { qsort(a, n, sizeof *a, item_cmp); return; }
        scratch     = bigger;
        scratch_cap = n;
    }
    dst = scratch;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            size_t lo = i, mid = (i + w < n) ? i + w : n;
            size_t hi = (i + 2 * w < n) ? i + 2 * w : n;
            size_t l = lo, r = mid, k = lo;
            while (l < mid && r < hi) {
                dst[k++] = (src[r].key < src[l].key) ? src[r++] : src[l++];
            }
            while (l < mid) { dst[k++] = src[l++]; }
            while (r < hi)  { dst[k++] = src[r++]; }
        }
        t = src; src = dst; dst = t;
    }
    if (src != a) { memcpy(a, src, n * sizeof *a); }
}

/* ------------------------------------------------------ the polygon walk */

static void walk_cell(stage *s, const stage_view *v, int32_t zi,
                      stage_stats *st)
{
    int32_t z[8];
    int32_t first, last, i;

    cdat_zube(&s->cdat, zi, z);
    first = z[2];                     /* zube +0x08 */
    last  = z[3];                     /* zube +0x0c */

    /* FUN_00452b97's two guards, in its order: -1 means the cell owns no
     * polygons, and the `JA` is UNSIGNED, so `first > last` also returns. */
    if (first == -1 || (uint32_t)first > (uint32_t)last) {
        return;
    }
    if (st) { st->cells_visible++; }

    for (i = first; (uint32_t)i <= (uint32_t)last; i++) {
        const uint8_t *pol = cdat_pol(&s->cdat, i);
        unsigned cls = pol[CDAT_POL_CLASS];
        unsigned tex;

        if (st) { st->polys_walked++; }

        if (cls == CDAT_POLY_ROUTE) {
            /* The 0xff layer - 12.5% of every level, its own material
             * namespace ([0x005f0c1c] / [0x005f0e1c]) and its own entry
             * point.  FUN_00453161 writes 1 into EVERY entry of
             * [0x005f0c1c], so one of these records ALWAYS takes the
             * depth-sorted list.
             *
             * READ 2026-09-14: it is an OBJECT INSTANCE, not a
             * polygon - a type at +0x01, an anchor `pnts` index at +0x04 and
             * two Euler triples - and FUN_0044fb03 emits nothing.  It
             * unpacks the record into globals and calls the .ro3 MODEL path
             * at FUN_0044e409.  Of the 237,428 such records in the corpus
             * 81.35% carry a type gated on [0x005bbfec], which nothing in
             * RAL.EXE writes, so the original draws them no more than this
             * port does.  Drawing the other 18.65% is the work, not this
             * file's.
             *
             * the other 18.65% IS drawn now, by
             * port/obj/objscene.c, and the hook below is how.  The record
             * still is not a polygon and this file still does not know what a
             * .ro3 is - it hands the index over and goes on walking. */
            if (st) { st->polys_route++; }
            if (s->obj_fn) {
                s->obj_fn(s->obj_ctx, v, i);
            }
            continue;
        }
        if (cls >= CDAT_POLY_DRAWABLE_MAX) {
            continue;                 /* `CMP AL,0x10` / `JNC` */
        }
        if (poly_reject(s, v, pol, st)) {
            continue;
        }

        /* 0x00452bcc.  The primary id is used WHEN IT IS NON-ZERO; a zero
         * primary falls back to the SECONDARY id. records
         * that `+0x21 == 0` disables the second layer; this is the same test
         * from the other end and it is the renderer's, not the surface
         * query's. */
        tex = pol[CDAT_POL_TEX0];
        if (tex == 0) {
            tex = pol[CDAT_POL_TEX1];
        }

        if (s->texflags[tex] & STAGE_TEXFLAG_SORTED) {
            list_push(s->list_sorted, &s->n_sorted, &s->overflow_sorted,
                      depth_key(s, v, pol, st), i);
            if (st) { st->polys_listed_sorted++; }
        } else {
            /* FUN_00452c55: `SHL EAX,0x10` / `MOV AX,BX`.  EAX is the id
             * AFTER the [0x005f0d1c] remap, which FUN_00453161 fills from the
             * PLATE (texinfo slot +0x20 = record >> 4) through a handle table
             * - so the batch key is the physical texture, sixteen slots to
             * one plate, and the low half is the polygon index for a stable
             * sort.  The guard is the original's. */
            uint32_t key;
            if ((uint32_t)i >= 0x1000000u) {
                continue;             /* `CMP EBX,0x1000000` / `JNC` - dropped */
            }
            key = ((uint32_t)s->plate[tex] << 16) | ((uint32_t)i & 0xffffu);
            list_push(s->list_tex, &s->n_tex, &s->overflow_tex, key, i);
            if (st) { st->polys_listed_tex++; }
        }
    }
}

/* ------------------------------------------------------ vertex assembly */
/* TRANSCRIBED, from the pipeline at FUN_0045026e - and
 * finished the uvs, which were the one thing here that was still the port's
 * own. */

/* [0x005f0998], read verbatim from the image.  Eight rows of four selectors,
 * one per corner, in the units the original uses (a byte offset of 0/4/8/12
 * into the texinfo slot's four uv pairs) divided by 4.
 *
 *   0045142e  MOV ESI,[ESI*0x10 + 0x5f0998]     ; sym*16 + corner*4
 *   00451436  MOV AX,word ptr [ESI + EBX*0x1]   ; EBX = the texinfo slot
 *
 * With corner 0 = (0,0), 1 = (1,0), 2 = (0,1), 3 = (1,1) these are exactly
 * the eight symmetries of the square: row 1 flips u, row 2 flips v, row 3 is
 * the half turn, row 6 is the transpose.  Two references image-wide and NO
 * writer, so the table is the image's and not a run-time build. */
const uint8_t stage_uv_sym[STAGE_UV_SYMMETRIES][4] = {
    { 0, 1, 2, 3 },   /* identity                 */
    { 1, 0, 3, 2 },   /* flip u                   */
    { 2, 3, 0, 1 },   /* flip v                   */
    { 3, 2, 1, 0 },   /* half turn                */
    { 2, 0, 3, 1 },   /* quarter turn             */
    { 3, 1, 2, 0 },   /* quarter turn, flipped    */
    { 0, 2, 1, 3 },   /* transpose                */
    { 1, 3, 0, 2 }    /* anti-transpose           */
};

/* [0x005f0890], three dwords of static .data with three readers and no
 * writer.  `python tools/ralimg.py bytes 0x5f0890 16` gives
 *
 *   0x005f0890  03 00 00 00 05 00 00 00 04 00 00 00 00 00 00 00
 *
 * and the port keeps only the three entries, because entry 3 and above is 0
 * and mode 0 is not an arm the stage can be drawn by - see layer2_mode. */
const uint8_t stage_layer_mode[STAGE_LAYER_MODES] = { 3u, 5u, 4u };

/* FUN_004505a0 at 0x004505ee takes the symmetry from the CLASS byte's low
 * three bits for layer 0, and at 0x0045064a from pols+0x20's low three bits
 * for layer 1.  Both index the same table.
 *
 * TRANSCRIBED END TO END, - the base pairs are no longer the port's
 * invention.  FUN_004511ab (layer 0) and FUN_004513e3 (layer 1) are the same
 * 568-byte function with a different input:
 *
 *   004511f6  ESI = [sym*16 + 0x5f0998]        the corner selector, 0/4/8/12
 *   004511fe  AX  = word [ESI + EBX]           u, EBX = the texinfo slot
 *   00451202  SHL EAX,0x8                      and the same four instructions
 *   0045120c  AX  = word [ESI + EBX + 2]       for v, three more times
 *
 * and FUN_0044c396, reached from FUN_00450e81 via 0x0044ae2b, turns each of
 * those integers into the float the vertex carries:
 *
 *   0044c3c1  EAX = slot uv;  TEST EAX,EAX;  JZ -> leave it
 *   0044c3cd  FILD / FSTP float     the integer as a float
 *   0044c3de  SUB EAX,0xc000000     ...MINUS 24 EXPONENT STEPS = * 2^-24
 *
 * (u16 << 8) * 2^-24 == u16 / 65536, so THE STORED u16 IS A 0.16 FIXED-POINT
 * TEXTURE COORDINATE.  The zero test is not an optimisation: 0.0f has no
 * exponent field to subtract from and the subtraction would produce a huge
 * negative number.'s family, exactly.
 *
 * AND THE ONE THING A PORT WOULD NEVER GUESS - 0x004512a0:
 *
 *   0045129d  EAX = [texinfo slot + 0x20]      the PLATE id
 *   004512a0  CMP EAX,0x320  JNC -> skip       >= 800
 *   004512a7  CMP EAX,0x190  JC  -> skip       <  400
 *   004512ae  SHL [u0],1 ... eight of them     ALL EIGHT VALUES DOUBLED
 *
 * The uvs of plates 400..799 are doubled.  Why is not stated anywhere in the
 * code - but it is measurable in the corpus, and the corpus is decisive: in
 * `files\a`, ALL 320 plates with an id in 400..799 are 128x128 and every plate
 * outside that window is 256x256.  So the coordinates are authored as a
 * fraction of a 256-texel plate and the half-size band is renormalised.
 *
 *   ids 400..799 in files\a : 320 of 320 are 128x128
 *   ids outside, in files\a : 282 at 256x256, plus the two odd plates
 *
 * TRANSCRIBE THE ID WINDOW, DO NOT INFER FROM THE SIZE.  files\b holds 16
 * plates that are 128x128 and are NOT in 400..799; a port that scaled by the
 * measured dimensions would differ from the original on every one of them.
 * Standing rule 1.
 *
 * STILL THE PORT'S OWN, and narrower than it was: the alternate uv source at
 * 0x004512ec, which replaces all four pairs from a table at [0x00581a08] when
 * the texinfo slot's SIGNED byte +0x2d is positive.  What that is has not been
 * read. */
#define STAGE_UV_DOUBLE_LO 0x190u   /* 0x004512a7 */
#define STAGE_UV_DOUBLE_HI 0x320u   /* 0x004512a0 */

static void quad_uv(const stage *s, unsigned slot, unsigned sym,
                    float u[4], float v[4])
{
    unsigned dbl = 0u;
    int i;

    if (slot < LD_TEXSLOTS) {
        unsigned plate = s->plate[slot];
        dbl = (plate >= STAGE_UV_DOUBLE_LO && plate < STAGE_UV_DOUBLE_HI)
              ? 1u : 0u;
    }
    for (i = 0; i < 4; i++) {
        unsigned c = stage_uv_sym[sym & 7u][i];
        unsigned su = 0u, sv = 0u;

        if (slot < LD_TEXSLOTS) {
            su = s->uv[slot][c][0];
            sv = s->uv[slot][c][1];
        }
        u[i] = (float)(su << dbl) * (1.0f / 65536.0f);
        v[i] = (float)(sv << dbl) * (1.0f / 65536.0f);
    }
}

/* The per-corner shade byte at pols+0x14..+0x17 is an INDEX into a 256-entry
 * ARGB table, not a grey level.  FUN_004507cb at 0x004507d1:
 *
 *   MOVZX EAX,byte [ESI+0x14] / MOV EAX,[EAX*0x4 + 0x5e7fdc]
 *
 * and FUN_0044d1ad fills that table, entry for entry:
 *
 *   0044d1bc  EAX = hi[k] - lo[k];  IMUL i;  IDIV 255;  ADD lo[k]
 *   0044d1ed  OR ECX,0xff000000
 *
 * so table[i] = lerp(lo, hi, i/255) with ALPHA FORCED TO 0xff.  The x87 order
 * makes the first channel the high one, i.e. 0xffRRGGBB.  IDIV truncates
 * toward zero, which is not what a float lerp does, so this is integer
 * arithmetic on purpose. */
static uint32_t shade_colour(const stage_view *v, unsigned shade)
{
    uint32_t c = 0xff000000u;
    int k;

    for (k = 0; k < 3; k++) {
        int lo = (int)v->shade_lo[k];
        int d  = (int)v->shade_hi[k] - lo;
        int x  = lo + (d * (int)shade) / 255;      /* IDIV: toward zero */

        if (x < 0)   { x = 0; }
        if (x > 255) { x = 255; }
        c |= (uint32_t)x << (16 - 8 * k);
    }
    return c;
}

/* The DIFFUSE modulate.  See stage.h for why 0xff is not the identity.
 *
 * FUN_0044d9bc's scalar arm, 0x0044da14 onward, is three copies of
 *
 *   AND EAX,0xff / AND EDX,0xff / MUL EDX / SHR EAX,8 / SHL EAX,k / OR EBP,EAX
 *
 * for k = 0, 8, 16 - so byte 3 is never written and comes out ZERO.  The MMX
 * arm 0x0044d9df widens all four bytes, but the fourth byte of the global
 * colour is 0 in every state this port can reach, so it agrees. */
uint32_t stage_modulate8(uint32_t c, uint32_t m)
{
    uint32_t out = 0;
    int k;

    for (k = 0; k < 3; k++) {
        uint32_t a = (c >> (k * 8)) & 0xffu;
        uint32_t b = (m >> (k * 8)) & 0xffu;
        out |= ((a * b) >> 8) << (k * 8);
    }
    return out;
}

/* FUN_0044c516, transcribed to the end of the basic block (standing rule 3 -
 * this is the quote stopped one instruction short of):
 *
 *   0044c516  [0x005f0b14] = [0x005bd538] * 0x20        ; zfar = R cells
 *   0044c5d5  EAX = 0x64 - [0x005bd93c]                 ; 100 - detail
 *   0044c5e5  clamp to >= 0x5a, then 0044c5f8 clamp to <= 0x64
 *   0044c60b  [0x005d70b8] = D * 0.01f                  ; 0.90 .. 1.00
 *   0044c61d  [0x005d705c] = (float)(zfar << 16) / [0x005d70b8]
 *   0044c640  [0x005d7060] = 1.0f / [0x005d705c]
 *
 * so the scale is D / (zfar * 65536) and sz = view_z * that.  A corner at the
 * far plane gets sz = D, 0.90 .. 1.00.  THE FAR PLANE IS THE CELL GRID: zfar
 * is R * 32 world units and a cell is 32 world units, so the depth range ends
 * exactly where the visible-cell grid does.  View z is in RAW 16.16 units,
 * which is the same reading the near plane needs to make sense (4096.0f is
 * 0.0625 world units, not 4096 of them). */
float stage_depth_scale(int32_t radius, int32_t detail)
{
    int32_t zfar = radius * 32;
    int32_t d    = 100 - detail;

    if (d < 90)  { d = 90; }      /* `CMP 0x5a` / `JNS` at 0x0044c5e5 */
    if (d > 100) { d = 100; }     /* `CMP 0x64` / `JS`  at 0x0044c5f8 */

    return ((float)d * 0.01f) / ((float)zfar * 65536.0f);
}

/* ===================================================================== *
 *  THE VIEW-SPACE CLIP - FUN_0044AD8A's three gates,   *
 * ===================================================================== *
 *
 * The shared primitive submit is 160 bytes and nobody had printed it until
 * `python tools/ralimg.py disasm 0x44ad8a 160` prints all three
 * gates at once:
 *
 *   0044add7  CALL 0x0044c292     ; count corners out of the near/far range
 *   0044ade1  CMP EAX,[0x005d6f8c] / JE  -> RETURN    ; ALL out: draw nothing
 *   0044adfa  TEST EAX,EAX        / JZ  -> skip       ; none out: no clip
 *   0044adfc  CALL 0x0044bbea                         ; SOME out: CLIP
 *   0044ae01  CMP [0x005d6f8c],3  / JB  -> RETURN     ; the clip REWROTE it
 *   0044ae0a  CALL 0x0044b1f5                         ; only now the screen clip
 *
 * and FUN_0044BBEA is two passes over the same 790-byte body:
 *
 *   0044bbf4  [0x005d6fb0] = 0x1000        ; the NEAR threshold, as an integer
 *   0044bbfe  CALL 0x0044bc2e              ; pass 1
 *   0044bc03  CMP [0x005d4240],0 / JNE -> RET
 *   0044bc0c  CMP [0x005d4630],0 / JZ  -> RET
 *   0044bc15  CMP [0x005d6f8c],3 / JS  -> RET
 *   0044bc1e  [0x005d6fb0] = [0x005d70c4]  ; the FAR threshold
 *   0044bc28  CALL 0x0044bf45              ; pass 2
 *
 * FUN_0044BF45 IS FUN_0044BC2E WITH THREE BITS FLIPPED.  Diffing the two
 * 790-byte listings with the addresses stripped leaves seven lines, and three
 * of them are the branch conditions - `JB`/`JAE` swapped at 0x0044BC85,
 * 0x0044BC93 and 0x0044BCA7 - two are the relocated targets of the other
 * four, and one is the relative displacement of the identical CALL to
 * 0x0044BBDC.  Every float instruction is byte-identical.  So it is ONE clip
 * routine parameterised by a threshold and a comparison sense, and `inside`
 * is `thresh < z` for the near pass and `thresh >= z` for the far one.  Note
 * the two agree at the boundary rather than overlapping: z == thresh is
 * OUTSIDE for near and INSIDE for far.
 *
 * The two flags the far pass is gated on are both measured:
 *   [0x005d4240] has THREE references image-wide - this read, and the `= 1` /
 *   `= 0` pair at 0x004534C5/0x004534F7 that brackets one sub-render.  It is
 *   0 in .data.
 *   [0x005d4630] has SIX - this read, two unconditional `= 1` stores at
 *   0x00440F84 / 0x0044107B inside a run of NINE adjacent flags all set to 1
 *   together, and a push/zero/pop around 0x004539C7.
 * So in the ordinary frame the far pass RUNS, and this port runs it.
 *
 * THE FAR THRESHOLD IS NOT THE FAR PLANE.  0x0044C672 is
 * `FLD [0x005d705c] / FMUL [0x005d70c0] / FISTP [0x005d70c4]`, and
 * [0x005d70c0] has ONE reference image-wide - that FMUL - and no writer:
 * `66 66 66 3f` = 0.9f.  So the clip's far plane is 90% of the depth scale's,
 * while FUN_0044C292's out-of-range COUNT uses [0x005d705c] itself.  The two
 * deliberately disagree: a corner between 0.9*far and far is "in range", so
 * on its own it triggers no clip, but if some OTHER corner does trigger one
 * it is clipped at 0.9.  That is what the instructions say and it is
 * transcribed rather than tidied.
*/

/* One corner of the working polygon: the 0x44-stride record at [0x0091d910]
 * reduced to the fields FUN_0044BC2E actually interpolates, plus the two
 * screen coordinates it reprojects.
 *
 * WHAT THE ORIGINAL INTERPOLATES, field for field (0x0044BCE9-0x0044BE2F):
 *   cache +0x10 / +0x14   view x, y            float lerp
 *   cache +0x18           view z               NOT lerped - set to the
 *                                              threshold exactly, 0x0044BD0D
 *   working +0x08 / +0x0c uv layer 0           float lerp
 *   working +0x10 / +0x14 uv layer 1           float lerp
 *   working +0x18/+19/+1a colour R, G, B       byte lerp through FILD/FISTP
 *   working +0x20         THE FOG BYTE         byte lerp  (not in this
 *                                              port yet - noted, not ported)
 *   working +0x21         the layer-1 blend weight, [0x005f0924]  byte lerp
 *
 * and NOT working +0x1c, the added-light dword FUN_00451712 computes.  A
 * clip-created corner inherits whatever is in the scratch slot.  That is a
 * reading of an omission, not of a store, so it is recorded and not relied
 * on; the port has no dynamic lights to carry anyway.
 *
 * This port keeps ONE uv pair per corner because its second texture layer is
 * a second pass (see draw_list), so a layer-1 polygon runs the whole clip
 * again with the other pair - which is arithmetically the same lerp. */
typedef struct {
    float    vx, vy, vz;      /* cache +0x10 / +0x14 / +0x18                */
    float    sx, sy;          /* cache +0x0c / +0x1c                        */
    float    tu, tv;          /* working +0x08/+0x0c or +0x10/+0x14         */
    uint32_t argb;            /* working +0x18..+0x1a, and +0x21 as alpha   */
    /* working +0x20.  Written by the applier at 0x0044ADEE, which runs
     * BEFORE the clip at 0x0044ADFC - so the clipper interpolates an ALREADY
     * FOGGED byte rather than re-fogging a new corner from its own view z.
     * 0x0044BDE1 lerps it as a byte, through FILD/FISTP, exactly like the
     * three colour channels beside it. */
    uint8_t  fog;
    int      pinned;          /* the port's own: sx/sy are the (0,0)
                               * placeholder, not a projection.  Only the
                               * assertion counter reads it. */
} stage_corner;

/* 0x0044BE3B-0x0044BE88, the clip's projection of a new corner.  Its
 * ARITHMETIC is byte-identical to FUN_0044afe9's: 0x0044BE4D-0x0044BE88 and
 * 0x0044B045-0x0044B07A are the same 53 bytes of instructions, and only the
 * FLD/FSTP around them differ, in addressing mode - the transform uses
 * [EDX+0x10..0x18], the clip three scratch slots.  So a clip-created corner is
 * projected by the same code that projects an ordinary one.
 * THAT IS THE ANSWER TO the item 6: the new corners do not inherit an
 * sx/sy, they get a real projection, and they get it here.
 *
 * The one branch inside it, `CMP [0x005bbfe8],0` at 0x0044BE4D, substitutes
 * (float)[0x00581ac8] + 1 for the divisor.  [0x005bbfe8] has SIXTEEN absolute
 * references image-wide; fifteen decode as `CMP dword ptr [0x005bbfe8],0` and
 * the sixteenth (0x00474d23) is not an instruction boundary.  NO WRITER, and
 * .data holds 0.  The branch is dead in the shipped binary, so it is recorded
 * here and not ported. */
static void corner_project(const stage_view *v, stage_corner *c)
{
    float sc = v->focal / c->vz;

    c->sx =  c->vx * sc + v->centre_x;
    c->sy = -c->vy * sc + v->centre_y;   /* FCHS at 0x0044BE72 */
    c->pinned = 0;
}

/* 0x0044BD63-0x0044BE2F, five times over.  Each byte goes out through
 * `XOR EAX,EAX / MOV AL,byte` into a dword scratch, `FILD`, lerp, `FISTP`,
 * and only the low byte of the result is stored - so the rounding is
 * round-to-nearest-even and there is no clamp.  t is in [0,1] by
 * construction, so no clamp is needed; the mask is the transcription. */
/* 0x0044BDE1, the fog byte's own copy of the same five-instruction shape.
 * Split out rather than folded into corner_lerp_argb's loop because the fog
 * byte is NOT part of the ARGB dword in either the original or this port: it
 * lives at working +0x20 and ends up in the SPECULAR alpha, not the diffuse. */
static uint8_t corner_lerp_byte(uint8_t a, uint8_t b, float t)
{
    float fa = (float)a, fb = (float)b;

    return (uint8_t)(fistp(fa + (fb - fa) * t) & 0xff);
}

static uint32_t corner_lerp_argb(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0u;
    int k;

    for (k = 0; k < 4; k++) {
        float fa = (float)((a >> (k * 8)) & 0xffu);
        float fb = (float)((b >> (k * 8)) & 0xffu);

        out |= (uint32_t)(fistp(fa + (fb - fa) * t) & 0xff) << (k * 8);
    }
    return out;
}

/* FUN_0044C292, 99 bytes: walk every corner and count the ones whose view z
 * is below the near value or at/beyond the far one.
 *
 *   0044c2bc  FLD [0x0057b09c] / FLD [EDX+0x18] / FCOMPP / JB  -> count
 *   0044c2cf  FLD [0x005d705c] / FLD [EDX+0x18] / FCOMPP / JAE -> count
 *
 * [0x0057b09c] is the same 4096.0f FUN_0044afe9 pins against and [0x005d705c]
 * is the far value the depth scale is built from, so the count tests the same
 * two numbers the rest of the path already uses. */
static int corners_out_of_range(const stage_corner *ring, int n,
                                float near_z, float far_z)
{
    int i, out = 0;

    for (i = 0; i < n; i++) {
        if (ring[i].vz < near_z || ring[i].vz >= far_z) { out++; }
    }
    return out;
}

/* FUN_0044BC2E / FUN_0044BF45 - one Sutherland-Hodgman pass against one
 * view-space z plane.  `near_pass` selects the comparison sense; everything
 * else is shared, which is a property of the original and not a convenience
 * of this transcription (see the header comment above).
 *
 * The original copies the order table to a scratch at [0x0091fc10], appends
 * element 0 as the wrap, walks the edges, and writes the survivors back over
 * [0x0091fb10] while EBX counts them - finishing with
 * `MOV [0x005d6f8c],EBX` at 0x0044BF3D.  This does the same with a local. */
static int clip_plane(stage_corner *ring, int n, const stage_view *v,
                      float thresh, int near_pass, stage_stats *st)
{
    stage_corner out[STAGE_POLY_MAX];
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const stage_corner *a = &ring[i];
        const stage_corner *b = &ring[(i + 1) % n];
        int ain = near_pass ? (thresh < a->vz) : (thresh >= a->vz);
        int bin = near_pass ? (thresh < b->vz) : (thresh >= b->vz);

        /* 0x0044BCAD: the in/in and in/out arms both emit the START corner
         * first; the out/in arm jumps straight past it to the interpolation
         * at 0x0044BCBC, and the out/out arm emits nothing. */
        if (ain && m < STAGE_POLY_MAX) {
            out[m++] = *a;
        }
        if (ain != bin && m < STAGE_POLY_MAX) {
            stage_corner nc;
            float d = b->vz - a->vz;
            /* 0x0044BCD2: t = (thresh - a.vz) / (b.vz - a.vz), computed
             * ONLY on a straddling edge, so d cannot be zero unless both
             * endpoints sit exactly on the plane - in which case they are
             * both OUTSIDE for the near pass and both INSIDE for the far
             * one, and this arm is not taken.  The guard is the port's. */
            float t = (d != 0.0f) ? (thresh - a->vz) / d : 0.0f;

            nc.vx = a->vx + (b->vx - a->vx) * t;
            nc.vy = a->vy + (b->vy - a->vy) * t;
            nc.vz = thresh;                    /* 0x0044BD0D, exactly */
            nc.tu = a->tu + (b->tu - a->tu) * t;
            nc.tv = a->tv + (b->tv - a->tv) * t;
            nc.argb = corner_lerp_argb(a->argb, b->argb, t);
            nc.fog  = corner_lerp_byte(a->fog, b->fog, t);   /* 0x0044BDE1 */
            corner_project(v, &nc);
            out[m++] = nc;
            if (st) { st->corners_added++; }
        }
    }
    memcpy(ring, out, (size_t)m * sizeof out[0]);
    return m;
}

/* FUN_0044BBEA, 68 bytes: the near pass always, then the far pass unless one
 * of its three guards says otherwise.  Returns the new corner count. */
static int clip_poly(stage_corner *ring, int n, const stage_view *v,
                     float near_clip, float far_clip, stage_stats *st)
{
    n = clip_plane(ring, n, v, near_clip, 1, st);   /* 0x0044BBFE */
    if (n < 3) {                                    /* 0x0044BC15, signed */
        return n;
    }
    return clip_plane(ring, n, v, far_clip, 0, st); /* 0x0044BC28 */
}

/* THE RING ORDER, and it is not the cdat corner order.
 *
 * FUN_00450E81 fills the four 0x44-stride working slots from FUN_00450388's
 * four corner indices, and the pairing is visible in four stores:
 *
 *   00450f83  ECX = [0x005f08b4]  -> [0x0091d910]   slot 0 <- corner 2
 *   00450e81  ECX = [0x005f08ac]  -> [0x0091d954]   slot 1 <- corner 0
 *   00450ed7  ECX = [0x005f08b0]  -> [0x0091d998]   slot 2 <- corner 1
 *   00450f2d  ECX = [0x005f08b8]  -> [0x0091d9dc]   slot 3 <- corner 3
 *
 * The clipper walks the slots as a RING with a wrap element, so the polygon's
 * edges are 2-0, 0-1, 1-3, 3-2.  A Sutherland-Hodgman pass over 0,1,2,3
 * instead would clip against three edges the polygon does not have.
 *
 * IT IS ALSO A CROSS-CHECK ON THE TRIANGULATION THIS FILE ALREADY HAD.
 * Fanning the ring (2,0,1,3) from its first corner gives triangles (2,0,1)
 * and (2,1,3), which are cyclic rotations of exactly the pair the old
 * tri_fwd = {0,1,2, 3,2,1} emitted - so the winding measurement in
 * and tools/stagewind.sh is UNCHANGED by this edit, and the
 * agreement is evidence that both readings are right.
 *
 * DO NOT "TIDY" THIS TO { 0, 1, 2, 3 }.  It was tried, on purpose, and EVERY
 * ASSERTION IN THE PROBE STILL PASSED - including the three new view-clip
 * ones - and it still printed "OK - a stage renders".  The picture shows it
 * at once: LEVEL63 comes out with sawtooth triangular holes bitten along
 * every terrain edge, because each quad is trimmed against its two diagonals
 * instead of its sides.  Coverage moves 30,881 -> 26,007 and the winding
 * verdict goes from a clean CW to INCONCLUSIVE, and neither of those is
 * asserted anywhere. */
static const int STAGE_RING[4] = { 2, 0, 1, 3 };

/* Which arm of FUN_00449AC7 a second layer is drawn by, from the SECOND
 * texture slot's TEXTINFO +0x24.  FUN_00450BB3 0x00450be8-0x00450bf3.  An
 * index past the table's three live entries selects mode 0, which is a flat
 * arm the stage never reaches in the shipped corpus - the port returns 0 for
 * it so the caller's counter can see it happen rather than silently drawing
 * it as mode 3. */
static unsigned layer2_mode(const stage *s, const stage_view *v, unsigned slot)
{
    unsigned sel;

    if (v->layer2_mode_override) {
        return (unsigned)v->layer2_mode_override;
    }
    sel = s->texmode[slot & 0xffu];
    return (sel < STAGE_LAYER_MODES) ? stage_layer_mode[sel] : 0u;
}

/* the BACK of a two-sided polygon (class bit 0x08), FUN_00450A9C,
 * which FUN_004502D7 calls after the front (0x0045032a .. 0x00450381) - in
 * every race, because loadlevel_init sets [0x005d41b0] (0x00442624):
 *
 *   00450aa9  swap corner slots 0 <-> 1 ([0x5f08ac] / [0x5f08b0])
 *   00450abf  and 2 <-> 3 unless corner 3 is -1 (a triangle)
 *   00450ade  layer-0 symmetry = [0x5f093c][sym & 7] | (sym & 8)
 *   00450af9  shade k  = [0x5e7fdc][pols+0x28+k]     (the front: +0x14+k)
 *   00450b39  weight k = pols+0x2c+k                 (the front: +0x22+k)
 *   00450b5d  the same mode select, then everything swapped back
 *
 * by SLOT, so slot k carries cdat corner back_corner[k] and the back's own
 * bytes k.  Layer 1's symmetry ([0x5f08fc]) is not remapped.  The reversed
 * order is the reversed winding: the hardware cull keeps the face the
 * front's culled, as the flipped facing flags do in the original. */
static const unsigned char back_sym[8] = { 1, 0, 3, 2, 6, 7, 4, 5 };  /* 0x005f093c */
#define POL_BACK_SHADE 0x28
#define POL_BACK_BLEND 0x2c
/* The same two runs of bytes, read by the FRONT as baked-light indices when
 * the flags word has bit 4 (0x0045081F, 0x00450865). */
#define POL_LIGHT1     0x28
#define POL_LIGHT2     0x2c
static uint32_t light_add(uint32_t argb, const uint8_t c[3], unsigned i);

static int build_poly(stage *s, const stage_view *v, const uint8_t *pol,
                      int layer2, unsigned mode, unsigned slot, int back,
                      plat_vertex *out, int outmax,
                      stage_stats *st)
{
    const stage_vtx *c[4];
    stage_corner ring[STAGE_POLY_MAX];
    float u[4], vv[4];
    float dscale, far_count, far_clip;
    uint32_t global, tint;
    int i, n = 4, nv = 0;
    unsigned corner[4] = { 0, 1, 2, 3 };

    if (back) {
        corner[0] = 1; corner[1] = 0;
        if (cdat_u32(pol, CDAT_POL_CORNER0 + 12u) != 0xffffffffu) {
            corner[2] = 3; corner[3] = 2;
        }
    }
    for (i = 0; i < 4; i++) {
        c[i] = vtx_get(s, v,
                   cdat_u32(pol, (unsigned)(CDAT_POL_CORNER0 + corner[i] * 4)),
                   st);
    }

    /* FUN_004503b3, the per-corner reject, and it is NOT a near-plane clip.
     *
     * WHAT THIS COMMENT USED TO SAY, AND IT WAS WRONG -:
     *   "the original has no near-plane clipper anywhere on this path".
     * IT HAS ONE.  FUN_0044BBEA, reached from 0x0044ADFC INSIDE the shared
     * primitive submit FUN_0044AD8A, is a Sutherland-Hodgman pass in VIEW
     * space against the same 4096 near value (as the integer 0x1000), and it
     * REWRITES the corner count - which is why 0x0044AE01 re-tests it >= 3.
     * Two more gates share those 160 bytes:
     *
     *   0044ade1  CMP [0x005d6f88],[0x005d6f8c] / JE -> return
     *                                      ; EVERY corner out of range: drop
     *   0044adfc  CALL 0x0044bbea          ; ANY corner out of range: clip
     *   0044ae08  CMP [0x005d6f8c],3 / JB -> return
     *                                      ; clipped below a triangle: drop
     *
     * ~~THIS PORT HAS NONE OF THE THREE~~  IT HAS ALL THREE AS OF:
     * they are below, after the uv and colour build, in the block headed
     * "FUN_0044AD8A's three gates".  Over the 48 shipped levels the pinned
     * quads were 0.35% of the geometry and 10.5% of every frame - see
     * tools/stagepinned.sh and.
     *
     * NOTE drop_pinned_quads IS STILL NOT THE FIX, and is wrong in a specific
     * way: the original does not DROP a quad with one bad corner, it CLIPS it
     * and draws the remainder.  It is now also REDUNDANT - with the clip on,
     * no pinned corner reaches the backend at all.
     *
     * What remains true below: a corner with view z below the near plane keeps
     * screen (0,0) (FUN_0044afe9), FUN_004503b3 accepts the polygon on any
     * OTHER corner, and FUN_0044b1f5 is a SCREEN-SPACE clipper against the
     * viewport rectangle that cannot trim a (0,0) corner because (0,0) is
     * inside that rectangle.  None of those three was ever the gate.
     *
     * What FUN_004503b3 does is reject the whole polygon unless SOME corner
     * has a non-negative view z and lies inside a horizontal wedge:
     *
     *   004503d2  TEST dword [EDX+0x18],0x80000000 / JNZ -> next corner
     *   004503e0  FCOM [0x005f08c0]  / JC  -> accept      ; |vx| < 2^20
     *   004503f2  FMUL [0x005f08bc]  / FCOMP [EDX+0x18]
     *   00450401  JC  -> accept                           ; |vx|*1.2 < vz
     *   004504f8  STC                                     ; no corner: reject
     *
     * Both constants have FOUR references image-wide, all of them the four
     * unrolled copies of this test, and NO writer: [0x005f08bc] is 1.2f and
     * [0x005f08c0] is 1048576.0f.  The sign test is on the BIT PATTERN, so
     * -0.0f counts as behind the camera -. */
    {
        int accept = 0;

        for (i = 0; i < 4 && !accept; i++) {
            float vx = c[i]->vx, vz = c[i]->vz;

            if (signbit(vz)) { continue; }          /* `TEST ...,0x80000000` */
            if (fabsf(vx) < 1048576.0f) { accept = 1; break; }
            if (fabsf(vx) * 1.2f < (v->wedge_k > 1.0f ? vz * v->wedge_k : vz)) { accept = 1; break; }
        }
        if (!accept) {
            return 0;
        }
    }

    /* Measured always, acted on only under the diagnostic knob.  A
     * corner below the near plane carries the (0,0) placeholder rather
     * than a projected position, so the quad's SHAPE is wrong, not just
     * its extent - and no screen-space clip can recover it. */
    {
        int pinned = 0;
        for (i = 0; i < 4; i++) {
            if (c[i]->vz < v->near_z) { pinned++; }
        }
        if (pinned) {
            if (st) { st->polys_pinned++; }
            if (v->drop_pinned_quads) { return 0; }
        }
    }

    /* The view-z range is measured over the INPUT corners, before the clip,
     * on purpose:'s cross-check is "the
     * 31 levels with a pinned quad are exactly the 31 with a negative minimum
     * view z", and taking it after the clip would make it trivially
     * non-negative and destroy the check.  The SCREEN bbox below is taken
     * over what is actually emitted, which is what it always claimed to be
     * and is only now true. */
    if (st) {
        for (i = 0; i < 4; i++) {
            if (c[i]->vz < st->min_vz) { st->min_vz = c[i]->vz; }
            if (c[i]->vz > st->max_vz) { st->max_vz = c[i]->vz; }
        }
    }

    /* Layer 0 takes the symmetry from the class byte's low three bits
     * (0x004505ee); layer 1 from pols+0x20's (0x0045064a). */
    quad_uv(s, slot, layer2 ? pol[CDAT_POL_TEX1_SYM]
                     : back ? back_sym[pol[CDAT_POL_CLASS] & 7u]
                     : pol[CDAT_POL_CLASS],
            u, vv);

    /* all three are functions of (radius, detail), which are constant
     * for the frame; worked out when either changes, not once per polygon. */
    {
        static int     ds_ok;
        static int32_t ds_radius, ds_detail;
        static float   ds_scale, ds_count, ds_clip;

        if (!ds_ok || ds_radius != v->radius || ds_detail != v->detail) {
            ds_radius = v->radius;
            ds_detail = v->detail;
            ds_scale  = stage_depth_scale(v->radius, v->detail);

            /* [0x005d705c], the far value FUN_0044C292 counts against: the
             * reciprocal of the depth scale by construction (0x0044c640 is
             * `FLD1 / FDIV`).  [0x005d70c4], the far value the CLIP uses, is
             * that times 0.9f through a FISTP - see the header comment. */
            ds_count  = 1.0f / ds_scale;
            ds_clip   = (float)fistp(ds_count * 0.9f);
            ds_ok     = 1;
        }
        dscale    = ds_scale;
        far_count = ds_count;
        far_clip  = ds_clip;
    }

    /* Build the working polygon in the original's RING order - see
     * STAGE_RING.  The colour is the post-ramp ARGB the original already has
     * in the working vertex by the time the clip runs, not the shade index:
     * FUN_004507cb writes bytes +0x18..+0x1a and the clipper lerps THOSE. */
    /* FUN_0044d9bc, the bracket the ENTIRE pipeline runs inside - it is the
     * stage path's only writer of the global colour [0x005d6f60]:
     *
     *   0044d9bc  PUSH [0x005d6f60]
     *   0044d9c2  EAX = movzx byte [ESI+0x01]          ; pols+0x01, un-animated
     *   0044d9c6  EAX = [EAX*4 + 0x005bd53c]           ; the per-slot tint
     *   0044d9cd  CMP EAX,0 / JE 0x0044da73            ; ZERO: no modulate
     *   0044d9df..0044da6e  [0x005d6f60] = (tint * [0x005d6f60]) >> 8 per byte
     *   0044da73  CALL the pipeline
     *   0044da78  POP  [0x005d6f60]
     *
     * BOTH LAYERS SEE THE SAME VALUE, because the bracket wraps the pipeline
     * and the pipeline draws both.  This port splits the layers into two
     * build_poly calls, so the tint is looked up from pols+0x01 in BOTH -
     * never from the layer's own slot.
     *
     * MEASURED, and it is why the port gained this and not a comment: the
     * table is non-zero in 12 of the 48 shipped levels, but only LEVEL22's
     * slot 232 is ever named by a drawable polygon - 297 of 1,822,369
     * corpus-wide, 0.016%.  What changes EVERY vertex is the rest state,
     * because (c*255)>>8 is c-1 and not c,
     * `python tools/stagetint.py`. */
    /* FUN_00451659 (the per-corner headlight, called from 0x00450BFD)
     * leaves a polygon UNLIT when `[0x005F0974] == 4` and `[0x005F08EC] == 0`
     * (0x00451672..0x00451684, read 2026-11-11).  [0x005F0974] is 2, or the
     * mode of the polygon's SECOND layer when it has one (0x00450BC7..0x00450BF3);
     * [0x005F08EC] is the animated slot of layer 0 (0x004505E8).  So a polygon
     * that has NO primary texture and a mode-4 second layer takes no headlight:
     * the start line's stripe (LEVEL46 poly 220, plate 999) stays at the night
     * shade, faint, where the port lit it to white.  Only pass 1 can draw such a
     * polygon (pass 0 skips tex0 == 0). */
    int unlit_decal = layer2 && mode == 4u
                      && stage_texslot(s, pol[CDAT_POL_TEX0], v->time_base) == 0u;

    global = STAGE_DIFFUSE_REST;
    tint   = s->slot_tint[pol[CDAT_POL_TEX0]];
    /* NOTE which half this `if` is.  A ZERO TINT skips the modulate of the
     * GLOBAL - it does not skip the arm's per-corner modulate, which runs
     * against 0x00ffffff and still costs every channel its low bit. */
    if (tint != 0) { global = stage_modulate8(tint, global); }

    for (i = 0; i < 4; i++) {
        int k = STAGE_RING[i];
        unsigned shade = pol[(back ? POL_BACK_SHADE : CDAT_POL_SHADE) + k];

        ring[i].vx = c[k]->vx;
        ring[i].vy = c[k]->vy;
        ring[i].vz = c[k]->vz;
        ring[i].sx = c[k]->sx;
        ring[i].sy = c[k]->sy;
        ring[i].tu = u[k];
        ring[i].tv = vv[k];
        /* 0x00449cc8-0x00449d68, the mode-2 arm, and modes 3 4 5 8 9 10 write
         * the same expression from the same two inputs:
         *   EAX = [EBP+0x18]                   ; the post-ramp corner colour
         *   ...modulate against [0x005d6f60]...
         *   OR EAX,0xff000000 / MOV [EBP+0x24],EAX
         * Mode 0 and mode 6 do NOT: they write the global flat, with no
         * per-corner term at all.  Neither is ever the stage's - the mode is
         * 2 unless the polygon has a SECOND texture layer. */
        {
            uint32_t sc = shade_colour(v, shade);
            /* 0x0045080D: the front's baked lights, before the arm's modulate */
            if (!back && (cdat_u16(pol, CDAT_POL_FLAGS) & 4u)) {
                sc = light_add(sc, v->light1, pol[POL_LIGHT1 + k]);
                sc = light_add(sc, v->light2, pol[POL_LIGHT2 + k]);
            }
            /* FUN_00451659 / FUN_0045172D: the headlights, per corner.  THE
             * PORT DOES NOT SUBDIVIDE: the original splits a lit triangle
             * 1-to-4 and lights the three midpoints too; here the light is
             * Gouraud-interpolated across the polygon from its corners. */
            if ((v->nlit > 0 || v->nflash > 0) && !unlit_decal) {
                int32_t wp[3];
                uint32_t L;
                cdat_pnt(&s->cdat, (int32_t)cdat_u32(pol,
                         (unsigned)(CDAT_POL_CORNER0 + corner[k] * 4)), wp);
                L = stage_point_light(v, wp);
                if (L) {
                    uint8_t c3[3];
                    c3[0] = (uint8_t)(L >> 16); c3[1] = (uint8_t)(L >> 8);
                    c3[2] = (uint8_t)L;
                    sc = light_add(sc, c3, 255u);
                }
            }
            ring[i].argb = v->disable_diffuse_modulate
                         ? sc
                         : (stage_modulate8(sc, global) | 0xff000000u);
        }
        /* ~~PORT'S CHOICE~~ - NO LONGER A CHOICE AT ALL, and the retraction
         * is the.  This line used to be documented as the port's own
         * substitution for a second texture stage it does not have.  It is
         * not a substitution: it is what the original computes.
         *
         *   0044a657  MOV EAX,[EBP+0x24]     ; the layer-0 diffuse
         *   0044a65a  ROL EAX,8
         *   0044a65d  MOV AL,[EBP+0x21]      ; the layer-1 blend weight
         *   0044a660  ROR EAX,8
         *   0044a663  MOV [EBP+0x2c],EAX     ; the layer-1 DIFFUSE
         *
         * "keep the RGB, take the alpha from the weight", which is this line.
         * Modes 3 and 4 both do it; mode 5 does something else entirely and
         * is handled at the emit loop, from this same alpha.
         *
         * pols+0x22..+0x25 is in CDAT CORNER ORDER, not ring order - working
         * corner k takes [0x005f0924 + k*4] and FUN_00450A9C fills that array
         * `MOVZX EAX,byte [ESI+0x22+k] / MOV [0x5f0924+k*4],EAX` at
         * 0x00450a72 - so the index is `k`, the STAGE_RING entry, and not
         * `i`.  Getting that round the other way rotates every second layer's
         * blend by one corner and nothing counts it.
         *
         * The clip lerps the byte either way: the original interpolates
         * working +0x21 at FUN_0044BE0B, `XOR EAX,EAX / MOV AL,[ESI+0x21] /
         * FILD / lerp / FISTP`, which is corner_lerp_argb's alpha lane. */
        if (layer2) {
            ring[i].argb = (ring[i].argb & 0x00ffffffu)
                         | ((uint32_t)pol[(back ? POL_BACK_BLEND : CDAT_POL_BLEND)
                                          + k] << 24);
        }
        /* THE FOG BYTE, 0x0044ADEE -> 0x0044C321.  It is applied HERE, in the
         * ring build, and not after the clip, because the applier's call site
         * is FOURTEEN BYTES BEFORE the clipper's inside FUN_0044AD8A - so the
         * clip interpolates an already-fogged byte.  A port that fogged after
         * clipping would differ, slightly, on every clipped polygon.
         */
        ring[i].fog = (uint8_t)stage_fog_byte(v->fog, c[k]->vz);

        /* FUN_0044afe9 zeroes both screen coordinates before its near test,
         * so `vz < near_z` is exactly "this corner carries the placeholder". */
        ring[i].pinned = (c[k]->vz < v->near_z) ? 1 : 0;
    }

    /* ---- FUN_0044AD8A's three gates ------------------------------------ */
    if (!v->disable_view_clip) {
        int nout = corners_out_of_range(ring, n, v->near_z, far_count);

        /* 0x0044ade1  CMP EAX,[0x005d6f8c] / JE -> RETURN */
        if (nout == n) {
            if (st) { st->polys_out_of_range++; }
            return 0;
        }
        /* 0x0044adfa  TEST EAX,EAX / JZ -> past the clip */
        if (nout != 0) {
            n = clip_poly(ring, n, v, v->near_z, far_clip, st);
            if (st) { st->polys_clipped++; }
            /* 0x0044ae01  CMP [0x005d6f8c],3 / JB -> RETURN */
            if (n < 3) {
                if (st) { st->polys_clip_empty++; }
                return 0;
            }
        }
    }

    /* ---- the fan ------------------------------------------------------- *
     * FUN_0044AA44 walks [0x0091fb10] in order and fills [0x009089a0] with
     * [0x005d6f8c] D3DTLVERTEXes; Direct3D then draws them as one primitive.
     * plat.h has triangles and nothing else, so the port fans the ring from
     * its first corner - which for an unclipped quad is bit-for-bit the pair
     * of triangles this file emitted before (see STAGE_RING). */
    for (i = 1; i + 1 < n; i++) {
        int idx[3];
        int j;

        idx[0] = 0;
        if (v->reverse_winding) { idx[1] = i + 1; idx[2] = i;     }
        else                    { idx[1] = i;     idx[2] = i + 1; }

        if (nv + 3 > outmax) { break; }

        for (j = 0; j < 3; j++) {
            const stage_corner *cc = &ring[idx[j]];
            plat_vertex *o = &out[nv++];

            /* FUN_0044aa44 at 0x0044aa8a: `FLD [EBP+0x18]` (view z) / `FMUL
             * [0x005d7060]` / `FSTP [EDI+0x8]`.  A LINEAR z, not a w curve. */
            float sz = cc->vz * dscale;

            o->sx = cc->sx;
            o->sy = cc->sy;

            /* 0x0044aa96, to the END of the basic block this time: `FLD1` /
             * `FDIV [EDI+0x8]` / `FMUL [0x00684560]`.  The divisor is sz, the
             * value just STORED - not view z, and not the clamped sz below,
             * which is the port's.  [0x00684560] is 0.01f and has ONE
             * reference image-wide, the FMUL itself, so it is a hard
             * constant.
             *
             * The SSE variant at 0x0044aaf8 computes 65536.0f / view_z
             * instead, which equals this one only when R*32 == 100 - detail.
             * The two genuinely differ; it is harmless because rhw
             * only perspective-corrects here and a uniform scale cancels in
             * the ratio.
             *
             * PORT'S CHOICE at sz <= 0 ONLY.  The original divides unguarded
             * and hands Direct3D +/-inf for a corner at or behind the eye.
             * With the view clip on, no such corner survives to here - which
             * is the point of - so this guard now only fires under
             * disable_view_clip. */
            o->rhw = (sz > 0.0f) ? (0.01f / sz) : 0.0f;

            /* PORT'S CHOICE.  GL's depth range is [0,1] and so was the D3D
             * viewport's, but the clamp was in the hardware there and there
             * is no clamp in FUN_0044aa44.  It must not feed rhw above. */
            if (sz < 0.0f) { sz = 0.0f; }
            if (sz > 1.0f) { sz = 1.0f; }
            o->sz = sz;

            o->diffuse  = cc->argb;
            /* 0x00449D6B-0x00449D71: `MOV AL,[EBP+0x20]` / `SHL EAX,0x18` /
             * `MOV [EBP+0x28],EAX`, and FUN_0044AB15 copies +0x28 to the
             * D3DTLVERTEX's +0x14.  RGB zero, alpha the fog byte - which is
             * why plat_gl.c's shader reads `v_specular.a` and adds
             * `v_specular.rgb` harmlessly. */
            o->specular = (uint32_t)cc->fog << 24;

            /* ---- the SECOND layer's two dwords, +0x2c and +0x30 --------- *
             *
             * Computed HERE and not in the ring build, because the original
             * computes them after the clip has run: FUN_0044AD8A clips, then
             * FUN_00449AC7 walks the surviving corner ring and writes +0x2c
             * and +0x30 from the ALREADY-INTERPOLATED +0x18, +0x20 and +0x21.
             * Mode 5 multiplies two of those bytes together, and the product
             * of two lerps is not the lerp of the product, so doing it in the
             * ring build would differ on every clipped polygon.
             *
             * THE THREE ARMS ARE NOT VARIATIONS ON ONE - all three dwords
             * change together, which is why layer2_mode_override forces the
             * whole arm rather than just the blend:
             *
             *   mode 3  0044a657  +0x2c = layer-0 RGB, alpha <- +0x21
             *           0044a666  +0x30 = [+0x20] << 24        FOGGED
             *   mode 4  0044a86c  +0x2c = the same
             *           0044a87b  +0x30 = 0xff000000           NOT fogged
             *   mode 5  00449edb  +0x2c = grey g in all three channels,
             *                     alpha ZERO.  `MOV AL,[EBP+0x21] /
             *                     MOV BL,[EBP+0x20] / MUL EBX / MOV AL,AH /
             *                     SHL EAX,8 / MOV AL,AH / AND EAX,0xffffff`
             *                     is g = (weight * fog) >> 8 broadcast to
             *                     all three channels with the top byte cleared
             *           00449ef6  +0x30 = 0xff000000           NOT fogged
             *
             * Modes 4 and 5 leave FOGENABLE alone and 1 respectively, but
             * both hand the shader a specular alpha of 255 - and the mix is
             * `mix(fogcolor, c, specular.a)`, so 255 is "no fog" whatever
             * FOGENABLE says.  MODE 4'S SECOND LAYER IS UNFOGGED THROUGH THE
             * VERTEX, not through the render state, which is why reading only
             * the state block makes it look fogged. */
            if (layer2) {
                stage_layer2_colour(mode, cc->argb,
                                    (cc->argb >> 24) & 0xffu, cc->fog,
                                    &o->diffuse, &o->specular);
            }
            o->tu = cc->tu;
            o->tv = cc->tv;

            if (st) {
                /* Bucketed on the corner's own view z against the
                 * SAME far_count the range test uses, so the bucket edges are
                 * the draw distance's and not this probe's. */
                int qi = (far_count > 0.0f)
                       ? (int)((cc->vz / far_count) * 4.0f) : 0;
                if (qi < 0) { qi = 0; }
                if (qi > 3) { qi = 3; }
                st->fog_q_n[qi]++;
                st->fog_q_sum[qi] += cc->fog;

                if (cc->pinned) { st->verts_pinned_emitted++; }
                if (o->sx < st->min_sx) { st->min_sx = o->sx; }
                if (o->sx > st->max_sx) { st->max_sx = o->sx; }
                if (o->sy < st->min_sy) { st->min_sy = o->sy; }
                if (o->sy > st->max_sy) { st->max_sy = o->sy; }
            }
        }
    }
    return nv;
}

/* ------------------------------------------------------------ the frame */

#define STAGE_BATCH_QUADS 256

typedef struct {
    plat_vertex  v[STAGE_BATCH_QUADS * 6];
    unsigned     n;
    plat_tex     bound;
    int          have_bound;
    stage_stats *st;
} stage_batch;

static void batch_flush(stage_batch *b)
{
    if (b->n) {
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, b->v, b->n);
        b->n = 0;
        if (b->st) { b->st->batches++; }
    }
}

static void batch_emit(stage_batch *b, plat_tex t, const plat_vertex *q, int n)
{
    int i;

    if (!b->have_bound || b->bound != t) {
        batch_flush(b);
        plat_gfx_tex_bind(t);
        b->bound = t;
        b->have_bound = 1;
    }
    /* A clipped polygon fans to more than six vertices, so the flush test has
     * to take the run's own length rather than the old fixed 6. */
    if (b->n + (unsigned)n > sizeof b->v / sizeof b->v[0]) {
        batch_flush(b);
    }
    for (i = 0; i < n; i++) {
        b->v[b->n++] = q[i];
    }
}

/* The pass-2 render state, per arm of FUN_00449AC7.  Read through BOTH the
 * direct helper and the thunk 0x0044cbb6 - `MOV EAX,<value> / MOV EBX,<D3D
 * state id> / CALL 0x0044cbb6` - and transcribed to the end of each block:
 *
 *   mode 3  0044a736  TMB=4  FOG=1  SRC=5  DST=6  ABE=1
 *           0044a72c  ZWRITE=0 before the draw, 0044a786 ZWRITE=1 after
 *   mode 4  0044a94a  TMB=4  FOG=1  SRC=5  DST=2  ABE=1
 *           0044a940  ZWRITE=0 before the draw, 0044a99a ZWRITE=1 after
 *   mode 5  00449fc0  TMB=2         SRC=1  DST=4  FOG=0  ABE=1
 *           NO ZWRITE CHANGE AT ALL
 *
 * THE ZWRITE PAIR IS NOT IN THE ARM, which is why reading the arm alone
 * misses it: `CALL 0x0044c46c` and `CALL 0x0044c47c` are four-instruction
 * helpers that set D3D state 14 to 0 and 1, with 14 and 15 call sites
 * image-wide.  Mode 5's arm calls neither, so its second layer WRITES DEPTH -
 * and its blend is `dst *= (1 - src)`, so it is a dark map that occludes.
 * Three arms, three different answers to "does the second layer write z".
 *
 * D3DBLEND: 1 ZERO, 2 ONE, 4 INVSRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA -
 * and plat.h's plat_blend is the same
 * numbering, so the values below are the image's and not a translation.
 *
 * MODE 0 IS NOT IMPLEMENTED, on purpose.  stage_layer_mode's fourth entry and
 * beyond is 0, no shipped level reaches it, and a guess at a flat arm nobody
 * has read would be exactly the kind of invention the port does not make.  It
 * falls through to mode 3's block and st->layer2_mode_n[0] counts it. */
/* `fog_on` is the frame's own FOGENABLE, not a second opinion about it: the
 * arms write 1 because the game always has a ramp loaded, and this port draws
 * with v->fog == NULL in every gate that has not built one.  Writing a literal
 * 1 here would turn fog ON in frames stage_draw had just turned it off for. */
void stage_layer2_block(unsigned mode, int blended, int fog_on,
                        uint32_t out[STAGE_L2_STATES])
{
    (void)blended;      /* kept in the signature: mode 5's ZWRITE is the
                         * list's, and the caller needs to be told which. */
    switch (mode) {
    case 5u:                                       /* 0x00449fc0 */
        out[STAGE_L2_TEXBLEND]  = 2u;
        out[STAGE_L2_SRCBLEND]  = PLAT_BLEND_ZERO;
        out[STAGE_L2_DSTBLEND]  = PLAT_BLEND_INVSRCCOLOR;
        out[STAGE_L2_FOGENABLE] = 0u;
        /* The arm calls NEITHER 0x0044c46c nor 0x0044c47c, so it leaves
         * ZWRITE where pass 1 put it - which for this port is the list's own
         * value.  Restored rather than assumed, and reported as unwritten. */
        out[STAGE_L2_ZWRITE]    = STAGE_L2_UNWRITTEN;
        break;
    case 4u:                                       /* 0x0044a94a */
        out[STAGE_L2_TEXBLEND]  = 4u;
        out[STAGE_L2_SRCBLEND]  = PLAT_BLEND_SRCALPHA;
        out[STAGE_L2_DSTBLEND]  = PLAT_BLEND_ONE;
        out[STAGE_L2_FOGENABLE] = fog_on ? 1u : 0u;
        out[STAGE_L2_ZWRITE]    = 0u;              /* 0x0044a940 */
        break;
    default:                                       /* 0x0044a736 */
        out[STAGE_L2_TEXBLEND]  = 4u;
        out[STAGE_L2_SRCBLEND]  = PLAT_BLEND_SRCALPHA;
        out[STAGE_L2_DSTBLEND]  = PLAT_BLEND_INVSRCALPHA;
        out[STAGE_L2_FOGENABLE] = fog_on ? 1u : 0u;
        out[STAGE_L2_ZWRITE]    = 0u;              /* 0x0044a72c */
        break;
    }
}

void stage_layer2_colour(unsigned mode, uint32_t argb0, unsigned weight,
                         unsigned fog, uint32_t *diffuse, uint32_t *specular)
{
    if (mode == 5u) {
        /* 0x00449edb, and the shift chain is transcribed rather than
         * simplified: MUL leaves the product in AX, MOV AL,AH takes its high
         * byte, and the two SHL/MOV pairs broadcast it into three channels
         * with the top byte cleared by the AND. */
        unsigned g = (weight * fog) >> 8;

        *diffuse = (g << 16) | (g << 8) | g;       /* alpha ZERO */
    } else {
        /* 0x0044a657 and 0x0044a86c: keep the RGB, take the alpha from the
         * weight.  argb0 already carries it, because the ring build put it
         * there so the clip would interpolate it - see build_poly. */
        *diffuse = (argb0 & 0x00ffffffu) | ((uint32_t)(weight & 0xffu) << 24);
    }
    /* 0x0044a666 for mode 3, 0x0044a87b and 0x00449ef6 for modes 4 and 5.
     * An opaque specular alpha is NO FOG, because the shader's mix is
     * mix(fogcolor, c, specular.a) - so modes 4 and 5 are unfogged through
     * the VERTEX whatever FOGENABLE says. */
    *specular = (mode == 3u) ? ((uint32_t)(fog & 0xffu) << 24) : 0xff000000u;
}

static void layer2_state(unsigned mode, int blended, int fog_on)
{
    uint32_t b[STAGE_L2_STATES];

    stage_layer2_block(mode, blended, fog_on, b);
    plat_gfx_set_state(PLAT_RS_TEXBLEND,  b[STAGE_L2_TEXBLEND]);
    plat_gfx_set_state(PLAT_RS_SRCBLEND,  b[STAGE_L2_SRCBLEND]);
    plat_gfx_set_state(PLAT_RS_DSTBLEND,  b[STAGE_L2_DSTBLEND]);
    plat_gfx_set_state(PLAT_RS_FOGENABLE, b[STAGE_L2_FOGENABLE]);
    plat_gfx_set_state(PLAT_RS_ZWRITE,
                       b[STAGE_L2_ZWRITE] == STAGE_L2_UNWRITTEN
                           ? (blended ? 0u : 1u) : b[STAGE_L2_ZWRITE]);
}

/* THE SECOND LAYER IS A SEPARATE PASS OVER THE SAME LIST, and the reason is
 * measured rather than tidy.
 *
 * `+0x21 == 0` disables the second layer, and 334,144 of
 * 1,570,376 drawable polygons - 21.3% - carry one, so a one-layer port is
 * wrong on a fifth of the stage.
 *
 * ~~The original interpolates the four per-corner weights INSIDE ONE DRAW;
 * plat_gfx_tex_bind is stage 0 only, so the port cannot.~~  RETRACTED,
 * and it was wrong in the port's favour.  THE ORIGINAL IS NOT
 * MULTITEXTURED EITHER.  FUN_00449AC7's two-pass arms swap the texture
 * handle and re-draw:
 *
 *   0044a67d  [0x005d6f50] -> [0x005d6f58]    ; the LAYER-0 texture
 *   0044a691  ESI=8, EDX=0x24 -> FUN_0044AB15 ; pack uv pair 0, colour +0x24
 *   0044a6aa  CALL 0x0044ad28                 ; TEXTUREADDRESS from [0x5f1024]
 *             ...the pass-1 state block...
 *   0044a6fa  CALL 0x0044ad5b                 ; BIND [0x005d6f58] and DRAW
 *   0044a709  [0x005d6f54] -> [0x005d6f58]    ; the LAYER-1 texture
 *   0044a71d  ESI=0x10, EDX=0x2c              ; uv pair 1, colour +0x2c
 *             ...the pass-2 state block...
 *   0044a781  CALL 0x0044ad5b                 ; BIND and DRAW again
 *
 * One texture stage, two binds, two DrawPrimitives - which is exactly what
 * this function does.  So the port's shape is the original's and the only
 * thing left over is the ORDER.
 *
 * Emitting the second draw immediately after its own first layer - which is
 * what this function did first, and which is what the original does - rebinds
 * a different texture between every pair of layer-1 quads and DESTROYS the
 * batching that the whole `(texture << 16) | index` sort key exists to create:
 * measured, 564 quads came out as 307 draw calls, 1.8 quads a call.  Deferring
 * it to a second sweep keeps layer 1 grouped by plate.  The DATA and the
 * NUMBER of passes are the original's; their interleaving is the port's.
 *
 * THE PASS-2 STATE BLOCK IS PER POLYGON.  It is chosen by the second slot's
 * TEXTINFO +0x24 through stage_layer_mode, and the three arms are genuinely
 * different pictures - mode 4 is ADDITIVE and mode 5 MULTIPLIES THE FRAME
 * DOWN.  The sweep therefore sets state inside the loop and flushes the batch
 * first; st->layer2_state_flushes counts what that costs.  It is kept in list
 * order rather than grouped by mode on purpose: grouping would batch better
 * and would reorder three different blend equations against each other, which
 * is a change to the arithmetic and not to the scheduling. */
static void draw_list(stage *s, const stage_view *v, const stage_item *list,
                      int n, int blended, int record_batches, stage_stats *st)
{
    stage_batch b;
    plat_vertex q[STAGE_POLY_VERTS];
    int i, pass;
    unsigned long before = st ? st->batches : 0;
    unsigned cur_mode;

    b.n = 0;
    b.bound = 0;
    b.have_bound = 0;
    b.st = st;

    for (pass = 0; pass < 2; pass++) {
        cur_mode = 0u;
        if (pass == 0) {
            /* The pass-1 block, identical in all three two-pass arms and in
             * the one-pass mode-2 arm: 0x0044a6af-0x0044a6f5, TMB=4, FOG=1,
             * SRC=5, DST=6, ABE=1.  FOGENABLE is stage_draw's, above; BLEND
             * and ZWRITE are the list's, which is the reading and not
             * this arm's - neither arm touches ZWRITE before its first draw.
             *
             * TEXTUREMAPBLEND was NOT being written by this port at all, and
             * the GL uniform defaults to 0, which is the MODULATE branch of
             * the shader - so `c.a = t.a` and the vertex alpha was discarded.
             * THAT WAS FATAL ON LAYER 1, whose alpha IS the blend weight, and
             * layer2_state below is what fixes it.
             *
             * THIS WRITE IS FAITHFULNESS, NOT THE FIX, and the perturbation
             * sweep says so: deleting this one line changes no pixel on any
             * level, because layer 0's diffuse alpha is 0xff by construction
             * and `t.a * 1.0 == t.a`.  It is here because the original writes
             * it in every pass-1 block, and because a mode-5 polygon leaves
             * the state at 2;. */
            plat_gfx_set_state(PLAT_RS_TEXBLEND, 4u);
            plat_gfx_set_state(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA);
            plat_gfx_set_state(PLAT_RS_DSTBLEND, PLAT_BLEND_INVSRCALPHA);
            /* Re-asserted per pass, not once per frame: a mode-5 polygon in
             * the PREVIOUS list's second pass left FOGENABLE at 0, and the
             * original re-writes FOG=1 in every pass-1 block for exactly the
             * same reason. */
            plat_gfx_set_state(PLAT_RS_FOGENABLE, v->fog ? 1u : 0u);
            /* BLEND 0 on both lists: near to far, a part-alpha edge
             * texel blended over the sky and wrote depth, holding off the far
             * trees - a pale rim.  Cut-out only, as the original. */
            plat_gfx_set_state(PLAT_RS_BLEND,  0u);
            /* ZWRITE 1 on BOTH lists.  The sorted list runs near to far, so
             * with ZWRITE 0 a far tree drawn after a near one passed LEQUAL
             * and blended over it - near trees went see-through.
             * The original's only transparency here is the alpha test (the
             * ZWRITE-0 pairs 0x0044a72c / 0x0044a940 are second-layer modes
             * 3 and 4, not this list). */
            plat_gfx_set_state(PLAT_RS_ZWRITE, 1u);
            /* REMASTER, with the ZWRITE above: on the sorted list a
             * filtered edge texel of low alpha blended over the sky drawn so
             * far AND wrote depth, so the far trees drawn after it were held
             * off - a pale rim round near trees.  Cutting at half alpha
             * discards those texels instead.  The original's ref is 0. */
            plat_gfx_set_state(PLAT_RS_ALPHAREF, blended ? 0x80u : 0u);
        } else {
            /* Pass 2's blend weight is its alpha: back to the original's 0. */
            plat_gfx_set_state(PLAT_RS_ALPHAREF, 0u);
            plat_gfx_set_state(PLAT_RS_BLEND, 1u);
        }

        for (i = 0; i < n; i++) {
            const uint8_t *pol = cdat_pol(&s->cdat, list[i].poly);
            unsigned tex0 = pol[CDAT_POL_TEX0];
            unsigned tex1 = pol[CDAT_POL_TEX1];
            unsigned slot, mode = 0u;
            int nv;

            if (pass == 0) {
                /* 0x00452bcc's zero-primary fallback picks the LIST only;
                 * the material reads +0x01 with none (0x004505a0), so pass 1
                 * binds the unloaded plate-0 sentinel.  REMASTER:
                 * draw nothing here - the secondary is drawn by pass 2 alone,
                 * which is the light shafts' additive beam.  Taking the
                 * secondary here pasted plate 736 on opaque. */
                if (tex0 == 0 && tex1 != 0) { continue; }
                slot = stage_texslot(s, tex0, v->time_base);
                nv = build_poly(s, v, pol, 0, 2u, slot & 0xffu, 0,
                                q, STAGE_POLY_VERTS, st);
                if (nv == 0) {
                    continue;
                }
                if (st) { st->quads_drawn++; }
            } else {
                if (tex1 == 0 || v->disable_second_layer) {
                    continue;
                }
                slot = stage_texslot(s, tex1, v->time_base);
                mode = layer2_mode(s, v, slot);
                nv = build_poly(s, v, pol, 1, mode, slot & 0xffu, 0,
                                q, STAGE_POLY_VERTS, st);
                if (nv == 0) {
                    continue;
                }
                if (st) {
                    st->layer2_drawn++;
                    st->layer2_mode_n[mode < 6u ? mode : 0u]++;
                    if ((pol[CDAT_POL_BLEND]     | pol[CDAT_POL_BLEND + 1]
                       | pol[CDAT_POL_BLEND + 2] | pol[CDAT_POL_BLEND + 3])
                        == 0u) {
                        st->layer2_zero_weight++;
                    } else if (mode == 4u || mode == 5u) {
                        st->layer2_live_45++;
                    }
                }
                if (mode != cur_mode) {
                    /* A state change applies to everything already queued, so
                     * the queue has to go out first.  batch_flush is a no-op
                     * on an empty batch, which is what makes the first
                     * polygon of the pass free. */
                    if (b.n) {
                        batch_flush(&b);
                        if (st) { st->layer2_state_flushes++; }
                    }
                    layer2_state(mode, blended, v->fog != NULL);
                    cur_mode = mode;
                }
            }
            batch_emit(&b, s->tex[slot & 0xffu], q, nv);
            /* a two-sided polygon's back, straight after its front
             * (FUN_004502D7: 0x004507cb, then 0x00450a9c), same slot and
             * mode - see build_poly. */
            if ((pol[CDAT_POL_CLASS] & 0x08u) && !v->disable_back_pass) {
                nv = build_poly(s, v, pol, pass, pass ? mode : 2u,
                                slot & 0xffu, 1, q, STAGE_POLY_VERTS, st);
                if (nv) {
                    batch_emit(&b, s->tex[slot & 0xffu], q, nv);
                    if (st && pass == 0) { st->backs_drawn++; }
                }
            }
        }
        batch_flush(&b);
        /* Record the FIRST pass's draw-call count separately.  That is the one
         * the `(texture << 16) | index` key is supposed to make cheap; the
         * second-layer pass walks the same list in the same order with a
         * different texture per polygon and cannot batch, and averaging the two
         * would let a broken key hide behind it. */
        if (pass == 0 && record_batches && st) {
            st->batches_tex = st->batches - before;
        }
    }
}

void stage_draw(stage *s, const stage_view *v, stage_stats *st)
{
    int32_t i, last;
    uint64_t t0 = plat_time_us(), t1, t2;           

    if (st) {
        memset(st, 0, sizeof *st);
        st->min_sx = st->min_sy = st->min_vz =  3.0e38f;
        st->max_sx = st->max_sy = st->max_vz = -3.0e38f;
    }
    s->n_tex = s->n_sorted = 0;
    s->overflow_tex = s->overflow_sorted = 0;

    /* FOGCOLOR is set once at level load (0x0044DBDC -> 0x0041A187, the
     * image's ONLY write site for D3D state 34) and FOGENABLE per pass.  The
     * stage's pass is FUN_00449AC7's mode 2 - [0x005F0974] is set to 2 at
     * 0x00450BC7 in the stage emitter, unless the texinfo slot's byte +0x2c
     * selects another through the table at [0x005F0890] - and mode 2's state
     * block turns fog ON:
     *
     *   00449db9  EAX=1 EBX=0x1c -> the thunk 0x0044CBB6   ; FOGENABLE = 1
     *
     * while mode 0's, 328 bytes earlier in another arm of the same dispatch,
     * turns
     * it OFF (0x00449C7B, EAX=0 EBX=0x1c - which already
     * had as a site and could not attribute to a pass).  So the ambient state
     * is fogged and the unlit path is the exception. Done-when 6. */
    if (v->fog) {
        plat_gfx_set_state(PLAT_RS_FOGCOLOR,  v->fog->color);
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 1u);
    } else {
        plat_gfx_set_state(PLAT_RS_FOGENABLE, 0u);
    }

    /* The cache is tagged by the FULL index and the tags survive a frame in
     * the original - but the TRANSFORM does not, because the matrix moves.
     * The game invalidates with 0xffffffff; where it does so is in the frame
     * setup this port does not have yet, so it is done here, once. */
    for (i = 0; i < (int32_t)STAGE_VCACHE_SLOTS; i++) {
        s->vcache[i].tag = 0xffffffffu;
    }

    grid_rebuild(s, v);

    /* FUN_00452a74: 0x400 slots, `CMP EBX,0xfffffff` / `JNC` to skip the
     * fill value.  Note the comparison is UNSIGNED and against the fill
     * itself, so every index below 0x0fffffff is taken. */
    last = s->cdat.nzube - 1;
    for (i = 0; i < STAGE_GRID_DWORDS; i++) {
        int32_t zi = s->grid[i];
        if (zi == last) {
            last = -1;                 /* `MOV EBP,0xffffffff` at 0x00452a8c */
        }
        if ((uint32_t)zi >= STAGE_GRID_EMPTY) {
            continue;
        }
        walk_cell(s, v, zi, st);
    }
    /* 0x00452ab3.  The LAST zube record is walked whether or not it was
     * visible - which in every shipped payload is the 0xffffffff terminator,
     * whose ranges are -1, so FUN_00452b97 returns immediately.  Transcribed
     * because it costs nothing and because a payload whose last record is NOT
     * the terminator would behave differently. */
    if (last != -1 && last >= 0) {
        walk_cell(s, v, last, st);
    }
    t1 = plat_time_us();

    /* Both lists are sorted BEFORE anything is drawn - 0x00452e60 and
     * 0x00452dd2, each guarded by `count >= 2`. */
    if (s->n_tex > 1) {
        item_sort(s->list_tex, (size_t)s->n_tex);
    }
    if (s->n_sorted > 1) {
        item_sort(s->list_sorted, (size_t)s->n_sorted);
    }
    t2 = plat_time_us();

    /* 0x00452da9 walks list A, 0x00452d82 walks list B.  Both ascending: the
     * texture list is grouped by plate, and the depth list runs NEAR TO FAR.
     * Near-to-far with an alpha test and z-write is the cutout ordering, not
     * the blended one - for why that reading is
     * offered rather than asserted.  draw_list now writes depth on this list
     * too: without it, near trees showed the far ones through. */
    draw_list(s, v, s->list_tex,    s->n_tex,    0, 1, st);
    draw_list(s, v, s->list_sorted, s->n_sorted, 1, 0, st);
    if (st) {
        st->us_walk = t1 - t0;
        st->us_sort = t2 - t1;
        st->us_draw = plat_time_us() - t2;
    }
}

/* ------------------------------------------------------------- the view */

void stage_view_default(stage_view *v, int screen_w, int screen_h,
                        int32_t eye_x, int32_t eye_y, int32_t eye_z,
                        float yaw)
{
    float c = cosf(yaw), sn = sinf(yaw);

    memset(v, 0, sizeof *v);

    /* A yaw-only view matrix in the original's convention: rotate about y,
     * then translate by minus the eye.  v' = v * M with the basis in the
     * columns, so the rows below are the world axes expressed in view space.
     *
     * PORT'S CHOICE - the game builds [0x0059f014] from a camera this port
     * does not have.  The SHAPE is FUN_0044b1ab's and is transcribed; the
     * contents are a helper for bring-up and for the gate. */
    v->m[ 0] =  c;   v->m[ 1] = 0.0f; v->m[ 2] =  sn;  v->m[ 3] = 0.0f;
    v->m[ 4] = 0.0f; v->m[ 5] = 1.0f; v->m[ 6] = 0.0f; v->m[ 7] = 0.0f;
    v->m[ 8] = -sn;  v->m[ 9] = 0.0f; v->m[10] =  c;   v->m[11] = 0.0f;
    v->m[12] = -( (float)eye_x * c   + (float)eye_z * -sn);
    v->m[13] = -( (float)eye_y );
    v->m[14] = -( (float)eye_x * sn  + (float)eye_z *  c);
    v->m[15] = 1.0f;

    v->eye_x = eye_x;
    v->eye_z = eye_z;

    /* The wedge FUN_00452c90 culls to is exactly 90 degrees horizontally, so
     * a focal length of half the width puts the cull planes on the screen
     * edges.  That is a consequence of the transcription, not a free choice:
     * any other focal length leaves geometry culled inside the viewport or
     * drawn outside it. */
    v->centre_x = (float)screen_w * 0.5f;
    v->centre_y = (float)screen_h * 0.5f;
    v->focal    = (float)screen_w * 0.5f;

    v->near_z    = 4096.0f;           /* [0x0057b09c] */
    v->radius    = STAGE_RADIUS_DEFAULT;
    v->flag_mask = 0u;
    v->time_base = 0;
    v->reverse_winding = 0;

    /* [0x005bd93c] comes from LOCALDAT through FUN_00437aa7.  THE SELECTOR'S
     * INDEX IS NOW READ - it is LOCALDAT +0x1fd0, `MOV EBX,[0x005BD940]` at
     * 0x00437AA9 is the only reference to that address image-wide, and all 48
     * shipped levels carry 0 - but this port still has no reader for the
     * resulting D, and 0 is the full-range end of the clamp.  See
     * stage_fog_build, which DOES run the selector.  Owner. */
    v->detail = 0;

    /* No ramp unless the caller builds one; stage_draw then leaves
     * FOGENABLE off and every vertex carries specular alpha 255. */
    v->fog = NULL;

    /* The identity ramp.  See stage.h - the level colour pair the game builds
     * the real ramp from is not loaded, and black -> white reproduces the
     * grey the port emitted before the ramp law was transcribed. */
    v->shade_lo[0] = v->shade_lo[1] = v->shade_lo[2] = 0x00u;
    v->shade_hi[0] = v->shade_hi[1] = v->shade_hi[2] = 0xffu;
    memset(v->light1, 0, sizeof v->light1);
    memset(v->light2, 0, sizeof v->light2);
    memset(v->light3, 0, sizeof v->light3);
    v->nlit = 0;
    v->nflash = 0;
}

/* 0x0044D157: in a race FUN_0044D1AD reads [0x005BBF70], the LOCALDAT image's
 * +0x600 - lo at +0x00..+0x08, hi at +0x0C..+0x14, one dword per channel.
 * The corpus keeps every channel in 0..255; IDIV's lerp then never leaves it,
 * so a byte holds the end exactly.  This is what makes a night stage dark:
 * LEVEL46's hi is (25,25,37), LEVEL73's (148,89,42), LEVEL21's (255,255,176). */
void stage_view_shade_ld(stage_view *v, const ld_file *ld, int have_ld)
{
    int k;

    if (!have_ld) {
        return;
    }
    for (k = 0; k < 3; k++) {
        v->shade_lo[k] = (uint8_t)(ld_dw(ld, LD_SHADE_LO_OFF + 4u * (unsigned)k) & 0xffu);
        v->shade_hi[k] = (uint8_t)(ld_dw(ld, LD_SHADE_HI_OFF + 4u * (unsigned)k) & 0xffu);
        /* 0x0044D168 / 0x0044D179: [0x005BD208] and [0x005BD214] */
        v->light1[k] = (uint8_t)(ld_dw(ld, LD_LIGHT1_OFF + 4u * (unsigned)k) & 0xffu);
        v->light2[k] = (uint8_t)(ld_dw(ld, LD_LIGHT2_OFF + 4u * (unsigned)k) & 0xffu);
        /* 0x0044D18A: [0x005BD220], the headlights' ramp */
        v->light3[k] = (uint8_t)(ld_dw(ld, LD_LIGHT3_OFF + 4u * (unsigned)k) & 0xffu);
    }
}

static int32_t clamp255(int32_t x)
{
    return x < 0 ? 0 : (x > 255 ? 255 : x);
}

/* the impact flash's term of the per-point light, FUN_0044D82C / FUN_0044D473,
 * saturating-added to `rgb`.  The same accumulator ([0x005E0954]) is the ground's
 * corner light and an object's light BASE, so a placed object and a car both take it. */
uint32_t stage_flash_add(const stage_view *v, const int32_t p[3], uint32_t rgb)
{
    int n;

    /* the impact flash, FUN_0044D82C (0x0044D83E..0x0044D9AD), per slot:
     *   d = FUN_0044CBBC(anchor - point)      ; octagonal distance, (>> 8)
     *   l = clamp(((0x200000 - d) * k) >> 17, 0, 255)   ; 32-bit IMUL, SAR
     *   rgb += grey(l) * 0xFFFFFF >> 8        ; per channel l * 255 >> 8, saturating
     * a slot with k == 0 is skipped.  Saturating adds commute, so the order
     * against the headlights is the original's no matter what. */
    for (n = 0; n < v->nflash; n++) {
        int32_t  k = v->flash[n].k;
        int32_t  a = (v->flash[n].pos[0] - p[0]) >> 8;
        int32_t  b = (v->flash[n].pos[1] - p[1]) >> 8;
        int32_t  c = (v->flash[n].pos[2] - p[2]) >> 8;
        int32_t  t, l;
        unsigned g;

        if (k == 0) { continue; }
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        if (c < 0) c = -c;
        if (a < b) { t = a; a = b; b = t; }           /* a >= b */
        if (b < c) { t = b; b = c; c = t; }           /* b >= c */
        if (a < b) { t = a; a = b; b = t; }
        t = (int32_t)((uint32_t)a * 0xf2u + (uint32_t)b * 0x60u + (uint32_t)c * 0x4bu);
        l = (int32_t)((uint32_t)(0x200000 - t) * (uint32_t)k) >> 17;
        if (l < 0)   l = 0;
        if (l > 255) l = 255;
        g = ((unsigned)l * 0xffu) >> 8;
        for (t = 0; t < 3; t++) {
            unsigned sh = 16u - 8u * (unsigned)t;
            unsigned x  = ((rgb >> sh) & 0xffu) + g;
            if (x > 255u) { x = 255u; }
            rgb = (rgb & ~(0xffu << sh)) | (x << sh);
        }
    }
    return rgb;
}

/* 0x0044D65A for one car at world point p (16.16), summed over the view's
 * lit cars into an RGB the way [0x005E0954] accumulates (PADDUSB).
 *
 *   d = frame * (p - car+0x134)                    ; FUN_0044B1AB, FISTP x y z
 *   a = clamp(0x100 - (((z - 0x200000)^2 + (2x)^2) * 2^-32 >> 2), 0, 255)
 *   h = clamp((0x100000 - |y|) >> 12, 0, 255)       ; SAR
 *   i = clamp(((h * 2a) >> 8) * abc >> 8, 0, 255)   ; MUL: unsigned
 *   rgb += ramp3[i]
 *
 * 0x0044D72E..0x0044D785 computes a second, nearer falloff into EAX and the
 * `MOV EAX,0x100000` at 0x0044D799 overwrites it unread: dead in the
 * original, so not here.  The FISTPs round to nearest, as lrintf does. */
uint32_t stage_point_light(const stage_view *v, const int32_t p[3])
{
    uint32_t rgb = 0;
    int n, k;

    for (n = 0; n < v->nlit; n++) {
        float   dx = (float)(p[0] - v->lit[n].pos[0]);
        float   dy = (float)(p[1] - v->lit[n].pos[1]);
        float   dz = (float)(p[2] - v->lit[n].pos[2]);
        const float *f = v->lit[n].frame;
        int32_t x = (int32_t)lrintf(f[0] * dx + f[3] * dy + f[6] * dz);
        int32_t y = (int32_t)lrintf(f[1] * dx + f[4] * dy + f[7] * dz);
        int32_t z = (int32_t)lrintf(f[2] * dx + f[5] * dy + f[8] * dz);
        float   fz = (float)(z - 0x200000), fx = (float)(x + x);
        int32_t a, h;
        uint32_t i;
        float   q = (fz * fz + fx * fx) * (1.0f / 65536.0f) * (1.0f / 65536.0f);

        /* past 2^31 the FISTP stores the indefinite 0x80000000: >> 2 is
         * 0x20000000, and 0x100 minus it clamps to 0 - as any far point does */
        a = (q >= 2147483648.0f) ? 0
          : clamp255(0x100 - (int32_t)((uint32_t)lrintf(q) >> 2));
        if (y < 0) { y = -y; }
        h = clamp255((0x100000 - y) >> 12);
        i = ((uint32_t)h * (uint32_t)(a + a)) >> 8;
        i = (i * (uint32_t)v->lit[n].abc) >> 8;
        if ((int32_t)i < 0) { i = 0; }
        if (i > 255u)       { i = 255u; }
        for (k = 0; k < 3; k++) {
            unsigned sh = 16u - 8u * (unsigned)k;
            unsigned c  = ((rgb >> sh) & 0xffu) + ((unsigned)v->light3[k] * i) / 255u;
            if (c > 255u) { c = 255u; }
            rgb = (rgb & ~(0xffu << sh)) | (c << sh);
        }
    }
    rgb = stage_flash_add(v, p, rgb);
    return rgb;
}

/* FUN_0044D200's ramp entry i for colour c - black -> c, IDIV truncating -
 * saturating-added to `argb` the way 0x0045082C's PADDUSB does (the scalar
 * arm agrees on these tables: both carry alpha 0xff). */
static uint32_t light_add(uint32_t argb, const uint8_t c[3], unsigned i)
{
    int k;

    for (k = 0; k < 3; k++) {
        unsigned sh = 16u - 8u * (unsigned)k;
        unsigned x  = ((argb >> sh) & 0xffu) + ((unsigned)c[k] * i) / 255u;

        if (x > 255u) { x = 255u; }
        argb = (argb & ~(0xffu << sh)) | (x << sh);
    }
    return argb;
}
