/* sound_plat.c - sound.h's backend on the platform (plat.h section 6).
 *
 *
 * Streams by host path (vfs_resolve), effects as samples loaded on their
 * first play - command 3's "load the set" happens lazily - each with ONE
 * voice of its own that a replay restarts: the voice pool is the original's
 * bump allocator (512 slots, never returned), so a voice per play would run
 * it dry. */

#include "sound.h"
#include "vfs.h"
#include "wav.h"
#include "plat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the menus' nine and, since, the co-drivers' 95 a voice (both voices
 * when two players choose differently: about 6 MB each) -
 * the speech loaded before each race (snd_cd_race), the menus' on first
 * play, all kept for the session; since the race's 60-odd car,
 * surface and impact files too (snd_car_race) */
#define FX_MAX 384

/* the race's sounds keep a voice per sound id (0x0050571C's 417),
 * bound to the file's sample from fx[] */
#define ID_MAX 0x1A1

static struct {
    vfs        fs;
    struct { char path[64]; plat_sample smp; plat_voice v; uint32_t ms;
             uint32_t frames, rate; } fx[FX_MAX];
    int        nfx;
    plat_voice id[ID_MAX];
} B;

static int b_exists(void *ud, const char *game_path)
{
    char host[1100];
    (void)ud;
    return vfs_resolve(&B.fs, game_path, host, sizeof host) == 0;
}

static int b_stream_play(void *ud, int stream, const char *game_path)
{
    char host[1100];
    (void)ud;
    if (vfs_resolve(&B.fs, game_path, host, sizeof host) != 0) {
        fprintf(stderr, "sound: %s: missing (%s)\n", game_path, host);
        return -1;
    }
    /* never looping: the game's service tick starts the next track itself */
    return plat_audio_stream_play(stream == SND_STREAM_SPEECH ? PLAT_STREAM_SPEECH
                                                              : PLAT_STREAM_MUSIC,
                                  host, 0);
}

static plat_stream pst(int stream)
{
    return stream == SND_STREAM_SPEECH ? PLAT_STREAM_SPEECH : PLAT_STREAM_MUSIC;
}

static void b_stream_stop(void *ud, int stream)
{
    (void)ud;
    plat_audio_stream_stop(pst(stream));
}

static void b_stream_volume(void *ud, int stream, int mb)
{
    (void)ud;
    plat_audio_stream_set_volume(pst(stream), mb);
}

static int b_stream_playing(void *ud, int stream)
{
    (void)ud;
    return plat_audio_stream_playing(pst(stream));
}

/* 0x0040B8B2 on the file's own format: the data bytes (the parser's &~3,
 * 0x0040B18C) over the block (bits * channels >> 3, 0x0040B227) are the
 * frames, and frames * 1000 / rate (clamped 100..200,000, 0x0040B1FB) the
 * length - IMUL and an unsigned DIV, 32-bit */
static uint32_t length_ms(const wav_info *w)
{
    uint32_t block = (w->bits * w->channels) >> 3, rate = w->rate, frames;
    if (block == 0) return 0;
    if (rate < 100u) rate = 100u;
    if (rate > 200000u) rate = 200000u;
    frames = (w->bytes & ~3u) / block;
    return (frames * 1000u) / rate;
}

/* The slot for a file, loaded and given its own voice on first use; -1
 * when the table is full */
static int fx_slot(const char *game_path)
{
    int i;
    wav_info w;
    for (i = 0; i < B.nfx; i++)
        if (strcmp(B.fx[i].path, game_path) == 0) return i;
    if (B.nfx == FX_MAX) return -1;
    snprintf(B.fx[i].path, sizeof B.fx[i].path, "%s", game_path);
    memset(&w, 0, sizeof w);
    B.fx[i].smp = wav_load_sample_info(&B.fs, game_path, &w);
    B.fx[i].v = PLAT_VOICE_NONE;
    B.fx[i].ms = 0;                         /* not loaded: 0x0040B90D */
    B.fx[i].frames = B.fx[i].rate = 0;
    if (B.fx[i].smp != PLAT_SAMPLE_NONE) {
        B.fx[i].v = plat_audio_voice_alloc();
        plat_audio_voice_bind(B.fx[i].v, B.fx[i].smp);
        B.fx[i].ms = length_ms(&w);
        {
            uint32_t block = (w.bits * w.channels) >> 3;
            B.fx[i].frames = block ? (w.bytes & ~3u) / block : 0u;
            B.fx[i].rate = w.rate;
        }
        /* the co-driver's files (files\%speech\...) in the SPEECH group,
         * the rest EFFECTS - plat.h section 6 */
        if (strstr(game_path, "speech"))
            plat_audio_voice_set_group(B.fx[i].v, PLAT_AUDIO_SPEECH);
    }
    B.nfx++;
    return i;
}

static uint32_t b_effect_ms(void *ud, const char *game_path)
{
    int i = fx_slot(game_path);
    (void)ud;
    return i < 0 ? 0u : B.fx[i].ms;
}

static void b_effect_stop(void *ud, const char *game_path)
{
    int i;
    (void)ud;
    for (i = 0; i < B.nfx; i++)
        if (strcmp(B.fx[i].path, game_path) == 0) break;
    if (i < B.nfx && B.fx[i].v != PLAT_VOICE_NONE)
        plat_audio_voice_stop(B.fx[i].v);
}

static void b_effect(void *ud, const char *game_path, int mb)
{
    static int timing = -1;             /* getenv once */
    uint64_t t0 = plat_time_us();
    int i = fx_slot(game_path);
    (void)ud;
    if (timing < 0) timing = getenv("RALLY_SOUND_TIMING") != NULL;
    if (timing)                         /* TEMPORARY - freeze hunt */
        fprintf(stderr, "sound-timing: %s slot %.3f ms\n", game_path,
                (double)(plat_time_us() - t0) / 1000.0);
    if (i < 0 || B.fx[i].v == PLAT_VOICE_NONE) return;
    plat_audio_voice_stop(B.fx[i].v);
    plat_audio_voice_set_pos(B.fx[i].v, 0);
    plat_audio_voice_set_volume(B.fx[i].v, mb);
    plat_audio_voice_play(B.fx[i].v, 0);
}

/* ----: a voice per sound id ------------------------------------- */

static plat_voice id_voice(int32_t id)
{
    return (id >= 0 && id < ID_MAX) ? B.id[id] : PLAT_VOICE_NONE;
}

static int b_id_load(void *ud, int32_t id, const char *game_path,
                     uint32_t *frames, uint32_t *rate)
{
    int i;
    (void)ud;
    if (id < 0 || id >= ID_MAX) return -1;
    i = fx_slot(game_path);
    if (i < 0 || B.fx[i].smp == PLAT_SAMPLE_NONE) return -1;
    if (B.id[id] == PLAT_VOICE_NONE) {
        B.id[id] = plat_audio_voice_alloc();
        if (B.id[id] == PLAT_VOICE_NONE) return -1;
        plat_audio_voice_bind(B.id[id], B.fx[i].smp);
    }
    *frames = B.fx[i].frames;
    *rate = B.fx[i].rate;
    return 0;
}

static void b_id_set(void *ud, int32_t id, int mb, int pan_mb, uint32_t hz)
{
    plat_voice v = id_voice(id);
    (void)ud;
    if (v == PLAT_VOICE_NONE) return;
    plat_audio_voice_set_volume(v, mb);
    plat_audio_voice_set_pan(v, pan_mb);
    plat_audio_voice_set_freq(v, hz);
}

static void b_id_start(void *ud, int32_t id, int looping, int rewind)
{
    plat_voice v = id_voice(id);
    (void)ud;
    if (v == PLAT_VOICE_NONE) return;
    if (rewind) plat_audio_voice_set_pos(v, 0);
    plat_audio_voice_play(v, looping);
}

static void b_id_stop(void *ud, int32_t id)
{
    plat_voice v = id_voice(id);
    (void)ud;
    if (v != PLAT_VOICE_NONE) plat_audio_voice_stop(v);
}

static int b_id_playing(void *ud, int32_t id)
{
    plat_voice v = id_voice(id);
    (void)ud;
    return v != PLAT_VOICE_NONE && plat_audio_voice_playing(v);
}

static void b_master(void *ud, int mb)
{
    (void)ud;
    plat_audio_group_volume(PLAT_AUDIO_MASTER, mb <= -10000 ? 0.0f
                            : (float)pow(10.0, mb / 2000.0));
}

void snd_plat_backend(snd_backend *be, const char *assets)
{
    int i;
    memset(&B, 0, sizeof B);
    for (i = 0; i < ID_MAX; i++) B.id[i] = PLAT_VOICE_NONE;
    vfs_init(&B.fs, assets, NULL);
    memset(be, 0, sizeof *be);
    be->exists = b_exists;
    be->stream_play = b_stream_play;
    be->stream_stop = b_stream_stop;
    be->stream_volume = b_stream_volume;
    be->stream_playing = b_stream_playing;
    be->effect = b_effect;
    be->master = b_master;
    be->effect_stop = b_effect_stop;
    be->effect_ms = b_effect_ms;
    be->id_load = b_id_load;
    be->id_set = b_id_set;
    be->id_start = b_id_start;
    be->id_stop = b_id_stop;
    be->id_playing = b_id_playing;
}
