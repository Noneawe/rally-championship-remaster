/* plat_audio_ma.c - plat.h section 6 on miniaudio.
 *
 * THE DECISION (the user's, 2026-10-10): the platform layer is a remaster,
 * so the mixer is a modern library and not a transcription of how the
 * original drove DirectSound and A3D.  WHAT plays and WHEN stays the game's
 * (..); this file only makes the twelve operations real.
 *
 * miniaudio (port/third_party/miniaudio, version and licence in
 * runs in its NO-DEVICE mode: an ma_engine that mixes, resamples
 * and spatialises, and that someone else pulls.  plat_sdl2.c pulls it from
 * SDL2's audio callback, so SDL2 stays the only device layer on every target;
 * the audio-mix gate pulls it with no device at all.  No SDL here.
 *
 * WHAT IS KEPT FROM THE ORIGINAL
 *  the 512-slot BUMP allocator: free
 *     releases a voice's sound but never returns the slot.  Reclaiming would
 *     be the deviation.
 *  DirectSound's units on every call (plat.h section 6 lists them), since
 *     the game code above computes them for DirectSound.
 *
 * THREADS.  The game thread calls everything below except
 * plat_audio_ma_render, which runs on the audio thread and touches only
 * miniaudio's engine.  The hand-over is miniaudio's: its sound and node
 * setters are atomics the mixer reads, so no game state is shared and there
 * is no lock of ours.  (ma_audio_buffer_ref's cursor is read here for
 * get_pos while the mixer advances it: a racy but untorn 64-bit read on the
 * targets the port builds for.)
*/

#include "plat.h"
#include "plat_audio_ma.h"

#include "miniaudio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PLAT_SAMPLE_MAX 1024    /* the install has 451 .wav */

typedef struct {
    int       used;
    void     *pcm;              /* our copy */
    unsigned  bytes, rate, channels, bits;
    ma_uint64 frames;
} sample;

typedef struct {
    int         used;           /* allocated and not freed */
    plat_sample smp;            /* PLAT_SAMPLE_NONE = unbound */
    int         live;           /* ref and snd are initialised */
    ma_audio_buffer_ref ref;    /* this voice's own cursor over the sample */
    ma_sound    snd;
    /* the settings, kept across bind so they may come before it */
    int         mb, pan;
    unsigned    hz;             /* 0 = the sample's own rate */
    int         group;
    int         spatial;
    float       pos[3];
} voice;

static int       g_ready;
static unsigned  g_channels = 2;    /* the device's; silence is this wide */
static ma_engine g_engine;
static ma_sound_group g_group[PLAT_AUDIO__GROUPS];  /* [MASTER] unused */
static sample    g_smp[PLAT_SAMPLE_MAX];
static voice     g_voice[PLAT_VOICE_MAX];
static unsigned  g_voice_count;     /* the original's 0x004F3308 */
/* the two streams (plat.h section 6): MUSIC, SPEECH */
typedef struct {
    ma_sound snd;
    int      live;              /* snd is initialised */
    int      mb;                /* the volume, kept across plays */
} stream;
static stream    g_stream[PLAT_STREAM__COUNT];

/* ---- the unit conversions: DirectSound's, stated once -------------------- */

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* SetVolume: hundredths of a dB of attenuation, DSBVOLUME_MIN..MAX. */
static float mb_gain(int mb)
{
    mb = clampi(mb, -10000, 0);
    return (float)pow(10.0, (double)mb / 2000.0);
}

/* SetPan: the far channel is cut by |pan| hundredths of a dB and the near
 * one is untouched.  miniaudio's BALANCE mode scales the far channel by
 * (1 - |p|) and leaves the near one alone (ma_stereo_balance_pcm_frames_f32),
 * so p = sign(pan) * (1 - 10^(-|pan|/2000)) is DirectSound's law exactly. */
static float pan_balance(int pan)
{
    float cut;
    pan = clampi(pan, -10000, 10000);
    cut = 1.0f - (float)pow(10.0, -(double)abs(pan) / 2000.0);
    return pan < 0 ? -cut : cut;
}

static unsigned frame_bytes(const sample *s) { return s->channels * (s->bits / 8); }

static int voice_ok(plat_voice v) { return v < g_voice_count && g_voice[v].used; }

static ma_node *group_node(int g)
{
    return (g == PLAT_AUDIO_MASTER) ? ma_engine_get_endpoint(&g_engine)
                                    : (ma_node *)&g_group[g];
}

static void voice_apply(voice *p)
{
    const sample *s = &g_smp[p->smp];
    ma_sound_set_volume(&p->snd, mb_gain(p->mb));
    ma_sound_set_pan(&p->snd, pan_balance(p->pan));
    ma_sound_set_pitch(&p->snd, p->hz ? (float)p->hz / (float)s->rate : 1.0f);
    ma_sound_set_spatialization_enabled(&p->snd, p->spatial ? MA_TRUE : MA_FALSE);
    if (p->spatial) {
        ma_sound_set_position(&p->snd, p->pos[0], p->pos[1], p->pos[2]);
    }
}

static void voice_unbind(voice *p)
{
    if (p->live) {
        ma_sound_uninit(&p->snd);
        ma_audio_buffer_ref_uninit(&p->ref);
        p->live = 0;
    }
    p->smp = PLAT_SAMPLE_NONE;
}

/* ---- the owner's three calls --------------------------------------------- */

int plat_audio_ma_init(unsigned rate, unsigned channels)
{
    ma_engine_config cfg;
    int g;

    if (g_ready) return 0;
    g_channels = channels;
    cfg = ma_engine_config_init();
    cfg.noDevice   = MA_TRUE;
    cfg.channels   = channels;
    cfg.sampleRate = rate;
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        plat_log("AUDIO: ma_engine_init failed (%u Hz, %u ch)\n", rate, channels);
        return -1;
    }
    /* A group is itself a sound: left spatialised, it would place the whole
     * group's mix at the origin. */
    for (g = PLAT_AUDIO_EFFECTS; g < PLAT_AUDIO__GROUPS; g++) {
        if (ma_sound_group_init(&g_engine, MA_SOUND_FLAG_NO_SPATIALIZATION |
                                MA_SOUND_FLAG_NO_PITCH, NULL, &g_group[g])
            != MA_SUCCESS) {
            plat_log("AUDIO: group %d failed\n", g);
            while (--g >= PLAT_AUDIO_EFFECTS) ma_sound_group_uninit(&g_group[g]);
            ma_engine_uninit(&g_engine);
            return -1;
        }
    }
    g_ready = 1;
    plat_log("AUDIO: miniaudio %s, %u Hz, %u ch\n", ma_version_string(),
             rate, channels);
    return 0;
}

void plat_audio_ma_shutdown(void)
{
    unsigned i;
    int g;

    if (g_ready) {
        for (i = 0; i < PLAT_STREAM__COUNT; i++) plat_audio_stream_stop((plat_stream)i);
        for (i = 0; i < g_voice_count; i++) voice_unbind(&g_voice[i]);
        for (g = PLAT_AUDIO_EFFECTS; g < PLAT_AUDIO__GROUPS; g++)
            ma_sound_group_uninit(&g_group[g]);
        ma_engine_uninit(&g_engine);
        g_ready = 0;
    }
    for (i = 0; i < PLAT_SAMPLE_MAX; i++) {
        free(g_smp[i].pcm);
        memset(&g_smp[i], 0, sizeof g_smp[i]);
    }
}

void plat_audio_ma_render(float *out, unsigned frames)
{
    if (!g_ready) {
        memset(out, 0, (size_t)frames * g_channels * sizeof *out);
        return;
    }
    ma_engine_read_pcm_frames(&g_engine, out, frames, NULL);
}

/* ---- samples -------------------------------------------------------------- */

plat_sample plat_audio_sample_load(const void *pcm, unsigned bytes,
                                   unsigned rate, unsigned channels,
                                   unsigned bits)
{
    unsigned i, fb;

    if (!pcm || bytes == 0 || rate == 0 || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16)) {
        if (pcm) plat_log("AUDIO: sample refused: %u bytes %u Hz %u ch %u bits\n",
                          bytes, rate, channels, bits);
        return PLAT_SAMPLE_NONE;
    }
    fb = channels * (bits / 8);
    for (i = 0; i < PLAT_SAMPLE_MAX; i++) {
        if (!g_smp[i].used) break;
    }
    if (i == PLAT_SAMPLE_MAX) {
        plat_log("AUDIO: sample table full (%d)\n", PLAT_SAMPLE_MAX);
        return PLAT_SAMPLE_NONE;
    }
    bytes -= bytes % fb;            /* a torn last frame is not played */
    if (bytes == 0 || !(g_smp[i].pcm = malloc(bytes))) return PLAT_SAMPLE_NONE;
    memcpy(g_smp[i].pcm, pcm, bytes);
    g_smp[i].used = 1;
    g_smp[i].bytes = bytes;
    g_smp[i].rate = rate;
    g_smp[i].channels = channels;
    g_smp[i].bits = bits;
    g_smp[i].frames = bytes / fb;
    return (plat_sample)i;
}

void plat_audio_sample_free(plat_sample s)
{
    unsigned i;

    if (s >= PLAT_SAMPLE_MAX || !g_smp[s].used) return;
    /* A voice still reading the PCM is unbound first: nothing may point into
     * freed memory, on either thread. */
    for (i = 0; i < g_voice_count; i++) {
        if (g_voice[i].smp == s) voice_unbind(&g_voice[i]);
    }
    free(g_smp[s].pcm);
    memset(&g_smp[s], 0, sizeof g_smp[s]);
}

/* ---- voices --------------------------------------------------------------- */

plat_voice plat_audio_voice_alloc(void)     /* was 0x0040AF00 */
{
    voice *p;

    if (g_voice_count >= PLAT_VOICE_MAX) {
        return PLAT_VOICE_NONE;     /* the original gurus here */
    }
    p = &g_voice[g_voice_count];
    memset(p, 0, sizeof *p);
    p->used  = 1;
    p->smp   = PLAT_SAMPLE_NONE;
    p->group = PLAT_AUDIO_EFFECTS;
    return (plat_voice)g_voice_count++;
}

void plat_audio_voice_free(plat_voice v)
{
    if (!voice_ok(v)) return;
    voice_unbind(&g_voice[v]);
    g_voice[v].used = 0;            /* slot is NOT returned to the pool */
}

void plat_audio_voice_bind(plat_voice v, plat_sample s)
{
    voice *p;
    const sample *d;

    if (!voice_ok(v)) return;
    p = &g_voice[v];
    voice_unbind(p);
    if (s >= PLAT_SAMPLE_MAX || !g_smp[s].used || !g_ready) return;
    d = &g_smp[s];
    if (ma_audio_buffer_ref_init(d->bits == 8 ? ma_format_u8 : ma_format_s16,
                                 d->channels, d->pcm, d->frames, &p->ref)
        != MA_SUCCESS) {
        return;
    }
    p->ref.sampleRate = d->rate;    /* the engine resamples from this */
    if (ma_sound_init_from_data_source(&g_engine, &p->ref,
                                       MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       p->group == PLAT_AUDIO_MASTER ? NULL
                                           : &g_group[p->group],
                                       &p->snd) != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&p->ref);
        return;
    }
    p->smp  = s;
    p->live = 1;
    voice_apply(p);
}

void plat_audio_voice_play(plat_voice v, int looping)   /* 0x0040B7C9 */
{
    if (!voice_ok(v) || !g_voice[v].live) return;
    ma_sound_set_looping(&g_voice[v].snd, looping ? MA_TRUE : MA_FALSE);
    ma_sound_start(&g_voice[v].snd);   /* at its end: from the top */
}

void plat_audio_voice_stop(plat_voice v)                /* 0x0040B80A */
{
    if (!voice_ok(v) || !g_voice[v].live) return;
    ma_sound_stop(&g_voice[v].snd);    /* a pause: the cursor stays */
}

void plat_audio_voice_set_volume(plat_voice v, int mb)  /* 0x0040B844 */
{
    if (!voice_ok(v)) return;
    g_voice[v].mb = clampi(mb, -10000, 0);
    if (g_voice[v].live) ma_sound_set_volume(&g_voice[v].snd, mb_gain(mb));
}

void plat_audio_voice_set_pan(plat_voice v, int pan)    /* 0x0040B86D */
{
    if (!voice_ok(v)) return;
    g_voice[v].pan = clampi(pan, -10000, 10000);
    if (g_voice[v].live) ma_sound_set_pan(&g_voice[v].snd, pan_balance(pan));
}

void plat_audio_voice_set_freq(plat_voice v, unsigned hz)   /* 0x0040B821 */
{
    voice *p;

    if (!voice_ok(v)) return;
    p = &g_voice[v];
    p->hz = hz == 0 ? 0 : hz < 100 ? 100 : hz > 200000 ? 200000 : hz;
    if (p->live) {
        ma_sound_set_pitch(&p->snd, p->hz ? (float)p->hz / (float)g_smp[p->smp].rate
                                          : 1.0f);
    }
}

void plat_audio_voice_set_pos3d(plat_voice v, float x, float y, float z)
{
    voice *p;

    if (!voice_ok(v)) return;
    p = &g_voice[v];
    p->spatial = 1;
    p->pos[0] = x; p->pos[1] = y; p->pos[2] = z;
    if (p->live) {
        ma_sound_set_spatialization_enabled(&p->snd, MA_TRUE);
        ma_sound_set_position(&p->snd, x, y, z);
    }
}

int plat_audio_voice_playing(plat_voice v)              /* 0x0040B78E */
{
    if (!voice_ok(v) || !g_voice[v].live) return 0;
    return ma_sound_is_playing(&g_voice[v].snd) ? 1 : 0;
}

unsigned plat_audio_voice_get_pos(plat_voice v)         /* 0x0040C1F0 */
{
    ma_uint64 f = 0;
    const voice *p;

    if (!voice_ok(v) || !g_voice[v].live) return 0;
    p = &g_voice[v];
    if (ma_sound_get_cursor_in_pcm_frames((ma_sound *)&p->snd, &f) != MA_SUCCESS)
        return 0;
    return (unsigned)(f * frame_bytes(&g_smp[p->smp]));
}

void plat_audio_voice_set_pos(plat_voice v, unsigned off)   /* 0x0040B7B0 */
{
    const voice *p;
    ma_uint64 f;

    if (!voice_ok(v) || !g_voice[v].live) return;
    p = &g_voice[v];
    f = off / frame_bytes(&g_smp[p->smp]);
    if (f > g_smp[p->smp].frames) f = g_smp[p->smp].frames;
    ma_sound_seek_to_pcm_frame((ma_sound *)&p->snd, f);
}

/* ---- the listener, the streams, the groups -------------------------------- */

static int nonzero3(const float *a)
{
    return a && (a[0] != 0.0f || a[1] != 0.0f || a[2] != 0.0f);
}

void plat_audio_listener(const float pos[3], const float fwd[3],
                         const float up[3])
{
    if (!g_ready) return;
    if (pos) ma_engine_listener_set_position(&g_engine, 0, pos[0], pos[1], pos[2]);
    /* A zero vector has no direction; keep the last one rather than hand
     * the spatialiser a NaN. */
    if (nonzero3(fwd))
        ma_engine_listener_set_direction(&g_engine, 0, fwd[0], fwd[1], fwd[2]);
    if (nonzero3(up))
        ma_engine_listener_set_world_up(&g_engine, 0, up[0], up[1], up[2]);
}

static const int stream_group[PLAT_STREAM__COUNT] = {
    PLAT_AUDIO_MUSIC, PLAT_AUDIO_SPEECH
};

int plat_audio_stream_play(plat_stream s, const char *path, int looping)
{
    stream *p;

    if (!g_ready || !path || (int)s < 0 || s >= PLAT_STREAM__COUNT) return -1;
    p = &g_stream[s];
    plat_audio_stream_stop(s);
    /* STREAM: decoded a page at a time by miniaudio's resource manager on its
     * own job thread - 27 minutes of music would be 143 MB decoded. */
    if (ma_sound_init_from_file(&g_engine, path,
                                MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                &g_group[stream_group[s]], NULL, &p->snd)
        != MA_SUCCESS) {
        plat_log("AUDIO: stream %d: %s: cannot open\n", (int)s, path);
        return -1;
    }
    p->live = 1;
    ma_sound_set_volume(&p->snd, mb_gain(p->mb));
    ma_sound_set_looping(&p->snd, looping ? MA_TRUE : MA_FALSE);
    ma_sound_start(&p->snd);
    return 0;
}

void plat_audio_stream_stop(plat_stream s)
{
    if ((int)s < 0 || s >= PLAT_STREAM__COUNT || !g_stream[s].live) return;
    ma_sound_uninit(&g_stream[s].snd);
    g_stream[s].live = 0;
}

void plat_audio_stream_set_volume(plat_stream s, int mb)
{
    if ((int)s < 0 || s >= PLAT_STREAM__COUNT) return;
    g_stream[s].mb = clampi(mb, -10000, 0);
    if (g_stream[s].live) ma_sound_set_volume(&g_stream[s].snd, mb_gain(mb));
}

int plat_audio_stream_playing(plat_stream s)
{
    if ((int)s < 0 || s >= PLAT_STREAM__COUNT || !g_stream[s].live) return 0;
    /* a non-looping sound that reached its end is marked, and stopped by
     * the mixer's next pass: either says it has played out */
    return ma_sound_is_playing(&g_stream[s].snd) &&
           !ma_sound_at_end(&g_stream[s].snd) ? 1 : 0;
}

int plat_audio_music_play(const char *path, int looping)
{
    return plat_audio_stream_play(PLAT_STREAM_MUSIC, path, looping);
}

void plat_audio_music_stop(void)
{
    plat_audio_stream_stop(PLAT_STREAM_MUSIC);
}

void plat_audio_group_volume(plat_audio_group g, float gain)
{
    if (!g_ready || (int)g < 0 || g >= PLAT_AUDIO__GROUPS || !(gain >= 0.0f))
        return;
    if (g == PLAT_AUDIO_MASTER) ma_engine_set_volume(&g_engine, gain);
    else ma_sound_group_set_volume(&g_group[g], gain);
}

void plat_audio_voice_set_group(plat_voice v, plat_audio_group g)
{
    voice *p;

    if (!voice_ok(v) || (int)g < 0 || g >= PLAT_AUDIO__GROUPS) return;
    p = &g_voice[v];
    p->group = (int)g;
    if (p->live) ma_node_attach_output_bus(&p->snd, 0, group_node(p->group), 0);
}
