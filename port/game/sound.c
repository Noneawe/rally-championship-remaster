/* sound.c - the game's sound requests: sound.h.
 * Every function below names the original's it transcribes; the reading is
 */

#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 0x0040EE7C's chain, in its order: the first set bit wins. */
static const uint32_t fe_bits[9] = {
    SND_SHATTER, SND_RIGHTTHP, SND_BLOWAWAY, SND_REVVING, SND_VIDEOSET,
    SND_ERROR, SND_SELECT, SND_DESELECT, SND_MOVE
};

static int32_t mb_of(const snd *s, int32_t v)
{
    return s->mb[v < 0 ? 0 : v > 64 ? 64 : v];
}

static void logf_(snd *s, const char *what, const char *arg, int32_t n)
{
    if (!s->log) return;
    fprintf(s->log, "%u %s %s %d\n", (unsigned)s->now, what, arg ? arg : "-", (int)n);
    fflush(s->log);
}

void snd_init(snd *s, const snd_backend *be)
{
    int v;
    memset(s, 0, sizeof *s);
    s->be = *be;
    s->mb[0] = -10000;
    for (v = 1; v <= 64; v++)
        s->mb[v] = (int32_t)floor(2000.0 * log10(v / 64.0) + 0.5);
    s->set.overall = 64;
    s->set.music_menu = s->set.music_race = s->set.music_race_set = 32;
    s->set.fx_menu = s->set.fx_race = s->set.speech_menu = 64;
    s->set.codriver = 64;
    s->set.engine = 64;
    s->tps = 50.0f;                        /* 0x0040F00C's 50              */
    /* the records' .data: +0x00 is 0 and 1, the queues empty and idle */
    s->cd[0].base = 0; s->cd[1].base = 1;
    s->cd[0].cur = s->cd[1].cur = -1;
    s->cd_players = 1;                     /* [0x0050F2F0] = 1 in the image */
    s->cd_single = 1;                      /* [0x0050F2EC] = 1             */
    memset(s->tok, 0xFF, sizeof s->tok);   /* no samples until the image's */
}

void snd_set_table(snd *s, const int32_t mb[65])
{
    memcpy(s->mb, mb, sizeof s->mb);
}

/* 0x0040C30F: stop the stream, clear +0 */
static void m_stop(snd *s)
{
    /* the original's stop of an idle stream does nothing audible; the port
     * leaves an idle stream alone, so --music's test stream survives */
    if (!s->m_on) return;
    if (s->be.stream_stop) s->be.stream_stop(s->be.ud, SND_STREAM_MUSIC);
    logf_(s, "music-stop", NULL, 0);
    s->m_on = 0;
}

/* 0x0040C091 for the music: open, set the volume, play */
static int m_open(snd *s, const char *path)
{
    if (s->be.stream_volume)
        s->be.stream_volume(s->be.ud, SND_STREAM_MUSIC, mb_of(s, s->m_vol));
    if (!s->be.stream_play || s->be.stream_play(s->be.ud, SND_STREAM_MUSIC, path) != 0)
        return -1;
    s->m_on = 1;
    logf_(s, "music-play", path, s->m_vol);
    return 0;
}

static int exists(const snd *s, const char *path)
{
    return s->be.exists && s->be.exists(s->be.ud, path);
}

/* 0x0040C384, transcribed branch for branch.  The track number is formatted
 * into ";3files\music\00.wav" (0x00501980) by 0x00402011. */
void snd_music(snd *s, int op, int32_t eax, int32_t ebx, int32_t ecx)
{
    char path[32];
    int32_t start;

    if (s->nosound) return;
    if (op == 0) goto stop_forget;                       /* JB 0x0040C47E */
    if (op == 1) {
        if (!s->m_on) return;
        goto service;                                    /* 0x0040C462 */
    }
    if (op == 3) {
        if (!s->m_on) return;
        s->m_vol = ecx;
        if (s->be.stream_volume)
            s->be.stream_volume(s->be.ud, SND_STREAM_MUSIC, mb_of(s, ecx));
        logf_(s, "music-volume", NULL, ecx);
        return;
    }
    if (s->m_on) m_stop(s);                              /* 0x0040C3C4 */
    if (ecx <= 0) return;
    if (s->nomusic) return;
    if (eax == 0) goto stop_forget;
    s->m_next = ebx;
    s->m_vol = ecx;
play:                                                    /* 0x0040C3EF */
    if (eax == 10) {
        /* the playlist: 10 + [0x00511E88], the position stepped mod 6; a
         * missing file moves on, and a whole lap of missing files gives up
         * WITHOUT the stop (0x0040C443 -> 0x0040C483) */
        start = s->set.pos;
        for (;;) {
            int32_t t = 10 + s->set.pos;
            if ((uint32_t)++s->set.pos >= 6) s->set.pos = 0;
            snprintf(path, sizeof path, "files\\music\\%02d.wav", (int)t);
            if (exists(s, path)) break;
            if (start == s->set.pos) { s->m_next = 0; return; }
        }
    } else {
        snprintf(path, sizeof path, "files\\music\\%02d.wav", (int)eax);
    }
    if (m_open(s, path) != 0) goto stop_forget;          /* JB 0x0040C47E */
service:                                                 /* 0x0040C462 */
    if (s->be.stream_playing && s->be.stream_playing(s->be.ud, SND_STREAM_MUSIC))
        return;
    m_stop(s);                                           /* the track ended */
    eax = s->m_next;
    if (eax != 0) goto play;
stop_forget:                                             /* 0x0040C47E */
    m_stop(s);
    s->m_next = 0;
}

/* 0x0040C30F on the narration record */
static void n_stop(snd *s)
{
    if (!s->n_on) return;
    if (s->be.stream_stop) s->be.stream_stop(s->be.ud, SND_STREAM_SPEECH);
    logf_(s, "speech-stop", NULL, 0);
    s->n_on = 0;
}

/* 0x0040C03B: as the music's, one file, no next and no `nomusic` */
void snd_speech(snd *s, int op, const char *path, int32_t ecx)
{
    if (s->nosound) return;
    if (op == 0) { n_stop(s); return; }
    if (op == 1) {
        if (!s->n_on) return;
        goto service;
    }
    if (op == 3) {
        if (!s->n_on) return;
        s->n_vol = ecx;
        if (s->be.stream_volume)
            s->be.stream_volume(s->be.ud, SND_STREAM_SPEECH, mb_of(s, ecx));
        return;
    }
    if (s->n_on) n_stop(s);
    if (ecx <= 0) return;
    if (!path) { n_stop(s); return; }
    s->n_vol = ecx;
    if (s->be.stream_volume)
        s->be.stream_volume(s->be.ud, SND_STREAM_SPEECH, mb_of(s, ecx));
    if (!s->be.stream_play || s->be.stream_play(s->be.ud, SND_STREAM_SPEECH, path) != 0) {
        n_stop(s);
        return;
    }
    s->n_on = 1;
    logf_(s, "speech-play", path, ecx);
service:
    if (s->be.stream_playing && s->be.stream_playing(s->be.ud, SND_STREAM_SPEECH))
        return;
    n_stop(s);
}

/* 0x00409BA5: both streams serviced, the narration first */
void snd_tick(snd *s)
{
    snd_speech(s, 1, NULL, 0);
    snd_music(s, 1, 0, 0, 0);
}

/* 0x0040EE7C.  The window is taken BEFORE the bits are tested, so a mask with
 * no known bit still spends it; JS on now - [0x0050BF90]. */
void snd_fe(snd *s, uint32_t mask, uint32_t now_ms)
{
    int k;
    int32_t vol;
    if (s->nosound || mask == 0) return;
    if ((int32_t)(now_ms - s->fe_until) < 0) {
        s->now = now_ms;
        logf_(s, "fe-dropped", NULL, (int32_t)mask);
        return;
    }
    s->fe_until = now_ms + 0x7D;
    for (k = 0; k < 9; k++)
        if (mask & fe_bits[k]) break;
    if (k == 9) return;
    /* the volume each branch loads: 0x80 in-game effects, 0x10/0x20/0x40 the
     * caller's [0x00707E54] (AUDIO SETUP's sliders - not ported, so the
     * incidental effects stand in), the rest incidental effects */
    vol = fe_bits[k] == SND_SHATTER ? s->set.fx_race : s->set.fx_menu;
    s->now = now_ms;
    logf_(s, "fe", s->fe_path[k][0] ? s->fe_path[k] : "(no file)", vol);
    if (s->fe_path[k][0] && s->be.effect)
        s->be.effect(s->be.ud, s->fe_path[k], mb_of(s, vol));
}

/* command 8 -> 0x0040AECA: [0x004F3298] = EAX, the primary buffer's volume */
void snd_overall(snd *s, int32_t v)
{
    if (s->nosound) return;
    s->set.overall = v;
    if (s->be.master) s->be.master(s->be.ud, mb_of(s, v));
    logf_(s, "master", NULL, v);
}

/* 0x0040F651 */
void snd_music_volume(snd *s, int32_t v)
{
    if (v != s->m_vol) snd_music(s, 3, 0, 0, v);
}

static int32_t vol_step(int32_t v)          /* AND ~3, then [0, 0x40] */
{
    v &= ~3;
    if (v < 0) v = 0;
    if (v >= 0x40) v = 0x40;
    return v;
}

int32_t snd_race_volume_key(snd *s, int up, int ctrl)
{
    if (!ctrl) {                                 /* 0x00438CB6 / 0x00438CF7 */
        snd_overall(s, vol_step(s->set.overall + (up ? 4 : -4)));
        return up ? 0x114 : 0x113;
    }
    /* 0x00438D3D / 0x00438D7E: Ctrl+F4 from the slider (see sound.h) */
    s->set.music_race = vol_step((up ? s->set.music_race_set : s->set.music_race)
                                 + (up ? 4 : -4));
    snd_music_volume(s, s->set.music_race);
    return up ? 0x116 : 0x115;
}

/* 0x0040F621: AL track, AH next, ECX volume -> op 2 */
void snd_cue(snd *s, int track, int next, int32_t vol)
{
    snd_music(s, 2, track & 0xFF, next & 0xFF, vol);
}

void snd_music_stop(snd *s)                    /* 0x0040F639 */
{
    snd_music(s, 0, 0, 0, 0);
}

void snd_cue_main_menu(snd *s)  { snd_cue(s, 4, 4, s->set.music_menu); }
void snd_cue_car_setup(snd *s)  { snd_cue(s, 9, 9, s->set.music_menu); }
void snd_cue_after_race(snd *s) { snd_cue(s, 3, 3, s->set.music_menu); }

/* 0x0040EE16: stop, command 5 (the advanced-audio word: the port deletes what
 * it configures), command 3 (the frontend set: the backend loads a sample on
 * its first play), command 8 */
void snd_mode_frontend(snd *s)
{
    snd_music_stop(s);
    snd_overall(s, s->set.overall);
}

/* ---- the co-driver ----------------------------------------------- */

static const char *cd_file_of(const snd *s, int32_t id)
{
    return (id >= 0 && id < SND_CD_IDS) ? s->cd_name[id] : NULL;
}

/* command 1 (0x00409A0D) for a speech sample: EBX = EDX = -0x6F, the
 * record's own volume and rate - its rate is -0x70, the file's own, and its
 * volume the co-driver slider through +0x2C, which is what plays here (the
 * sample's own factor is unread, as for the menus' sounds) */
static void cd_play(snd *s, int32_t id)
{
    const char *f = cd_file_of(s, id);
    logf_(s, "codriver", f ? f : "(no file)", id);
    if (f && s->be.effect) s->be.effect(s->be.ud, f, mb_of(s, s->set.codriver));
}

/* command 1 at volume 0: the sample stops if it is playing */
static void cd_stop(snd *s, int32_t id)
{
    const char *f = cd_file_of(s, id);
    logf_(s, "codriver-stop", f ? f : "(no file)", id);
    if (f && s->be.effect_stop) s->be.effect_stop(s->be.ud, f);
}

void snd_cd_off(snd *s)                                  /* 0x0040D213 */
{
    int k;
    for (k = 0; k < 2; k++) {
        s->cd[k].count = 0;                              /* +0x2A4 */
        s->cd[k].cur = -1;                               /* +0x29C */
        s->cd[k].on = 0;                                 /* +0x08  */
    }
    s->cd_service = 0;
}

void snd_cd_voices(snd *s)                               /* 0x0040ECFB */
{
    int k, a, b;
    a = 0; b = 1;                                        /* EBX male, EDX female */
    if (s->cd[0].voice != 0) { a = 1; b = 0; }           /* XCHG             */
    if (s->cd[0].voice == s->cd[1].voice) b = a;         /* MOV EDX,EBX      */
    for (k = 0; k < SND_CD_PAIRS; k++) {
        s->cd_name[SND_CD_FIRST + 2 * k] = s->cd_file[a][k][0] ? s->cd_file[a][k] : NULL;
        s->cd_name[SND_CD_FIRST + 2 * k + 1] = s->cd_file[b][k][0] ? s->cd_file[b][k] : NULL;
    }
}

void snd_cd_race(snd *s, int32_t players, uint32_t now_ms)
{
    if (s->nosound) return;
    snd_cd_off(s);
    s->cd[0].voice = s->set.voice[0];                    /* 0x004158CB */
    s->cd[1].voice = s->set.voice[1];                    /* 0x004158F9 */
    snd_cd_voices(s);
    /* Every speech file the race can ask for, loaded NOW, before the first
     * step - as the original loads the race's sound set before the start
     * (0x0040EE51's commands) - so the race loop never reads a file.  The
     * backend keeps one copy per name: with one voice that is 95 files. */
    if (s->be.effect_ms) {
        int32_t id;
        for (id = SND_CD_FIRST; id < SND_CD_FIRST + 2 * SND_CD_PAIRS; id++)
            if (s->cd_name[id]) (void)s->be.effect_ms(s->be.ud, s->cd_name[id]);
    }
    s->clock = now_ms;                                   /* 0x0040F00C */
    s->tps = 50.0f;                                      /* [0x0050C860] */
    if (players < 1) players = 1;                        /* 0x0040D432.. */
    if (players > 8) players = 8;
    s->cd_players = players;
    snd_cd_open(s);
    logf_(s, "codriver-race", s->cd[0].voice ? "female" : "male", players);
}

void snd_cd_open(snd *s)                                 /* 0x0040D423 */
{
    int32_t k;
    for (k = 0; k < s->cd_players && k < 2; k++) s->cd[k].on = 1;
    s->cd_service = 1;                                   /* [0x0050C858] = 1 */
}

/* 0x0040EC7F, EAX = 1 append / 0 at the head, EBX the token */
static void cd_push(snd *s, snd_codriver *q, int32_t token, int front)
{
    snd_cd_entry *e;
    int32_t k, v;
    if (q->count == SND_CD_QUEUE) return;                /* full: dropped */
    if (q->count == 0) {
        e = &q->e[0];
    } else if (!front) {
        e = &q->e[q->count];
    } else {
        /* the sample playing stops: [edi + cur*4 + 0x18] - with cur = -1
         * that is the head's part COUNT, read as an id, as the original */
        cd_stop(s, q->cur < 0 ? q->e[0].n : q->e[0].id[q->cur]);
        q->cur = -1;
        memmove(&q->e[1], &q->e[0], (size_t)q->count * sizeof q->e[0]);
        e = &q->e[0];
    }
    q->count++;
    e->token = token;
    for (k = 0; k < 6; k++) {
        v = (token >= 0 && token < SND_CD_TOKENS) ? s->tok[token][k] : -1;
        if (v >= 0) v += q->base;                        /* JS: -1 stays */
        e->id[k] = v;
        if (v == -1) break;
    }
    e->n = k;
}

void snd_cd_push(snd *s, int32_t player, int32_t token, int front)
{
    snd_codriver *q;
    if (s->nosound) return;
    if ((uint32_t)player > 1u) return;                   /* CMP EAX,1 / JA */
    q = &s->cd[player];                                  /* [0x0050F2E4 + p*4] */
    if (q->on != 1) return;
    logf_(s, front ? "codriver-front" : "codriver-queue", NULL, token);
    cd_push(s, q, token, front);
}

void snd_cd_sample(snd *s, int32_t player, int32_t id)   /* 0x0040EABF */
{
    if (s->nosound) return;
    if ((uint32_t)player > 1u) return;
    cd_play(s, id + player);
}

/* 0x0040EC55 with EAX = 0: the head removed */
static void cd_pop(snd_codriver *q)
{
    if (q->count == 0) return;
    q->count--;
    if (q->count == 0) return;
    memmove(&q->e[0], &q->e[1], (size_t)q->count * sizeof q->e[0]);
}

static void cd_serve(snd *s, snd_codriver *q)            /* 0x0040EB6D */
{
    int32_t id;
    uint32_t ms;
    if (q->on != 1 || q->count == 0) return;
    if (q->cur != -1 && (int32_t)(s->clock - q->end) < 0) return;   /* JS */
    q->cur++;
    while ((uint32_t)q->cur >= (uint32_t)q->e[0].n) {    /* JB: the entry done */
        cd_pop(q);
        q->cur = 0;
        if (q->count == 0) { q->cur = -1; return; }
    }
    id = q->e[0].id[q->cur];
    {
        const char *f = cd_file_of(s, id);
        ms = (f && s->be.effect_ms) ? s->be.effect_ms(s->be.ud, f) : 0u;
    }
    q->end = s->clock + ms + 0x19u;                      /* + 25 ms */
    if (id == 0x113 || id == 0x114)                      /* `tostop`: the queue */
        q->end += 0x1000000u;                            /* waits ~4.7 hours */
    cd_play(s, id);
}

void snd_cd_tick(snd *s, uint32_t now_ms)
{
    uint32_t dt;
    if (s->nosound || !s->cd_service) return;            /* [0x0050C858] */
    dt = now_ms - s->clock;
    if (dt < 0x14u) return;                              /* 0x0040F03F: JB */
    s->clock = now_ms;
    s->now = now_ms;
    /* 0x0040F04C: [0x0050C860] = FILD(1000 / min(dt, 1000)), unsigned DIV */
    s->tps = (float)(1000u / (dt < 1000u ? dt : 1000u));
    /* 0x0040D4A7: 0x0040D63C, the car records, before the queue */
    if (s->race_tick && s->cd_players != 0) s->race_tick(s->race_ud, s->clock, s->tps);
    if (s->cd_players != 0) {                            /* 0x0040EB37 */
        cd_serve(s, &s->cd[0]);
        if (!s->cd_single) cd_serve(s, &s->cd[1]);       /* [0x0050F2EC] != 1 */
    }
    /* 0x0040D4B1 / 0x0040D4B6: the weather and the ambience (worldsound.h), after the co-driver whatever it did */
    if (s->world_tick) s->world_tick(s->world_ud, s->clock, s->tps);
}

/* 0x0040EE51 then 0x004424E6 with [0x005FE9D8] = 0 (no finish routine is
 * ported): 0x0046CFCF */
void snd_race_music(snd *s)
{
    snd_music_stop(s);
    snd_cue(s, 8, 10, s->set.music_race);
    if (s->set.race_music_on != 1) snd_music_stop(s);
}
