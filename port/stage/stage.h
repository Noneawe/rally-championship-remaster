/* stage.h - the stage renderer.
 *
 * This is "a stage renders": the thing that makes
 * a renderer rather than a model viewer.
 *
 * WHAT THIS IS A TRANSCRIPTION OF
 * ------------------------------
 * The original's stage draw path is six functions and they are all read, at
 * the instruction, in.
 * Every constant and every field offset below cites the address it came from.
 *
 *   FUN_00452886   rebuild the (2R+1)^2 visible-cell grid at [0x005f103c]
 *   FUN_00452a74   walk the grid, one call per occupied cell
 *   FUN_00452b97   walk one cell's polygon run; classify; append to a list
 *   FUN_00452c90   the reject test: a flag mask, then z >= |x| on corner 0
 *   FUN_0044afe9   transform one point into the [0x0057c238] vertex cache
 *   FUN_004531d6   the depth key for the sorted list
 *   FUN_00453161   build the per-texture dispatch tables from TEXTINFO +0x26
 *
 * ADDED THE EMITTER, and it is NOT FUN_0045172d.  That function is
 * the LIGHTING SUBDIVIDER.  The emitter is the seven-step pipeline at
 * FUN_0045026e, and transcribes all of it:
 *
 *   FUN_00452eee   dispatch on the list record's top byte; class 0xff aside,
 *                  a stage quad goes to FUN_0044d9bc
 *   FUN_0044d9bc   modulate the global colour by the per-material tint
 *   FUN_0045026e   the pipeline: corners, backface, reject, colours, draw
 *   FUN_00450388   the four corner indices; corner3 == corner2 means TRIANGLE
 *   FUN_004503b3   the per-corner reject, |vx|*1.2 < vz, carry-flag return
 *   FUN_004505a0   texture animation AND the two UV symmetry indices
 *   FUN_004507cb   the per-corner colour: shade byte -> a 256-entry RAMP
 *   FUN_004511ab   layer-0 UVs, permuted by [0x005f0998]
 *   FUN_0044aa44   sz and rhw - the D3DTLVERTEX position block
 *   FUN_0044b1f5   the SCREEN-SPACE viewport clipper
 *
 * TOOK THE UVs OFF THAT LIST.  stage_set_textinfo now loads the
 * four base pairs as well as the material byte, and FUN_0044c396 - which
 * nothing had read - says what the u16 in them MEANS:
 *
 *   0044c3cd  FILD [u16 << 8] / FSTP float / SUB EAX,0xc000000
 *
 * 0x0c000000 is twenty-four exponent steps, so that is a multiply by 2^-24,
 * and (u16 << 8) * 2^-24 is u16 / 65536.  THE STORED VALUE IS A 0.16
 * FIXED-POINT TEXTURE COORDINATE.  It is a bit-pattern float idiom of exactly
 * the kind lists, which is why the zero test
 * beside it exists: 0 has no exponent to subtract from.
 *
 * WHAT IS THE PORT'S OWN CHOICE, and is marked as such at each site:
 *  ~~the second texture layer is a second PASS, because plat_gfx_tex_bind
 *     is stage 0 only.  The original interpolates the blend in one pass.~~
 *     RETRACTED.  THE ORIGINAL IS NOT MULTITEXTURED EITHER: every
 *     two-layer arm of FUN_00449AC7 swaps the texture handle at [0x005d6f58]
 *     and calls the bind-and-draw at 0x0044ad5b a second time.  One texture
 *     stage, two draws, and this port's shape is the original's.  What IS the
 *     port's choice is where the second draw goes: deferring it to a second
 *     sweep over the same list keeps layer 1 grouped by plate, at the cost of
 *     an ordering the original does not have.  draw_list says so at the line.
 *  the second pass's STATE BLOCK is not a choice either.  It comes from the
 *     SECOND texture slot's TEXTINFO +0x24 through stage_layer_mode, and the
 *     three arms differ in the blend, in the layer-1 diffuse AND in whether
 *     the layer is fogged.
 *  the two ends of the shade ramp, for the same reason: the ramp LAW is
 *     transcribed, the level colour pair it is built from is not loaded.
 *  no software backface test.  FUN_0045053c is one, gated on [0x005d41b0],
 *     and which of that flag's three write sites is on the in-game stage path
 *     is not established.
 *
 * THE ONE THING A CALLER MUST KNOW.  cdat.h's accessors do not bounds-check.
 * stage_load checks `fits` once and refuses a payload that does not, so no
 * caller of this header has to.  Read cdat.h's accessor contract anyway.
*/
#ifndef RAL_STAGE_H
#define RAL_STAGE_H

#include <stddef.h>
#include <stdint.h>

#include "cdat.h"
#include "localdat.h"
#include "plat.h"

/* ---- the visible-cell grid, FUN_00452886 ------------------------------- */
/* `MOV ECX,0x400` / `LEA EDI,[0x5f103c]` / `STOSD.REP` fills 0x400 dwords;
 * `IMUL ECX,ECX,0x20` gives the row stride.  A 32-row grid of stride 32 holds
 * (2R+1)^2 only while 2R <= 31, so R is bounded at 15 - a 480 world-unit draw
 * radius, since a cell is 32 world units. */
#define STAGE_GRID_DWORDS 0x400
#define STAGE_GRID_STRIDE 0x20
#define STAGE_GRID_EMPTY  0x0fffffffu   /* `MOV EAX,0xfffffff` at 0x004528db */
#define STAGE_RADIUS_MAX  15

/* [0x005bd538], set by FUN_00437aa7 and CLAMPED TO >= 1 at 0x00437acd. */
#define STAGE_RADIUS_DEFAULT 2          /* `MOV [0x005bd538],0x2` 0x00452750 */

/* ---- the transformed-vertex cache, [0x0057c238] ------------------------ */
/* 4,096 slots of 0x20, direct-mapped on `pnts index & 0xfff` and tagged with
 * the FULL index.  FUN_0044afce is the "fill if the tag misses" wrapper. */
/* Cars whose headlights light the stage: the roster is [0x005F0A50] long, 8
 * in a race. */
#define STAGE_MAX_LIT 8
#define STAGE_MAX_FLASH 16

#define STAGE_VCACHE_SLOTS 4096
#define STAGE_VCACHE_MASK  0xfffu

/* The slot, field for field.  FUN_0044afe9 writes every one of these and
 * nothing else in the image writes the array.  +0x04 and +0x08 are never
 * written on this path and are kept so the layout is the original's. */
typedef struct {
    uint32_t tag;              /* +0x00  the full pnts index; empty = ~0u    */
    uint32_t unused04;         /* +0x04  no writer on the draw path          */
    uint32_t unused08;         /* +0x08                                      */
    float    sx;               /* +0x0c  screen x  =  vx*(F/vz) + centre_x   */
    float    vx, vy, vz;       /* +0x10 +0x14 +0x18  view space              */
    float    sy;               /* +0x1c  screen y  = -vy*(F/vz) + centre_y   */
} stage_vtx;

/* ---- the two draw lists ------------------------------------------------ */
/* FUN_00452c55 appends to [0x0097bb30]; FUN_00452c28 to [0x00973b30]. Both
 * are arrays of two dwords - a SORT KEY and the polygon index - and both are
 * quicksorted by the key (0x00452e83 and 0x00452df5) before being walked.
 *
 *   list A, keyed (batch_tex << 16) | (index & 0xffff)   -> batch by texture
 *   list B, keyed ((avg_or_min_z + 0x2000000) >> 10)     -> back-to-front
 *
 * CAPACITY IS A READING, NOT A STATEMENT.  Neither appender bounds-checks its
 * count.  The two bases are 0x8000 apart, which is 4,096 eight-byte records,
 * and nothing references 0x00983b30, so 4,096 is the extent the layout
 * implies rather than one the code asserts.  THE PORT CLAMPS; the original
 * would have walked into whatever follows. */
#define STAGE_LIST_MAX 4096

typedef struct {
    uint32_t key;
    int32_t  poly;
} stage_item;

/* ---- the working polygon, [0x0091d910] and [0x0091fb10] ---------------- */
/* The shared primitive submit FUN_0044AD8A drives a 0x44-stride working-vertex
 * array at [0x0091d910] through a corner ORDER TABLE at [0x0091fb10], whose
 * length is [0x005d6f8c].  FUN_0044BBEA's two Sutherland-Hodgman passes both
 * REWRITE that table and that length, so the polygon that reaches the vertex
 * writer has a variable corner count - which is why 0x0044AE01 re-tests it
 * against 3.
 *
 * A convex quad clipped against one plane gains at most one corner, so near
 * then far gives at most six.  Eight is headroom, and clip_plane refuses to
 * exceed it rather than trusting the arithmetic.
 *
 * THE ORDER TABLE IS A RING, NOT A STRIP.  The clipper walks edge (i, i+1)
 * with a wrap element appended at 0x0044BC42, so working slot k and slot k+1
 * are adjacent corners of the polygon.  FUN_00450E81 fills the four slots in
 * the order 2, 0, 1, 3 (0x00450F83, 0x00450E81, 0x00450ED7, 0x00450F2D
 * against FUN_00450388's [0x005f08ac..b8]) - so the ring is cdat corner
 * 2, 0, 1, 3, and NOT 0, 1, 2, 3.  See STAGE_RING in stage.c. */
#define STAGE_POLY_MAX   8
#define STAGE_POLY_VERTS ((STAGE_POLY_MAX - 2) * 3)

/* ---- the per-texture dispatch, FUN_00453161 ---------------------------- */
/* Built once per level from the 256 texinfo slots at 0x00922330, whose byte
 * +0x2e is TEXTINFO record byte +0x26 - copied there by initgametexinfbuf_q
 * at 0x00437b8d.  So the material class of a polygon is a property of the
 * TEXTURE, read out of a shipped file, not of the geometry. */
#define STAGE_TEXFLAG_SORTED  0x01u   /* `TEST byte [ECX+0x92235e],0x1` 453172 */
#define STAGE_TEXFLAG_MINZ    0x02u   /* `TEST byte [ECX+0x92235e],0x2` 4531f2 */

/* ---- the vertex mode, [0x005f0974], and the SECOND texture layer ------- *
 *
 * The same copy loop that carries +0x26 to the slot's +0x2e carries TEXTINFO
 * record +0x24 to the slot's +0x2c, three instructions earlier:
 *
 *   00437b81  MOV AL,[EDI+0x24] / MOV [ESI+0x2c],AL      ; the LAYER MODE
 *   00437b87  MOV AL,[EDI+0x25] / MOV [ESI+0x2d],AL
 *   00437b8d  MOV AL,[EDI+0x26] / MOV [ESI+0x2e],AL      ; the material class
 *
 * and FUN_00450BB3 turns that byte into the vertex mode:
 *
 *   00450bc7  [0x005f0974] = 2                   ; the DEFAULT
 *   00450bd1  CMP [0x005f08f8],0 / JZ +35        ; a SECOND texture layer?
 *   00450bda  EBX = [0x005f08f8] << 6 + 0x00922330      ; the SECOND slot
 *   00450be8  EAX = movzx byte [EBX+0x2c]
 *   00450bec  EAX = [EAX*4 + 0x005f0890]         ; {0->3, 1->5, 2->4, else 0}
 *   00450bf3  [0x005f0974] = EAX
 *
 * SO THE MODE IS A PROPERTY OF THE SECOND TEXTURE, not of the first and not
 * of the geometry - and mode 2, the one-pass arm, is NOT reachable through
 * the override.  A polygon with `pols+0x21 != 0` is drawn TWICE, with the
 * second pass's blend state chosen by this table.
 *
 * THE TABLE IS STATIC .data with three readers and no writer -
 * `python tools/ralimg.py refs 0x5f0890` gives 0x0043bb7a, 0x0044404b and
 * 0x00450bec, all reads:
 *
 *   0x005f0890  03 00 00 00 05 00 00 00 04 00 00 00 00 00 00 00
 *
 * Corpus shares over 1,822,369 drawable polygons, `python tools/stagetint.py`
 * section 3: mode 2 80.10%, mode 3 18.15%, mode 5 1.54%, mode 4 0.21%.  No
 * polygon in the shipped corpus resolves to mode 0, so the fourth table entry
 * is never selected and this port asserts nothing about it. */
#define STAGE_LAYER_MODES 3
extern const uint8_t stage_layer_mode[STAGE_LAYER_MODES];  /* [0x005f0890] */
#define STAGE_MODE_DEFAULT 2u

/* The pass-2 RENDER STATE and the pass-2 COLOUR, both returned rather than
 * applied, so that the probe can check the transcribed constants directly
 * instead of inferring them from a picture.
 *
 * THAT IS NOT DECORATION.  A perturbation sweep over nine facts in this
 * file caught two through the frame buffer and let seven through, and four
 * of the seven were arithmetic in exactly these two functions: breaking
 * mode 5's blend alone still left its DIFFUSE different, and breaking its
 * diffuse alone still left its BLEND different, so the frames differed
 * either way and every pixel assertion passed.  Standing rule 14 - a fix
 * that cannot be falsified one at a time has not been shown to do
 * anything - and the answer is to assert the halves, not the picture.
 *
 * stage_layer2_block fills five entries in D3D's own numbering, indexed by
 * the STAGE_L2_* constants; 0xffffffff means "this arm does not write this
 * state", which is mode 5's ZWRITE and is the difference section 3.2 of
 * exists to record. */
#define STAGE_L2_TEXBLEND  0
#define STAGE_L2_SRCBLEND  1
#define STAGE_L2_DSTBLEND  2
#define STAGE_L2_FOGENABLE 3
#define STAGE_L2_ZWRITE    4
#define STAGE_L2_STATES    5
#define STAGE_L2_UNWRITTEN 0xffffffffu
void stage_layer2_block(unsigned mode, int blended, int fog_on,
                        uint32_t out[STAGE_L2_STATES]);

/* The layer-1 diffuse and specular, from the ALREADY-CLIPPED layer-0
 * colour, blend weight and fog byte.  FUN_00449AC7's +0x2c and +0x30. */
void stage_layer2_colour(unsigned mode, uint32_t argb0, unsigned weight,
                         unsigned fog,
                         uint32_t *diffuse, uint32_t *specular);

/* ---- vertex fog, ------------------------------------- *
 *
 * THE STAGE HAS NO SKY GEOMETRY.  The horizon is fog, and the fog is a
 *
 *
 *
 * DISTANCE-INDEXED BYTE RAMP that spans exactly the draw distance:
 *
 *   0044dbfc  CALL 0x00437aa7          ; re-run the preset selector first
 *   0044dc07  CALL 0x0044dd01          ; the noise blend, see below
 *   0044dc67  [0x005ea4e0] = [0x005f0b18] << 3     ; length = zfar * 8
 *   0044dc80  the loop: accumulate, add a base, clamp 0..255, store
 *
 * and the applier runs on EVERY primitive of both the stage and the model
 * path, from the middle of the shared submit:
 *
 *   0044adee  CALL 0x0044c2f5 -> 0x0044c321   ; per corner:
 *   0044c33a    FLD [EDX+0x18] / FISTP        ; the SAME view z sz uses
 *   0044c301    SHR EAX,0xd                   ; raw 16.16 -> world * 8
 *   0044c312    MOV AL,byte [EAX + 0x005e94e0]
 *   0044c363    XOR AL,0xff                   ; 255 - ramp
 *   0044c365    MOV [ESI+0x20],AL             ; the working vertex's fog byte
 *
 * AND BYTE +0x20 IS THE D3DTLVERTEX SPECULAR ALPHA.  That was an INFERENCE
 * until opened FUN_00449A97 - the last call in the same submit,
 * which nobody had printed.  Its mode-2 arm ends:
 *
 *   00449d68  MOV [EBP+0x24],EAX       ; the modulated diffuse | 0xff000000
 *   00449d6b  MOV AL,byte [EBP+0x20]   ; the fog byte
 *   00449d6e  SHL EAX,0x18             ; the upper bits shift out: fog << 24
 *   00449d71  MOV [EBP+0x28],EAX       ; the working vertex's specular dword
 *
 * and FUN_0044AB15, called with ESI=8 and EDX=0x24, is the packer that copies
 * working +0x24 / +0x28 to [0x009089a0 + i*0x20] +0x10 / +0x14 -
 * D3DTLVERTEX.color and D3DTLVERTEX.specular:
 *
 *   0044ab58  MOV EAX,[EDX+EBP]   / MOV [EDI+0x10],EAX
 *   0044ab5e  MOV EAX,[EDX+EBP+4] / MOV [EDI+0x14],EAX
 *
 * Four sites in two other functions corroborate it from the far end: they
 * skip the working vertex entirely and stuff 0x0044C2FB's (255-ramp)<<24
 * DIRECTLY into [0x009089b4] / [0x009089d4] / [0x009089f4] / [0x00908a14],
 * which is 0x009089a0 + 0x14 for corners 0..3. */

/* The ramp store [0x005E94E0] is 0x1000 bytes and the length is R * 256, so
 * the original is safe to R = 16 and has no guard.  STAGE_RADIUS_MAX is 15. */
#define STAGE_FOG_STORE   0x1000

/* The noise map [0x009CAEE4], 4,208 bytes filled at 0x0041E3AE from a
 * FIXED-SEED LFSR - so it is the same map in every run of every build, and a
 * port reproduces it by running the same generator rather than by shipping a
 * table. */
#define STAGE_FOG_NOISE_N 0x1070
#define STAGE_FOG_NOISE_W 64

typedef struct {
    uint8_t  ramp[STAGE_FOG_STORE];   /* [0x005E94E0]                        */
    int32_t  len;                     /* [0x005EA4E0] = zfar * 8             */

    /* [0x005E94D4], built at 0x0044DCB8 from the ramp accumulator's FINAL
     * value plus LOCALDAT +0x658.  0x0044C355 substitutes it for the lookup
     * whenever [0x005E948C] == 4.
     *
     * [0x005E948C] has TEN immediate writers image-wide (`python
     * tools/ralimg.py find c7058c945e00`) carrying five distinct values -
     * 0, 1, 2, 4 and 5 - and the only 4 is 0x00453489, inside FUN_0045343E,
     * which sets it back to 0 at 0x004534ED in the same function.  Which of
     * the two the stage draw path runs under is NOT established, so `mode`
     * defaults to 0 and the flat value is built and carried rather than
     * used. */
    int32_t  flat;
    int      mode;

    /* [0x005D6E94] | 0xFF000000, from LOCALDAT +0x618..+0x620. */
    uint32_t color;
} stage_fog;

/* Build the ramp.  `radius` is [0x005BD538] - taken from the caller so a
 * probe that forces R is honoured, exactly as stage_depth_scale does - and
 * `eye_x` / `eye_z` stand in for [0x006CD308] / [0x006CD310], the world
 * position FUN_004365A6 copies out of car 0's working block.  This port has
 * no car, so it samples at the eye.  `ld` may be absent, which gives a ramp
 * of zeros and a black fog colour. */
void stage_fog_build(stage_fog *f, const ld_file *ld, int have_ld,
                     int32_t radius, int32_t eye_x, int32_t eye_z);

/* FUN_0044C321 for one corner: 255 - ramp[clamp(fistp(view_z) >> 13)]. */
unsigned stage_fog_byte(const stage_fog *f, float view_z);

/* The 4,208-byte noise map, generated once.  Exposed so a gate can check the
 * generator rather than only the ramp it feeds. */
const uint8_t *stage_fog_noise(void);

/* ---- the view ---------------------------------------------------------- */
typedef struct {
    /* [0x0059f014], the matrix FUN_0044b1ab multiplies by: 4 rows of 4
     * floats, basis in the columns, translation in row 3.  v' = v * M. */
    float    m[16];

    /* [0x005ba600] / [0x005ba604] - the x and z the grid origin is taken
     * from, as 16.16 world coordinates.  FUN_00452886 uses SAR, not the SHR
     * of FUN_00440424; the `& 0x7ff` that follows makes the two agree. */
    int32_t  eye_x, eye_z;

    /* [0x005BA5EC], the camera's y.  The stage never reads it; the
     * object path's translation does (objscene_camera).  has_eye_y 0 keeps
     * the old rule - eye y recovered from m[13], exact only with no pitch,
     * which is every view stage_view_default builds - so a caller that never
     * sets it sees no change. */
    int32_t  eye_y;
    int      has_eye_y;

    float    focal;             /* [0x005d6f1c]                              */
    float    centre_x;          /* [0x005d6f30]                              */
    float    centre_y;          /* [0x005d6f34]                              */
    float    near_z;            /* [0x0057b09c] = 4096.0f = 0.0625 world     */

    int32_t  radius;            /* [0x005bd538], clamped to 1..15            */

    /* [0x005f453c].  FUN_00452c90 rejects a polygon whose flags word
     * (pols+0x02) has any of these bits set - a per-frame draw mask.  Which
     * bits mean what is NOT read; 0 disables the test, which is what the
     * game itself does whenever [0x005d41b0] is zero. */
    uint32_t flag_mask;

    /* THE PORT'S FIX: 0 here, so the stage gates stay
     * faithful; rally turns it on unless RALLY_FAITHFUL_CULL is set.  FUN_00452c90 tests corner 0
     * alone against its wedge, so a ground quad whose corner 0 is behind or
     * beside the eye drops out whole while its other corners are on screen.
     * Non-zero rejects only when ALL FOUR corners are outside the SAME side
     * plane. */
    int      cull_all_corners;

    /* The port's own knob, and it exists because CULLMODE has zero write sites
     * image-wide, so the winding the original culled under is not stated
     * anywhere.  0 emits the corner order FUN_00420c64 computes its normals in
     *(0,1,2) and (3,2,1); 1 emits both triangles reversed.
     * port/tests/stage_render_probe.c measures which one faces the camera.
     *
     * THE QUESTION IS ANSWERED as of 2026-09-14: front faces are CLOCKWISE, on
     * the stage path (tools/stagewind.sh, 47 of 48 levels decisive) and on the
     * MODEL path (tools/modelwind.sh, 375,472 front faces against 261 -
     * The backends now cull with
     * glFrontFace(GL_CW).  This field stays a knob, because it is how the
     * measurement is re-run and how a regression would be found - but 0 is
     * now known to be right rather than merely chosen. */
    int      reverse_winding;

    /* DIAGNOSTIC, default 0 = the original's behaviour.
     *
     * A corner with vz < near_z keeps screen (0,0) - FUN_0044afe9
     * zeroes both coordinates BEFORE the near test - and the quad is
     * still submitted, because FUN_004503b3 accepts on ANY corner.
     * says the screen-space clipper
     * then "deals with it", but (0,0) is INSIDE the viewport
     * rectangle section 8.3 clips against, so it survives and the quad
     * is drawn as a screen-filling wedge anchored at that corner.
     *
     * Set to 1 to drop such quads instead.  That is NOT what the
     * original does; it exists so the wedges can be turned off and the
     * rest of the picture looked at.
     *
     * SINCE THE VIEW CLIP LANDED THIS KNOB IS A
     * HISTORICAL DIAGNOSTIC AND NOTHING MORE.  The original CLIPS such a
     * quad and draws the remainder; dropping it discards the part that was
     * visible.  With disable_view_clip = 0 no pinned corner survives to the
     * backend at all, so setting this changes nothing except the counter's
     * meaning.  It is kept so stagepinned.sh's A/B still runs. */
    int      drop_pinned_quads;

    /* DIAGNOSTIC, default 0 = the original's behaviour, and the ONLY reason
     * it exists is standing rule 11: a gate is a claim, and a claim gets
     * checked in the failing direction.  Set to 1 to submit the polygon
     * unclipped, which is what this port did before - so the probe can
     * run the same frame both ways and assert that the clip is what removes
     * the wedges, rather than asserting that today's picture looks fine.
     *
     * It disables all THREE of FUN_0044AD8A's gates, not just the clip:
     *   0044ade1  every corner out of range   -> draw nothing
     *   0044adfc  any corner out of range     -> CLIP in view space
     *   0044ae08  fewer than 3 corners left   -> draw nothing */
    int      disable_view_clip;

    /* DIAGNOSTIC, default 0 = the original's behaviour, and it exists for the
     * same reason disable_view_clip does.  Set to 1 to write the raw ramp
     * colour into the diffuse with no modulate at all - which is what this
     * port did before - so the probe can run the same frame both ways.
     *
     * It disables BOTH halves: the per-slot tint AND the rest-state multiply
     * by 0x00ffffff, which is the half that touches every vertex in every
     * level.  See STAGE_DIFFUSE_REST. */
    int      disable_diffuse_modulate;

    /* DIAGNOSTIC, default 0 = the original's behaviour, and it exists for the
     * same reason the two above do: the second texture layer's state block is
     * chosen per polygon out of a shipped file, and a claim that the choice
     * matters has to be checkable in the failing direction.
     *
     * 0   take the mode from the second slot's TEXTINFO +0x24 - the original.
     * 3   draw EVERY second layer with mode 3's block, which is what this
     *     port did before.  Reverting to it is the A/B that says
     *     whether modes 4 and 5 move a pixel, and it is the only setting
     *     under which the port's old picture can be re-rendered.
     * 4   force the additive block everywhere.
     * 5   force the inverse-source-colour block everywhere.
     *
     * Forcing a mode changes the STATE BLOCK, the layer-1 diffuse AND the
     * layer-1 specular together, because the three are one arm of
     * FUN_00449AC7 and are not independently selectable in the original. */
    int      layer2_mode_override;

    /* DIAGNOSTIC, default 0 = the original's behaviour.  Set to 1 to skip
     * the second pass entirely - which is what a one-layer port draws, and
     * which is the only way to ask "is this second layer worth any pixels"
     * without also changing what it is blended with.  It is what tells a
     * frame whose three forced blend blocks agree (LEVEL42) apart from a
     * frame where the blend is not reaching the backend at all. */
    int      disable_second_layer;

    /* DIAGNOSTIC, default 0 = the original's behaviour.  Set to 1 to
     * skip the BACK of a two-sided polygon (class bit 0x08, FUN_00450A9C) -
     * what the port drew before 2026-10-19: fences invisible from behind. */
    int      disable_back_pass;

    /* The animation time base, [0x006c2118].  FUN_004505a0 substitutes a
     * slot for phase ((t / rate) & 7) whenever LOCALDAT's rate byte is
     * non-zero.  The rate is a DIVISOR: the table at [0x0057bbd0] that
     * 0x0041e48f fills holds 1/i. */
    int32_t  time_base;

    /* [0x005bd93c], set at 0x00437b17 - and note the source is a TABLE,
     * `MOV EAX,[EBX*4 + 0x005bd9a8]`, so it is a preset index rather than a
     * raw LOCALDAT field.  FUN_0044c516 computes D = 100 - detail, clamps it
     * to 90..100 and divides by 100.  0 is the full-range case.
     *
     * ~~It reaches the renderer ONLY through the depth scale.~~  NOT ANY MORE,
     * and the correction is the: it also sets WHERE GEOMETRY IS CUT.  The
     * view clip's far threshold is [0x005d70c4] = FISTP([0x005d705c] * 0.9f)
     * at 0x0044C672, and [0x005d705c] is (zfar << 16) / D - so the far clip
     * sits at 0.9/D of the true far plane.  At D = 1.00 (detail 0, what this
     * port uses) that is 0.9 * zfar; at D = 0.90 it is exactly zfar.  Detail
     * therefore moves the draw distance by 10%, not just the depth
     * resolution.
     *
     * WHICH PRESET THE GAME RUNS *IS* READ, as of - and it came
     * out of, because FUN_0044DBFC's first instruction is a call to the
     * same selector.  The index is [0x005BD940] = LOCALDAT +0x1fd0, it has
     * ONE absolute reference image-wide (the read at 0x00437AA9), and all 48
     * shipped levels carry 0.  stage_fog_build below runs the selector; this
     * port still has no reader for the resulting D, so `detail` stays 0,
     * which is slot 0's value on every shipped level anyway. */
    int32_t  detail;

    /* The two ends of the shade ramp FUN_0044d1ad builds, as 0..255 triples.
     * The per-corner shade byte indexes the 256-entry table the ramp fills;
     * shade_lo is entry 0 and shade_hi is entry 255.  The game takes them
     * from a level colour pair: [0x005bbf70], LOCALDAT +0x600 / +0x60c, in a
     * race, [0x005e09a0] out of one.  stage_view_init leaves black -> white,
     * the identity ramp; stage_view_shade_ld loads the level's pair. */
    uint8_t  shade_lo[3];             /* r, g, b */
    uint8_t  shade_hi[3];

    /* The two BAKED LIGHT ramps, black -> colour (FUN_0044D200): a polygon
     * whose flags word has bit 4 adds ramp1[pols+0x28+k] and ramp2[pols+0x2c+k]
     * to corner k, saturating (0x0045080D..0x00450A6D, the front only).  The
     * colours are LOCALDAT +0x1898 / +0x18a4 in a race; black adds nothing,
     * which is stage_view_init's default.  The floodlights' pools on LEVEL46. */
    uint8_t  light1[3];
    uint8_t  light2[3];

    /* The cars' HEADLIGHTS on the ground - FUN_00451659's per-corner light,
     * 0x0044D603 -> 0x0044D65A per car with the latch on, into ramp 3
     * (black -> light3, LOCALDAT +0x18b0, [0x005E8BDC]), saturating-added to
     * the corner's colour before the arm's modulate (FUN_0045172D).  `nlit`
     * 0 is the gate [0x005F0A18] off: no car with +0x25C and +0x84 set.
     * stage_view_init leaves it 0. */
    int      nlit;
    struct {
        int32_t pos[3];               /* car+0x134, 16.16                     */
        float   frame[9];             /* world -> car rows, car+0x9CC's 3x3   */
        int32_t abc;                  /* car+0xABC, the lamps' brightness     */
    } lit[STAGE_MAX_LIT];
    uint8_t  light3[3];

    /* the type-0x3B IMPACT FLASH's slots (FUN_00452D06 builds them: the
     * table at 0x005F0F1C, count [0x005F0B08], up to 16).  Each is a point light
     * at the object's anchor pnt, strength k (1..15, fading to 0), summed into
     * the same per-point accumulator the headlights use (FUN_0044D82C for the
     * ground, FUN_0044D473 for objects - the same arithmetic).  `nflash` 0 is no
     * flash.  stage_view_init leaves it 0. */
    int      nflash;
    struct {
        int32_t pos[3];               /* the anchor pnt, 16.16 world          */
        int32_t k;                    /* slot +4, [0x005F0F20]                */
    } flash[STAGE_MAX_FLASH];

    /* NULL means "emit no fog byte and leave FOGENABLE off", which is
     * what this port did before; the original always has a ramp,
     * because FUN_0044DBFC runs at level load.  When it is set, stage_draw
     * writes FOGCOLOR and FOGENABLE from it and every emitted vertex carries
     * its specular alpha.
     *
     * IT IS A POINTER RATHER THAN A MEMBER because the ramp is 4 KiB and
     * stage_view is copied by value (the probe does `stage_view cv = v`). */
    const stage_fog *fog;
} stage_view;

/* ---- the depth scale, [0x005d7060] and FUN_0044c516 -------------------- */
/* sz = view_z * stage_depth_scale(radius, detail), and FUN_0044aa44 divides
 * 0.01f by the RESULT to get rhw.  Radius and detail reach the vertex through
 * this and nothing else.  It is recomputed per quad rather than cached in the
 * view, so that a caller which changes radius after stage_view_default - the
 * probe does exactly that - cannot be reading a stale scale. */
float stage_depth_scale(int32_t radius, int32_t detail);

/* ---- the UV symmetries, [0x005f0998] ----------------------------------- */
/* Eight rows of four corner selectors - the dihedral group of the square.
 * Static data with NO writer: two references image-wide, both the loads at
 * 0x004511f8 and 0x00451430. */
#define STAGE_UV_SYMMETRIES 8
extern const uint8_t stage_uv_sym[STAGE_UV_SYMMETRIES][4];

/* ---- the DIFFUSE modulate, [0x005d6f60] -------------------------------- */
/* The global colour every vertex-mode arm multiplies the per-corner ramp
 * colour by before it becomes D3DTLVERTEX.color.  Its REST STATE is
 * 0x00ffffff: that is its initialised value in the image at 0x005d6f60, and
 * a census of all 26 writers (python tools/ralimg.py refs 0x5d6f60)
 * finds every one of them either inside a save/restore bracket or an explicit
 * store of exactly this constant.
 *
 * AND THE MODULATE IS NOT THE IDENTITY EVEN AT REST.  The arithmetic is
 * (a*b)>>8 per byte, so multiplying by 0xff maps every channel c in 1..255 to
 * c-1 and leaves 0 at 0. */
#define STAGE_DIFFUSE_REST 0x00ffffffu

/* (a*b)>>8 per byte over the low THREE bytes, with the top byte cleared -
 * FUN_0044d9bc 0x0044da03 in scalar form and 0x0044d9df in MMX form, and the
 * two are bit-identical because 255*255 = 65025 fits in PMULLW's low word and
 * PACKUSWB never has to saturate.  The arms restore the alpha with
 * `OR EAX,0xff000000` afterwards; this function does not. */
uint32_t stage_modulate8(uint32_t c, uint32_t m);

/* ---- the stage --------------------------------------------------------- */
typedef struct {
    cdat_payload cdat;
    ld_file      ld;
    int          have_ld;

    /* the level's 256 texture slots, resolved once */
    uint32_t     texinfo_rec[LD_TEXSLOTS];   /* LOCALDAT +0x0100            */
    uint8_t      texflags[LD_TEXSLOTS];      /* TEXTINFO +0x26              */
    /* TEXTINFO +0x24 -> texinfo slot +0x2c, the SECOND LAYER'S mode selector
     * FUN_00450BB3 indexes stage_layer_mode with.  Read off the slot named by
     * pols+0x21, never the one named by pols+0x01. */
    uint8_t      texmode[LD_TEXSLOTS];       /* TEXTINFO +0x24              */
    uint16_t     plate[LD_TEXSLOTS];         /* record >> 4                 */
    /* LOCALDAT +0x1bcc, the live [0x005bd53c]: the per-texture-slot diffuse
     * tint FUN_0044d9bc multiplies into the global colour.  Indexed by the
     * polygon's PRIMARY texture id (pols+0x01) and NOT by the animated slot,
     * because 0x0044d9c2 reads the record byte directly and runs before
     * FUN_004505a0 ever substitutes a phase. */
    uint32_t     slot_tint[LD_TEXSLOTS];     /* LOCALDAT +0x1bcc            */
    /* TEXTINFO record bytes +0x00..+0x0f, copied verbatim into the texinfo
     * slot's +0x00..+0x0f at 0x00437b93: FOUR (u16 u, u16 v) PAIRS.  Loaded by
     * stage_set_textinfo; all zero until it is called, which draws the
     * top-left texel of the plate over the whole polygon rather than
     * inventing a unit square.  See quad_uv. */
    uint16_t     uv[LD_TEXSLOTS][4][2];
    plat_tex     tex[LD_TEXSLOTS];           /* 0 until stage_set_texture   */

    int32_t      grid[STAGE_GRID_DWORDS];
    int32_t      grid_x, grid_z;             /* [0x005f1034] / [0x005f1038] */
    int          grid_valid;
    int32_t      grid_r;                     /* the radius `grid` is for */
    /* THE PORT'S: the grid the OTHER view left.  The mirror (R 2) and
     * the main view (R 9) alternate, and each one's miss swept the whole zube
     * array; a miss now looks here first.  `grid` stays what the last drawn
     * view left - snd_world_collect reads it. */
    int32_t      prev_grid[STAGE_GRID_DWORDS];
    int32_t      prev_x, prev_z, prev_r;
    int          prev_valid;

    stage_vtx    vcache[STAGE_VCACHE_SLOTS];

    stage_item   list_tex[STAGE_LIST_MAX];   /* [0x0097bb30]                */
    stage_item   list_sorted[STAGE_LIST_MAX];/* [0x00973b30]                */
    int          n_tex, n_sorted;
    int          overflow_tex, overflow_sorted;

    /* The class-0xff records the cell walk meets are OBJECT
     * INSTANCES, not polygons, and what draws them is the .ro3 model path -
     * a different container, a different transform and a different corner
     * ring.  port/obj owns all three, so the stage hands the record over
     * rather than growing a second renderer inside itself.
     *
     * WHAT THE HOOK IS NOT.  The original does not draw here: FUN_00453161
     * writes 1 into every entry of [0x005F0C1C] (0x00453182), so a class-0xff
     * record ALWAYS takes the DEPTH-SORTED list and is dispatched later by
     * FUN_00452EEE, in sorted order, alongside the translucent stage quads.
     * This hook fires during the walk instead.  With the depth test on and
     * the objects opaque the picture is the same; the place it would differ
     * is a translucent object, and what FUN_004531D6 makes of an instance
     * record's `corners` - which are an anchor index and four angle bytes -
     * is not read by anyone, owner. */
    void       (*obj_fn)(void *ctx, const stage_view *v, int32_t poly);
    void        *obj_ctx;
} stage;

/* Per-frame counters.  Diagnostics, and the only thing the gate can assert
 * about a headless run. */
typedef struct {
    unsigned long cells_visible;
    unsigned long polys_walked;
    unsigned long polys_route;      /* class 0xff - object instances,  */
    unsigned long polys_culled_flag;
    unsigned long polys_culled_geom;
    /* Quads submitted with at least one corner pinned to screen (0,0)
     * by the near test.  Counted whether or not they are dropped. */
    unsigned long polys_pinned;

    /* The three gates of FUN_0044AD8A, each counted separately, because a
     * single "clipped" tally cannot tell a clip that fired from a clip that
     * dropped the polygon outright. */
    unsigned long polys_out_of_range;  /* 0044ade1  EVERY corner out: drop  */
    unsigned long polys_clipped;       /* 0044adfc  some out: clip ran      */
    unsigned long polys_clip_empty;    /* 0044ae08  under 3 corners: drop   */
    unsigned long corners_added;       /* new corners the clip created      */

    /* THE ASSERTION COUNTER.  A vertex that reached plat_gfx_draw while its
     * cache slot still carried FUN_0044afe9's (0,0) placeholder - i.e. one
     * the view clip should have removed and did not.  With the clip on this
     * is zero on every level; with disable_view_clip it is not, and that
     * pair is what stage_render_probe asserts.  Standing rule 11. */
    unsigned long verts_pinned_emitted;
    unsigned long polys_listed_tex;
    unsigned long polys_listed_sorted;
    unsigned long quads_drawn;
    unsigned long backs_drawn;      /* two-sided polygons' back passes */
    unsigned long layer2_drawn;
    /* The second layer's vertex mode, by polygon: [3], [4] and [5] of this
     * array against the corpus shares in stage_layer_mode's comment.  [0]
     * counts second layers whose slot selects table entry 3 or above, which
     * maps to mode 0 and which no shipped level reaches - so a non-zero [0]
     * means the level set changed or the slot lookup is wrong. */
    unsigned long layer2_mode_n[6];
    /* Second layers drawn whose four per-corner weights at pols+0x22..+0x25
     * are ALL ZERO.  Those are invisible in every one of the three modes and
     * the arithmetic says why: the weight is the diffuse alpha, so mode 3's
     * SRCALPHA/INVSRCALPHA and mode 4's SRCALPHA/ONE both scale the source by
     * zero, and mode 5's grey is (0 * fog) >> 8 = 0, whose ZERO/INVSRCCOLOR
     * blend leaves the destination at dst * (1 - 0).  A frame whose second
     * layers are all like this is a frame in which the three state blocks are
     * genuinely indistinguishable - which is why the gate counts them rather
     * than asserting through them.  Corpus: 7.5% of LEVEL42's second layers,
     * 29.6% of LEVEL21's, 0.3% of LEVEL61's. */
    unsigned long layer2_zero_weight;
    /* Second layers drawn in mode 4 or mode 5 that DO carry a weight -
     * the ones whose state block differs from mode 3's and whose
     * difference can actually reach the frame.  It is the only honest
     * gate on "the per-polygon selection changed the picture": both
     * halves of that condition are properties of the DATA, so a broken
     * selection cannot make the assertion decline itself away. */
    unsigned long layer2_live_45;
    /* plat_gfx_draw calls the pass-2 sweep had to issue purely because the
     * mode changed between two consecutive polygons of the same plate.  The
     * batching cost of drawing the second layer in list order rather than
     * grouped by blend state - see draw_list. */
    unsigned long layer2_state_flushes;
    unsigned long batches;          /* plat_gfx_draw calls issued       */
    unsigned long batches_tex;      /* ...of them, for the opaque list  */
    unsigned long vtx_transformed;  /* cache misses                          */
    unsigned long vtx_hits;

    /* The screen-space bounding box of everything that reached the backend,
     * and the view-space z range behind it.  Not decoration: a renderer whose
     * cell walk, cull and batching all report healthy numbers while nothing
     * appears is a renderer whose geometry landed outside the viewport, and
     * these four floats are the difference between finding that in a minute
     * and finding it in an afternoon.  They are in the vertex's own units -
     * pixels for sx/sy, raw 16.16 for z. */
    float         min_sx, max_sx, min_sy, max_sy;
    float         min_vz, max_vz;

    /* ---- the fog byte, by DEPTH, -------------------- *
     *
     * The probe's picture-level fog check bands the frame by SCREEN ROW,
     * which is a proxy for depth and only a good one while the camera is
     * level and the stage is a ground plane - standing rule 1, and it cost a
     * gate that a CONSTANT fog factor passed on one level and failed on
     * another.  These are the same question asked where the depth actually
     * is: the mean fog byte of every EMITTED vertex, in four quartiles of the
     * draw distance, plus the count in each.
     *
     * A correct distance ramp gives a non-increasing sequence.  A constant
     * factor gives four equal means.  An inverted sense gives an increasing
     * one.  None of the three needs anything to be true about the camera.
     *
     * The quartiles are of [0, far_count), the same far value FUN_0044C292
     * counts against - so quartile 3 is the band the view clip trims at
     * 0.9x, and it is normal for it to be the thinnest. */
    unsigned long fog_q_n[4];
    unsigned long fog_q_sum[4];

    /* where stage_draw's time goes, microseconds - the cells walked
     * (cull, transform, clip, the lists built), the two sorts, the draws */
    unsigned long long us_walk, us_sort, us_draw;
} stage_stats;

/* ---- the API ----------------------------------------------------------- */

/* Take a `.sq` or a raw cdat00c payload, and optionally the level's LOCALDAT.
 * Returns 0 on success.  Refuses a payload whose declared sections do not fit
 * the buffer - cdat.h's accessor precondition, checked ONCE, here. */
int  stage_load(stage *s,
                const void *cdat_buf, size_t cdat_len,
                const void *localdat_buf, size_t localdat_len,
                char *err, size_t errlen);
void stage_free(stage *s);

/* Hand the renderer a texture for one of the 256 slots.  Optional: a stage
 * with no textures draws untextured (plat_gfx_tex_bind(0)), which is what the
 * gate does, because decoding the TEXTINFO plates themselves is not this
 * ticket -, owner. */
void stage_set_texture(stage *s, unsigned slot, plat_tex t);

/* Hand the renderer `files\a\TEXTINFO` - 2,048,000 bytes, 16,000 records of
 * 0x80.  Only ONE byte of each record is read here: +0x26, which
 * initgametexinfbuf_q copies to the texinfo slot's +0x2e and FUN_00453161
 * turns into the opaque/sorted dispatch.  Without it every polygon is treated
 * as opaque and texture-batched, which is what the flag byte says for the
 * great majority of them anyway. */
void stage_set_textinfo(stage *s, const void *buf, size_t len);

/* Register the class-0xff object emitter, or NULL to go back to
 * counting and skipping.  See `obj_fn` above for what this hook is not. */
void stage_set_object_hook(stage *s,
                           void (*fn)(void *ctx, const stage_view *v,
                                      int32_t poly),
                           void *ctx);

/* A sane default view for a level: eye at the given world position, looking
 * down +z with the level's own focal convention.  The port's own helper - the
 * game builds [0x0059f014] from the camera it already has. */
void stage_view_default(stage_view *v, int screen_w, int screen_h,
                        int32_t eye_x, int32_t eye_y, int32_t eye_z,
                        float yaw);

/* The race's shade ramp ends, LOCALDAT +0x600 / +0x60c (FUN_0044D0EF's race
 * arm).  Without a LOCALDAT the view keeps its identity ramp. */
void stage_view_shade_ld(stage_view *v, const ld_file *ld, int have_ld);

/* The cars' headlight at world point p (16.16): 0x0044D65A's sum over v->lit,
 * which is also FUN_0044D2A1's, the arm a placed object takes (FUN_0044F3DC,
 * 0 when no car's lamps are on. */
uint32_t stage_point_light(const stage_view *v, const int32_t p[3]);
/* the impact flash's term alone, saturating-added to `rgb` (an object's or a car's light base). */
uint32_t stage_flash_add(const stage_view *v, const int32_t p[3], uint32_t rgb);

/* One frame.  Rebuilds the visible-cell grid if the eye changed cell, walks
 * it, culls, sorts both lists and issues the draws through plat.h.  `st` may
 * be NULL. */
void stage_draw(stage *s, const stage_view *v, stage_stats *st);

/* FUN_00452886 alone: the visible-cell grid for `v` (its radius and eye),
 * rebuilt only when the origin cell moved - what stage_draw does first.  For
 * a caller that needs the grid with no picture (the gate). */
void stage_grid_build(stage *s, const stage_view *v);

/* FUN_004505a0: the animated slot actually drawn for `slot` at `time_base`. */
unsigned stage_texslot(const stage *s, unsigned slot, int32_t time_base);

/* The cell key FUN_00440424 builds, and its inverse, exposed for tests. */
uint32_t stage_cell_key(int32_t world_x, int32_t world_z);

#endif /* RAL_STAGE_H */
