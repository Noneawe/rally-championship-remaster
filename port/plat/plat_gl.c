/* plat_gl.c - the renderer.
 *
 * Sections 3, 4 and 5 of plat.h - geometry, render state, textures - plus the
 * scene bracket. It is compiled into BOTH SDL2 backends; the desktop-GL and
 * GLES2 builds differ only in the context profile plat_sdl2.c asks for, and
 * nothing in this file branches on which one it got.
 *
 * WHY THERE IS NO GL HEADER HERE
 * ------------------------------
 * Every entry point is resolved through SDL_GL_GetProcAddress and every type
 * and enum is declared below. That is not minimalism for its own sake: it
 * means the port links SDL2 and nothing else, on the dev host and on the
 * aarch64 cross build alike, and that `cmake --build port/build --target
 * check` keeps working on a machine with no GL development package at all.
 * It also keeps the gates honest - adding -lGLESv2 would make the
 * cross build depend on a sysroot detail that nobody has measured.
 *
 * WHAT THE ORIGINAL HANDS US, AND WHY THIS IS SHORT
 * -------------------------------------------------
 * The engine transforms and lights on the
 * CPU and submits screen-space D3DTLVERTEX arrays - nine call sites in the
 * whole 862,180-byte .text. So there is no matrix stack, no lighting, no
 * clipping and no T&L to reimplement. The vertex shader is a pass-through and
 * the fragment shader is one texture fetch, one modulate and an alpha test.
 *
 * THE VERTEX, AND THE ONE THING THE PUBLISHED TRANSCRIPTION GOT WRONG
 * -------------------------------------------------------------------
 * 32 bytes: sx sy sz rhw (floats), diffuse, specular (D3DCOLOR dwords,
 * 0xAARRGGBB), tu tv. sx/sy are pixels, already offset by the screen origin.
 *
 * quotes the producer as
 *
 *     0044aa96  fld1
 *     0044aa98  fdiv dword ptr [edi + 8]      ; rhw = 1/z  @ +0x0c
 *
 * and stops there. The next instruction is
 *
 *     0044aa9b  fmul dword ptr [0x684560]     ; * 0.01f
 *     0044aaa1  fstp dword ptr [edi + 0xc]
 *
 * so rhw is 0.01/z, not 1/z.
 *
 * AND `z` THERE IS NOT VIEW z -. `[edi + 8]` is sz, stored one
 * instruction earlier by `fld [ebp+0x18] / fmul [0x005d7060] / fstp [edi+8]`,
 * so it is view z ALREADY SCALED by the depth scale FUN_0044c516 builds.
 * stopped one instruction short in one direction and this comment stopped one
 * instruction short in the other.
 * The SSE branch of the same function divides view z directly and the two do
 * NOT agree. None of it changes this file, for the reason below.
 *
 * It does NOT change this
 * file, and the reason is worth writing down rather than rediscovering: a
 * uniform positive scale on w cancels in the perspective divide and in
 * perspective-correct attribute interpolation, and this shader takes depth
 * from sz rather than from w. 0x00684560 has one absolute reference
 * image-wide - the fmul itself - so the factor is a compile-time constant and
 * cannot vary per draw. If it ever did, this comment is where to start.
 *
 * RENDER STATE
 * ------------
 * The full set is 22 states over 259 write sites, `python tools/rsstates.py
 * census`, and maps every one of them to what this
 * file does with it. The shadow-cache filter is transcribed, not invented -
 * the original filters redundant writes at 0x00419EBD and a GLES2 backend
 * wants exactly that.
 *
 * HEADLESS
 * --------
 * Same contract as the rest of the shim: with no GL context every entry point
 * returns normally and counts. plat_video_ok() is how a caller tells the two
 * apart. The counters are what let a bring-up run say "4,812 vertices were
 * submitted and none of them drew".
*/

#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "plat_glthread.h"
#include "plat_touch.h"

/* plat_sdl2.c: 1 when the touch overlay is on; fills the rest if not NULL */
int plat_touch_view(int *race, unsigned *lit, float *steer);   /* the renames: this file is the GL thread's side */
#include "plat.h"

#include <SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void plat_gl_swap_join(void);   /* plat_sdl2.c: wait for the swap thread */
#include <string.h>

/* ---- 0. the GL subset, declared rather than included -------------------- */

#if defined(_WIN32)
#  define PLAT_GLAPI __stdcall
#else
#  define PLAT_GLAPI
#endif

typedef unsigned int   plgl_enum;
typedef unsigned int   plgl_uint;
typedef int            plgl_int;
typedef int            plgl_sizei;
typedef unsigned char  plgl_boolean;
typedef float          plgl_float;
typedef unsigned int   plgl_bitfield;
typedef char           plgl_char;
typedef ptrdiff_t      plgl_sizeiptr;

#define GL_FALSE                 0
#define GL_TRUE                  1
#define GL_POINTS                0x0000
#define GL_LINE_STRIP            0x0003
#define GL_TRIANGLES             0x0004
#define GL_TRIANGLE_STRIP        0x0005
#define GL_TRIANGLE_FAN          0x0006
#define GL_NEVER                 0x0200
#define GL_LESS                  0x0201
#define GL_EQUAL                 0x0202
#define GL_LEQUAL                0x0203
#define GL_GREATER               0x0204
#define GL_NOTEQUAL              0x0205
#define GL_GEQUAL                0x0206
#define GL_ALWAYS                0x0207
#define GL_ZERO                  0
#define GL_ONE                   1
#define GL_SRC_COLOR             0x0300
#define GL_ONE_MINUS_SRC_COLOR   0x0301
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_DST_ALPHA             0x0304
#define GL_ONE_MINUS_DST_ALPHA   0x0305
#define GL_DST_COLOR             0x0306
#define GL_ONE_MINUS_DST_COLOR   0x0307
#define GL_SRC_ALPHA_SATURATE    0x0308
#define GL_CULL_FACE             0x0B44
#define GL_SCISSOR_TEST          0x0C11
#define GL_DEPTH_TEST            0x0B71
#define GL_BLEND                 0x0BE2
#define GL_DITHER                0x0BD0
#define GL_TEXTURE_2D            0x0DE1
#define GL_UNSIGNED_BYTE         0x1401
#define GL_UNSIGNED_SHORT        0x1403
#define GL_FLOAT                 0x1406
#define GL_RGBA                  0x1908
#define GL_BGRA_EXT              0x80E1   /* GL_EXT_texture_format_BGRA8888 */
#define GL_RGB                   0x1907
#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_NEAREST               0x2600
#define GL_LINEAR                0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR  0x2703
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_REPEAT                0x2901
#define GL_CLAMP_TO_EDGE         0x812F
#define GL_TEXTURE0              0x84C0
#define GL_TEXTURE1              0x84C1
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_STREAM_DRAW           0x88E0
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_VERTEX_SHADER         0x8B31
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_UNPACK_ALIGNMENT      0x0CF5
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_FRONT                 0x0404
#define GL_BACK                  0x0405
#define GL_CW                    0x0900
#define GL_CCW                   0x0901
#define GL_DEPTH_BUFFER_BIT      0x00000100
#define GL_FRAMEBUFFER           0x8D40
#define GL_RENDERBUFFER          0x8D41
#define GL_COLOR_ATTACHMENT0     0x8CE0
#define GL_DEPTH_ATTACHMENT      0x8D00
#define GL_DEPTH_COMPONENT16     0x81A5
#define GL_DEPTH_COMPONENT24     0x81A6
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_STATIC_DRAW           0x88E4

/* the letterbox: the off-screen frame and the scaling draw.  Core in
 * GLES2 and GL 3.0, an extension's names before that - so resolved apart from
 * the rest, and their absence only means no scaling (the frame is drawn
 * straight to the window, at the game's size, as before). */
struct plgl_fbo {
    void      (PLAT_GLAPI *GenFramebuffers)(plgl_sizei, plgl_uint *);
    void      (PLAT_GLAPI *BindFramebuffer)(plgl_enum, plgl_uint);
    void      (PLAT_GLAPI *FramebufferTexture2D)(plgl_enum, plgl_enum, plgl_enum,
                                                 plgl_uint, plgl_int);
    plgl_enum (PLAT_GLAPI *CheckFramebufferStatus)(plgl_enum);
    void      (PLAT_GLAPI *GenRenderbuffers)(plgl_sizei, plgl_uint *);
    void      (PLAT_GLAPI *BindRenderbuffer)(plgl_enum, plgl_uint);
    void      (PLAT_GLAPI *RenderbufferStorage)(plgl_enum, plgl_enum,
                                                plgl_sizei, plgl_sizei);
    void      (PLAT_GLAPI *FramebufferRenderbuffer)(plgl_enum, plgl_enum,
                                                    plgl_enum, plgl_uint);
    plgl_boolean (PLAT_GLAPI *IsEnabled)(plgl_enum);
    void      (PLAT_GLAPI *DisableVertexAttribArray)(plgl_uint);
};

static struct plgl_fbo glf;

struct plgl {
    void        (PLAT_GLAPI *Enable)(plgl_enum);
    void        (PLAT_GLAPI *Disable)(plgl_enum);
    void        (PLAT_GLAPI *CullFace)(plgl_enum);
    void        (PLAT_GLAPI *FrontFace)(plgl_enum);
    void        (PLAT_GLAPI *DepthFunc)(plgl_enum);
    void        (PLAT_GLAPI *DepthMask)(plgl_boolean);
    void        (PLAT_GLAPI *BlendFunc)(plgl_enum, plgl_enum);
    void        (PLAT_GLAPI *Viewport)(plgl_int, plgl_int, plgl_sizei, plgl_sizei);
    void        (PLAT_GLAPI *Scissor)(plgl_int, plgl_int, plgl_sizei, plgl_sizei);
    void        (PLAT_GLAPI *ClearColor)(plgl_float, plgl_float, plgl_float,
                                         plgl_float);
    void        (PLAT_GLAPI *Clear)(plgl_bitfield);
    void        (PLAT_GLAPI *DrawArrays)(plgl_enum, plgl_int, plgl_sizei);
    void        (PLAT_GLAPI *DrawElements)(plgl_enum, plgl_sizei, plgl_enum,
                                           const void *);
    void        (PLAT_GLAPI *GenBuffers)(plgl_sizei, plgl_uint *);
    void        (PLAT_GLAPI *DeleteBuffers)(plgl_sizei, const plgl_uint *);
    void        (PLAT_GLAPI *BindBuffer)(plgl_enum, plgl_uint);
    void        (PLAT_GLAPI *BufferData)(plgl_enum, plgl_sizeiptr, const void *,
                                         plgl_enum);
    void        (PLAT_GLAPI *BufferSubData)(plgl_enum, plgl_sizeiptr,
                                            plgl_sizeiptr, const void *);
    void        (PLAT_GLAPI *GenTextures)(plgl_sizei, plgl_uint *);
    void        (PLAT_GLAPI *DeleteTextures)(plgl_sizei, const plgl_uint *);
    void        (PLAT_GLAPI *BindTexture)(plgl_enum, plgl_uint);
    void        (PLAT_GLAPI *ActiveTexture)(plgl_enum);
    void        (PLAT_GLAPI *TexImage2D)(plgl_enum, plgl_int, plgl_int,
                                         plgl_sizei, plgl_sizei, plgl_int,
                                         plgl_enum, plgl_enum, const void *);
    void        (PLAT_GLAPI *TexSubImage2D)(plgl_enum, plgl_int, plgl_int,
                                            plgl_int, plgl_sizei, plgl_sizei,
                                            plgl_enum, plgl_enum, const void *);
    void        (PLAT_GLAPI *TexParameteri)(plgl_enum, plgl_enum, plgl_int);
    void        (PLAT_GLAPI *GenerateMipmap)(plgl_enum);
    void        (PLAT_GLAPI *PixelStorei)(plgl_enum, plgl_int);
    plgl_uint   (PLAT_GLAPI *CreateShader)(plgl_enum);
    void        (PLAT_GLAPI *ShaderSource)(plgl_uint, plgl_sizei,
                                           const plgl_char *const *,
                                           const plgl_int *);
    void        (PLAT_GLAPI *CompileShader)(plgl_uint);
    void        (PLAT_GLAPI *GetShaderiv)(plgl_uint, plgl_enum, plgl_int *);
    void        (PLAT_GLAPI *GetShaderInfoLog)(plgl_uint, plgl_sizei,
                                               plgl_sizei *, plgl_char *);
    void        (PLAT_GLAPI *DeleteShader)(plgl_uint);
    plgl_uint   (PLAT_GLAPI *CreateProgram)(void);
    void        (PLAT_GLAPI *AttachShader)(plgl_uint, plgl_uint);
    void        (PLAT_GLAPI *BindAttribLocation)(plgl_uint, plgl_uint,
                                                 const plgl_char *);
    void        (PLAT_GLAPI *LinkProgram)(plgl_uint);
    void        (PLAT_GLAPI *GetProgramiv)(plgl_uint, plgl_enum, plgl_int *);
    void        (PLAT_GLAPI *GetProgramInfoLog)(plgl_uint, plgl_sizei,
                                                plgl_sizei *, plgl_char *);
    void        (PLAT_GLAPI *UseProgram)(plgl_uint);
    plgl_int    (PLAT_GLAPI *GetUniformLocation)(plgl_uint, const plgl_char *);
    void        (PLAT_GLAPI *Uniform1i)(plgl_int, plgl_int);
    void        (PLAT_GLAPI *Uniform1f)(plgl_int, plgl_float);
    void        (PLAT_GLAPI *Uniform2f)(plgl_int, plgl_float, plgl_float);
    void        (PLAT_GLAPI *Uniform4f)(plgl_int, plgl_float, plgl_float,
                                        plgl_float, plgl_float);
    void        (PLAT_GLAPI *EnableVertexAttribArray)(plgl_uint);
    void        (PLAT_GLAPI *DisableVertexAttribArray)(plgl_uint);
    void        (PLAT_GLAPI *VertexAttribPointer)(plgl_uint, plgl_int,
                                                  plgl_enum, plgl_boolean,
                                                  plgl_sizei, const void *);
    const unsigned char *(PLAT_GLAPI *GetString)(plgl_enum);
};

static struct plgl gl;

/* ---- 1. the shaders ---------------------------------------------------- */
/* One program. GLSL ES 1.00, which desktop GL accepts as GLSL 1.10 once the
 * precision qualifiers are guarded - that guard is the whole of the difference
 * between the two backends inside this file, and it is in the source string
 * rather than in a #if so that neither backend can compile a shader the other
 * has never seen. */

static const char *VS_SRC =
"#ifdef GL_ES\n"
"precision highp float;\n"
"#endif\n"
"attribute vec4 a_pos;       /* sx, sy, sz, rhw */\n"
"attribute vec4 a_diffuse;   /* D3DCOLOR, so B,G,R,A in memory */\n"
"attribute vec4 a_specular;\n"
"attribute vec2 a_uv;\n"
"attribute vec2 a_uv2;       /* the reflection's u,v (draw_env) */\n"
"attribute vec4 a_ecol;      /* its D3DCOLOR, alpha the weight */\n"
"uniform vec2 u_viewport;\n"
"varying vec4 v_diffuse;\n"
"varying vec4 v_specular;\n"
"varying vec2 v_uv;\n"
"varying vec2 v_uv2;\n"
"varying vec4 v_ecol;\n"
"void main()\n"
"{\n"
"    /* w up to an arbitrary positive scale: the engine's rhw carries a\n"
"       constant 0.01 factor, which cancels. See the file header.\n"
"\n"
"       THE ALTERNATIVE WAS TRIED AND REVERTED. Emitting\n"
"       w = 1 and dividing the UVs by rhw in the fragment shader produces the\n"
"       SAME lit pixels (identical coverage on LEVEL01 and LEVEL61, with and\n"
"       without the depth test), because GL interpolates window DEPTH linearly\n"
"       in screen space whatever the clip w is. It differs only in the SHADING,\n"
"       over 13-34% of the frame, and which of the two matches Direct3D's\n"
"       Gouraud interpolation of a pre-transformed vertex is not established.\n"
"       It also passes rhw - about 3e-9 here - through a varying that GLES2\n"
"       only guarantees as mediump, whose range starts at 2^-14. On a device\n"
"       where mediump is really 16-bit that underflows to zero and every\n"
"       texture coordinate collapses. This form lets the hardware do the\n"
"       divide at gl_Position precision instead. */\n"
"    float rhw = a_pos.w;\n"
"    float w = (rhw != 0.0) ? (1.0 / rhw) : 1.0;\n"
"    vec2 ndc = vec2(a_pos.x / u_viewport.x * 2.0 - 1.0,\n"
"                    1.0 - a_pos.y / u_viewport.y * 2.0);\n"
"    float ndcz = a_pos.z * 2.0 - 1.0;\n"
"    gl_Position = vec4(ndc * w, ndcz * w, w);\n"
"    v_diffuse  = a_diffuse.bgra;\n"
"    v_specular = a_specular.bgra;\n"
"    v_uv       = a_uv;\n"
"    v_uv2      = a_uv2;\n"
"    v_ecol     = a_ecol.bgra;\n"
"    gl_PointSize = 1.0;\n"
"}\n";

static const char *FS_SRC =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#endif\n"
"/* each state test is a constant in a specialised program (S_*), a uniform in\n"
"   the generic one: Mali does not fold uniform branches away */\n"
"#ifdef S_TEX\n#define IS_TEX (S_TEX != 0)\n#else\n#define IS_TEX (u_textured > 0.5)\n#endif\n"
"#ifdef S_B4\n#define IS_B4 (S_B4 != 0)\n#else\n#define IS_B4 (u_texblend > 3.0)\n#endif\n"
"#ifdef S_FOG\n#define IS_FOG (S_FOG != 0)\n#else\n#define IS_FOG (u_fogenable > 0.5)\n#endif\n"
"#ifdef S_ENV\n#define IS_ENV (S_ENV != 0)\n#else\n#define IS_ENV (u_env > 0.5)\n#endif\n"
"uniform sampler2D u_tex;\n"
"uniform float u_textured;    /* 0 = no texture bound */\n"
"uniform float u_texblend;    /* D3D TEXTUREMAPBLEND: 2 modulate, 4 mod-alpha */\n"
"uniform float u_alphafunc;   /* D3D comparison, 0 = alpha test off */\n"
"uniform float u_alpharef;    /* 0..1 */\n"
"uniform float u_fogenable;\n"
"uniform vec4  u_fogcolor;\n"
"uniform sampler2D u_tex2;    /* the reflection's map, unit 1 */\n"
"uniform float u_env;         /* 1 inside plat_gfx_draw_env */\n"
"varying vec4 v_diffuse;\n"
"varying vec4 v_specular;\n"
"varying vec2 v_uv;\n"
"varying vec2 v_uv2;\n"
"varying vec4 v_ecol;\n"
"void main()\n"
"{\n"
"    vec4 c = v_diffuse;\n"
"    if (IS_TEX) {\n"
"        vec4 t = texture2D(u_tex, v_uv);\n"
"        /* MODULATE (2) takes the texture's alpha and ignores the vertex's;\n"
"           MODULATEALPHA (4) multiplies them. Those are the only two values\n"
"           the image writes to D3D state 21. */\n"
"        c.rgb = t.rgb * v_diffuse.rgb;\n"
"        c.a   = IS_B4 ? (t.a * v_diffuse.a) : t.a;\n"
"    }\n"
"    /* Specular RGB is added after the texture blend, and specular ALPHA is\n"
"       the vertex fog factor - that is what FOGTABLEMODE = NONE means for a\n"
"       pre-transformed vertex. INTERPRETATION, not a measurement. */\n"
"    c.rgb += v_specular.rgb;\n"
"    if (IS_FOG) {\n"
"        c.rgb = mix(u_fogcolor.rgb, c.rgb, clamp(v_specular.a, 0.0, 1.0));\n"
"    }\n"
"#ifdef S_AF_GREATER\n"
"    if (c.a <= u_alpharef) { discard; }\n"
"#elif !defined(NO_DISCARD)   /* the variant for draws whose test cannot fail */\n"
"    if (u_alphafunc > 0.5) {\n"
"        float a = c.a, r = u_alpharef, f = u_alphafunc;\n"
"        bool pass =\n"
"            (f < 1.5) ? false :\n"
"            (f < 2.5) ? (a <  r) :\n"
"            (f < 3.5) ? (a == r) :\n"
"            (f < 4.5) ? (a <= r) :\n"
"            (f < 5.5) ? (a >  r) :\n"
"            (f < 6.5) ? (a != r) :\n"
"            (f < 7.5) ? (a >= r) : true;\n"
"        if (!pass) { discard; }\n"
"    }\n"
"#endif\n"
"    /* the reflection layer in the same pass.  It was a second draw\n"
"       over the first - MODULATEALPHA, unfogged, SRCALPHA / ONE added onto\n"
"       what the base wrote with its blending off - so it is that sum here,\n"
"       rounded to 8 bits once instead of twice (plat_gfx_draw_env). */\n"
"    if (IS_ENV) {\n"
"        vec4 e = texture2D(u_tex2, v_uv2);\n"
"        c.rgb += e.rgb * v_ecol.rgb * (e.a * v_ecol.a);\n"
"    }\n"
"    gl_FragColor = c;\n"
"}\n";

/* the letterbox's scaling draw - the off-screen frame as one textured
 * quad.  Not the game's program and none of the game's state: see
 * plat_gl_present_begin for what it touches and puts back. */
static const char *BLIT_VS_SRC =
"#ifdef GL_ES\n"
"precision highp float;\n"
"#endif\n"
"attribute vec2 a_bpos;\n"
"attribute vec2 a_buv;\n"
"varying vec2 v_buv;\n"
"void main()\n"
"{\n"
"    v_buv = a_buv;\n"
"    gl_Position = vec4(a_bpos, 0.0, 1.0);\n"
"}\n";

static const char *BLIT_FS_SRC =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#endif\n"
"uniform sampler2D u_src;\n"
"varying vec2 v_buv;\n"
"void main()\n"
"{\n"
"    gl_FragColor = texture2D(u_src, v_buv);\n"
"}\n";

/* ---- 2. state ---------------------------------------------------------- */

enum { ATTR_POS = 0, ATTR_DIFFUSE, ATTR_SPECULAR, ATTR_UV, ATTR_UV2, ATTR_ECOL };

static int       g_loaded;        /* GL entry points resolved */
static int       g_ready;         /* program linked, buffers created */
static int       g_failed;        /* tried and could not - do not retry */
static plgl_uint g_vbo, g_ibo;
static int       g_stream;                 /* plat_gfx_set_stream */

/* the game's program exists twice, from one source.  [0] is FS_SRC as
 * is; [1] is compiled with NO_DISCARD and has no alpha test at all - on a
 * Mali a shader that CONTAINS `discard` loses early depth test and forward
 * pixel kill for every draw it runs, whatever the uniform says.  select_prog
 * picks [1] only where the test cannot reject a fragment.  Uniforms live in
 * the program, so each one is shadowed here and the whole set is pushed when
 * the program changes; a write goes to the current program only. */
enum { U_VIEWPORT, U_TEXTURED, U_TEXBLEND, U_ALPHAFUNC, U_ALPHAREF,
       U_FOGENABLE, U_FOGCOLOR, U_ENV, U__COUNT };
static const char *const g_uname[U__COUNT] = {
    "u_viewport", "u_textured", "u_texblend", "u_alphafunc", "u_alpharef",
    "u_fogenable", "u_fogcolor", "u_env" };
static const int g_ulen[U__COUNT] = { 2, 1, 1, 1, 1, 1, 4, 1 };
#define NPROG 65                           /* [0] generic, [1+key] specialised */
static plgl_uint  g_progs[NPROG];
static plgl_int   g_uloc[NPROG][U__COUNT];
static char       g_pfail[NPROG];
static plgl_float g_uval[U__COUNT][4];
static int        g_cur;                   /* the game's program in use */
static int        g_no_fast;               /* RALLY_GL_NO_FAST: [0] only */
static unsigned long g_fast_draws;

/* The shadow cache, transcribed from 0x00419EBD. Identical in every backend on
 * purpose - if the copies ever diverge, believe plat_null.c, which is the one
 * runs against. */
static uint32_t g_rs[PLAT_RS__COUNT];
static int      g_rs_valid[PLAT_RS__COUNT];

static plat_tex g_bound_tex;
static unsigned long g_draws, g_verts, g_indices;

/* the share of the run's draws that took the program without the
 * alpha test, said once at exit.  stderr, not plat_log: the log file is
 * closed by plat_shutdown before atexit runs. */
static void report_fast_share(void)
{
    if (g_draws) {
        fprintf(stderr, "GFX: %lu of %lu draws (%.1f%%) on the program "
                "without the alpha test\n", g_fast_draws, g_draws,
                100.0 * (double)g_fast_draws / (double)g_draws);
    }
}

/* Texture handles. plat_tex 0 is "no texture", so slot 0 is never issued. */
#define PLAT_GL_MAX_TEX 4096
static plgl_uint g_tex_name[PLAT_GL_MAX_TEX];
/* Level 0's dimensions, kept so plat_gfx_tex_update can re-upload without
 * being told them again - the original's 0x00419B8A takes them from the
 * texture's own descriptor for the same reason. */
static unsigned  g_tex_w[PLAT_GL_MAX_TEX], g_tex_h[PLAT_GL_MAX_TEX];
/* THE PORT'S: textures marked by plat_gfx_tex_smooth, which take the
 * whole mip chain (trilinear) where the game asks for a linear minify.
 * g_aniso: the anisotropic ceiling, 0 where the extension is missing. */
static unsigned char g_tex_smooth[PLAT_GL_MAX_TEX];
/* no texel of the texture has alpha 0 - level 0 and the levels below
 * it kept apart, because plat_gfx_tex_update replaces only level 0.  With
 * MODULATE the fragment's alpha is the sampled one, so a texture that never
 * samples 0 cannot fail GREATER-than-0. */
static unsigned char g_tex_nz0[PLAT_GL_MAX_TEX], g_tex_nzm[PLAT_GL_MAX_TEX];
/* the sampler parameters last SENT to each texture object (0 =
 * unknown, so the first apply sends all of them, as before).  GL keeps them on
 * the object, so a bind that finds them already right sends nothing: on Mali a
 * TexParameteri can rebuild the texture descriptor.  Reset when the handle is
 * created or destroyed; plat_gfx_tex_smooth needs no hook, since the min
 * filter is compared as the value it resolves to. */
static plgl_enum g_tex_pwrap[PLAT_GL_MAX_TEX], g_tex_pmag[PLAT_GL_MAX_TEX],
                 g_tex_pmin[PLAT_GL_MAX_TEX];
/* what is bound on unit 1 (the env draws' reflection map), ~0u =
 * unknown; and whether the env arrays and u_env are currently ON (they stay on
 * across consecutive env draws and go off at the next draw of another kind). */
static plgl_uint g_u1_name = ~0u;
static int       g_env_on;
/* glCopyTexSubImage2D, resolved once in load_gl. */
static void (PLAT_GLAPI *g_copysub)(plgl_enum, plgl_int, plgl_int, plgl_int,
                                    plgl_int, plgl_int, plgl_sizei, plgl_sizei);
/* the four capabilities plat_gl_present_begin saves, tracked here as
 * they are set (index = g_caps' order: scissor, depth, blend, cull) instead
 * of asking GL with glIsEnabled.  Cull is on from bring-up, the rest are GL's
 * default, off. */
static int g_cap_on[4];
static void cap_set(plgl_enum cap, int on)
{
    switch (cap) {
    case GL_SCISSOR_TEST: g_cap_on[0] = on; break;
    case GL_DEPTH_TEST:   g_cap_on[1] = on; break;
    case GL_BLEND:        g_cap_on[2] = on; break;
    case GL_CULL_FACE:    g_cap_on[3] = on; break;
    default: break;
    }
    if (on) { gl.Enable(cap); } else { gl.Disable(cap); }
}
static float     g_aniso = -1.0f;
static void (PLAT_GLAPI *gl_TexParameterf)(plgl_enum, plgl_enum, float);
static plat_tex  g_next_tex = 1;

/* the letterbox.  g_fbo_ok: the entry points resolved and the
 * off-screen frame was built; g_fbo_on: this frame is being drawn into it
 * (the window is not the game's size).  Built once, at the game's size, the
 * first time the window differs - so a window that IS the game's size (every
 * gate, and the handheld at its panel's size) never builds it at all. */
static int       g_fbo_ok, g_fbo_tried, g_fbo_on;
static plgl_uint g_fbo, g_fbo_tex, g_fbo_depth, g_blit_prog, g_blit_vbo;
static plgl_int  u_blit_src;

/* plat_sdl2.c's: the window's drawable size, 0 x 0 without one. */
void plat_video_drawable_size(int *w, int *h);

/* ---- 3. bring-up ------------------------------------------------------- */

static int load_gl(void)
{
    int missing = 0;

#define GET(name)                                                             \
    do {                                                                      \
        *(void **)&gl.name = SDL_GL_GetProcAddress("gl" #name);               \
        if (!gl.name) { plat_log("GFX: missing gl" #name "\n"); missing++; }   \
    } while (0)

    GET(Enable);        GET(Disable);      GET(DepthFunc);   GET(DepthMask);
    GET(CullFace);      GET(FrontFace);
    GET(BlendFunc);     GET(Viewport);     GET(ClearColor);  GET(Clear);
    GET(Scissor);
    GET(DrawArrays);    GET(DrawElements); GET(GenBuffers);  GET(DeleteBuffers);
    GET(BindBuffer);    GET(BufferData);   GET(BufferSubData); GET(GenTextures);
    GET(DeleteTextures); GET(BindTexture); GET(ActiveTexture);
    GET(TexImage2D);    GET(TexSubImage2D); GET(TexParameteri);
    GET(GenerateMipmap);
    GET(PixelStorei);   GET(CreateShader); GET(ShaderSource);
    GET(CompileShader); GET(GetShaderiv);  GET(GetShaderInfoLog);
    GET(DeleteShader);  GET(CreateProgram); GET(AttachShader);
    GET(BindAttribLocation); GET(LinkProgram); GET(GetProgramiv);
    GET(GetProgramInfoLog);  GET(UseProgram);  GET(GetUniformLocation);
    GET(Uniform1i);     GET(Uniform1f);    GET(Uniform2f);   GET(Uniform4f);
    GET(EnableVertexAttribArray); GET(DisableVertexAttribArray);
    GET(VertexAttribPointer);
    GET(GetString);
#undef GET

    /* optional - its absence only makes plat_gfx_frame_copy fail. */
    *(void **)&g_copysub = SDL_GL_GetProcAddress("glCopyTexSubImage2D");

    return missing == 0;
}

static plgl_uint compile(plgl_enum kind, const char *src, const char *what)
{
    plgl_uint s = gl.CreateShader(kind);
    plgl_int  ok = GL_FALSE;

    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetShaderInfoLog(s, (plgl_sizei)sizeof log, NULL, log);
        /* One exact phrase, because the build gates on it: port/tests adds a
         * FAIL_REGULAR_EXPRESSION for "GFX: SHADER FAILED" so a shader that
         * stops compiling fails `check` rather than producing a black screen
         * nobody looks at until much later. */
        plat_log("GFX: SHADER FAILED (%s): %s\n", what, log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

/* One game program: the vertex shader and FS_SRC under `define` ("" or the
 * variant's).  Returns its name, 0 on failure, and leaves it in use
 * with its uniform locations in g_uloc[idx] and its two samplers set. */
static plgl_uint build_one(int idx, const char *define, const char *what)
{
    plgl_uint vs, fs, prog;
    plgl_int  ok = GL_FALSE;
    plgl_int  loc;
    char     *src;
    int       u;

    vs = compile(GL_VERTEX_SHADER, VS_SRC, "vertex");
    if (!vs) {
        return 0;
    }
    src = (char *)malloc(strlen(define) + strlen(FS_SRC) + 1);
    if (!src) {
        gl.DeleteShader(vs);
        return 0;
    }
    strcpy(src, define);
    strcat(src, FS_SRC);
    fs = compile(GL_FRAGMENT_SHADER, src, what);
    free(src);
    if (!fs) {
        gl.DeleteShader(vs);
        return 0;
    }

    prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    /* Bound before linking so the locations are ours, not the driver's. */
    gl.BindAttribLocation(prog, ATTR_POS,      "a_pos");
    gl.BindAttribLocation(prog, ATTR_DIFFUSE,  "a_diffuse");
    gl.BindAttribLocation(prog, ATTR_SPECULAR, "a_specular");
    gl.BindAttribLocation(prog, ATTR_UV,       "a_uv");
    gl.BindAttribLocation(prog, ATTR_UV2,      "a_uv2");
    gl.BindAttribLocation(prog, ATTR_ECOL,     "a_ecol");
    gl.LinkProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);

    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetProgramInfoLog(prog, (plgl_sizei)sizeof log, NULL, log);
        plat_log("GFX: SHADER FAILED (link, %s): %s\n", what, log);
        return 0;
    }

    gl.UseProgram(prog);
    for (u = 0; u < U__COUNT; u++) {
        g_uloc[idx][u] = gl.GetUniformLocation(prog, g_uname[u]);
    }
    loc = gl.GetUniformLocation(prog, "u_tex");
    gl.Uniform1i(loc, 0);
    loc = gl.GetUniformLocation(prog, "u_tex2");
    gl.Uniform1i(loc, 1);
    gl.Uniform1f(g_uloc[idx][U_ENV], 0.0f);
    return prog;
}

static int build_program(void)
{
    g_progs[0] = build_one(0, "", "fragment");
    if (!g_progs[0]) {
        return 0;
    }
    g_no_fast = getenv("RALLY_GL_NO_FAST") != NULL;
    g_cur     = 0;
    return 1;
}

/* Specialised program for `key` (bits: 1 no alpha test, 2 greater-only alpha
 * test, 4 textured, 8 modulate-alpha, 16 fog, 32 env), built on first use.
 * A build that fails marks the key and the generic program serves it. */
static int spec_prog(int key)
{
    char def[256];
    int  idx = 1 + key;
    uint64_t t0;

    if (g_progs[idx]) {
        return idx;
    }
    if (g_pfail[idx]) {
        return 0;
    }
    snprintf(def, sizeof def, "%s%s#define S_TEX %d\n#define S_B4 %d\n"
             "#define S_FOG %d\n#define S_ENV %d\n",
             (key & 1) ? "#define NO_DISCARD\n" : "",
             (key & 2) ? "#define S_AF_GREATER\n" : "",
             (key >> 2) & 1, (key >> 3) & 1, (key >> 4) & 1, (key >> 5) & 1);
    t0 = plat_time_us();
    g_progs[idx] = build_one(idx, def, "fragment, specialised");
    plat_log("GFX: program variant %d built in %u ms\n", key, (unsigned)((plat_time_us() - t0) / 1000u));
    if (!g_progs[idx]) {
        g_pfail[idx] = 1;
        gl.UseProgram(g_progs[g_cur]);
        return 0;
    }
    gl.UseProgram(g_progs[g_cur]);      /* build_one left it in use */
    return idx;
}

/* Write one shadowed uniform: kept for the next program switch, and sent to
 * the program in use now.  A location of -1 (the variant has no alpha test)
 * is ignored by GL. */
static void uni_send(int prog, int u)
{
    plgl_int          loc = g_uloc[prog][u];
    const plgl_float *v   = g_uval[u];

    switch (g_ulen[u]) {
    case 2:  gl.Uniform2f(loc, v[0], v[1]); break;
    case 4:  gl.Uniform4f(loc, v[0], v[1], v[2], v[3]); break;
    default: gl.Uniform1f(loc, v[0]); break;
    }
}

static void uni_set(int u, plgl_float a, plgl_float b, plgl_float c,
                    plgl_float d)
{
    g_uval[u][0] = a;
    g_uval[u][1] = b;
    g_uval[u][2] = c;
    g_uval[u][3] = d;
    uni_send(g_cur, u);
}

/* the program for the next draw.  The variant has no alpha test, so
 * it is chosen only where the test would pass every fragment anyway:
 *  the test is off; or
 *  it is GREATER 0, the draw is textured and MODULATE (the fragment's alpha
 *     is the texel's, the vertex's is ignored), and the bound texture has no
 *     texel with alpha 0.
 * Anything else, including every untextured draw with the test on (the vertex
 * alpha can be 0), keeps the program with the test. */
static void select_prog(int env)
{
    int key, want;

    if (g_no_fast) {
        return;
    }
    key = (g_uval[U_TEXTURED][0] > 0.5f ? 4 : 0) |
          (g_uval[U_TEXBLEND][0] > 3.0f ? 8 : 0) |
          (g_uval[U_FOGENABLE][0] > 0.5f ? 16 : 0) | (env ? 32 : 0);
    if (g_uval[U_ALPHAFUNC][0] < 0.5f) {
        key |= 1;
    } else if (g_uval[U_ALPHAFUNC][0] == (plgl_float)PLAT_CMP_GREATER) {
        if (g_uval[U_ALPHAREF][0] == 0.0f && g_uval[U_TEXTURED][0] > 0.5f &&
            !(g_uval[U_TEXBLEND][0] > 3.0f) &&
            g_bound_tex && g_bound_tex < PLAT_GL_MAX_TEX &&
            g_tex_nz0[g_bound_tex] && g_tex_nzm[g_bound_tex]) {
            key |= 1;           /* the test cannot reject a fragment */
        } else {
            key |= 2;
        }
    }
    want = spec_prog(key);
    if (want != g_cur) {
        int u;

        g_cur = want;
        gl.UseProgram(g_progs[g_cur]);
        for (u = 0; u < U__COUNT; u++) {
            uni_send(g_cur, u);
        }
    }
    g_fast_draws += (unsigned long)(key & 1);
}

/* the env draws leave their two attribute arrays and u_env ON (see
 * plat_gfx_draw_env); every draw of another kind calls this first. */
static void env_off(void)
{
    if (g_env_on) {
        gl.DisableVertexAttribArray(ATTR_UV2);
        gl.DisableVertexAttribArray(ATTR_ECOL);
        uni_set(U_ENV, 0.0f, 0.0f, 0.0f, 0.0f);
        g_env_on = 0;
    }
}

/* the off-screen frame at the game's size, and the program that
 * scales it into the window.  Returns 0 (and the frame keeps going straight
 * to the window) if any of it is missing.  Leaves the game's program, its
 * bound texture and its vertex buffer bound, as it found them; the caller
 * binds the framebuffer it wants. */
static int fbo_build(int w, int h)
{
    static const plgl_float quad[16] = {   /* x y u v, a triangle strip */
        -1.0f, -1.0f, 0.0f, 0.0f,    1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,    1.0f,  1.0f, 1.0f, 1.0f };
    plgl_uint vs, fs;
    plgl_int  ok = GL_FALSE;
    int missing = 0;

#define GETF(name)                                                            \
    do {                                                                      \
        *(void **)&glf.name = SDL_GL_GetProcAddress("gl" #name);              \
        if (!glf.name) { plat_log("GFX: missing gl" #name "\n"); missing++; }  \
    } while (0)
    GETF(GenFramebuffers);  GETF(BindFramebuffer); GETF(FramebufferTexture2D);
    GETF(CheckFramebufferStatus); GETF(GenRenderbuffers);
    GETF(BindRenderbuffer); GETF(RenderbufferStorage);
    GETF(FramebufferRenderbuffer);      /* IsEnabled: no longer used, */
    GETF(DisableVertexAttribArray);
#undef GETF
    if (missing) {
        plat_log("GFX: no off-screen frame - the picture is not scaled to "
                 "the window\n");
        return 0;
    }

    vs = compile(GL_VERTEX_SHADER, BLIT_VS_SRC, "letterbox vertex");
    fs = vs ? compile(GL_FRAGMENT_SHADER, BLIT_FS_SRC, "letterbox fragment") : 0;
    if (!fs) {
        if (vs) gl.DeleteShader(vs);
        return 0;
    }
    g_blit_prog = gl.CreateProgram();
    gl.AttachShader(g_blit_prog, vs);
    gl.AttachShader(g_blit_prog, fs);
    gl.BindAttribLocation(g_blit_prog, ATTR_POS, "a_bpos");
    gl.BindAttribLocation(g_blit_prog, ATTR_UV,  "a_buv");
    gl.LinkProgram(g_blit_prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    gl.GetProgramiv(g_blit_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetProgramInfoLog(g_blit_prog, (plgl_sizei)sizeof log, NULL, log);
        plat_log("GFX: SHADER FAILED (letterbox link): %s\n", log);
        g_blit_prog = 0;
        return 0;
    }
    gl.UseProgram(g_blit_prog);
    u_blit_src = gl.GetUniformLocation(g_blit_prog, "u_src");
    gl.Uniform1i(u_blit_src, 0);
    gl.UseProgram(g_progs[g_cur]);

    gl.GenBuffers(1, &g_blit_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_blit_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (plgl_sizeiptr)sizeof quad, quad,
                  GL_STATIC_DRAW);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);

    /* Colour: RGBA8, linear (it is scaled), clamped, no mipmaps - the
     * three things GLES2 requires of a texture whose size is not a power
     * of two. */
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GenTextures(1, &g_fbo_tex);
    gl.BindTexture(GL_TEXTURE_2D, g_fbo_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (plgl_int)GL_RGBA, w, h, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (plgl_int)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (plgl_int)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (plgl_int)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (plgl_int)GL_CLAMP_TO_EDGE);
    gl.BindTexture(GL_TEXTURE_2D, (g_bound_tex && g_bound_tex < PLAT_GL_MAX_TEX)
                                  ? g_tex_name[g_bound_tex] : 0);

    /* Depth: 24 bits where the driver has them (desktop GL; GLES2 with
     * OES_depth24), else 16.  NOT the 16-bit floor plat_gl_profile asks the
     * window for: the window gets 24 on every driver measured, and a 16-bit
     * off-screen depth changed 1,173 of LEVEL33's 480,000 pixels by up to 160
     * (coplanar faces resolving the other way); 24 bits took that to deltas
     * of 3 (under WSLg).
     *
     * WHAT IS LEFT, MEASURED: a frame drawn into this texture is not
     * byte-identical to the same frame drawn into the window.  LEVEL33, step
     * 1000 of game/drives/mixed.txt: 248 of 480,000 pixels on NVIDIA's own
     * GL (247 of them by at most 4, one HUD edge pixel by 125), 1,140 under
     * WSLg; a 32-bit float depth changes neither count, and the same
     * off-screen frame in a game-sized window matches fullscreen to the byte,
     * so it is the render target, not the scaling.  It never reaches a gate:
     * the off-screen frame is used only when the window is not the game's
     * size, and every gate draws headless or in a game-sized window. */
    glf.GenRenderbuffers(1, &g_fbo_depth);
    glf.BindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
    glf.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    glf.GenFramebuffers(1, &g_fbo);
    glf.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glf.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_fbo_tex, 0);
    glf.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g_fbo_depth);
    if (glf.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        plat_log("GFX: no 24-bit off-screen depth - 16 bits\n");
        glf.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    }
    if (glf.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        plat_log("GFX: off-screen frame %dx%d incomplete - the picture is not "
                 "scaled to the window\n", w, h);
        glf.BindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }
    glf.BindFramebuffer(GL_FRAMEBUFFER, 0);
    plat_log("GFX: off-screen frame %dx%d, letterboxed into the window\n", w, h);
    return 1;
}

/* Draw the next frame off-screen if the window is not the game's size, or
 * straight into the window if it is. */
static void fbo_select(int dw, int dh)
{
    int w = 0, h = 0, want;

    plat_video_get_mode(&w, &h, NULL);
    want = dw > 0 && dh > 0 && (dw != w || dh != h || plat_touch_view(NULL, NULL, NULL));
    if (want && !g_fbo_tried) {
        g_fbo_tried = 1;
        g_fbo_ok = fbo_build(w, h);
    }
    g_fbo_on = want && g_fbo_ok;
    if (g_fbo_ok) {
        glf.BindFramebuffer(GL_FRAMEBUFFER, g_fbo_on ? g_fbo : 0);
    }
    gl.Viewport(0, 0, w, h);
}

/* Everything below calls this first. Bring-up is lazy because the GL context
 * does not exist until plat_video_set_mode has run, and the game's own order
 * of operations is not this file's to dictate. */
static int gfx_ready(void)
{
    int w, h;

    if (g_ready) {
        return 1;
    }
    if (g_failed || !plat_video_ok()) {
        return 0;
    }

    if (!g_loaded) {
        if (!load_gl()) {
            plat_log("GFX: GL entry points missing - drawing disabled\n");
            g_failed = 1;
            return 0;
        }
        g_loaded = 1;
        plat_log("GFX: %s / %s / %s\n",
                 (const char *)gl.GetString(GL_VENDOR),
                 (const char *)gl.GetString(GL_RENDERER),
                 (const char *)gl.GetString(GL_VERSION));
    }

    if (!build_program()) {
        g_failed = 1;
        return 0;
    }

    gl.GenBuffers(1, &g_vbo);
    gl.GenBuffers(1, &g_ibo);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    gl.EnableVertexAttribArray(ATTR_POS);
    gl.EnableVertexAttribArray(ATTR_DIFFUSE);
    gl.EnableVertexAttribArray(ATTR_SPECULAR);
    gl.EnableVertexAttribArray(ATTR_UV);
    gl.VertexAttribPointer(ATTR_POS, 4, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)offsetof(plat_vertex, sx));
    gl.VertexAttribPointer(ATTR_DIFFUSE, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)offsetof(plat_vertex, diffuse));
    gl.VertexAttribPointer(ATTR_SPECULAR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)offsetof(plat_vertex, specular));
    gl.VertexAttribPointer(ATTR_UV, 2, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)offsetof(plat_vertex, tu));

    /* CULLMODE has ZERO write sites image-wide, so the game never states a
     * winding and the port cannot transcribe one.  It was DISABLED here until
     * 2026-09-14, on the argument that taking D3D's default would be wrong for
     * half the polygons if the guess were inverted, and that "wrong for half"
     * is much harder to see than "wrong for all".
     *
     * BOTH PATHS ARE NOW MEASURED AND THEY AGREE, so the guess is gone:
     *
     *   stage  tools/stagewind.sh   48 shipped levels, 47 decisive CW, 0 CCW
     *   model  tools/modelwind.sh   536 .ro3, 375,472 front faces CW against
     *                               261 CCW - 99.93% - and 71 of the 72
     *                               models the pixel count can decide say CW
     *
     * Front faces are CLOCKWISE as they appear on the screen, which is exactly
     * what Direct3D's default D3DCULL_CCW keeps: the game never writes
     * CULLMODE because the default was already right for its winding, and this
     * is the port supplying what D3D supplied.
     * section 5 and. */
    cap_set(GL_CULL_FACE, 1);
    gl.CullFace(GL_BACK);
    gl.FrontFace(GL_CW);

    /* GL_UNPACK_ALIGNMENT 4 is GL's default and is right for RGBA8, but a
     * texture uploaded after some other code set it to 1 would still be fine,
     * whereas the reverse is not. State it rather than inherit it. */
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    plat_video_get_mode(&w, &h, NULL);
    gl.Viewport(0, 0, w, h);
    uni_set(U_VIEWPORT, (plgl_float)(w ? w : 1), (plgl_float)(h ? h : 1),
            0.0f, 0.0f);

    g_ready = 1;
    atexit(report_fast_share);
    plat_log("GFX: program linked, viewport %dx%d\n", w, h);
    {
        int dw, dh;
        plat_video_drawable_size(&dw, &dh);
        fbo_select(dw, dh);         /* the first frame too */
    }
    return 1;
}

/* plat_video_present's two halves, either side of the swap.  The
 * game's frame is in the off-screen frame at the game's size; this draws it
 * into the window, scaled to fit with its aspect kept (black bars), and then
 * gives back every piece of GL state it touched - the four capabilities, the
 * program, the texture, the vertex buffer and the attribute arrays - because
 * plat_gfx_set_state's shadow filter means the game never re-states a value it
 * believes is already set (the HUD overlay's lesson, plat_gfx_state_restore).
 * The frame's own pixels are never scaled: plat_gfx_read_frame and a
 * screenshot read the off-screen frame, at the game's size. */
static plgl_boolean g_saved_cap[4];
static const plgl_enum g_caps[4] = { GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_BLEND,
                                     GL_CULL_FACE };
static int g_blitted;


/* ---- the touch controls' overlay -------------------------------------------
 * Drawn into the window after the letterbox blit (plat_gl_present_begin), in
 * window pixels, over the picture and the black bars alike.  One flat-colour
 * program of its own, client-side vertices; the layout is plat_touch.c's, the
 * same that plat_sdl2.c tests fingers against. */
static const char *OV_VS_SRC =
"#ifdef GL_ES\n"
"precision highp float;\n"
"#endif\n"
"attribute vec2 a_bpos;\n"
"uniform vec2 u_res;\n"
"void main()\n"
"{\n"
"    gl_Position = vec4(a_bpos.x / u_res.x * 2.0 - 1.0,\n"
"                       1.0 - a_bpos.y / u_res.y * 2.0, 0.0, 1.0);\n"
"}\n";
static const char *OV_FS_SRC =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#endif\n"
"uniform vec4 u_col;\n"
"void main() { gl_FragColor = u_col; }\n";

static plgl_uint g_ov_prog;
static plgl_int  g_ov_res, g_ov_col;
static int       g_ov_failed;

static int ov_build(void)
{
    plgl_uint vs, fs;
    plgl_int ok = 0;
    vs = compile(GL_VERTEX_SHADER, OV_VS_SRC, "overlay vertex");
    fs = vs ? compile(GL_FRAGMENT_SHADER, OV_FS_SRC, "overlay fragment") : 0;
    if (!fs) {
        if (vs) gl.DeleteShader(vs);
        return 0;
    }
    g_ov_prog = gl.CreateProgram();
    gl.AttachShader(g_ov_prog, vs);
    gl.AttachShader(g_ov_prog, fs);
    gl.BindAttribLocation(g_ov_prog, ATTR_POS, "a_bpos");
    gl.LinkProgram(g_ov_prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    gl.GetProgramiv(g_ov_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_ov_prog = 0;
        return 0;
    }
    g_ov_res = gl.GetUniformLocation(g_ov_prog, "u_res");
    g_ov_col = gl.GetUniformLocation(g_ov_prog, "u_col");
    return 1;
}

static void ov_rect(float x, float y, float w, float h,
                    float r, float g, float b, float a)
{
    plgl_float q[8];
    q[0] = x;     q[1] = y;
    q[2] = x + w; q[3] = y;
    q[4] = x;     q[5] = y + h;
    q[6] = x + w; q[7] = y + h;
    gl.Uniform4f(g_ov_col, r, g, b, a);
    gl.VertexAttribPointer(ATTR_POS, 2, GL_FLOAT, GL_FALSE, 0, q);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 3 x 5 glyphs, rows top to bottom, 3 bits a row */
static unsigned ov_glyph(char c)
{
    switch (c) {
    case 'G': return 0x7 << 12 | 0x4 << 9 | 0x5 << 6 | 0x5 << 3 | 0x7;
    case 'A': return 0x7 << 12 | 0x5 << 9 | 0x7 << 6 | 0x5 << 3 | 0x5;
    case 'S': return 0x7 << 12 | 0x4 << 9 | 0x7 << 6 | 0x1 << 3 | 0x7;
    case 'B': return 0x6 << 12 | 0x5 << 9 | 0x6 << 6 | 0x5 << 3 | 0x6;
    case 'R': return 0x6 << 12 | 0x5 << 9 | 0x6 << 6 | 0x5 << 3 | 0x5;
    case 'K': return 0x5 << 12 | 0x5 << 9 | 0x6 << 6 | 0x5 << 3 | 0x5;
    case 'U': return 0x5 << 12 | 0x5 << 9 | 0x5 << 6 | 0x5 << 3 | 0x7;
    case 'P': return 0x7 << 12 | 0x5 << 9 | 0x7 << 6 | 0x4 << 3 | 0x4;
    case 'D': return 0x6 << 12 | 0x5 << 9 | 0x5 << 6 | 0x5 << 3 | 0x6;
    case 'N': return 0x7 << 12 | 0x5 << 9 | 0x5 << 6 | 0x5 << 3 | 0x5;
    case 'H': return 0x5 << 12 | 0x5 << 9 | 0x7 << 6 | 0x5 << 3 | 0x5;
    case 'C': return 0x7 << 12 | 0x4 << 9 | 0x4 << 6 | 0x4 << 3 | 0x7;
    case 'M': return 0x5 << 12 | 0x7 << 9 | 0x7 << 6 | 0x5 << 3 | 0x5;
    case 'O': return 0x7 << 12 | 0x5 << 9 | 0x5 << 6 | 0x5 << 3 | 0x7;
    case 'E': return 0x7 << 12 | 0x4 << 9 | 0x7 << 6 | 0x4 << 3 | 0x7;
    case 'I': return 0x2 << 12 | 0x2 << 9 | 0x2 << 6 | 0x2 << 3 | 0x2;
    case '^': return 0x2 << 12 | 0x7 << 9 | 0x2 << 6 | 0x2 << 3 | 0x2;
    case 'v': return 0x2 << 12 | 0x2 << 9 | 0x2 << 6 | 0x7 << 3 | 0x2;
    case '<': return 0x1 << 12 | 0x3 << 9 | 0x7 << 6 | 0x3 << 3 | 0x1;
    case '>': return 0x4 << 12 | 0x6 << 9 | 0x7 << 6 | 0x6 << 3 | 0x4;
    default:  return 0;
    }
}

static void ov_label(const touch_btn *b, float a)
{
    size_t n = strlen(b->label), i;
    float s = b->h * 0.5f / 5.0f, sw = b->w * 0.8f / (float)(4 * n - 1), x0, y0;
    int r, c;
    if (sw < s) s = sw;
    s = (float)(int)s;
    if (s < 1.0f) return;
    x0 = b->x + (b->w - s * (float)(4 * n - 1)) * 0.5f;
    y0 = b->y + (b->h - s * 5.0f) * 0.5f;
    for (i = 0; i < n; i++) {
        unsigned g = ov_glyph(b->label[i]);
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (g >> (14 - r * 3 - c) & 1u)
                    ov_rect(x0 + s * (float)(4 * (int)i + c), y0 + s * (float)r,
                            s, s, 1.0f, 1.0f, 1.0f, a);
    }
}

static void ov_draw(int dw, int dh)
{
    touch_btn b[TOUCH_MAX_BTN];
    unsigned lit = 0;
    float steer = 0.0f, u = (float)dh / 10.0f;
    int race = 0, n, i;

    if (g_ov_failed || !plat_touch_view(&race, &lit, &steer)) return;
    if (!g_ov_prog && !ov_build()) {
        g_ov_failed = 1;
        plat_log("GFX: touch overlay program failed\n");
        return;
    }
    n = plat_touch_layout(dw, dh, race, b);
    gl.UseProgram(g_ov_prog);
    gl.Uniform2f(g_ov_res, (plgl_float)dw, (plgl_float)dh);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.Viewport(0, 0, dw, dh);
    cap_set(GL_BLEND, 1);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (i = 0; i < n; i++) {
        int on = (int)(lit >> i & 1u);
        float r = b[i].axis == 2 ? 0.9f : b[i].axis == 1 ? 0.2f : 0.9f;
        float g = b[i].axis == 2 ? 0.2f : b[i].axis == 1 ? 0.8f : 0.9f;
        float bl = b[i].axis ? 0.2f : 0.9f;
        ov_rect(b[i].x - 2.0f, b[i].y - 2.0f, b[i].w + 4.0f, b[i].h + 4.0f,
                1.0f, 1.0f, 1.0f, 0.15f);
        ov_rect(b[i].x, b[i].y, b[i].w, b[i].h, r * 0.4f, g * 0.4f, bl * 0.4f,
                on ? 0.70f : 0.22f);
        ov_label(&b[i], on ? 1.0f : 0.65f);
    }
    if (race == 1) {                  /* the steering hint: a bar and its knob */
        float cx = (float)dw * 0.5f, cy = (float)dh - 0.6f * u;
        ov_rect(cx - 2.2f * u, cy - 0.06f * u, 4.4f * u, 0.12f * u,
                1.0f, 1.0f, 1.0f, 0.25f);
        ov_rect(cx + steer * 2.2f * u - 0.3f * u, cy - 0.3f * u, 0.6f * u, 0.6f * u,
                1.0f, 1.0f, 1.0f, 0.55f);
    }
}

void plat_gl_present_begin(int dw, int dh);
void plat_gl_present_begin(int dw, int dh)
{
    plat_gl_swap_join();
    int w = 0, h = 0, rw, rh, i;

    g_blitted = 0;
    if (!g_ready || !g_fbo_on || dw <= 0 || dh <= 0) {
        return;
    }
    plat_video_get_mode(&w, &h, NULL);
    env_off();      /* the blit's program has no env arrays */
    for (i = 0; i < 4; i++) {
        g_saved_cap[i] = (plgl_boolean)g_cap_on[i];     /* was glIsEnabled */
        cap_set(g_caps[i], 0);
    }
    glf.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, dw, dh);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    /* the largest rectangle of the game's aspect that fits */
    if ((long)dw * h <= (long)dh * w) {
        rw = dw;
        rh = (int)((long)dw * h / w);
    } else {
        rh = dh;
        rw = (int)((long)dh * w / h);
    }
    gl.Viewport((dw - rw) / 2, (dh - rh) / 2, rw, rh);
    gl.UseProgram(g_blit_prog);
    gl.BindTexture(GL_TEXTURE_2D, g_fbo_tex);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_blit_vbo);
    glf.DisableVertexAttribArray(ATTR_DIFFUSE);
    glf.DisableVertexAttribArray(ATTR_SPECULAR);
    gl.VertexAttribPointer(ATTR_POS, 2, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)(4 * sizeof(plgl_float)), (const void *)0);
    gl.VertexAttribPointer(ATTR_UV, 2, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)(4 * sizeof(plgl_float)),
                           (const void *)(2 * sizeof(plgl_float)));
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ov_draw(dw, dh);
    g_blitted = 1;
}

void plat_gl_present_end(int dw, int dh);
void plat_gl_present_end(int dw, int dh)
{
    plat_gl_swap_join();
    int i;

    if (!g_ready) {
        return;
    }
    if (g_blitted) {
        for (i = 0; i < 4; i++) {
            cap_set(g_caps[i], g_saved_cap[i] ? 1 : 0);
        }
        gl.UseProgram(g_progs[g_cur]);
        gl.BindTexture(GL_TEXTURE_2D, (g_bound_tex && g_bound_tex < PLAT_GL_MAX_TEX)
                                      ? g_tex_name[g_bound_tex] : 0);
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
        gl.EnableVertexAttribArray(ATTR_DIFFUSE);
        gl.EnableVertexAttribArray(ATTR_SPECULAR);
        g_blitted = 0;
    }
    fbo_select(dw, dh);
}

/* ---- 4. render state, mapped ------------------------------------------- */

static plgl_enum gl_cmp(uint32_t d3d)
{
    switch (d3d) {
    case PLAT_CMP_NEVER:    return GL_NEVER;
    case PLAT_CMP_LESS:     return GL_LESS;
    case PLAT_CMP_EQUAL:    return GL_EQUAL;
    case PLAT_CMP_LEQUAL:   return GL_LEQUAL;
    case PLAT_CMP_GREATER:  return GL_GREATER;
    case PLAT_CMP_NOTEQUAL: return GL_NOTEQUAL;
    case PLAT_CMP_GEQUAL:   return GL_GEQUAL;
    default:                return GL_ALWAYS;
    }
}

static plgl_enum gl_blend(uint32_t d3d)
{
    switch (d3d) {
    case PLAT_BLEND_ZERO:          return GL_ZERO;
    case PLAT_BLEND_ONE:           return GL_ONE;
    case PLAT_BLEND_SRCCOLOR:      return GL_SRC_COLOR;
    case PLAT_BLEND_INVSRCCOLOR:   return GL_ONE_MINUS_SRC_COLOR;
    case PLAT_BLEND_SRCALPHA:      return GL_SRC_ALPHA;
    case PLAT_BLEND_INVSRCALPHA:   return GL_ONE_MINUS_SRC_ALPHA;
    case PLAT_BLEND_DESTALPHA:     return GL_DST_ALPHA;
    case PLAT_BLEND_INVDESTALPHA:  return GL_ONE_MINUS_DST_ALPHA;
    case PLAT_BLEND_DESTCOLOR:     return GL_DST_COLOR;
    case PLAT_BLEND_INVDESTCOLOR:  return GL_ONE_MINUS_DST_COLOR;
    case PLAT_BLEND_SRCALPHASAT:   return GL_SRC_ALPHA_SATURATE;
    default:                       return GL_ONE;
    }
}

/* D3D TEXTUREMIN 6 is the only mipmap-selecting value the image writes; 1 and
 * 2 are point and linear. Mag has no mipmap forms. */
static plgl_enum gl_filter(uint32_t d3d, int is_min)
{
    if (!is_min) {
        return (d3d == 1) ? GL_NEAREST : GL_LINEAR;
    }
    switch (d3d) {
    case 1:  return GL_NEAREST;
    case 2:  return GL_LINEAR;
    case 6:  return GL_LINEAR_MIPMAP_LINEAR;
    default: return GL_LINEAR;
    }
}

static uint32_t rs_get(plat_rs s, uint32_t dflt)
{
    return g_rs_valid[s] ? g_rs[s] : dflt;
}

/* The game's TEXMIN for texture `t` - promoted from linear to trilinear when
 * `t` is smooth.  Every other texture filters as the original asks. */
static plgl_enum tex_min_filter(plat_tex t)
{
    uint32_t v = rs_get(PLAT_RS_TEXMIN, 2);

    if (v == 2 && t && t < PLAT_GL_MAX_TEX && g_tex_smooth[t]) {
        return GL_LINEAR_MIPMAP_LINEAR;
    }
    return gl_filter(v, 1);
}

/* the sampler parameters for texture `t`, which must be bound on the
 * active unit.  A parameter already sent to it is not sent again.  A handle
 * with no GL name (the binder bound 0 for it) is not cached: GL's texture 0
 * takes all four, as it always did. */
static int tex_cacheable(plat_tex t)
{
    return t && t < PLAT_GL_MAX_TEX && g_tex_name[t];
}

static plgl_enum tex_wrap_now(void)
{
    return (rs_get(PLAT_RS_TEXADDRESS, 1) == 3) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

static int tex_sampler_dirty(plat_tex t)
{
    return !tex_cacheable(t)
        || g_tex_pwrap[t] != tex_wrap_now()
        || g_tex_pmag[t]  != gl_filter(rs_get(PLAT_RS_TEXMAG, 2), 0)
        || g_tex_pmin[t]  != tex_min_filter(t);
}

static void tex_sampler_send(plat_tex t)
{
    plgl_enum wrap = tex_wrap_now();
    plgl_enum mag  = gl_filter(rs_get(PLAT_RS_TEXMAG, 2), 0);
    plgl_enum min  = tex_min_filter(t);
    int       c    = tex_cacheable(t);

    if (!c || g_tex_pwrap[t] != wrap) {
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (plgl_int)wrap);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (plgl_int)wrap);
        if (c) g_tex_pwrap[t] = wrap;
    }
    if (!c || g_tex_pmag[t] != mag) {
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (plgl_int)mag);
        if (c) g_tex_pmag[t] = mag;
    }
    if (!c || g_tex_pmin[t] != min) {
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (plgl_int)min);
        if (c) g_tex_pmin[t] = min;
    }
}

static void apply_state(plat_rs state, uint32_t value)
{
    switch (state) {
    case PLAT_RS_ZTEST:
        cap_set(GL_DEPTH_TEST, value ? 1 : 0);
        break;
    case PLAT_RS_ZWRITE:
        gl.DepthMask(value ? GL_TRUE : GL_FALSE);
        break;
    case PLAT_RS_ZFUNC:
        gl.DepthFunc(gl_cmp(value));
        break;
    case PLAT_RS_BLEND:
        cap_set(GL_BLEND, value ? 1 : 0);
        break;
    case PLAT_RS_SRCBLEND:
    case PLAT_RS_DSTBLEND:
        gl.BlendFunc(gl_blend(rs_get(PLAT_RS_SRCBLEND, PLAT_BLEND_SRCALPHA)),
                     gl_blend(rs_get(PLAT_RS_DSTBLEND, PLAT_BLEND_INVSRCALPHA)));
        break;
    case PLAT_RS_DITHER:
        if (value) { gl.Enable(GL_DITHER); } else { gl.Disable(GL_DITHER); }
        break;

    /* Uniforms. Set here rather than at draw time because the shadow filter
     * above has already removed the redundant writes - which is the whole
     * reason the original keeps that filter. */
    case PLAT_RS_ALPHATEST:
    case PLAT_RS_ALPHAFUNC:
    case PLAT_RS_ALPHAREF:
        uni_set(U_ALPHAFUNC,
                rs_get(PLAT_RS_ALPHATEST, 0)
                    ? (plgl_float)rs_get(PLAT_RS_ALPHAFUNC, PLAT_CMP_GREATER)
                    : 0.0f, 0.0f, 0.0f, 0.0f);
        uni_set(U_ALPHAREF,
                (plgl_float)rs_get(PLAT_RS_ALPHAREF, 0) / 255.0f,
                0.0f, 0.0f, 0.0f);
        break;
    case PLAT_RS_TEXBLEND:
        uni_set(U_TEXBLEND, (plgl_float)value, 0.0f, 0.0f, 0.0f);
        break;
    case PLAT_RS_FOGENABLE:
        uni_set(U_FOGENABLE, value ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        break;
    case PLAT_RS_FOGCOLOR: {
        /* 0xAARRGGBB, and 0x0041A187 ORs in 0xFF000000 before writing it, so
         * the alpha is always opaque and is not used here. */
        plgl_float r = (plgl_float)((value >> 16) & 0xFF) / 255.0f;
        plgl_float g = (plgl_float)((value >>  8) & 0xFF) / 255.0f;
        plgl_float b = (plgl_float)( value        & 0xFF) / 255.0f;
        uni_set(U_FOGCOLOR, r, g, b, 1.0f);
        break;
    }

    /* Texture-object state. Applied to whatever is bound now AND re-applied by
     * the binder, because in GL these live on the texture and in D3D they do
     * not - a sampler state set while texture A is bound must still be in
     * force after texture B is bound. */
    case PLAT_RS_TEXADDRESS:
    case PLAT_RS_TEXMAG:
    case PLAT_RS_TEXMIN:
        if (g_bound_tex && g_bound_tex < PLAT_GL_MAX_TEX) {
            tex_sampler_send(g_bound_tex);      /* only what differs */
        }
        break;

    /* Accepted, recorded in the shadow, and deliberately not mapped. Each one
     * is a decision with a measurement behind it -
     * section 4 gives the reason for every entry in this list. */
    case PLAT_RS_TEXPERSPECTIVE:  /* GLES2 is always perspective-correct     */
    case PLAT_RS_SHADEMODE:       /* always GOURAUD; GLES2 has no flat mode  */
    case PLAT_RS_SUBPIXEL:        /* GL is always subpixel-correct           */
    case PLAT_RS_FOGMODE:         /* always NONE, i.e. vertex fog, which the */
                                  /* fragment shader does from specular.a    */
    case PLAT_RS_COLORKEY:        /* patch 5.30 deletes all 14 writes        */
    case PLAT_RS_RANGEFOG:        /* no effect on a pre-transformed vertex   */
    case PLAT_RS__COUNT:
        break;
    }
}

void plat_gfx_set_state(plat_rs state, uint32_t value)
{
    plat_gl_swap_join();
    if (state >= PLAT_RS__COUNT) {
        return;
    }
    if (g_rs_valid[state] && g_rs[state] == value) {
        return;                     /* redundant - the original returns too */
    }
    g_rs[state]       = value;
    g_rs_valid[state] = 1;

    if (gfx_ready()) {
        apply_state(state, value);
    }
}

void plat_gfx_reset_state(void)     /* was 0x00419EA1 */
{
    plat_gl_swap_join();
    memset(g_rs, 0, sizeof g_rs);
    memset(g_rs_valid, 0, sizeof g_rs_valid);
}

/* ---- 5. the scene bracket ---------------------------------------------- */

void plat_video_frame_begin(void)
{
    plat_gl_swap_join();
    if (!gfx_ready()) {
        return;
    }
    /* The original has no per-frame prologue at all: the DirectDraw blitter
     * owns the back buffer outside the scene and leaves no hole to clear.
     * A GL backend has no blitter yet, so it clears. This is the port's
     * addition, not a transcription - when the 2D path lands (the second
     * pass), the colour clear is the first thing to reconsider. */
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    /* glClear OBEYS glDepthMask, and this cost an afternoon.
     * The draw path turns depth writes off for the translucent pass and
     * leaves them off, so from the SECOND frame onward the depth clear was a
     * no-op: the buffer kept the first frame's values, every later frame's
     * geometry failed LEQUAL against them, and a correct renderer drew a
     * black screen while every counter it reported said it was working.
     *
     * The symptom is not "the picture is wrong", it is "the picture is
     * empty", and nothing upstream of here can see it.  Force the mask on for
     * the clear and put the shadowed value back, so the caller's state is
     * unchanged and the clear is unconditional. */
    gl.DepthMask(1);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.DepthMask((plgl_boolean)(rs_get(PLAT_RS_ZWRITE, 1) ? 1 : 0));
}

void plat_video_scene_begin(void)    /* was GOTO3D 0x00419F29 */
{
    plat_gl_swap_join();
    int i;

    if (!gfx_ready()) {
        return;
    }
    gl.UseProgram(g_progs[g_cur]);

    /* The thirteen states 0x00419F29 issues at every scene begin, in the
     * order it issues them. `python tools/rsstates.py block 0x419f29 0xf5`
     * prints exactly this list from the image. Two of the thirteen -
     * DITHERENABLE from [0x00564E64] and FOGTABLEMODE - are the game's to set
     * because it has the global; the rest are constants and are set here so a
     * port that has not yet wired the caller still draws something sane. */
    plat_gfx_set_state(PLAT_RS_SUBPIXEL,       1);
    plat_gfx_set_state(PLAT_RS_ZTEST,          1);
    plat_gfx_set_state(PLAT_RS_ZWRITE,         1);
    plat_gfx_set_state(PLAT_RS_SHADEMODE,      2);
    plat_gfx_set_state(PLAT_RS_ZFUNC,          PLAT_CMP_LEQUAL);
    plat_gfx_set_state(PLAT_RS_ALPHATEST,      1);
    plat_gfx_set_state(PLAT_RS_ALPHAREF,       0);
    plat_gfx_set_state(PLAT_RS_ALPHAFUNC,      PLAT_CMP_GREATER);
    plat_gfx_set_state(PLAT_RS_TEXPERSPECTIVE, 1);
    plat_gfx_set_state(PLAT_RS_TEXMAG,         2);
    plat_gfx_set_state(PLAT_RS_TEXMIN,         2);
    plat_gfx_set_state(PLAT_RS_FOGMODE,        0);

    /* The blend function is never left unset in the original either, but its
     * first write is in a block rather than the bracket. Seed it so the very
     * first blended draw of a run cannot inherit GL's (ONE, ZERO). */
    for (i = 0; i < 2; i++) {
        plat_gfx_set_state(i ? PLAT_RS_DSTBLEND : PLAT_RS_SRCBLEND,
                           i ? PLAT_BLEND_INVSRCALPHA : PLAT_BLEND_SRCALPHA);
    }
}

void plat_video_scene_end(void)      /* was GOTO2D 0x00419EEC */
{
    plat_gl_swap_join();
    /* EndScene has no GL counterpart. The original's is a flag flip and a
     * vtable call; there is nothing to flush here that the present does not
     * already flush. Kept so the bracket stays symmetrical in the log. */
}

/* ---- 6. geometry ------------------------------------------------------- */

static plgl_enum gl_prim(plat_prim p)
{
    switch (p) {
    case PLAT_PRIM_POINTS:         return GL_POINTS;
    case PLAT_PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case PLAT_PRIM_TRIANGLES:      return GL_TRIANGLES;
    case PLAT_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    default:                       return GL_TRIANGLE_FAN;
    }
}

/* how a draw's vertices reach the GPU (plat_gfx_set_stream).  The
 * RG40XX-H's Mali spent ~40 us a draw, ~300 draws a frame for the car alone,
 * and which upload is cheapest on a tile-based driver was measured there
 * (2026-10-19, LEVEL41, the EVO 4):
 *   0  one BufferData per draw, orphaning the store: car gl ~12 ms
 *   1  client-side arrays, no buffer bound: ~10 ms, the HUD 1 ms less -
 *      rally's default on ARM
 *   ~~2  one store a frame, orphaned at frame begin, each draw appended with
 *      BufferSubData~~ - REMOVED: the menus fell to 2-8 fps and car select
 *      to 0.1; a tile-based driver copies or waits on a store its queued
 *      draws still read.  Not to be retried. */
void plat_gfx_set_stream(int mode)
{
    plat_gl_swap_join();
    g_stream = mode == 1 ? 1 : 0;
}

static void upload_vertices(const plat_vertex *v, unsigned nv)
{
    uintptr_t base;
    size_t bytes = (size_t)nv * sizeof(plat_vertex);

    if (g_stream == 1) {
        gl.BindBuffer(GL_ARRAY_BUFFER, 0);
        base = (uintptr_t)v;
    } else {
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (plgl_sizeiptr)bytes, v,
                      GL_STREAM_DRAW);
        base = 0;
    }
    /* Re-pointed every draw: the store or its offset moves, and on GLES2 the
     * attribute pointers are per-call state rather than living in a VAO. */
    gl.VertexAttribPointer(ATTR_POS, 4, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)(base + offsetof(plat_vertex, sx)));
    gl.VertexAttribPointer(ATTR_DIFFUSE, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)(base + offsetof(plat_vertex, diffuse)));
    gl.VertexAttribPointer(ATTR_SPECULAR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)(base + offsetof(plat_vertex, specular)));
    gl.VertexAttribPointer(ATTR_UV, 2, GL_FLOAT, GL_FALSE,
                           (plgl_sizei)sizeof(plat_vertex),
                           (const void *)(base + offsetof(plat_vertex, tu)));
}

static plat_gfx_tap_fn g_tap;                  /* plat_gfx_set_tap */
void plat_gfx_set_tap(plat_gfx_tap_fn fn) { g_tap = fn; }

void plat_gfx_draw(plat_prim p, const plat_vertex *v, unsigned nv)
{
    plat_gl_swap_join();
    g_draws++;
    g_verts += nv;
    if (g_tap && v && nv) {
        g_tap(p, v, sizeof(plat_vertex), nv, NULL, 0);
    }
    if (!gfx_ready() || !v || nv == 0) {
        return;
    }
    env_off();
    select_prog(0);
    upload_vertices(v, nv);
    gl.DrawArrays(gl_prim(p), 0, (plgl_sizei)nv);
}

/* plat.h's plat_gfx_draw_env.  The base layer as plat_gfx_draw draws
 * it, plus the reflection added in the same fragment (FS_SRC's u_env).  The
 * reflection map goes on unit 1 with the sampler state the scene is under,
 * as plat_gfx_tex_bind would have put on it; unit 0 is left as it was. */
void plat_gfx_draw_env(plat_prim p, const plat_vertex_env *v, unsigned nv,
                       plat_tex env)
{
    plat_gl_swap_join();
    uintptr_t base;
    size_t    bytes = (size_t)nv * sizeof(plat_vertex_env);
    plgl_sizei st = (plgl_sizei)sizeof(plat_vertex_env);
    plgl_uint name;

    g_draws++;
    g_verts += nv;
    if (g_tap && v && nv) {
        g_tap(p, &v->v, sizeof(plat_vertex_env), nv, NULL, 0);
    }
    if (!gfx_ready() || !v || nv == 0) {
        return;
    }
    if (g_stream == 1) {
        gl.BindBuffer(GL_ARRAY_BUFFER, 0);
        base = (uintptr_t)v;
    } else {
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
        gl.BufferData(GL_ARRAY_BUFFER, (plgl_sizeiptr)bytes, v, GL_STREAM_DRAW);
        base = 0;
    }
    gl.VertexAttribPointer(ATTR_POS, 4, GL_FLOAT, GL_FALSE, st,
        (const void *)(base + offsetof(plat_vertex_env, v) + offsetof(plat_vertex, sx)));
    gl.VertexAttribPointer(ATTR_DIFFUSE, 4, GL_UNSIGNED_BYTE, GL_TRUE, st,
        (const void *)(base + offsetof(plat_vertex_env, v) + offsetof(plat_vertex, diffuse)));
    gl.VertexAttribPointer(ATTR_SPECULAR, 4, GL_UNSIGNED_BYTE, GL_TRUE, st,
        (const void *)(base + offsetof(plat_vertex_env, v) + offsetof(plat_vertex, specular)));
    gl.VertexAttribPointer(ATTR_UV, 2, GL_FLOAT, GL_FALSE, st,
        (const void *)(base + offsetof(plat_vertex_env, v) + offsetof(plat_vertex, tu)));
    gl.VertexAttribPointer(ATTR_UV2, 2, GL_FLOAT, GL_FALSE, st,
        (const void *)(base + offsetof(plat_vertex_env, eu)));
    gl.VertexAttribPointer(ATTR_ECOL, 4, GL_UNSIGNED_BYTE, GL_TRUE, st,
        (const void *)(base + offsetof(plat_vertex_env, ecol)));

    /* unit 1 is touched only when its binding or the map's sampler
     * state is not already what this draw wants, and the arrays and u_env are
     * left ON for the next env draw (env_off turns them off at the first draw
     * of another kind). */
    name = (env && env < PLAT_GL_MAX_TEX) ? g_tex_name[env] : 0;
    if (g_u1_name != name || tex_sampler_dirty(env)) {
        gl.ActiveTexture(GL_TEXTURE1);
        if (g_u1_name != name) {
            gl.BindTexture(GL_TEXTURE_2D, name);
            g_u1_name = name;
        }
        tex_sampler_send(env);
        gl.ActiveTexture(GL_TEXTURE0);
    }
    select_prog(1);
    if (!g_env_on) {
        gl.EnableVertexAttribArray(ATTR_UV2);
        gl.EnableVertexAttribArray(ATTR_ECOL);
        uni_set(U_ENV, 1.0f, 0.0f, 0.0f, 0.0f);
        g_env_on = 1;
    }

    gl.DrawArrays(gl_prim(p), 0, (plgl_sizei)nv);
}

void plat_gfx_draw_indexed(plat_prim p, const plat_vertex *v, unsigned nv,
                           const uint16_t *idx, unsigned ni)
{
    plat_gl_swap_join();
    g_draws++;
    g_verts   += nv;
    g_indices += ni;
    if (g_tap && v && idx && nv && ni) {
        g_tap(p, v, sizeof(plat_vertex), nv, idx, ni);
    }
    if (!gfx_ready() || !v || !idx || nv == 0 || ni == 0) {
        return;
    }
    env_off();
    select_prog(0);
    upload_vertices(v, nv);
    if (g_stream == 1) {                       /* client-side, as the vertices */
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        gl.DrawElements(gl_prim(p), (plgl_sizei)ni, GL_UNSIGNED_SHORT, idx);
        return;
    }
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER,
                  (plgl_sizeiptr)(ni * sizeof(uint16_t)), idx, GL_STREAM_DRAW);
    gl.DrawElements(gl_prim(p), (plgl_sizei)ni, GL_UNSIGNED_SHORT, NULL);
}

/* Not in plat.h: diagnostics for bring-up, the same shape as plat_video_ok
 * before promoted it. Declared here so -Wmissing-prototypes stays quiet. */
void plat_gfx_counters(unsigned long *draws, unsigned long *verts,
                       unsigned long *indices);
void plat_gfx_counters(unsigned long *draws, unsigned long *verts,
                       unsigned long *indices)
{
    plat_gl_swap_join();
    if (draws)   { *draws   = g_draws; }
    if (verts)   { *verts   = g_verts; }
    if (indices) { *indices = g_indices; }
}

/* how many draws so far ran on the program without the alpha test.
 * Not in plat.h; for the A/B and the gates, which must see it non-zero. */
unsigned long plat_gfx_fast_draws(void);
unsigned long plat_gfx_fast_draws(void)
{
    plat_gl_swap_join();
    return g_fast_draws;
}

/* Not in plat.h either: reads one pixel out of the back buffer so a gate can
 * assert that a draw PRODUCED something, rather than that it returned. It is
 * the difference between "the shader linked" and "the renderer works", and
 * without it this whole file would be committed on the strength of a log line.
 * Returns 0 when there is nothing to read. port/tests/gfx_render_probe.c. */
int plat_gfx_read_pixel(int x, int y, unsigned char rgba[4]);
int plat_gfx_read_pixel(int x, int y, unsigned char rgba[4])
{
    plat_gl_swap_join();
    void (PLAT_GLAPI *readpixels)(plgl_int, plgl_int, plgl_sizei, plgl_sizei,
                                  plgl_enum, plgl_enum, void *);

    if (!gfx_ready() || !rgba) {
        return 0;
    }
    *(void **)&readpixels = SDL_GL_GetProcAddress("glReadPixels");
    if (!readpixels) {
        return 0;
    }
    /* GL's origin is bottom-left and the shim's y is top-down, matching the
     * engine's screen space. Flip so the caller can name a pixel the way it
     * named the vertex. */
    {
        int w = 0, h = 0;
        plat_video_get_mode(&w, &h, NULL);
        readpixels(x, (h > 0 ? h - 1 - y : y), 1, 1, GL_RGBA,
                   GL_UNSIGNED_BYTE, rgba);
    }
    return 1;
}

/* Not in plat.h: the winding measurement.
 *
 * CULLMODE has ZERO write sites image-wide, so the original never states a
 * winding and no transcription can produce one. measured that the
 * format gate cannot settle it either - reversing a triangle's corner order in
 * cdat.c leaves every format gate green.  What CAN
 * settle it is drawing a stage with culling on and counting the pixels, which
 * is what port/tests/stage_render_probe.c does with this.
 *
 * mode 0 = disabled; 1 = cull back faces, front = CCW in GL's bottom-left
 *          screen space; 2 = the same with front = CW.
 *
 * THE ANSWER IS MODE 2, and as of 2026-09-14 it is measured on BOTH paths:
 *
 *   tools/stagewind.sh   48 shipped levels - 47 decisive CW, 0 CCW, CW keeping
 *                        a median 99.4% of the unculled frame against CCW's
 *                        14.8%..
 *   tools/modelwind.sh   536 .ro3 models - the fan's first triangle, eight
 *                        yaws each: 375,472 front faces CW against 261 CCW
 *                        (99.93%), and 71 of the 72 models whose pixel count
 *                        can decide say CW.
 *                        section 5.
 *
 * Front faces are clockwise as seen, which is exactly what Direct3D's default
 * D3DCULL_CCW keeps - so the game never writes CULLMODE because the default
 * was already right for its winding.
 *
 * THE BACKEND'S DEFAULT IS NOW MODE 2 rather than 0.  It was 0
 * until because only the STAGE path had been measured and the model path
 * "shares the shim but not this corner order"; that was the right call while it
 * held, and it no longer holds.
 *
 * This function is still deliberately NOT a plat_rs.  Adding a render state
 * the game never writes would put a decision the PORT has to make into the
 * shim's transcribed surface, which is exactly the confusion
 * exists to prevent.  It stays a diagnostic: it is how the
 * two probes drive the three-way comparison the figures above come from. */
int plat_gfx_cull_mode(int mode);
int plat_gfx_cull_mode(int mode)
{
    plat_gl_swap_join();
    if (!gfx_ready()) {
        return 0;
    }
    if (mode <= 0) {
        cap_set(GL_CULL_FACE, 0);
    } else {
        cap_set(GL_CULL_FACE, 1);
        gl.CullFace(GL_BACK);
        gl.FrontFace(mode == 1 ? GL_CCW : GL_CW);
    }
    return 1;
}

/* Not in plat.h either: an INSET, the rear-view mirror's viewport.
 * The original sets a D3D viewport over the inset and clears it through the
 * viewport object (0x00453E06: vtable +0x30 on [0x00564B40] with the
 * rectangle); the projection's centre and clip window
 * are the caller's (stage_view).  Here: scissor to (x, y, w, h) in top-down
 * pixels, and clear colour and depth inside it.  w <= 0 turns the scissor off.
 * Like plat_gfx_cull_mode, a port-side decision, not a render state. */
int plat_gfx_inset(int x, int y, int w, int h);
int plat_gfx_inset(int x, int y, int w, int h)
{
    plat_gl_swap_join();
    int sw, sh;
    if (!gfx_ready()) {
        return 0;
    }
    if (w <= 0 || h <= 0) {
        cap_set(GL_SCISSOR_TEST, 0);
        return 1;
    }
    plat_video_get_mode(&sw, &sh, NULL);
    cap_set(GL_SCISSOR_TEST, 1);
    gl.Scissor(x, sh - (y + h), w, h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.DepthMask(1);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.DepthMask((plgl_boolean)(rs_get(PLAT_RS_ZWRITE, 1) ? 1 : 0));
    return 1;
}

/* Not in plat.h either: a PILLAR, for 4:3 screens on a wider frame.  The
 * menus are drawn for 4:3; on a 16:9 frame they would stretch.  With w > 0 the
 * frame's x..x+w is the whole screen: pre-transformed coordinates 0..w map
 * into it, so the caller draws at w x h and the rest stays black.  w <= 0
 * puts the full frame back (the frame's own clear has already run). */
int plat_gfx_pillar(int x, int w);
int plat_gfx_pillar(int x, int w)
{
    plat_gl_swap_join();
    int fw = 0, fh = 0;
    if (!gfx_ready()) {
        return 0;
    }
    plat_video_get_mode(&fw, &fh, NULL);
    if (w <= 0) {
        x = 0;
        w = fw;
    }
    gl.Viewport(x, 0, w, fh);
    uni_set(U_VIEWPORT, (plgl_float)w, (plgl_float)fh, 0.0f, 0.0f);
    return 1;
}

/* Not in plat.h either: the render-state shadow and the bound texture, for a
 * caller that draws something the GAME does not draw - rally's stand-in HUD
 *and must hand the rest of the frame, and the next frame, exactly
 * the state it found.  Objects inherit state across frames
 * so an overlay that left its own would
 * change the 3D picture.  A state that was never set before the save cannot
 * be "unset" by the restore; it is left as the overlay set it. */
typedef struct {
    uint32_t v[PLAT_RS__COUNT];
    int      valid[PLAT_RS__COUNT];
    plat_tex tex;
} plat_gfx_saved;
void plat_gfx_state_save(plat_gfx_saved *s);
void plat_gfx_state_save(plat_gfx_saved *s)
{
    plat_gl_swap_join();
    memcpy(s->v, g_rs, sizeof(g_rs));
    memcpy(s->valid, g_rs_valid, sizeof(g_rs_valid));
    s->tex = g_bound_tex;
}
void plat_gfx_state_restore(const plat_gfx_saved *s);
void plat_gfx_state_restore(const plat_gfx_saved *s)
{
    plat_gl_swap_join();
    int i;
    /* The states first, while the overlay's own texture is still bound: a
     * sampler state lands on the bound texture (apply_state).  A state the
     * scene had NEVER set goes back to never set: the binder derives the
     * sampler parameters from rs_get's default for those, and the scene
     * never writes TEXADDRESS at all - so the overlay's clamp, left valid,
     * reached every plate bound afterwards.  Measured: 1,358 pixels
     * of rally's 3D picture changed outside the HUD's text, every one on a
     * polygon seam, until this.  Only the sampler group is derived from the
     * shadow at draw time; a never-set state of another kind keeps the GL
     * value the overlay gave it (none reaches rally: the scene sets the rest
     * every frame). */
    for (i = 0; i < PLAT_RS__COUNT; i++) {
        if (s->valid[i]) {
            plat_gfx_set_state((plat_rs)i, s->v[i]);
        } else if (g_rs_valid[i]) {
            g_rs_valid[i] = 0;
            g_rs[i] = s->v[i];
        }
    }
    /* Then the saved texture back.  The binder re-applies the sampler group
     * from the shadow, which is the saved shadow again by now. */
    plat_gfx_tex_bind(s->tex);
}

/* Not in plat.h either: the whole back buffer, so a gate can measure COVERAGE
 * rather than sample one pixel.  One pixel says "something drew"; a coverage
 * count says WHICH of two windings drew more, which is the only reason this
 * exists.  Returns 0 when there is nothing to read. */
int plat_gfx_read_frame(unsigned char *rgba, int w, int h);
int plat_gfx_read_frame(unsigned char *rgba, int w, int h)
{
    plat_gl_swap_join();
    void (PLAT_GLAPI *readpixels)(plgl_int, plgl_int, plgl_sizei, plgl_sizei,
                                  plgl_enum, plgl_enum, void *);
    int mw = 0, mh = 0;

    if (!gfx_ready() || !rgba || w <= 0 || h <= 0) {
        return 0;
    }
    plat_video_get_mode(&mw, &mh, NULL);
    if (w != mw || h != mh) {
        return 0;               /* a partial read would be a silent wrong answer */
    }
    *(void **)&readpixels = SDL_GL_GetProcAddress("glReadPixels");
    if (!readpixels) {
        return 0;
    }
    readpixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return 1;
}

/* Not in plat.h either: the full-screen DEPTH clear 0x0044CA08
 * issues before the cockpit on a wet stage - IDirect3DViewport3::Clear with
 * D3DCLEAR_ZBUFFER over the whole screen.  The colour
 * is left alone; the depth write mask is put back as the shadow has it. */
int plat_gfx_clear_depth(void);
int plat_gfx_clear_depth(void)
{
    plat_gl_swap_join();
    if (!gfx_ready()) {
        return 0;
    }
    gl.DepthMask(1);
    gl.Clear(GL_DEPTH_BUFFER_BIT);
    gl.DepthMask((plgl_boolean)(rs_get(PLAT_RS_ZWRITE, 1) ? 1 : 0));
    return 1;
}

/* Not in plat.h either: a texture the frame can be COPIED into - the
 * windscreen raindrops' plate 1999, which 0x00453C27 fills with a Blt of the
 * back buffer and 0x00453C9C copies texel for texel with the alpha forced to
 * 0xFF (`OR EAX,0xFF000000`).  GL_RGB is that forced alpha: a blended frame
 * leaves its alpha channel anything, and an RGB texture samples 1.0 whatever
 * it was.  `w` x `h` must be powers of two: the drops sample it with WRAP,
 * which GLES2 refuses on any other size.  Returns 0 without a context. */
plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h);
plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h)
{
    plat_gl_swap_join();
    plat_tex  handle;
    plgl_uint name = 0;

    if (!gfx_ready() || w == 0 || h == 0 || g_next_tex >= PLAT_GL_MAX_TEX) {
        return 0;
    }
    handle = g_next_tex++;
    gl.GenTextures(1, &name);
    g_tex_name[handle] = name;
    g_tex_w[handle]    = w;
    g_tex_h[handle]    = h;
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, name);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (plgl_int)GL_RGB, (plgl_sizei)w,
                  (plgl_sizei)h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    /* the same rule as plat_gfx_tex_create: left bound, and said so */
    g_bound_tex = handle;
    g_tex_nz0[handle] = g_tex_nzm[handle] = 1;   /* GL_RGB: alpha is 1.0 */
    g_tex_pwrap[handle] = g_tex_pmag[handle] = g_tex_pmin[handle] = 0;
    uni_set(U_TEXTURED, 1.0f, 0.0f, 0.0f, 0.0f);
    apply_state(PLAT_RS_TEXMIN, rs_get(PLAT_RS_TEXMIN, 2));
    return handle;
}

/* Copy the frame drawn so far, its bottom-left `w` x `h` pixels, into level
 * 0 of `t` at (0, 0).  GL's rows run bottom-up: texel row 0 is the frame's
 * BOTTOM row, which the caller's v must allow for.  The bound texture is put
 * back. */
int plat_gfx_frame_copy(plat_tex t, int w, int h);
int plat_gfx_frame_copy(plat_tex t, int w, int h)
{
    plat_gl_swap_join();
    if (!gfx_ready() || t == 0 || t >= PLAT_GL_MAX_TEX || !g_tex_name[t]
        || w <= 0 || h <= 0) {
        return 0;
    }
    if ((unsigned)w > g_tex_w[t]) w = (int)g_tex_w[t];
    if ((unsigned)h > g_tex_h[t]) h = (int)g_tex_h[t];
    if (!g_copysub) {
        return 0;
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_tex_name[t]);
    g_copysub(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (plgl_sizei)w, (plgl_sizei)h);
    gl.BindTexture(GL_TEXTURE_2D, (g_bound_tex && g_bound_tex < PLAT_GL_MAX_TEX)
                                  ? g_tex_name[g_bound_tex] : 0);
    return 1;
}

/* ---- 7. textures ------------------------------------------------------- */

/* 1 if no pixel of `n` has alpha 0.  Pixels are four bytes in memory
 * order R, G, B, A (plat.h), so the alpha is byte 3 whatever the endianness. */
static int no_zero_alpha(const uint32_t *px, size_t n)
{
    const unsigned char *b = (const unsigned char *)px;
    size_t i;

    for (i = 0; i < n; i++) {
        if (b[4 * i + 3] == 0) {
            return 0;
        }
    }
    return 1;
}

/* the upload formats.  RGBA is every texture's (plat.h's order); BGRA
 * is gfx2d's canvas as the original keeps it, where the extension exists, so
 * the per-frame swizzle in C is not needed.  The texels sampled are the same. */
static plat_tex tex_create_f(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels,
                             plgl_enum fmt);

plat_tex plat_gfx_tex_create(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels)
{
    plat_gl_swap_join();
    return tex_create_f(w, h, levels, nlevels, GL_RGBA);
}

static plat_tex tex_create_f(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels,
                             plgl_enum fmt)
{
    plat_tex  handle;
    plgl_uint name = 0;
    unsigned  i, lw = w, lh = h;

    if (g_next_tex >= PLAT_GL_MAX_TEX) {
        plat_log("GFX: texture table full (%u)\n", (unsigned)PLAT_GL_MAX_TEX);
        return 0;
    }
    handle = g_next_tex++;

    /* The handle is issued whether or not there is a context, so that a caller
     * which confuses two textures stays a bug that survives into a machine
     * with a display instead of being masked by every handle being 0. */
    if (!gfx_ready() || !levels || !levels[0] || w == 0 || h == 0 ||
        nlevels == 0) {
        return handle;
    }

    gl.GenTextures(1, &name);
    g_tex_name[handle] = name;
    g_tex_w[handle]    = w;
    g_tex_h[handle]    = h;
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, name);
    /* EVERY LEVEL COMES FROM THE CALLER.  glGenerateMipmap is not called and
     * must not be - plat.h says why, and getting it wrong is a silent
     * darkening rather than an error. */
    g_tex_nz0[handle] = g_tex_nzm[handle] = 1;
    g_tex_pwrap[handle] = g_tex_pmag[handle] = g_tex_pmin[handle] = 0;
    for (i = 0; i < nlevels; i++) {
        if (!levels[i]) {
            break;
        }
        if (!no_zero_alpha(levels[i], (size_t)lw * lh)) {
            if (i == 0) g_tex_nz0[handle] = 0; else g_tex_nzm[handle] = 0;
        }
        gl.TexImage2D(GL_TEXTURE_2D, (plgl_int)i, (plgl_int)fmt,
                      (plgl_sizei)lw, (plgl_sizei)lh, 0, fmt, GL_UNSIGNED_BYTE,
                      levels[i]);
        lw = lw > 1u ? lw / 2u : 1u;
        lh = lh > 1u ? lh / 2u : 1u;
    }
    /* Seeded from the current sampler state, then owned by it: see the
     * TEXADDRESS/TEXMAG/TEXMIN case in apply_state. */
    g_bound_tex = handle;
    /* AND the sampler-enable uniform WITH it.  Creating a texture really does
     * leave it bound, so the shadow above is right - but plat_gfx_tex_bind
     * short-circuits on a matching shadow, so WITHOUT this line the FIRST
     * bind of a freshly created texture is a no-op and u_textured stays 0:
     * the geometry draws untextured white and nothing anywhere says so.
     *
     * Found 2026-09-14 by port/tests/sprite_render_probe.c, which draws one
     * textured quad immediately after creating its texture - the first caller
     * in the port to do create-then-bind with no other bind between.  The
     * stage never hit it because it creates every plate up front and then
     * binds a DIFFERENT one per batch.  The failure was a uniformly white
     * frame, which is why the probe compares the lit SET and not a count:
     * a white frame "lights pixels" perfectly well. */
    uni_set(U_TEXTURED, 1.0f, 0.0f, 0.0f, 0.0f);
    apply_state(PLAT_RS_TEXMIN, rs_get(PLAT_RS_TEXMIN, 2));
    return handle;
}

void plat_gfx_tex_destroy(plat_tex t)
{
    plat_gl_swap_join();
    if (t == 0 || t >= PLAT_GL_MAX_TEX || !g_tex_name[t]) {
        return;
    }
    if (g_ready) {
        gl.DeleteTextures(1, &g_tex_name[t]);
        if (g_u1_name == g_tex_name[t]) {
            g_u1_name = ~0u;        /* unknown: force the next env draw to bind */
        }
    }
    g_tex_pwrap[t] = g_tex_pmag[t] = g_tex_pmin[t] = 0;
    g_tex_name[t] = 0;
    g_tex_w[t]    = 0;
    g_tex_h[t]    = 0;
    g_tex_smooth[t] = 0;
    g_tex_nz0[t]    = 0;
    g_tex_nzm[t]    = 0;
    if (g_bound_tex == t) {
        /* DeleteTextures unbinds it, so the shadow's 0 is right - and the
         * sampler-enable uniform goes with it, or the next tex_bind(0)
         * short-circuits on the shadow and untextured draws sample the
         * deleted name (black), gfx_tex_state_probe.c. */
        g_bound_tex = 0;
        if (g_ready) {
            uni_set(U_TEXTURED, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}

/* Level 0, in place, same dimensions - the 0x00419B8A shape.  plat.h says why
 * this exists and why a create/destroy pair per frame is not the alternative.
 * Silently does nothing for a handle with no GL name, which is the same
 * degradation plat_gfx_tex_create already has with no context. */
static void tex_update_f(plat_tex t, const uint32_t *level0, plgl_enum fmt);

void plat_gfx_tex_update(plat_tex t, const uint32_t *level0)
{
    plat_gl_swap_join();
    tex_update_f(t, level0, GL_RGBA);
}

static void tex_update_f(plat_tex t, const uint32_t *level0, plgl_enum fmt)
{
    if (!gfx_ready() || !level0 || t == 0 || t >= PLAT_GL_MAX_TEX ||
        !g_tex_name[t] || g_tex_w[t] == 0) {
        return;
    }
    g_tex_nz0[t] = (unsigned char)no_zero_alpha(
        level0, (size_t)g_tex_w[t] * g_tex_h[t]);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_tex_name[t]);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (plgl_sizei)g_tex_w[t],
                     (plgl_sizei)g_tex_h[t], fmt, GL_UNSIGNED_BYTE,
                     level0);
    /* The bind above is a real GL state change, so the shadow must follow it
     * or the next plat_gfx_tex_bind of the same handle becomes a no-op while
     * something else is bound - and so must the sampler-enable uniform, for
     * the same reason: only a real bind wrote it. */
    g_bound_tex = t;
    uni_set(U_TEXTURED, 1.0f, 0.0f, 0.0f, 0.0f);
    apply_state(PLAT_RS_TEXMIN, rs_get(PLAT_RS_TEXMIN, 2));
}

/* not in plat.h: a one-level texture, and its update, from pixels in
 * B, G, R, A byte order.  0 / no-op where the context lacks
 * GL_EXT_texture_format_BGRA8888 (plat_gfx_tex_bgra says which), and the
 * caller swizzles and uses plat_gfx_tex_create instead. */
int plat_gfx_tex_bgra(void);
int plat_gfx_tex_bgra(void)
{
    plat_gl_swap_join();
    static int ok = -1;

    if (!gfx_ready()) {
        return 0;
    }
    if (ok < 0) {
        ok = SDL_GL_ExtensionSupported("GL_EXT_texture_format_BGRA8888") ? 1 : 0;
    }
    return ok;
}

plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px);
plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px)
{
    plat_gl_swap_join();
    const uint32_t *lv[1];

    if (!plat_gfx_tex_bgra()) {
        return 0;
    }
    lv[0] = px;
    return tex_create_f(w, h, lv, 1, GL_BGRA_EXT);
}

void plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px);
void plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px)
{
    plat_gl_swap_join();
    if (plat_gfx_tex_bgra()) {
        tex_update_f(t, px, GL_BGRA_EXT);
    }
}

void plat_gfx_tex_smooth(plat_tex t, unsigned nlevels)
{
    plat_gl_swap_join();
    unsigned w, h, full = 1;

    if (!gfx_ready() || t == 0 || t >= PLAT_GL_MAX_TEX || !g_tex_name[t]) {
        return;
    }
    /* A chain short of 1x1 is INCOMPLETE under a mipmapped filter, and GLES2
     * has no GL_TEXTURE_MAX_LEVEL to say so: the texture would sample black.
     * Such a texture keeps the game's filter. */
    for (w = g_tex_w[t], h = g_tex_h[t]; w > 1u || h > 1u; full++) {
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
    }
    if (nlevels < full) {
        return;
    }
    g_tex_smooth[t] = 1;
    if (g_aniso < 0.0f) {
        g_aniso = 0.0f;
        if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
            void (PLAT_GLAPI *getf)(plgl_enum, float *);
            *(void **)&getf = SDL_GL_GetProcAddress("glGetFloatv");
            *(void **)&gl_TexParameterf = SDL_GL_GetProcAddress("glTexParameterf");
            if (getf && gl_TexParameterf) {
                getf(0x84FF, &g_aniso);    /* GL_MAX_TEXTURE_MAX_ANISOTROPY */
                if (g_aniso > 8.0f) g_aniso = 8.0f;
            }
        }
        if (g_aniso > 1.0f)
            plat_log("GFX: smooth textures: trilinear, anisotropic x%.0f\n",
                     (double)g_aniso);
        else
            plat_log("GFX: smooth textures: trilinear, no anisotropic\n");
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_tex_name[t]);
    if (g_aniso > 1.0f) {
        gl_TexParameterf(GL_TEXTURE_2D, 0x84FE, g_aniso);  /* MAX_ANISOTROPY */
    }
    g_bound_tex = t;
    uni_set(U_TEXTURED, 1.0f, 0.0f, 0.0f, 0.0f);    /* as tex_update */
    apply_state(PLAT_RS_TEXMIN, rs_get(PLAT_RS_TEXMIN, 2));
}

void plat_gfx_tex_bind(plat_tex t)  /* was 0x0041A052, stage 0 only */
{
    plat_gl_swap_join();
    if (g_bound_tex == t) {
        return;                     /* the original's per-stage shadow */
    }
    g_bound_tex = t;
    if (!gfx_ready()) {
        return;
    }
    if (t && t < PLAT_GL_MAX_TEX && g_tex_name[t]) {
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, g_tex_name[t]);
        uni_set(U_TEXTURED, 1.0f, 0.0f, 0.0f, 0.0f);
        /* D3D sampler state is per-stage, GL's is per-object. Re-apply. */
        apply_state(PLAT_RS_TEXMIN, rs_get(PLAT_RS_TEXMIN, 2));
    } else {
        gl.BindTexture(GL_TEXTURE_2D, 0);
        uni_set(U_TEXTURED, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}
