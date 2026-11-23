/* plat_null.c - the no-op backend.
 *
 *
 *
 * It exists for one reason: to prove that plat.h is a complete, self-contained,
 * compilable contract with no dependency on any platform header. If this file
 * builds clean at -Wall -Wextra -Wpedantic, the header is honest.
 *
 * It is also the harness the SDL2 backends are written against: every function
 * here is the shape the real one must have.
 *
 * Logging and time are real, because they cost nothing and because a null
 * backend that cannot print is useless for bring-up. Everything else is a stub
 * that returns the documented "nothing happened" value.
*/

#include "plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_log;
static int   g_w, g_h, g_bpp;

/* ---- 1. lifecycle ------------------------------------------------------ */

int plat_init(const plat_config *cfg)
{
    if (cfg && cfg->log_path) {
        g_log = fopen(cfg->log_path, "w");
    }
    if (!g_log) {
        g_log = stderr;
    }
    plat_log("PLAT: backend=%s\n", plat_backend_name());
    return 0;
}

void plat_shutdown(void)
{
    if (g_log && g_log != stderr) {
        fclose(g_log);
    }
    g_log = NULL;
}

const char *plat_backend_name(void) { return "null"; }

/* ---- 2. video ---------------------------------------------------------- */

int plat_video_set_mode(int width, int height, int bpp, int fullscreen)
{
    (void)fullscreen;
    g_w = width; g_h = height; g_bpp = bpp;
    return 0;
}

void plat_video_get_mode(int *width, int *height, int *bpp)
{
    if (width)  *width  = g_w;
    if (height) *height = g_h;
    if (bpp)    *bpp    = g_bpp;
}

/* Always 0: there is no surface, and saying so is the point of this backend.
 * Added to plat.h, which needs every backend to answer it. */
int plat_video_ok(void) { return 0; }

void plat_video_frame_begin(void) {}
void plat_video_scene_begin(void) {}
void plat_video_scene_end(void)   {}
void plat_video_present(void)     {}
void plat_video_set_vsync(int on) { (void)on; }

/* ---- 3. geometry ------------------------------------------------------- */

void plat_gfx_draw(plat_prim p, const plat_vertex *v, unsigned nv)
{
    (void)p; (void)v; (void)nv;
}

void plat_gfx_draw_env(plat_prim p, const plat_vertex_env *v, unsigned nv,
                       plat_tex env)
{
    (void)p; (void)v; (void)nv; (void)env;
}

void plat_gfx_draw_indexed(plat_prim p, const plat_vertex *v, unsigned nv,
                           const uint16_t *idx, unsigned ni)
{
    (void)p; (void)v; (void)nv; (void)idx; (void)ni;
}

/* ---- 4. render state --------------------------------------------------- */
/* The shadow cache is implemented HERE rather than in each backend, because it
 * is the original's behaviour (0x00419EBD) and not a backend optimisation. */

static uint32_t g_rs[PLAT_RS__COUNT];
static int      g_rs_valid[PLAT_RS__COUNT];

void plat_gfx_set_state(plat_rs state, uint32_t value)
{
    if (state >= PLAT_RS__COUNT) {
        return;
    }
    if (g_rs_valid[state] && g_rs[state] == value) {
        return;                     /* redundant - the original returns too */
    }
    g_rs[state]       = value;
    g_rs_valid[state] = 1;
}

void plat_gfx_reset_state(void)
{
    memset(g_rs, 0, sizeof g_rs);
    memset(g_rs_valid, 0, sizeof g_rs_valid);
}

/* ---- 5. textures ------------------------------------------------------- */

static plat_tex g_next_tex = 1;     /* 0 is "no texture" */

plat_tex plat_gfx_tex_create(unsigned w, unsigned h,
                             const uint32_t *const *levels, unsigned nlevels)
{
    (void)w; (void)h; (void)levels; (void)nlevels;
    return g_next_tex++;
}

void plat_gfx_tex_destroy(plat_tex t) { (void)t; }
void plat_gfx_tex_update(plat_tex t, const uint32_t *level0)
                                      { (void)t; (void)level0; }
void plat_gfx_tex_bind(plat_tex t)    { (void)t; }
void plat_gfx_tex_smooth(plat_tex t, unsigned nlevels)
                                      { (void)t; (void)nlevels; }

/* plat_gl.c's BGRA upload (gfx2d's canvas).  None here. */
int plat_gfx_tex_bgra(void);
int plat_gfx_tex_bgra(void) { return 0; }
plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px);
plat_tex plat_gfx_tex_create_bgra(unsigned w, unsigned h, const uint32_t *px)
                                      { (void)w; (void)h; (void)px; return 0; }
void plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px);
void plat_gfx_tex_update_bgra(plat_tex t, const uint32_t *px)
                                      { (void)t; (void)px; }

/* ---- 5a. plat_gl.c's off-contract extras -------------------------------- */
/* Not in plat.h (see plat_gl.c), but rally.c calls them, so a build without
 * SDL2 - the aarch64 cross build - needs them here to link.  Each returns what
 * plat_gl.c returns when there is no surface.  The saved-state struct must
 * match the one rally.c and plat_gl.c declare. */

typedef struct {
    uint32_t v[PLAT_RS__COUNT];
    int      valid[PLAT_RS__COUNT];
    plat_tex tex;
} plat_gfx_saved;

int plat_gfx_read_frame(unsigned char *rgba, int w, int h);
int plat_gfx_read_frame(unsigned char *rgba, int w, int h)
{
    (void)rgba; (void)w; (void)h;
    return 0;
}

int plat_gfx_cull_mode(int mode);
int plat_gfx_cull_mode(int mode)
{
    (void)mode;
    return 0;
}

int plat_gfx_pillar(int x, int w);
int plat_gfx_pillar(int x, int w)
{
    (void)x; (void)w;
    return 0;
}

int plat_gfx_inset(int x, int y, int w, int h);
int plat_gfx_inset(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}

int plat_gfx_clear_depth(void);
int plat_gfx_clear_depth(void)
{
    return 0;
}

plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h);
plat_tex plat_gfx_frame_tex_create(unsigned w, unsigned h)
{
    (void)w; (void)h;
    return 0;
}

int plat_gfx_frame_copy(plat_tex t, int w, int h);
int plat_gfx_frame_copy(plat_tex t, int w, int h)
{
    (void)t; (void)w; (void)h;
    return 0;
}

void plat_gfx_state_save(plat_gfx_saved *s);
void plat_gfx_state_save(plat_gfx_saved *s)
{
    memcpy(s->v, g_rs, sizeof(g_rs));
    memcpy(s->valid, g_rs_valid, sizeof(g_rs_valid));
    s->tex = 0;
}

void plat_gfx_state_restore(const plat_gfx_saved *s);
void plat_gfx_state_restore(const plat_gfx_saved *s)
{
    memcpy(g_rs, s->v, sizeof(g_rs));
    memcpy(g_rs_valid, s->valid, sizeof(g_rs_valid));
}

/* ---- 6. audio ---------------------------------------------------------- */
/* The 512-slot pool is the original's (0x0040AF00, cap 0x200), kept here for
 * the same reason as the state shadow: it is behaviour, not a backend detail. */

static int      g_voice_used[PLAT_VOICE_MAX];
static unsigned g_voice_count;

plat_sample plat_audio_sample_load(const void *pcm, unsigned bytes,
                                   unsigned rate, unsigned channels,
                                   unsigned bits)
{
    (void)pcm; (void)bytes; (void)rate; (void)channels; (void)bits;
    return PLAT_SAMPLE_NONE;
}

void plat_audio_sample_free(plat_sample s) { (void)s; }

plat_voice plat_audio_voice_alloc(void)
{
    if (g_voice_count >= PLAT_VOICE_MAX) {
        return PLAT_VOICE_NONE;     /* the original gurus here */
    }
    g_voice_used[g_voice_count] = 1;
    return (plat_voice)g_voice_count++;
}

void plat_audio_voice_free(plat_voice v)
{
    if (v < PLAT_VOICE_MAX) {
        g_voice_used[v] = 0;
    }
}

void plat_audio_voice_bind(plat_voice v, plat_sample s) { (void)v; (void)s; }
void plat_audio_voice_play(plat_voice v, int looping)   { (void)v; (void)looping; }
void plat_audio_voice_stop(plat_voice v)                { (void)v; }
void plat_audio_voice_set_volume(plat_voice v, int mb)  { (void)v; (void)mb; }
void plat_audio_voice_set_pan(plat_voice v, int pan)    { (void)v; (void)pan; }
void plat_audio_voice_set_freq(plat_voice v, unsigned hz) { (void)v; (void)hz; }

void plat_audio_voice_set_pos3d(plat_voice v, float x, float y, float z)
{
    (void)v; (void)x; (void)y; (void)z;
}

int      plat_audio_voice_playing(plat_voice v) { (void)v; return 0; }
unsigned plat_audio_voice_get_pos(plat_voice v) { (void)v; return 0; }

void plat_audio_voice_set_pos(plat_voice v, unsigned off) { (void)v; (void)off; }

void plat_audio_listener(const float pos[3], const float fwd[3],
                         const float up[3])
{
    (void)pos; (void)fwd; (void)up;
}

int  plat_audio_music_play(const char *path, int looping)
{
    (void)path; (void)looping;
    return -1;
}

void plat_audio_music_stop(void) {}

int  plat_audio_stream_play(plat_stream s, const char *path, int looping)
{
    (void)s; (void)path; (void)looping;
    return -1;
}

void plat_audio_stream_stop(plat_stream s) { (void)s; }
void plat_audio_stream_set_volume(plat_stream s, int mb) { (void)s; (void)mb; }
int  plat_audio_stream_playing(plat_stream s) { (void)s; return 0; }
void plat_audio_advance(unsigned ms) { (void)ms; }

/* the volume groups: silent here, as everything else in this section. */
void plat_audio_group_volume(plat_audio_group g, float gain)
{
    (void)g; (void)gain;
}

void plat_audio_voice_set_group(plat_voice v, plat_audio_group g)
{
    (void)v; (void)g;
}

/* ---- 7. input ---------------------------------------------------------- */

void plat_input_poll(void) {}

/* No window, so nobody can ask it to close. */
int plat_quit_requested(void) { return 0; }

/* no pad, so the game takes the keyboard's arms only. */
int plat_pad_read(plat_pad *p) { memset(p, 0, sizeof *p); return 0; }
void plat_gfx_set_stream(int mode) { (void)mode; }
void plat_gfx_set_tap(plat_gfx_tap_fn fn) { (void)fn; }  

int plat_input_key(int scancode)     { (void)scancode;  return 0; }

/* ---- 8. file ----------------------------------------------------------- */
/* Real, via stdio: the format tools and the asset loaders need it long before
 * there is a window, and stdio is not a platform dependency. */

struct plat_file { FILE *fp; };
struct plat_dir  { int unused; };

plat_file *plat_file_open(const char *path, int for_write)
{
    FILE *fp = fopen(path, for_write ? "wb" : "rb");
    plat_file *f;
    if (!fp) {
        return NULL;
    }
    f = (plat_file *)malloc(sizeof *f);
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

void plat_file_close(plat_file *f)
{
    if (f) {
        fclose(f->fp);
        free(f);
    }
}

int64_t plat_file_size(plat_file *f)
{
    long here, end;
    if (!f) {
        return -1;
    }
    here = ftell(f->fp);
    if (fseek(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    end = ftell(f->fp);
    fseek(f->fp, here, SEEK_SET);
    return (int64_t)end;
}

size_t plat_file_read(plat_file *f, void *buf, size_t n)
{
    return f ? fread(buf, 1, n, f->fp) : 0;
}

size_t plat_file_write(plat_file *f, const void *buf, size_t n)
{
    return f ? fwrite(buf, 1, n, f->fp) : 0;
}

int64_t plat_file_seek(plat_file *f, int64_t off, int whence)
{
    if (!f || fseek(f->fp, (long)off, whence) != 0) {
        return -1;
    }
    return (int64_t)ftell(f->fp);
}

int plat_file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
}

int         plat_dir_create(const char *path)  { (void)path; return -1; }
plat_dir   *plat_dir_open(const char *path)    { (void)path; return NULL; }
const char *plat_dir_next(plat_dir *d)         { (void)d; return NULL; }
void        plat_dir_close(plat_dir *d)        { (void)d; }

/* No dependencies, so no portable way to ask: the honest answer is "cannot
 * tell", and the caller falls back to the working directory. */
int plat_exe_dir(char *buf, size_t n)
{
    if (buf && n) {
        buf[0] = '\0';
    }
    return -1;
}

/* ---- 9. time ----------------------------------------------------------- */
/* Real. C99 clock() is coarse but it is portable and this is the null backend;
 * the SDL2 backends use SDL_GetPerformanceCounter. */

uint64_t plat_time_us(void)
{
    return (uint64_t)((double)clock() * 1000000.0 / (double)CLOCKS_PER_SEC);
}

void plat_sleep_ms(unsigned ms) { (void)ms; }

/* ---- 10. memory -------------------------------------------------------- */

void *plat_mem_reserve(size_t bytes) { return malloc(bytes); }

void plat_mem_release(void *p, size_t bytes) { (void)bytes; free(p); }

/* ---- 11. threads and sync --------------------------------------------- */
/* Single-threaded: start runs the function inline, waits are immediate. That
 * is a legitimate null backend and it is also how a deterministic run
 * would want the game to behave. */

struct plat_thread { int unused; };
struct plat_mutex  { int unused; };
struct plat_event  { int set; };

static plat_thread g_thread_stub;
static plat_mutex  g_mutex_stub;

plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name)
{
    (void)name;
    if (fn) {
        fn(arg);
    }
    return &g_thread_stub;
}

void plat_thread_join(plat_thread *t) { (void)t; }

plat_mutex *plat_mutex_create(void)   { return &g_mutex_stub; }
void plat_mutex_lock(plat_mutex *m)   { (void)m; }
void plat_mutex_unlock(plat_mutex *m) { (void)m; }
void plat_mutex_destroy(plat_mutex *m){ (void)m; }

plat_event *plat_event_create(void)
{
    plat_event *e = (plat_event *)malloc(sizeof *e);
    if (e) {
        e->set = 0;
    }
    return e;
}

void plat_event_set(plat_event *e) { if (e) { e->set = 1; } }

int plat_event_wait(plat_event *e, int timeout_ms)
{
    (void)timeout_ms;
    return e && e->set;
}

void plat_event_destroy(plat_event *e) { free(e); }

/* ---- 12. settings ------------------------------------------------------ */

int plat_setting_get(const char *key, char *buf, size_t n)
{
    (void)key;
    if (buf && n) {
        buf[0] = '\0';
    }
    return -1;
}

int plat_setting_set(const char *key, const char *value)
{
    (void)key; (void)value;
    return -1;
}

/* ---- 13. logging ------------------------------------------------------- */

void plat_log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) {
        g_log = stderr;
    }
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

void plat_log_raw(const char *s)
{
    if (!g_log) {
        g_log = stderr;
    }
    fputs(s, g_log);
    fflush(g_log);
}
