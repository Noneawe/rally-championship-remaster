/* plat.h - the platform shim for the Rally Championship port.
 *
 *
 *
 * This is the whole platform surface. Game code calls these and nothing else;
 * no translation unit above this header may include <windows.h>, <SDL.h>,
 * any <GL/...> header or any DirectX header.
 *
 * Backends (corrected 2026-09-12; - this comment
 * used to name plat_sdl_desktop.c and plat_sdl_handheld.c, which do not exist):
 *     plat_null.c            no-op, always builds, no dependencies
 *     plat_sdl2.c + plat_gl.c
 *                            SDL2 + desktop GL   - development, and the oracle
 *     the same, -DPLAT_SDL2_GLES2
 *                            SDL2 + GLES2        - the target
 *
 * plat_sdl2.c is the platform half and issues no GL call; plat_gl.c is the
 * renderer and issues no SDL2 call but SDL_GL_GetProcAddress.
 *
 * ABOVE this header, and NOT part of it, is port/stage/ -'s
 * stage renderer. It calls plat.h and nothing else, so the same object file
 * draws through all three backends.
 *
 * The two SDL2 backends are ONE source compiled twice: the whole of the
 * difference is the GL context profile. What each entry point below actually
 * DOES in each backend - real, stub, or degraded - is documented per entry
 * point in.
 * Read it before calling anything: much of the surface is still a stub, and
 * the stubs are silent. The GRAPHICS half is not - landed plat_gl.c on
 * 2026-09-13 and the SDL2 backends draw. ~~Audio,~~ settings ~~and
 * plat_input_action~~ are the stubs that remain (audio is real since,
 * plat_audio_ma.c; plat_input_action retired).
 *
 * WHY THIS SHAPE
 * --------------
 * Every declaration here corresponds to something the original binary
 * measurably does, and the comment on it cites the address. The design is
 * argued in; the measurements
 * behind it are in,
 * and.
 *
 * The rule that produced it: expose what the game asks for and nothing more.
 * This is not a general-purpose engine abstraction and must not become one.
*/

#ifndef PLAT_H
#define PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. lifecycle ------------------------------------------------------ */

typedef struct {
    const char *app_name;
    const char *log_path;         /* the game's own log; keep the tag format */
    int         want_fullscreen;
    int         audio_clock;      /* a fake-clock run: no audio device; the
                                     mixer runs only on plat_audio_advance */
} plat_config;

int  plat_init(const plat_config *cfg);
void plat_shutdown(void);
const char *plat_backend_name(void);

/* ---- 2. video ---------------------------------------------------------- */
/* The original has videomodemain and videomodegame, and a scene bracket the
 * log calls GOTO2D / GOTO3D. shows those are
 * EndScene (0x00419EEC) and BeginScene (0x00419F29), not two rendering paths -
 * so there is one surface here and a bracket, not two. */

int  plat_video_set_mode(int width, int height, int bpp, int fullscreen);
void plat_video_get_mode(int *width, int *height, int *bpp);

/* Non-zero when a real drawable surface exists. Promoted into the header by
 * which needs it: every backend degrades to counting instead of drawing
 * when there is no context, and a caller must be able to tell the two apart
 * without parsing the log. plat_sdl2.c carried it as a file-local prototype
 * with a comment saying "if it is still wanted when the renderer lands, it
 * belongs in the header". It is, so it does. */
int  plat_video_ok(void);
void plat_video_frame_begin(void);
void plat_video_scene_begin(void);   /* was GOTO3D  0x00419F29 */
void plat_video_scene_end(void);     /* was GOTO2D  0x00419EEC */
void plat_video_present(void);       /* was ddsflip 0x00405518 */

/* the platform layer only (a remaster, not a transcription): present
 * paced to the display - vsync, adaptive where the driver has it, and where
 * the swap does not block, a sleep to the display's refresh.  On by default;
 * 0 presents as fast as the loop runs (a frame-rate measurement).  The game's
 * 10 ms step accumulator never sees this: a paced frame only means more steps
 * fall due per frame.  No surface (--no-window, plat_null): nothing to pace. */
void plat_video_set_vsync(int on);

/* ---- 3. geometry ------------------------------------------------------- */
/* 32 bytes. The layout is proved three ways in
 * section 4: the stride constant at 0x0044AA62, the field writes that follow
 * it, and two static buffer pairs 0x4000 and 0x8000 bytes apart.
 *
 * sx/sy are SCREEN SPACE and rhw is 0.01/z - NOT 1/z - both computed by the
 * engine on the CPU (0x0044AA96: fld1 / fdiv / fmul [0x684560]). The published
 * transcription stopped before the fmul;. The constant
 * does not matter to the vertex shader, because the constant cancels in the
 * division that recovers the texture coordinate, and it does matter to anyone
 * diffing vertex buffers against the original.
 *
 * WHAT rhw IS FOR, precisely, and what it is NOT for. It perspective-corrects
 * the TEXTURE COORDINATES. It does NOT decide the depth: OpenGL interpolates
 * window depth LINEARLY IN SCREEN SPACE whatever the clip w is, so a backend
 * that emits w = 1/rhw and one that emits w = 1 produce the SAME lit pixels -
 * measured both ways over two whole levels, with and without the depth test.
 * published the opposite for an afternoon;
 * is the retraction.
 *
 * What the clip w DOES decide is whether the two vertex COLOURS are
 * perspective-corrected - a third of the frame's bytes - and which of those
 * Direct3D does for a pre-transformed vertex is NOT established.
 * owner. plat_gl.c keeps w = 1/rhw, so the hardware
 * does the divide at gl_Position precision; the hand-divided alternative pushes
 * rhw (about 3e-9 here) through a varying GLES2 guarantees only as mediump,
 * whose range starts at 2^-14.
 *
 * Do not "improve" this into a model-space vertex - the game has never produced
 * one. */

typedef struct {
    float    sx, sy, sz, rhw;
    uint32_t diffuse, specular;
    float    tu, tv;
} plat_vertex;

typedef enum {
    PLAT_PRIM_POINTS,            /* D3D 1  - 0x0041A0AB */
    PLAT_PRIM_LINE_STRIP,        /* D3D 3  - 0x0041A0C4 */
    PLAT_PRIM_TRIANGLES,         /* D3D 4 */
    PLAT_PRIM_TRIANGLE_STRIP,    /* D3D 5  - 0x0041A143 */
    PLAT_PRIM_TRIANGLE_FAN       /* D3D 6  - 0x0041A0E1, 0x0041A0FE */
} plat_prim;

/* The nine draw call sites of reduce to
 * these two. Indices are u16, as DrawIndexedPrimitive's lpwIndices already is. */
void plat_gfx_draw(plat_prim p, const plat_vertex *v, unsigned nv);

/* THE PORT'S: a base layer and an additive reflection in ONE draw.
 * The car's reflection was a second draw over each run - the map modulated
 * by `ecol` (alpha = the weight), unfogged, SRCALPHA / ONE - added onto
 * what the base wrote with BLEND off.  This adds it in the same fragment:
 * the same sum, rounded once.  Only for a base drawn with BLEND 0. */
typedef struct {
    plat_vertex v;           /* the base layer, as plat_gfx_draw takes it */
    float       eu, ev;      /* the reflection map's u,v                  */
    uint32_t    ecol;        /* D3DCOLOR; alpha = the weight, 0 = none    */
} plat_vertex_env;
/* plat_gfx_draw_env: declared after plat_tex, in section 5 */

/* how the draws' vertices are uploaded - 0 a buffer store per draw
 * (the default), 1 client-side arrays.  The same draws in the same order
 * either way: only the cost differs (plat_gl.c has the measure). */
void plat_gfx_set_stream(int mode);
void plat_gfx_draw_indexed(plat_prim p, const plat_vertex *v, unsigned nv,
                           const uint16_t *idx, unsigned ni);

/* THE PORT'S: a listener on the geometry the draws receive, for the
 * sun's polygon test (sun.h; the original's 0x0044E0F3 tests the polygons it
 * is about to draw).  Called for every draw, with the vertices as given:
 * `stride` is the byte step between them (sizeof(plat_vertex), or the env
 * vertex's), `idx` / `ni` the indices of plat_gfx_draw_indexed (NULL, 0 for
 * the others).  NULL turns it off, which is the default; it sees what is
 * submitted, before any culling or clipping, and draws nothing itself. */
typedef void (*plat_gfx_tap_fn)(plat_prim p, const plat_vertex *v, size_t stride,
                                unsigned nv, const uint16_t *idx, unsigned ni);
void plat_gfx_set_tap(plat_gfx_tap_fn fn);

/* ---- 4. render state --------------------------------------------------- */
/* One funnel, matching 0x00419EBD - shadow cache included, because the
 * original already filters redundant state changes against a 160-entry shadow
 * at 0x00564B80 and a GLES2 backend wants exactly that.
 *
 * COMPLETE as of, and it was not before. This enum used to
 * carry sixteen members with a comment saying so; the set below is the whole
 * of what the image writes, measured image-wide:
 *
 *     python tools/rsstates.py census
 *     -> 259 write sites, 22 distinct state ids
 *
 * 22, not 16, and not the 13 of the scene-begin block either. The reason the
 * earlier count was low is worth keeping: the published figure was "38 call
 * sites", taken as callers of the funnel, and the funnel has a two-instruction
 * THUNK at 0x0044CBB6 carrying 222 more. See.
 *
 * The D3D id in each comment is a measurement. The D3DRENDERSTATE_* name it
 * implies is an interpretation -. */

typedef enum {
    PLAT_RS_TEXADDRESS,      /* D3D  3  1 = wrap, 3 = clamp; both are used   */
    PLAT_RS_TEXPERSPECTIVE,  /* D3D  4  always 1. GLES2 has no other mode    */
    PLAT_RS_ZTEST,           /* D3D  7  ZENABLE                              */
    PLAT_RS_SHADEMODE,       /* D3D  9  always 2 = gouraud                   */
    PLAT_RS_ZWRITE,          /* D3D 14                                       */
    PLAT_RS_ALPHATEST,       /* D3D 15                                       */
    PLAT_RS_TEXMAG,          /* D3D 17  1 = nearest, 2 = linear              */
    PLAT_RS_TEXMIN,          /* D3D 18  1, 2, and 6 (a mipmap mode)          */
    PLAT_RS_SRCBLEND,        /* D3D 19  plat_blend below                     */
    PLAT_RS_DSTBLEND,        /* D3D 20                                       */
    PLAT_RS_TEXBLEND,        /* D3D 21  2 = modulate, 4 = modulate-alpha     */
    PLAT_RS_ZFUNC,           /* D3D 23  plat_cmp below                       */
    PLAT_RS_ALPHAREF,        /* D3D 24  0..255                               */
    PLAT_RS_ALPHAFUNC,       /* D3D 25  plat_cmp below                       */
    PLAT_RS_DITHER,          /* D3D 26  one site, value from a global        */
    PLAT_RS_BLEND,           /* D3D 27  ALPHABLENDENABLE                     */
    PLAT_RS_FOGENABLE,       /* D3D 28                                       */
    PLAT_RS_SUBPIXEL,        /* D3D 31  accepted and dropped: GL is always   */
                             /*         subpixel-correct                     */
    PLAT_RS_FOGCOLOR,        /* D3D 34  0xAARRGGBB, alpha forced opaque      */
    PLAT_RS_FOGMODE,         /* D3D 35  always 0 = none, i.e. VERTEX fog     */
    PLAT_RS_COLORKEY,        /* D3D 41  see the note below                   */
    PLAT_RS_RANGEFOG,        /* D3D 49  one site, value 1                    */
    PLAT_RS__COUNT
} plat_rs;

/* TWO STATES THE GAME NEVER WRITES, and the port must therefore supply the
 * Direct3D default rather than leave them to the GL default, because the two
 * defaults differ:
 *
 *   CULLMODE  (D3D 22)  zero write sites image-wide. D3D's default is
 *                       D3DCULL_CCW; GL's default is cull DISABLED.
 *   FILLMODE  (D3D  8)  zero write sites. D3D's default is SOLID, as is GL's.
 *
 * A backend that simply does not mention culling draws every back face.
 *
 * THE WINDING IS MEASURED ON BOTH PATHS AS OF 2026-09-14, and they agree.
 *
 *   STAGE -. `bash tools/stagewind.sh` draws each of the 48
 *   shipped levels three times and counts lit pixels: 47 of the 48 are
 *   decisive for CW and NONE for CCW, CW retaining a median 99.4% of the
 *   unculled frame against CCW's 14.8%. (Re-measured 2026-09-14 after the
 *   clip; carries the earlier figures.)
 *   section 9.
 *
 *   MODEL -. `bash tools/modelwind.sh` runs the .ro3 emitter over all
 *   536 models and crosses the screen-space signed area of each polygon's
 *   first fan triangle against the engine's OWN backface test (FUN_0045053C
 *   rejects when dot(vertex - eye, normal) > 0). Over eight yaws per model:
 *   375,472 front faces CW against 261 CCW - 99.93% - and 71 of the 72 models
 *   whose pixel count can decide say CW.
 *   section 5.
 *
 * Front faces are clockwise as seen, which is exactly what D3DCULL_CCW keeps -
 * so the game never writes CULLMODE because Direct3D's default was already
 * right for its winding, and a port must supply what D3D supplied.
 *
 * SO THE BACKENDS NOW ENABLE CULLING - glCullFace(GL_BACK),
 * glFrontFace(GL_CW), with the shim's y-flip - where they used to disable it.
 * The old default was NOT an omission: it was "wrong in the same direction for
 * every polygon is visible and fixable, wrong for half of them is not", held
 * while only one of the two paths had been measured. Both are measured now.
 * plat_gfx_cull_mode() in plat_gl.c is still how a caller overrides it, and is
 * how the two probes drive the comparison above. */

/* Comparison values for PLAT_RS_ZFUNC / PLAT_RS_ALPHAFUNC. The original sets
 * ZFUNC = LESSEQUAL and (ALPHAFUNC = GREATER, ALPHAREF = 0). */
typedef enum {
    PLAT_CMP_NEVER = 1, PLAT_CMP_LESS, PLAT_CMP_EQUAL, PLAT_CMP_LEQUAL,
    PLAT_CMP_GREATER,   PLAT_CMP_NOTEQUAL, PLAT_CMP_GEQUAL, PLAT_CMP_ALWAYS
} plat_cmp;

/* Blend factors for PLAT_RS_SRCBLEND / PLAT_RS_DSTBLEND, D3DBLEND numbering.
 * Only five of the thirteen are ever written: SRCALPHA(5)/INVSRCALPHA(6) is
 * the common pair, with ONE(2)/ONE(2) and SRCCOLOR(3)... see
 * for the measured pairs and their counts. */
typedef enum {
    PLAT_BLEND_ZERO = 1,    PLAT_BLEND_ONE,          PLAT_BLEND_SRCCOLOR,
    PLAT_BLEND_INVSRCCOLOR, PLAT_BLEND_SRCALPHA,     PLAT_BLEND_INVSRCALPHA,
    PLAT_BLEND_DESTALPHA,   PLAT_BLEND_INVDESTALPHA, PLAT_BLEND_DESTCOLOR,
    PLAT_BLEND_INVDESTCOLOR, PLAT_BLEND_SRCALPHASAT
} plat_blend;

/* PLAT_RS_COLORKEY is accepted and DROPPED, and that is a measurement rather
 * than a shortcut. Retail writes it at 14 sites; patch 5.30 writes it at NONE,
 * and its blocks are otherwise instruction-for-instruction retail's with that
 * one write deleted. The shipped patch therefore proves the game renders
 * correctly with colour-keying off, which is the only option GLES2 has - it
 * has no colour key, and 1-bit transparency goes through the alpha test the
 * scene bracket already enables. */

void plat_gfx_set_state(plat_rs state, uint32_t value);
void plat_gfx_reset_state(void);     /* was 0x00419EA1 */

/* ---- 5. textures ------------------------------------------------------- */
/* One format, RGBA8: the original
 * negotiates four candidates ("05x5", "1555", "4444", "8888" - its own labels)
 * and rejects palettised and 24 bpp outright. GLES2 has RGBA8 unconditionally,
 * so the negotiation, the three format roles and the four candidate slots all
 * go. 0 is "no texture" - the original's sentinel was -957.
 *
 * BYTE ORDER, because "rgba8" does not say it and getting it wrong swaps red
 * and blue in a way that looks deliberate: the pixel array is w*h texels of
 * four bytes each, in memory order R, G, B, A. That is GL_RGBA/GL_UNSIGNED_BYTE
 * exactly, so the backend uploads it without a swizzle. On a little-endian
 * host the uint32 therefore reads 0xAABBGGRR, which is NOT the D3DCOLOR order
 * the vertex's diffuse and specular fields use (0xAARRGGBB). The two are
 * different on purpose: the vertex fields are transcribed from the original,
 * the texture buffer is the port's own choice.
 *
 * THE CALLER SUPPLIES THE WHOLE MIP CHAIN, and that is not a convenience - it
 * is forced, and this entry point was WRONG about it until.  It
 * used to take one level plus a COUNT and call glGenerateMipmap.  The
 * original's reducer (FUN_004051a6,
 * section 7) skips any 2x2 tap whose alpha is zero and averages colour over
 * the CONTRIBUTING taps only; glGenerateMipmap averages all four, so it drags
 * the colour-keyed black into every edge and darkens every keyed plate as it
 * minifies.  A backend cannot reproduce that from one level, so it does not
 * get to try.
 *
 * `levels[i]` is the image at (w >> i) x (h >> i), each at least 1, and
 * `nlevels` is how many there are.  nlevels == 1 is a plain texture with no
 * chain, which is what most callers want. */

typedef uint32_t plat_tex;

plat_tex plat_gfx_tex_create(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels);
void     plat_gfx_tex_destroy(plat_tex t);
void     plat_gfx_tex_bind(plat_tex t);    /* was 0x0041A052, stage 0 only */
/* THE PORT'S: marks `t` (created with `nlevels` levels) for the
 * remaster's filtering - trilinear where the game asks for a linear minify,
 * and anisotropic where the GL offers it.  For high-fidelity override art;
 * the shipped plates keep the original's filter.  A chain that does not
 * reach 1x1 is left as it was. */
void     plat_gfx_tex_smooth(plat_tex t, unsigned nlevels);
/* plat_vertex_env's draw (section 4) - the base on the bound
 * texture, the reflection from `env` */
void     plat_gfx_draw_env(plat_prim p, const plat_vertex_env *v, unsigned nv,
                           plat_tex env);

/* Replaces LEVEL 0 of an existing texture in place, same dimensions, same
 * pixel order as plat_gfx_tex_create.  Added, 2026-09-14, and it is
 * a transcription rather than a convenience: 0x00419B8A is a SECOND uploader
 * in the original - level 0 only, no grade pass, no reduce, dimensions taken
 * from the descriptor rather than from the source - which is what a surface
 * that changes every frame needs.
 *
 * port/gfx2d is its first caller.  The 2D layer is one CPU canvas uploaded
 * once a frame, and a create/destroy pair per frame is not an option: the
 * GL backend's handle table only ever counts UP, so a frame loop that
 * recreated its texture would exhaust PLAT_GL_MAX_TEX and then silently stop
 * drawing.  A no-op on a handle that does not exist. */
void     plat_gfx_tex_update(plat_tex t, const uint32_t *level0);

/* ---- 6. audio ---------------------------------------------------------- */
/* Twelve operations, one per DirectSound buffer method the game calls.
 * The original's voice table is 512 slots of
 * 112 bytes at 0x004F330C (allocator 0x0040AF00); PLAT_VOICE_MAX keeps the
 * bound so the port cannot silently outgrow behaviour the original clamped.
 *
 * REAL in the SDL2 backends, on miniaudio mixing into
 * SDL2's device (plat_audio_ma.c).  The UNITS ARE DIRECTSOUND'S, because the
 * game logic that calls these is ported from code that computed them for
 * DirectSound:
 *   set_volume   hundredths of a dB, -10000..0 (gain = 10^(mb/2000))
 *   set_pan      hundredths of a dB, -10000..10000: the FAR channel is cut
 *                by |pan|, the near one is untouched (DirectSound's law)
 *   set_freq     the playback rate in Hz, 100..200000; 0 = the sample's own
 *   get_pos/set_pos  a BYTE offset into the sample's PCM, as
 *                GetCurrentPosition / SetCurrentPosition
 * A voice's settings are kept across bind, so they may be set before it.
 * Playing a voice that ran to its end starts it from the top; stop is a
 * pause.  sample_load COPIES the PCM: the caller may free its buffer. */

#define PLAT_VOICE_MAX 512

typedef uint32_t plat_voice;       /* PLAT_VOICE_NONE = no slot */
typedef uint32_t plat_sample;
#define PLAT_VOICE_NONE  ((plat_voice)0xFFFFFFFFu)
#define PLAT_SAMPLE_NONE ((plat_sample)0xFFFFFFFFu)

plat_sample plat_audio_sample_load(const void *pcm, unsigned bytes,
                                   unsigned rate, unsigned channels,
                                   unsigned bits);
void        plat_audio_sample_free(plat_sample s);

plat_voice plat_audio_voice_alloc(void);          /* was 0x0040AF00 */
void       plat_audio_voice_free(plat_voice v);
void       plat_audio_voice_bind(plat_voice v, plat_sample s);
void       plat_audio_voice_play(plat_voice v, int looping);  /* 0x0040B7C9 */
void       plat_audio_voice_stop(plat_voice v);               /* 0x0040B80A */
void       plat_audio_voice_set_volume(plat_voice v, int millibels); /* 0x0040B844 */
void       plat_audio_voice_set_pan(plat_voice v, int pan);          /* 0x0040B86D */
void       plat_audio_voice_set_freq(plat_voice v, unsigned hz);     /* 0x0040B821 */
void       plat_audio_voice_set_pos3d(plat_voice v, float x, float y, float z);
int        plat_audio_voice_playing(plat_voice v);                   /* 0x0040B78E */
unsigned   plat_audio_voice_get_pos(plat_voice v);  /* the original's only
    GetCurrentPosition is inside its STREAM refill, 0x0040C1F0, not a voice
    shim: no voice caller exists yet */
void       plat_audio_voice_set_pos(plat_voice v, unsigned off);     /* 0x0040B7B0 */

/* 3D.  NOT a transcription: the original's 3D path ([ESI+0x0C], 0x00501A50)
 * is unread (item 3), and the port's 3D is the
 * remaster's own, miniaudio's spatialiser.  A voice is 2D until
 * set_pos3d is first called on it.  The coordinate frame is the CALLER'S:
 * the listener's fwd and up define it, and owns checking that a sound
 * on the car's left is heard on the left. */
void plat_audio_listener(const float pos[3], const float fwd[3],
                         const float up[3]);

/* STREAMS: a host .wav (a path vfs_resolve produced) decoded a page at a
 * time, never loaded whole: TWO, as the
 * original's two streaming records - the
 * music 0x00501950, in the MUSIC group, and the narration 0x00501920, in the
 * SPEECH group - and they play at once.  A play replaces what that stream
 * was playing.  play: 0 = started, -1 = not.  set_volume is millibels, as
 * the voice's (the original's SetVolume from 0x00501B0C); kept across plays,
 * so it may come first.  playing: 0 once a non-looping stream has played
 * out, been stopped, or never started - the original's "+0" flag, which its
 * service tick (op 1) polls to start the next track.
 *
 * The ring, the refill and the silence pad at the end of a file
 * (0x0040C1F0) are the backend's business and are NOT reproduced: miniaudio
 * streams the file itself. */
typedef enum {
    PLAT_STREAM_MUSIC, PLAT_STREAM_SPEECH, PLAT_STREAM__COUNT
} plat_stream;

int  plat_audio_stream_play(plat_stream s, const char *path, int looping);
void plat_audio_stream_stop(plat_stream s);
void plat_audio_stream_set_volume(plat_stream s, int millibels);
int  plat_audio_stream_playing(plat_stream s);

/* The port's, not the original's (U1, 2026-10-19): with plat_config's
 * audio_clock set, no device is opened and the mixer renders `ms` of output
 * here and nowhere else, so a stream plays out on the CALLER's clock.  A
 * fake-clock run otherwise asked the host's device whether track 8 had
 * ended: a sink that drained faster than real time ended it inside a
 * two-second race (rally-sound red, host state, no commit).  A no-op
 * without audio_clock. */
void plat_audio_advance(unsigned ms);

/* the names for the MUSIC stream, kept: play(PLAT_STREAM_MUSIC, ...). */
int  plat_audio_music_play(const char *path, int looping);
void plat_audio_music_stop(void);

/* Volume groups - an ADDITION, not one of the original's operations:
 * what the AUDIO SETUP screen and a handheld's volume keys drive.
 * Gains are linear, 0..1 (values above 1 amplify).  Every voice starts in
 * EFFECTS; the music stream is always in MUSIC, the narration stream in
 * SPEECH.  MASTER scales everything. */
typedef enum {
    PLAT_AUDIO_MASTER, PLAT_AUDIO_EFFECTS, PLAT_AUDIO_SPEECH, PLAT_AUDIO_MUSIC,
    PLAT_AUDIO__GROUPS
} plat_audio_group;

void plat_audio_group_volume(plat_audio_group g, float gain);
void plat_audio_voice_set_group(plat_voice v, plat_audio_group g);

/* ---- 7. input ---------------------------------------------------------- */
/* The game's own model: one signed steer request on a +/-256-shaped axis
 * (0x005C65C8, 'inputst'), plus buttons. sections 2-3.
 *
 * The RAMP STAYS IN GAME CODE. 0x004383C3's 0xA00000-per-tick integration and
 * its ADD EAX,EAX re-centring doubling are gameplay feel, not platform
 * behaviour, and they run on the 10 ms simulation step (called from the tick
 * body at 0x0045E525). ~~The shim's job is only to say whether the source is
 * analogue, so the game can pick its proportional path.~~: the shim
 * reports the pad (plat_pad_read); the original's own FUN_00408D37 picks
 * the path, per binding, in game code. */

void plat_input_poll(void);

/* Non-zero once the user has asked the window to close (the title-bar close
 * button, Alt-F4, the OS asking the app to quit), as of the last
 * plat_input_poll.  Added, the first program with a main loop.
 *
 * It is a REQUEST, reported, not acted on: the caller decides what closing
 * means.  The original receives the same request as WM_DESTROY in its window
 * procedure, plat_wndproc_q, which runs startup steps 38-40 a second time
 *so the request is the game's to honour, and a
 * backend that quit on its own would hide that path.  plat_null never sees
 * one and returns 0. */
int  plat_quit_requested(void);

int  plat_input_key(int scancode);              /* menus, debug overlay */

/* THE PAD.  One game controller in the standard (Xbox) layout - what
 * SDL's GameController API presents for a Bluetooth pad on Windows and for
 * a handheld's built-in pad - read RAW: no dead zone, no saturation, no
 * binding.  What the game does with it is game code (car_input.c: the
 * original's FUN_00408D37 over a DirectInput-shaped device state, with
 *'s dead zone applied there).  Hot-plug: the
 * first controller attached is used, and a removed one is replaced by the
 * next.
 *
 * ~~plat_input_axis / plat_input_axis_is_analogue / plat_input_action~~ -
 * RETIRED: the action list exists (section 14.2) and the game polls
 * its bindings itself, so the shim reports the device and nothing more. */
enum {
    PLAT_PAD_A = 1u << 0,  PLAT_PAD_B = 1u << 1,
    PLAT_PAD_X = 1u << 2,  PLAT_PAD_Y = 1u << 3,
    PLAT_PAD_BACK = 1u << 4, PLAT_PAD_GUIDE = 1u << 5, PLAT_PAD_START = 1u << 6,
    PLAT_PAD_LSTICK = 1u << 7, PLAT_PAD_RSTICK = 1u << 8,
    PLAT_PAD_LB = 1u << 9, PLAT_PAD_RB = 1u << 10,
    PLAT_PAD_UP = 1u << 11, PLAT_PAD_DOWN = 1u << 12,
    PLAT_PAD_LEFT = 1u << 13, PLAT_PAD_RIGHT = 1u << 14
};
typedef struct plat_pad {
    int      present;           /* 0: nothing attached, the rest is zero */
    int      lx, ly, rx, ry;    /* -32768 .. 32767, +y DOWN (SDL's sense) */
    int      lt, rt;            /* 0 .. 32767 */
    unsigned buttons;           /* PLAT_PAD_* */
} plat_pad;

/* The pad as of the last plat_input_poll.  Returns p->present. */
int  plat_pad_read(plat_pad *p);
/* ---- 8. file ----------------------------------------------------------- */
/* Thin on purpose: the game does its own buffering, and every asset path
 * already goes through vfs_resolve_path (0x00402FEB), which stays ABOVE this
 * layer - the ;3 / ;4 volume prefixes and the % language expansion are game
 * grammar, not platform grammar. */

typedef struct plat_file plat_file;
typedef struct plat_dir  plat_dir;

plat_file *plat_file_open(const char *path, int for_write);
void       plat_file_close(plat_file *f);
int64_t    plat_file_size(plat_file *f);
size_t     plat_file_read(plat_file *f, void *buf, size_t n);
size_t     plat_file_write(plat_file *f, const void *buf, size_t n);
int64_t    plat_file_seek(plat_file *f, int64_t off, int whence);
int        plat_file_exists(const char *path);
int        plat_dir_create(const char *path);
plat_dir  *plat_dir_open(const char *path);
const char *plat_dir_next(plat_dir *d);
void       plat_dir_close(plat_dir *d);

/* the directory the running executable is in, with a trailing path
 * separator, into buf.  Returns 0, or -1 (buf "") when the backend cannot
 * tell - plat_null cannot.  So a double-clicked rally finds the game it was
 * copied beside without being told. */
int        plat_exe_dir(char *buf, size_t n);

/* ---- 9. time ----------------------------------------------------------- */
/* Better than GetTickCount, because the simulation's quantum is 10 ms and
 * proves the accumulator is real. */

uint64_t plat_time_us(void);
void     plat_sleep_ms(unsigned ms);

/* ---- 10. memory -------------------------------------------------------- */
/* Replaces VirtualAlloc / VirtualFree. The game manages its own heaps above
 * this - imageheapsize, wavheapsize and the rest. */

void *plat_mem_reserve(size_t bytes);
void  plat_mem_release(void *p, size_t bytes);

/* ---- 11. threads and sync --------------------------------------------- */
/* The original runs INPUTTDB, AUDIOTDB and two more.
 * Whether the port keeps them is the and the question, not this
 * header's; the surface is here either way. */

typedef struct plat_thread plat_thread;
typedef struct plat_mutex  plat_mutex;
typedef struct plat_event  plat_event;

plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name);
void         plat_thread_join(plat_thread *t);
plat_mutex  *plat_mutex_create(void);
void         plat_mutex_lock(plat_mutex *m);
void         plat_mutex_unlock(plat_mutex *m);
void         plat_mutex_destroy(plat_mutex *m);
plat_event  *plat_event_create(void);
void         plat_event_set(plat_event *e);
int          plat_event_wait(plat_event *e, int timeout_ms);
void         plat_event_destroy(plat_event *e);

/* ---- 12. settings ------------------------------------------------------ */
/* Replaces the seven ADVAPI32 Reg* imports. Same key/value shape, a file
 * behind it: the installer-vs-game choice comes from
 * the REGISTRY, not the command line, so this is load-bearing at boot. */

int plat_setting_get(const char *key, char *buf, size_t n);
int plat_setting_set(const char *key, const char *value);

/* ---- 13. logging ------------------------------------------------------- */
/* Deliberately preserved, tag format verbatim, so an original log and a ported
 * log diff directly. That is a free correctness check alongside, and
 * is the record of how much this logging has already
 * been worth. */

void plat_log(const char *fmt, ...);
void plat_log_raw(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* PLAT_H */
