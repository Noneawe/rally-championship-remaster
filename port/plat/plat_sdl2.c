/* plat_sdl2.c - the SDL2 backend, BOTH flavours.
 *
 *
 *
 * names two SDL2 backends:
 *
 *     plat_sdl_desktop.c    SDL2 + desktop GL   - development, and the oracle
 *     plat_sdl_handheld.c   SDL2 + GLES2        - the target
 *
 * They are ONE source file, compiled twice. Everything the shim currently does
 * is SDL2, and SDL2 is identical on both; the whole of the difference is the GL
 * context profile asked for at window creation. Two files would have been two
 * copies of the same 500 lines, and the ticket's own warning - "neither backend
 * is allowed to be the only one that builds" - is better served by a source
 * that CANNOT drift than by a second copy that can.
 *
 *     cc -DPLAT_SDL2_GLES2 ...     -> the handheld backend, "sdl2-gles2"
 *     cc ...                       -> the desktop backend,  "sdl2-gl"
 *
 * SCOPE. Implemented for real are the parts that are a one-line SDL2 call and
 * cannot be got wrong - lifecycle, the window and GL context, present, time,
 * sleep, memory, threads, sync, files, directories, logging, and the two input
 * queries that need no binding table. Still stubbed, each saying so at its
 * definition, are ~~audio mixing,~~ settings, and plat_input_action.  (Audio
 * is real since: the mixer is plat_audio_ma.c, the device is here.)
 *
 * THE RENDERER IS NOT HERE. It is plat_gl.c, added on 2026-09-13 and
 * compiled into both SDL2 backends; it owns every plat_gfx_* entry point,
 * plat_video_frame_begin and the scene bracket. This file is SDL2 and issues
 * no GL call; that file is GL and issues no SDL2 call but
 * SDL_GL_GetProcAddress. Keep it that way - it is what lets either be replaced
 * without touching the other.
 *
 * No GL header is included and no GL library is linked ANYWHERE in the port,
 * plat_gl.c included: it declares the GL subset it uses and resolves every
 * entry point through SDL. The GLES2/desktop split lives entirely in
 * plat_gl_profile() below.
 *
 * HEADLESS. Every entry point degrades instead of failing. Measured 2026-09-12
 * under WSL Ubuntu 24.04.3 / SDL2 2.30.0: with WSLg present the x11 driver
 * gives a real GL context in both profiles, and under SDL_VIDEODRIVER=dummy
 * window creation with SDL_WINDOW_OPENGL fails outright. A backend that
 * returned failure there would make `make check` unrunnable on any build
 * machine without a display, so video loss is logged and the backend keeps its
 * recorded mode. plat_video_ok() is how a caller finds out.
*/

#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "plat.h"
#include "plat_audio_ma.h"

/* No translation unit in the port includes SDL.h beside this one, so no
 * main is ever renamed to SDL_main; on Windows that means SDL2main is
 * not linked, and plat_init says so to SDL with SDL_SetMainReady. */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>                 /* _dup2, for plat_detach_own_console */
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* ---- 1. lifecycle ------------------------------------------------------ */

static FILE          *g_log;
static SDL_Window    *g_window;
static SDL_GLContext  g_glctx;
static int            g_w, g_h, g_bpp;
static int            g_fullscreen;
static int            g_video_ok;     /* a real window and context exist */
static int            g_audio_ok;
static int            g_audio_clocked; /* plat_config.audio_clock: no device */
static SDL_GameController *g_pad;  /* the pad, hot-plugged */
static const char    *g_app_name = "rally";

/* present pacing (plat_video_set_vsync).  g_vsync is what the caller
 * asked for; g_swap_interval what the driver accepted (-1 adaptive, 1, or 0
 * when it refused both); g_period_us the display's refresh period, the floor
 * the frame cap holds the present to when the swap does not block. */
static int            g_vsync = 1;
static int            g_swap_interval;
static uint64_t       g_period_us = 1000000u / 60u;
static uint64_t       g_last_present_us;

static void plat_audio_open(void);

/* plat_gl.c's half of the letterbox; not in plat.h. */
void plat_gl_present_begin(int dw, int dh);
void plat_gl_present_end(int dw, int dh);

/* Not in plat.h: the window's drawable size in pixels, for plat_gl.c, which
 * issues no SDL2 call of its own.  0 x 0 without a window. */
void plat_video_drawable_size(int *w, int *h);
void plat_video_drawable_size(int *w, int *h)
{
    int dw = 0, dh = 0;
    if (g_window) {
        SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    }
    if (w) *w = dw;
    if (h) *h = dh;
}

#if defined(_WIN32)
/* no console window for a player's launch.  rally.exe stays a CONSOLE
 * program (the toolchain file's SDL2_NO_MWINDOWS: run from a terminal, its
 * output must reach the terminal, and the terminal must wait for it).  But
 * when Explorer starts it, Windows makes it a console of its own - the only
 * process attached, and stdout a character device.  Then the console is let
 * go (it flashes for a moment) and stdout and stderr go to <app>.log in the
 * working directory, which for a double-click is the executable's folder.
 * Redirected output (a pipe, a file) is left alone. */
static void plat_detach_own_console(const char *app)
{
    DWORD pids[2];
    char path[260];
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!GetConsoleWindow() || GetConsoleProcessList(pids, 2) != 1 ||
        out == INVALID_HANDLE_VALUE || out == NULL ||
        GetFileType(out) != FILE_TYPE_CHAR) {
        return;
    }
    /* beside the executable, not in the working directory: a launch through
     * explorer.exe does not promise its own folder as the working directory,
     * and the first test of this wrote no log at all */
    {
        char *base = SDL_GetBasePath();
        snprintf(path, sizeof path, "%s%s.log", base ? base : "", app);
        SDL_free(base);
    }
    FreeConsole();
    if (freopen(path, "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        /* stderr onto the same file, sharing its offset, so the two
         * interleave in the order they were written */
        _dup2(_fileno(stdout), _fileno(stderr));
        setvbuf(stderr, NULL, _IONBF, 0);
        {
            char cwd[MAX_PATH];
            DWORD n = GetCurrentDirectoryA(sizeof cwd, cwd);
            fprintf(stderr, "PLAT: launched with a console of its own - "
                    "released; output here (working directory %s)\n",
                    n ? cwd : "?");
        }
    }
}
#endif

int plat_init(const plat_config *cfg)
{
    if (cfg && cfg->log_path) {
        g_log = fopen(cfg->log_path, "w");
    }
    if (!g_log) {
        g_log = stderr;
    }
    if (cfg && cfg->app_name) {
        g_app_name = cfg->app_name;
    }
    g_fullscreen = cfg ? cfg->want_fullscreen : 0;
#if defined(_WIN32)
    if (g_log == stderr) {
        plat_detach_own_console(g_app_name);
    }
#endif

    /* SDL_Init with no subsystem flags cannot fail for want of a device, so
     * each subsystem is brought up separately and each is allowed to be
     * absent. A machine with no sound card must still boot far enough to run
     * the format tools and. */
#if defined(_WIN32)
    /* SDL_MAIN_HANDLED (above): main() is the program's own, not SDL2main's
     * WinMain wrapper, and SDL refuses to initialise until told so. */
    SDL_SetMainReady();
    /* DPI-aware, per monitor.  Unaware, Windows told SDL the desktop
     * was 2560x1440 and then stretched the fullscreen window by the display
     * scale (150% on the development host): the letterbox came out 1.5 times
     * too big, cropped, its left bar 480 px instead of 320.  Aware, a pixel is
     * a pixel - and an 800x600 window is 800x600 of them. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_Init(0) != 0) {
        plat_log("PLAT: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        plat_log("PLAT: no video: %s\n", SDL_GetError());
    } else {
        plat_log("PLAT: video driver=%s\n", SDL_GetCurrentVideoDriver());
    }

    if (cfg && cfg->audio_clock) {
        /* plat_audio_advance: the mixer on the caller's clock, no device */
        if (plat_audio_ma_init(48000u, 2) == 0) {
            g_audio_clocked = 1;
            plat_log("PLAT: audio on the caller's clock (no device), 48000 Hz\n");
        }
    } else if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        plat_log("PLAT: no audio: %s\n", SDL_GetError());
    } else {
        g_audio_ok = 1;
        plat_log("PLAT: audio driver=%s\n", SDL_GetCurrentAudioDriver());
        plat_audio_open();
    }

    /* the pad.  The GameController subsystem (which brings up the
     * joystick one) is started here; a controller already attached arrives as
     * SDL_CONTROLLERDEVICEADDED on the first plat_input_poll, so the opening
     * is in one place, pad_attach. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        plat_log("PLAT: no game controller support: %s\n", SDL_GetError());
    }

    plat_log("PLAT: backend=%s sdl=%d.%d.%d\n", plat_backend_name(),
             SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    return 0;
}

static void plat_audio_close(void);

void plat_shutdown(void)
{
    plat_audio_close();
    if (g_pad) {
        SDL_GameControllerClose(g_pad);
        g_pad = NULL;
    }
    if (g_glctx) {
        SDL_GL_DeleteContext(g_glctx);
        g_glctx = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    g_video_ok = 0;
    g_audio_ok = 0;
    SDL_Quit();

    if (g_log && g_log != stderr) {
        fclose(g_log);
    }
    g_log = NULL;
}

const char *plat_backend_name(void)
{
#if defined(PLAT_SDL2_GLES2)
    return "sdl2-gles2";
#else
    return "sdl2-gl";
#endif
}

/* ---- 2. video ---------------------------------------------------------- */
/* The ONLY place the two backends differ. Both profiles were measured to
 * produce a context under WSLg/Mesa on 2026-09-12, so the split is exercised on
 * the development host and not merely compiled. */

static void plat_gl_profile(void)
{
    SDL_GL_ResetAttributes();
#if defined(PLAT_SDL2_GLES2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* 16-bit depth is a floor, not a transcription: what the original asked
     * DirectDraw for has not been read. owns the real answer. */
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
}

int plat_video_set_mode(int width, int height, int bpp, int fullscreen)
{
    /* RESIZABLE: the game draws at width x height whatever the window
     * is, and plat_gl.c letterboxes it into the window at the present. */
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

    g_w = width; g_h = height; g_bpp = bpp; g_fullscreen = fullscreen;

    if (g_glctx) {
        SDL_GL_DeleteContext(g_glctx);
        g_glctx = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    g_video_ok = 0;

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        plat_log("PLAT: set_mode %dx%d %dbpp - headless, mode recorded only\n",
                 width, height, bpp);
        return 0;
    }

    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    plat_gl_profile();
    g_window = SDL_CreateWindow(g_app_name,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                width, height, flags);
    if (!g_window) {
        plat_log("PLAT: no window (%s) - headless, mode recorded only\n",
                 SDL_GetError());
        return 0;
    }

    g_glctx = SDL_GL_CreateContext(g_window);
    if (!g_glctx) {
        plat_log("PLAT: no GL context (%s) - window kept, no present\n",
                 SDL_GetError());
        return 0;
    }

    SDL_GL_MakeCurrent(g_window, g_glctx);
    g_video_ok = 1;
    {
        SDL_DisplayMode dm;
        int d = SDL_GetWindowDisplayIndex(g_window);
        if (d >= 0 && SDL_GetCurrentDisplayMode(d, &dm) == 0 &&
            dm.refresh_rate > 0) {
            g_period_us = 1000000u / (unsigned)dm.refresh_rate;
        }
    }
    plat_video_set_vsync(g_vsync);
    {
        int dw, dh;
        plat_video_drawable_size(&dw, &dh);
        plat_log("PLAT: mode %dx%d %dbpp fullscreen=%d profile=%s "
                 "drawable %dx%d refresh %u Hz swap interval %d\n",
                 width, height, bpp, fullscreen, plat_backend_name(), dw, dh,
                 (unsigned)(1000000u / g_period_us), g_swap_interval);
    }
    return 0;
}

void plat_video_set_vsync(int on)
{
    g_vsync = on ? 1 : 0;
    if (!g_video_ok) {
        return;
    }
    /* Adaptive first (a late frame tears rather than waits a whole refresh),
     * then plain vsync; a driver that refuses both leaves the frame cap in
     * plat_video_present to do the pacing on its own. */
    if (!g_vsync) {
        SDL_GL_SetSwapInterval(0);
        g_swap_interval = 0;
    } else if (SDL_GL_SetSwapInterval(-1) == 0) {
        g_swap_interval = -1;
    } else if (SDL_GL_SetSwapInterval(1) == 0) {
        g_swap_interval = 1;
    } else {
        g_swap_interval = 0;
    }
}

/* Alt+Enter: fullscreen at the desktop's own mode, or back to the
 * window.  The context survives; plat_gl.c sees the new drawable size at the
 * next present. */
static void plat_toggle_fullscreen(void)
{
    if (!g_window) {
        return;
    }
    g_fullscreen = !g_fullscreen;
    if (SDL_SetWindowFullscreen(g_window, g_fullscreen
                                ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
        plat_log("PLAT: fullscreen toggle failed: %s\n", SDL_GetError());
        g_fullscreen = !g_fullscreen;
    }
}

void plat_video_get_mode(int *width, int *height, int *bpp)
{
    if (width)  *width  = g_w;
    if (height) *height = g_h;
    if (bpp)    *bpp    = g_bpp;
}

/* Not in plat.h. It exists so a caller - the selftest, and later - can
 * tell a real surface from a degraded one without parsing the log. If it is
 * still wanted when the renderer lands, it belongs in the header. */
/* Promoted into plat.h: the renderer has to be able to tell a real
 * surface from a degraded one, and the comment that used to sit here said it
 * belonged in the header if it was still wanted when the renderer landed. */
int plat_video_ok(void) { return g_video_ok; }

void plat_video_present(void)     /* was ddsflip 0x00405518 */
{
    int dw, dh;
    uint64_t now;

    if (!g_video_ok) {
        return;
    }
    /* when the window is not the game's size (fullscreen, or a
     * resized window), the frame was drawn off-screen at the game's size and
     * is scaled into the window here, letterboxed. */
    plat_video_drawable_size(&dw, &dh);
    plat_gl_present_begin(dw, dh);
    SDL_GL_SwapWindow(g_window);
    plat_gl_present_end(dw, dh);

    /* The frame cap: where the swap did not wait for the display (no vsync,
     * or a driver that ignores it), sleep out the rest of a refresh period.
     * 95% of it, so a swap that DID wait is never held a second time. */
    now = plat_time_us();
    if (g_vsync && g_last_present_us && now - g_last_present_us
                                            < g_period_us * 95u / 100u) {
        uint64_t left = g_period_us * 95u / 100u - (now - g_last_present_us);
        if (left >= 1000u) {
            SDL_Delay((Uint32)(left / 1000u));
        }
        now = plat_time_us();
    }
    g_last_present_us = now;
}

/* ---- 3, 4, 5. geometry, render state, textures -------------------------- */
/* NOT HERE ANY MORE. plat_gl.c is compiled into both SDL2 backends and owns
 * every plat_gfx_* entry point, plat_video_frame_begin and the scene bracket.
 *
 *
 * The split is on purpose: this file is SDL2 and contains no GL call, plat_gl.c
 * is GL and contains no SDL2 call except SDL_GL_GetProcAddress. Either can be
 * replaced without touching the other, which is what a port to a platform with
 * a different window system will want.
 *
 * The counters and the shadow cache moved with the code. If the shadow copy in
 * plat_gl.c and the one in plat_null.c ever diverge, believe plat_null.c - it
 * is the one runs against. */

/* ---- 6. audio ---------------------------------------------------------- */
/* every plat_audio_* entry point is plat_audio_ma.c's,
 * miniaudio mixing in its no-device mode.  What lives HERE is the device:
 * SDL2's, opened once at plat_init and pulled by its callback, so SDL2 stays
 * the only audio stack on every target (the ticket's decision - on
 * Anbernic-class firmware SDL2 is the audio path known to be configured).
 *
 * Float, stereo, at whatever rate the device prefers (SDL_AUDIO_ALLOW_
 * FREQUENCY_CHANGE: the mixer resamples, so asking for a rate SDL would
 * then convert is a second resampler for nothing).  512 frames is about
 * 11 ms at 48 kHz.
 *
 * The callback is the audio thread.  It calls the mixer and nothing else.
 *
 * FALLBACK, NOT BUILT: a target with miniaudio but no SDL audio would open
 * miniaudio's own device instead (drop MA_NO_DEVICE_IO, give ma_engine a
 * device).  Recorded, not written. */

static SDL_AudioDeviceID g_audio_dev;

static void plat_audio_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    plat_audio_ma_render((float *)(void *)stream,
                         (unsigned)len / (2 * (unsigned)sizeof(float)));
}

static void plat_audio_open(void)
{
    SDL_AudioSpec want, have;

    SDL_zero(want);
    want.freq     = 48000;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = 512;
    want.callback = plat_audio_cb;
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (g_audio_dev == 0) {
        plat_log("PLAT: no audio device: %s - silent\n", SDL_GetError());
        return;
    }
    if (plat_audio_ma_init((unsigned)have.freq, 2) != 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
        return;
    }
    plat_log("PLAT: audio device %d Hz, %u frames\n", have.freq,
             (unsigned)have.samples);
    SDL_PauseAudioDevice(g_audio_dev, 0);
}

static void plat_audio_close(void)
{
    /* The device first: once it is closed the callback cannot be running,
     * and only then may the mixer's state go. */
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    g_audio_clocked = 0;
    plat_audio_ma_shutdown();
}

void plat_audio_advance(unsigned ms)
{
    static float buf[512 * 2];
    unsigned long frames;

    if (!g_audio_clocked) return;
    frames = (unsigned long)ms * 48u;              /* 48000 Hz */
    while (frames) {
        unsigned k = frames > 512u ? 512u : (unsigned)frames;
        plat_audio_ma_render(buf, k);
        frames -= k;
    }
}

/* ---- 7. input ---------------------------------------------------------- */
/* All real since: the event pump, the quit request, the keyboard and the
 * pad (raw).
 *
 * The RAMP IS NOT HERE and must never move here. 0x004383C3's 0xA00000-per-tick
 * integration and its ADD EAX,EAX re-centring are gameplay feel running on the
 * 10 ms step (sections 2-3). Whether an input takes the
 * ramp is FUN_00408D37's choice per binding, in game code (section 15). */

/* Set by plat_input_poll on SDL_QUIT; plat_quit_requested reports it. */
static int g_quit_requested;

/* hot-plug.  The first controller attached is the pad; when it goes,
 * the next one still attached takes its place. */
static void pad_attach(int index)
{
    if (g_pad || !SDL_IsGameController(index)) return;
    g_pad = SDL_GameControllerOpen(index);
    plat_log("PLAT: pad=%s\n", g_pad ? SDL_GameControllerName(g_pad)
                                    : SDL_GetError());
}

static void pad_detach(SDL_JoystickID id)
{
    int i;
    if (!g_pad ||
        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) != id)
        return;
    SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    plat_log("PLAT: pad removed\n");
    for (i = 0; i < SDL_NumJoysticks() && !g_pad; i++) pad_attach(i);
}

void plat_input_poll(void)
{
    SDL_Event ev;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        return;
    }
    /* Drained, not dispatched. The window must stay responsive from the first
     * frame; game actions poll plat_input_key with the game's own bindings
     * (port/game/car_keys.h), so nothing here needs routing.
     * ~~SDL_QUIT is deliberately NOT handled~~ -: SDL_QUIT is RECORDED
     * and reported by plat_quit_requested, and still not acted on.  The
     * original gets the same request as WM_DESTROY in plat_wndproc_q; what it
     * does with it is the game's, so the backend only says it happened. */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            g_quit_requested = 1;
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat &&
                   (ev.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                    ev.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) &&
                   (ev.key.keysym.mod & KMOD_ALT)) {
            /* Alt+Enter, the Windows convention - the platform's,
             * not the game's; plat_input_key hides that RETURN. */
            plat_toggle_fullscreen();
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            pad_attach(ev.cdevice.which);
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            pad_detach(ev.cdevice.which);
        }
    }
}

int plat_quit_requested(void)
{
    /* the handhelds' convention, gptokeyb's kill combo done here: Select and
     * Start held together quit, as closing the window does */
    if (g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
        SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))
        g_quit_requested = 1;
    return g_quit_requested;
}

/* REAL: the pad, raw.  ~~plat_input_axis, plat_input_axis_is_analogue,
 * plat_input_action~~ - retired; plat.h section 7 says why. */
int plat_pad_read(plat_pad *p)
{
    static const struct { SDL_GameControllerButton b; unsigned bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, PLAT_PAD_A },
        { SDL_CONTROLLER_BUTTON_B, PLAT_PAD_B },
        { SDL_CONTROLLER_BUTTON_X, PLAT_PAD_X },
        { SDL_CONTROLLER_BUTTON_Y, PLAT_PAD_Y },
        { SDL_CONTROLLER_BUTTON_BACK, PLAT_PAD_BACK },
        { SDL_CONTROLLER_BUTTON_GUIDE, PLAT_PAD_GUIDE },
        { SDL_CONTROLLER_BUTTON_START, PLAT_PAD_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, PLAT_PAD_LSTICK },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PLAT_PAD_RSTICK },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PLAT_PAD_LB },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PLAT_PAD_RB },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PLAT_PAD_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PLAT_PAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PLAT_PAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PLAT_PAD_RIGHT }
    };
    size_t i;
    memset(p, 0, sizeof *p);
    if (!g_pad) {
        /* A TEST HOOK, for a headless run: RALLY_FAKE_PAD="buttons,lx,ly,lt,rt
         * [@N]" is a pad - buttons in hex (PLAT_PAD_* bits), the rest as the
         * struct's - at rest for its first N reads when "@N" ends it (the
         * original's program controller wants a quiet pad before a press). */
        const char *e = getenv("RALLY_FAKE_PAD");
        static long reads;
        long rest = 0;
        unsigned b = 0;
        int lx = 0, ly = 0, lt = 0, rt = 0;
        const char *a;
        if (!e) return 0;
        a = strchr(e, '@');
        if (a) rest = atol(a + 1);
        p->present = 1;
        if (reads++ < rest) return 1;
        if (sscanf(e, "%x,%d,%d,%d,%d", &b, &lx, &ly, &lt, &rt) < 1) {
            p->present = 0;
            return 0;
        }
        p->buttons = b;
        p->lx = (int16_t)lx; p->ly = (int16_t)ly;
        p->lt = (int16_t)lt; p->rt = (int16_t)rt;
        return 1;
    }
    p->present = 1;
    p->lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    p->ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    p->rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    p->ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    p->lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    p->rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(g_pad, map[i].b)) p->buttons |= map[i].bit;
    return 1;
}

/* REAL. The argument is an SDL scancode (SDL_SCANCODE_*).  The original's key
 * bytes are neither SDL nor DirectInput codes but its own folded numbering
 * (0x00407809); port/game/car_keys.h's car_key_sdl is the map. */
int plat_input_key(int scancode)
{
    int n = 0;
    const Uint8 *keys;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        return 0;
    }
    keys = SDL_GetKeyboardState(&n);
    if (!keys || scancode < 0 || scancode >= n) {
        return 0;
    }
    /* RETURN with Alt held is the fullscreen toggle (plat_input_poll),
     * so the game does not also see it - not a menu choice, not a camera
     * view. */
    if ((scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER)
        && (SDL_GetModState() & KMOD_ALT)) {
        return 0;
    }
    return keys[scancode] ? 1 : 0;
}

/* ---- 8. file ----------------------------------------------------------- */
/* REAL, via stdio and POSIX. Thin on purpose: vfs_resolve_path (0x00402FEB)
 * stays ABOVE this layer, so the ;3 / ;4 volume prefixes and the % language
 * expansion never reach it. SDL_RWops is deliberately not used - it buys
 * nothing here and the game does its own buffering. */

struct plat_file { FILE *fp; };

plat_file *plat_file_open(const char *path, int for_write)
{
    FILE *fp;
    plat_file *f;
    if (!path) {
        return NULL;
    }
    fp = fopen(path, for_write ? "wb" : "rb");
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

/* 64-bit throughout, unlike plat_null.c's long: the largest single corpus in
 * the project is already 66 MB of .sq and nothing guarantees a 64-bit long. */
int64_t plat_file_size(plat_file *f)
{
    int64_t here, end;
    if (!f) {
        return -1;
    }
#if defined(_WIN32)
    /* long is 32 bits on Win64, so the 64-bit pair, not ftell/fseek. */
    here = (int64_t)_ftelli64(f->fp);
    if (_fseeki64(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    end = (int64_t)_ftelli64(f->fp);
    _fseeki64(f->fp, here, SEEK_SET);
#else
    here = (int64_t)ftello(f->fp);
    if (fseeko(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    end = (int64_t)ftello(f->fp);
    fseeko(f->fp, (off_t)here, SEEK_SET);
#endif
    return end;
}

size_t plat_file_read(plat_file *f, void *buf, size_t n)
{
    return (f && buf) ? fread(buf, 1, n, f->fp) : 0;
}

size_t plat_file_write(plat_file *f, const void *buf, size_t n)
{
    return (f && buf) ? fwrite(buf, 1, n, f->fp) : 0;
}

int64_t plat_file_seek(plat_file *f, int64_t off, int whence)
{
    if (!f) {
        return -1;
    }
#if defined(_WIN32)
    if (_fseeki64(f->fp, off, whence) != 0) {
        return -1;
    }
    return (int64_t)_ftelli64(f->fp);
#else
    if (fseeko(f->fp, (off_t)off, whence) != 0) {
        return -1;
    }
    return (int64_t)ftello(f->fp);
#endif
}

int plat_file_exists(const char *path)
{
#if defined(_WIN32)
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
#else
    struct stat st;
    return path && stat(path, &st) == 0;
#endif
}

/* SDL_GetBasePath - the executable's folder, separator included, on
 * every target SDL2 supports. */
int plat_exe_dir(char *buf, size_t n)
{
    char *p;
    if (!buf || n == 0) {
        return -1;
    }
    buf[0] = '\0';
    p = SDL_GetBasePath();
    if (!p) {
        return -1;
    }
    if (strlen(p) >= n) {
        SDL_free(p);
        return -1;
    }
    memcpy(buf, p, strlen(p) + 1);
    SDL_free(p);
    return 0;
}

/* Directory enumeration. The _WIN32 arm was a stub until made Windows a
 * target that runs; it keeps the POSIX arm's contract: "." and ".." skipped,
 * and the name valid until the next call on the same handle. */

#if defined(_WIN32)

struct plat_dir {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              have;   /* fd holds an entry not yet returned */
};

int plat_dir_create(const char *path)
{
    DWORD a;
    if (!path) {
        return -1;
    }
    if (CreateDirectoryA(path, NULL)) {
        return 0;
    }
    a = GetFileAttributesA(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;
    }
    return -1;
}

plat_dir *plat_dir_open(const char *path)
{
    plat_dir *d;
    char      pat[MAX_PATH];
    size_t    n;
    if (!path) {
        return NULL;
    }
    n = strlen(path);
    if (n + 3 > sizeof pat) {
        return NULL;
    }
    memcpy(pat, path, n);
    if (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') {
        pat[n++] = '\\';
    }
    pat[n++] = '*';
    pat[n]   = '\0';
    d = (plat_dir *)malloc(sizeof *d);
    if (!d) {
        return NULL;
    }
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->have = 1;
    return d;
}

const char *plat_dir_next(plat_dir *d)
{
    if (!d) {
        return NULL;
    }
    for (;;) {
        if (!d->have && !FindNextFileA(d->h, &d->fd)) {
            return NULL;
        }
        d->have = 0;
        if (strcmp(d->fd.cFileName, ".") != 0 && strcmp(d->fd.cFileName, "..") != 0) {
            return d->fd.cFileName;
        }
    }
}

void plat_dir_close(plat_dir *d)
{
    if (d) {
        FindClose(d->h);
        free(d);
    }
}

#else

struct plat_dir { DIR *d; };

int plat_dir_create(const char *path)
{
    struct stat st;
    if (!path) {
        return -1;
    }
    if (mkdir(path, 0777) == 0) {
        return 0;
    }
    /* Already there is success: every caller of this wants the directory to
     * exist, not to have been the one that made it. */
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    return -1;
}

plat_dir *plat_dir_open(const char *path)
{
    plat_dir *h;
    DIR *d = path ? opendir(path) : NULL;
    if (!d) {
        return NULL;
    }
    h = (plat_dir *)malloc(sizeof *h);
    if (!h) {
        closedir(d);
        return NULL;
    }
    h->d = d;
    return h;
}

/* The returned pointer is owned by the DIR and is valid until the next call on
 * the same handle - the same contract readdir has. "." and ".." are skipped,
 * because every caller of this in the original is enumerating assets. */
const char *plat_dir_next(plat_dir *h)
{
    struct dirent *e;
    if (!h) {
        return NULL;
    }
    while ((e = readdir(h->d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        return e->d_name;
    }
    return NULL;
}

void plat_dir_close(plat_dir *h)
{
    if (h) {
        closedir(h->d);
        free(h);
    }
}

#endif

/* ---- 9. time ----------------------------------------------------------- */
/* REAL. The simulation quantum is 10 ms and the accumulator is real
 * so SDL_GetTicks' millisecond resolution is not enough;
 * this is the performance counter, scaled to microseconds without overflowing
 * at 2^64/1e6 by dividing the whole seconds out first. */

uint64_t plat_time_us(void)
{
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 now  = SDL_GetPerformanceCounter();
    if (freq == 0) {
        return (uint64_t)SDL_GetTicks() * 1000u;
    }
    return (uint64_t)((now / freq) * 1000000u + ((now % freq) * 1000000u) / freq);
}

void plat_sleep_ms(unsigned ms) { SDL_Delay(ms); }

/* ---- 10. memory -------------------------------------------------------- */
/* Replaces VirtualAlloc / VirtualFree. malloc is the right answer on both
 * targets: the game manages its own heaps above this (imageheapsize,
 * wavheapsize), so what it wants from the platform is one large block, not a
 * reserve/commit split. */

void *plat_mem_reserve(size_t bytes)          { return malloc(bytes); }
void  plat_mem_release(void *p, size_t bytes) { (void)bytes; free(p); }

/* ---- 11. threads and sync --------------------------------------------- */
/* REAL, via SDL's own primitives - they exist on both targets and cost
 * nothing. Whether the port KEEPS INPUTTDB and AUDIOTDB as threads is still
 * the and the question (item 4); this
 * implements the surface either way. */

struct plat_thread {
    SDL_Thread *t;
    void      (*fn)(void *);
    void       *arg;
};

static int plat_thread_trampoline(void *p)
{
    plat_thread *th = (plat_thread *)p;
    if (th->fn) {
        th->fn(th->arg);
    }
    return 0;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name)
{
    plat_thread *th = (plat_thread *)malloc(sizeof *th);
    if (!th) {
        return NULL;
    }
    th->fn  = fn;
    th->arg = arg;
    th->t   = SDL_CreateThread(plat_thread_trampoline, name ? name : "plat", th);
    if (!th->t) {
        /* Run it inline rather than dropping it, which is what plat_null.c
         * does unconditionally. A caller that joins still sees it finish. */
        plat_log("PLAT: thread '%s' inline: %s\n", name ? name : "plat",
                 SDL_GetError());
        plat_thread_trampoline(th);
    }
    return th;
}

void plat_thread_join(plat_thread *th)
{
    if (!th) {
        return;
    }
    if (th->t) {
        SDL_WaitThread(th->t, NULL);
    }
    free(th);
}

struct plat_mutex { SDL_mutex *m; };

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = (plat_mutex *)malloc(sizeof *m);
    if (!m) {
        return NULL;
    }
    m->m = SDL_CreateMutex();
    return m;
}

void plat_mutex_lock(plat_mutex *m)   { if (m && m->m) { SDL_LockMutex(m->m); } }
void plat_mutex_unlock(plat_mutex *m) { if (m && m->m) { SDL_UnlockMutex(m->m); } }

void plat_mutex_destroy(plat_mutex *m)
{
    if (m) {
        if (m->m) {
            SDL_DestroyMutex(m->m);
        }
        free(m);
    }
}

/* MANUAL RESET - the flag survives a successful wait - because plat.h has no
 * plat_event_reset and plat_null.c does not consume either, so consuming here
 * would make the two backends disagree about a thing diffs. Which
 * flavour the original's Win32 events were has NOT been read; that is an open
 * question with as owner, and the header will need a reset entry point if
 * the answer turns out to be auto-reset. */

struct plat_event {
    SDL_mutex *m;
    SDL_cond  *c;
    int        set;
};

plat_event *plat_event_create(void)
{
    plat_event *e = (plat_event *)malloc(sizeof *e);
    if (!e) {
        return NULL;
    }
    e->m   = SDL_CreateMutex();
    e->c   = SDL_CreateCond();
    e->set = 0;
    return e;
}

void plat_event_set(plat_event *e)
{
    if (!e || !e->m || !e->c) {
        return;
    }
    SDL_LockMutex(e->m);
    e->set = 1;
    SDL_CondBroadcast(e->c);
    SDL_UnlockMutex(e->m);
}

/* timeout_ms < 0 waits forever, 0 polls, > 0 bounds the wait. Returns 1 if the
 * event is set, 0 on timeout. */
int plat_event_wait(plat_event *e, int timeout_ms)
{
    int got;
    if (!e || !e->m || !e->c) {
        return 0;
    }
    SDL_LockMutex(e->m);
    if (!e->set) {
        if (timeout_ms < 0) {
            while (!e->set) {
                SDL_CondWait(e->c, e->m);
            }
        } else if (timeout_ms > 0) {
            /* Looped against a deadline, not a single CondWaitTimeout: a
             * condition variable may wake spuriously, and a sync primitive
             * that reports "timed out" early is the kind of bug that shows up
             * once in ten thousand frames. The clock is plat_time_us, in ms:
             * SDL_GetTicks64 needs SDL >= 2.0.18, and the handheld build
             * compiles against an older SDL (Debian bullseye's 2.0.14). */
            Uint64 deadline = plat_time_us() / 1000u + (Uint64)timeout_ms;
            while (!e->set) {
                Uint64 now = plat_time_us() / 1000u;
                if (now >= deadline) {
                    break;
                }
                SDL_CondWaitTimeout(e->c, e->m, (Uint32)(deadline - now));
            }
        }
    }
    got = e->set;
    SDL_UnlockMutex(e->m);
    return got;
}

void plat_event_destroy(plat_event *e)
{
    if (e) {
        if (e->c) {
            SDL_DestroyCond(e->c);
        }
        if (e->m) {
            SDL_DestroyMutex(e->m);
        }
        free(e);
    }
}

/* ---- 12. settings ------------------------------------------------------ */
/* STUB. This replaces the seven ADVAPI32 Reg* imports and it is load-bearing at
 * boot - shows the installer-vs-game choice comes from
 * the REGISTRY, not the command line. A file-backed key/value store under
 * SDL_GetPrefPath is the obvious implementation and it is NOT written here,
 * because the key set has not been enumerated and a store with the wrong keys
 * in it is worse than one that honestly says it has none. Owner: M5.
 *
 * Returning -1 means "no value", which is the same answer the game gets from a
 * clean machine on first run, so no caller is surprised by it. */

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
/* REAL, and the tag format is preserved verbatim so an original log and a
 * ported log diff directly. SDL_Log is deliberately not
 * used: it adds its own prefix and would break that diff. */

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

/* Not in plat.h, and not called by the game: a bring-up aid. The draw counters
 * moved to plat_gl.c with the renderer, so this asks that file for them rather
 * than keeping a second set that could disagree. */
void plat_gfx_counters(unsigned long *draws, unsigned long *verts,
                       unsigned long *indices);

void plat_sdl2_dump_counters(void);
void plat_sdl2_dump_counters(void)
{
    unsigned long draws = 0, verts = 0, indices = 0;

    plat_gfx_counters(&draws, &verts, &indices);
    plat_log("PLAT: draws=%lu verts=%lu indices=%lu audio=%d mixer=%d\n",
             draws, verts, indices, g_audio_ok, g_audio_dev != 0);
}
