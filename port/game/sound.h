/* sound.h - WHAT plays and WHEN: the music, the menus' sounds, the narration.
 *
 *
 * THE GAME'S LOGIC, TRANSCRIBED: the original posts every sound request to
 * its 50 Hz audio thread, and these are the
 * functions that thread runs, called directly:
 *
 *   snd_music   0x0040C384  the music stream's command (op 0 stop, 1 service,
 *                           2 play, 3 volume): next track, the 10-15
 *                           playlist and its position [0x00511E88], a missing
 *                           file skipped, `nomusic`, a volume <= 0 plays
 *                           nothing
 *   snd_speech  0x0040C03B  the narration stream's (the same ops, a path)
 *   snd_tick    0x00409BA5  the thread's tick: op 1 on both streams
 *   snd_fe      0x0040EE7C  the menus' sound player: a bit mask, the first
 *                           set bit in the original's order, one 125 ms window
 *   snd_overall command 8   the overall volume -> the master
 *   snd_cue     0x0040F621  a cue: (track, next, volume) as op 2
 *   snd_music_stop 0x0040F639
 *   snd_race_music 0x0046CFCF  the race's arm of `doingamemusic`
 *
 * the co-driver's speech queue:
 *
 *   snd_cd_race    0x0040D213, 0x0040ECFB, 0x0040F00C, 0x0040D423: the race's
 *                  speech set - queues emptied, files by voice, opened
 *   snd_cd_off     0x0040D213
 *   snd_cd_voices  0x0040ECFB  each speech id's file, by the players' voices
 *   snd_cd_push    0x0040EADE / 0x0040EB0A -> 0x0040EC7F: append, or at the
 *                  head with the playing sample stopped
 *   snd_cd_sample  0x0040EABF  a sample at once (the countdown)
 *   snd_cd_tick    0x0040F031's 20 ms gate, 0x0040EB36 -> 0x0040EB6D: the
 *                  head served, each sample timed by its length + 25 ms
 *
 * NOT TRANSCRIBED: the command ring and the thread (the calls are direct; a
 * request reaches the stream at once rather than at the next 20 ms tick),
 * the stream's ring, refill and silence pad (the platform streams the file),
 * and the sample player behind command 1 (0x0040A89E, the): a menu sound
 * plays its sample at the volume table's entry for its slider.
 *
 * Volumes are the game's 0..64 (linear amplitude, v/64) and reach the
 * platform as the table 0x00501B0C's millibels.  The backend is a table of
 * calls so the logic can be checked against a scripted one (`sound-check`).
*/
#ifndef RALLY_GAME_SOUND_H
#define RALLY_GAME_SOUND_H

#include <stdint.h>
#include <stdio.h>

/* 0x0040EE7C's bits */
enum {
    SND_MOVE = 0x01, SND_SELECT = 0x02, SND_DESELECT = 0x04, SND_ERROR = 0x08,
    SND_RIGHTTHP = 0x10, SND_BLOWAWAY = 0x20, SND_REVVING = 0x40,
    SND_SHATTER = 0x80, SND_VIDEOSET = 0x100
};

enum { SND_STREAM_MUSIC = 0, SND_STREAM_SPEECH = 1 };

/* What the platform does for it.  Game paths (`files\music\04.wav`); the
 * plat backend resolves them (sound_plat.c's role is snd_plat_backend). */
typedef struct {
    void *ud;
    int  (*exists)(void *ud, const char *game_path);               /* 0x00402CA3 */
    int  (*stream_play)(void *ud, int stream, const char *game_path); /* 0 = open */
    void (*stream_stop)(void *ud, int stream);                     /* 0x0040C30F */
    void (*stream_volume)(void *ud, int stream, int mb);           /* 0x0040C359 */
    int  (*stream_playing)(void *ud, int stream);
    void (*effect)(void *ud, const char *game_path, int mb);       /* command 1 */
    void (*master)(void *ud, int mb);                              /* 0x0040A60D */
    /* command 1 at volume 0 on a playing sample stops it (0x0040A9BD) */
    void (*effect_stop)(void *ud, const char *game_path);
    /* 0x0040B8B2: a sample's length in ms at its own rate - frames * 1000 /
     * rate; 0 when it is not loaded */
    uint32_t (*effect_ms)(void *ud, const char *game_path);
    /* the race's sounds, one voice per sound ID (two ids may share a
     * file and play at once - the cattle grid's roll and skid).  id_load
     * loads the file (game path, files\d\snd\...) and gives its frames and
     * rate, 0 = ready; id_set applies volume and pan (millibels, plat.h
     * section 6) and the rate in Hz; id_start plays it (looping for loop
     * type 1, from the top when `rewind`, else on from where it is). */
    int  (*id_load)(void *ud, int32_t id, const char *game_path,
                    uint32_t *frames, uint32_t *rate);
    void (*id_set)(void *ud, int32_t id, int mb, int pan_mb, uint32_t hz);
    void (*id_start)(void *ud, int32_t id, int looping, int rewind);
    void (*id_stop)(void *ud, int32_t id);
    int  (*id_playing)(void *ud, int32_t id);
} snd_backend;

/* The globals the requests read, 0..64 unless named otherwise. */
typedef struct {
    int32_t overall;        /* [0x004F3298] overall volume                    */
    int32_t pos;            /* [0x00511E88] the playlist position, 0..5        */
    int32_t music_menu;     /* [0x00511E8C] incidental music: the menus' cues  */
    int32_t music_race;     /* [0x00511E90] in-game music: the race's cue      */
    int32_t music_race_set; /* [0x00707FF4] AUDIO SETUP's in-game music slider
                               (Ctrl+F4 steps from THIS, not the above) */
    int32_t fx_menu;        /* [0x00707FF8] incidental effects: MOVE etc.      */
    int32_t fx_race;        /* [0x00707FFC] in-game effects (SHATTER)          */
    int32_t speech_menu;    /* [0x00708000] incidental speech: the narration   */
    int32_t race_music_on;  /* [0x0070CC68] AUDIO SETUP's in-game music: 1 on  */
    int32_t codriver;       /* [0x00708004] co-driver speech           */
    int32_t engine;         /* [0x00708008] car engine                 */
    int32_t voice[2];       /* [0x0070CD8C] / [0x0070CEF8]: PLAYER DETAILS'
                             * co-driver for players 1 and 2, 0 the male one;
                             * the launcher copies them to the player
                             * records' +0x298 (0x004158CB)                   */
} snd_settings;

/* ---- the co-driver - ------ *
 *
 * The per-player record (the 0x0050CD84 table, records 0 and 1) and its
 * speech queue at +0x298: sixteen 32-byte entries, each a token and up to
 * six sample ids.  The game thread pushes (0x0040EC7F); the audio thread's
 * tick serves the head (0x0040EB6D), one sample at a time, each timed by its
 * LENGTH on the thread's own clock, not by asking the voice. */
#define SND_CD_QUEUE   16
#define SND_CD_IDS     0x1A1      /* the sound table's 417 records             */
#define SND_CD_TOKENS  113        /* 0x0050C87C .. the first list, 0x0050CA40 */
#define SND_CD_FIRST   0xC1       /* the speech pairs: 0xC1/0xC2 .. 0x17D/0x17E */
#define SND_CD_PAIRS   95

typedef struct {
    int32_t token, n, id[6];      /* +0x10 + 32k: token, part count, ids       */
} snd_cd_entry;

typedef struct {
    int32_t      base;            /* record +0x00: 0 player 1, 1 player 2 - the
                                   * offset added to each sample id           */
    int32_t      on;              /* record +0x08: == 1 opens the queue        */
    int32_t      voice;           /* +0x298, the queue's +0x00: the co-driver  */
    int32_t      cur;             /* +0x04: the head's sample playing, -1 none */
    uint32_t     end;             /* +0x08: when it is over, on the clock      */
    int32_t      count;           /* +0x0C                                     */
    snd_cd_entry e[SND_CD_QUEUE];
} snd_codriver;

typedef struct snd {
    snd_backend  be;
    int32_t      mb[65];        /* 0x00501B0C: volume v -> SetVolume millibels */
    snd_settings set;
    int          nomusic;       /* [0x0050197C], the config's `nomusic`        */
    int          nosound;       /* [0x004F3274], `noaudio` / `nosound`: every
                                   request dropped (the ring cleared)          */
    /* the music record 0x00501950 */
    int          m_on;          /* +0                                         */
    int32_t      m_vol;         /* +4                                         */
    int32_t      m_next;        /* [0x0050194C]                               */
    /* the narration record 0x00501920 */
    int          n_on;
    int32_t      n_vol;
    uint32_t     fe_until;      /* [0x0050BF90]: the window's end, ms          */
    char         fe_path[9][64];/* the frontend set's files, by bit (snd_fe)   */
    FILE        *log;           /* --sound-log: one line per thing heard      */
    uint32_t     now;           /* the clock the log stamps, ms               */

    /* the co-driver */
    snd_codriver cd[2];         /* records 0 and 1 ([0x0050F2E4] / [+E8])    */
    int          cd_service;    /* [0x0050C858]: the race's set is up        */
    int32_t      cd_players;    /* [0x0050F2F0]: records opened, 1..8        */
    int          cd_single;     /* [0x0050F2EC] == 1: player 2's not served  */
    uint32_t     clock;         /* [0x0050C85C]: the audio thread's clock, ms,
                                   moved on in steps of 20 ms or more        */
    int32_t      tok[SND_CD_TOKENS][7];   /* 0x0050C87C: -1-terminated ids   */
    char         cd_file[2][SND_CD_PAIRS][48];   /* the image's two lists,
                                   0x00502BD6 (male) and 0x005038D0 (female) */
    const char  *cd_name[SND_CD_IDS];      /* 0x0040ECFB's: each id's file   */

    /* the car's sounds (carsound.h), run by the same 20 ms gate
     * BEFORE the co-driver, as 0x0040D484 calls 0x0040D63C before 0x0040EB36.
     * `tps` is [0x0050C860]: 1000 / the tick's milliseconds (at most 1000),
     * 50 until the first tick. */
    void       (*race_tick)(void *ud, uint32_t clock, float tps);
    void        *race_ud;
    /* the weather and the ambience (worldsound.h), AFTER the
     * co-driver, as 0x0040D4B1 / 0x0040D4B6 follow 0x0040EB36. */
    void       (*world_tick)(void *ud, uint32_t clock, float tps);
    void        *world_ud;
    float        tps;
} snd;

/* The image's defaults: every slider 64, both
 * music volumes 32, the switch off, the position 0; the volume table as
 * 2000*log10(v/64) until snd_set_table gives the image's own (two entries
 * differ by 1-2 mB). */
void snd_init(snd *s, const snd_backend *be);
void snd_set_table(snd *s, const int32_t mb[65]);

void snd_music(snd *s, int op, int32_t eax, int32_t ebx, int32_t ecx);
void snd_speech(snd *s, int op, const char *path, int32_t ecx);
void snd_tick(snd *s);
void snd_fe(snd *s, uint32_t mask, uint32_t now_ms);
void snd_overall(snd *s, int32_t v);

void snd_cue(snd *s, int track, int next, int32_t vol);
/* 0x0040F651: op 3 at `v`, only when it differs from the stream's
 * [0x00501954] (m_vol). */
void snd_music_volume(snd *s, int32_t v);
/* FUN_00438C42's F3 / F4 in a race (0x00438CA9..0x00438DC4).  Without
 * left Ctrl the overall volume [0x004F3298] -/+ 4 -> command 8; with it the
 * in-game music [0x00511E90] -> 0x0040F651.  The new value is ANDed with ~3
 * and clamped to [0, 64].  THE ORIGINAL'S Ctrl+F4 loads [0x00511E90] and then
 * overwrites EAX with the slider [0x00707FF4] (0x00438D91 / 0x00438D96), so
 * it steps up from the slider, not from the current volume: transcribed.
 * Returns the message 0x00438F80 shows (275-278). */
int32_t snd_race_volume_key(snd *s, int up, int ctrl);
void snd_music_stop(snd *s);

/* The cues `rally` has paths for. */
void snd_cue_main_menu(snd *s);     /* 0x0040F5B5: 4, loops, incidental      */
void snd_cue_car_setup(snd *s);     /* 0x0040F5C7: 9, loops, incidental      */
void snd_cue_after_race(snd *s);    /* 0x0040F5A3: 3, loops, incidental      */
/* 0x0040EE16, the frontend sound mode: stop the music, command 8. */
void snd_mode_frontend(snd *s);
/* Every race: 0x0040EE51 (stop), then `doingamemusic`'s race arm
 * 0x0046CFCF - cue 8 then the playlist at in-game music, stopped again at
 * once unless the switch is on. */
void snd_race_music(snd *s);

/* ---- the co-driver ---------------------------------------------- */

/* The race's speech set up: 0x0040D213's reset of every record (count 0,
 * cur -1, closed), 0x0040ECFB's choice of files by each player's voice -
 * every one of them loaded here, before the race, through effect_ms -
 * 0x0040F00C's clock, then 0x0040D423 - records 0 .. players-1 opened, the
 * service on.  `players` is [0x0050F2F0], clamped to 1..8 (the port holds
 * two records). */
void snd_cd_race(snd *s, int32_t players, uint32_t now_ms);
/* 0x0040D213 alone: every queue emptied, the service off. */
void snd_cd_off(snd *s);
/* 0x0040D423: records 0 .. cd_players-1 opened, the service on - also the
 * end of PAUSED / RETIRE? (0x0045ADAD). */
void snd_cd_open(snd *s);
/* 0x0040ECFB: each speech id's file - the male list for ids 0xC1 + 2k when
 * player 1's voice is 0 (else the female), the other list for 0xC2 + 2k
 * unless player 2's voice is the same as player 1's. */
void snd_cd_voices(snd *s);
/* 0x0040EADE (front 0) / 0x0040EB0A (front 1) -> 0x0040EC7F: a token into
 * player `player`'s queue - appended, or put at the head with the sample
 * playing stopped.  Nothing for a player above 1 or a closed record; a full
 * queue drops it. */
void snd_cd_push(snd *s, int32_t player, int32_t token, int front);
/* 0x0040EABF: sample `id` + `player`, played at once - the countdown. */
void snd_cd_sample(snd *s, int32_t player, int32_t id);
/* 0x0040F031's 20 ms gate, then 0x0040EB36: each open queue's head served. */
void snd_cd_tick(snd *s, uint32_t now_ms);

/* The platform's backend (plat.h section 6), over the install `assets`. */
void snd_plat_backend(snd_backend *be, const char *assets);

#endif
