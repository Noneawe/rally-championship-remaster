/* wav.h - the install's .wav files, read for the mixer.
 *
 * Every one of the 451 files is plain RIFF/WAVE, format tag 1 (PCM), 16-bit,
 * mono or stereo, 22,050 or 44,100 Hz (measured by tools/wavcensus.py).  So this reads exactly that container and
 * REJECTS ANYTHING ELSE LOUDLY: a compressed or headerless file, a format tag
 * other than 1, a data chunk that runs past the end of the file.  8-bit PCM
 * is accepted too (DirectSound's other PCM width), though the install has
 * none.
 *
 * NOT a transcription: the original's .wav loading path is unread
 * The port reads the files as they are.
 *
 * wav_census.c is the second implementation (standing rule 16): it runs this
 * parser over the whole install and prints tools/wavcensus.py's table, and the
 * wav-census gate diffs the two. */
#ifndef RALLY_GAME_WAV_H
#define RALLY_GAME_WAV_H

#include <stddef.h>

#include "plat.h"
#include "vfs.h"

typedef struct {
    const unsigned char *pcm;   /* points INTO the buffer parsed */
    unsigned bytes;             /* the data chunk's size, as its header says */
    unsigned tag, channels, rate, bits;
} wav_info;

/* Parse a whole file held in memory.  0 and `w` filled, or -1 and `*why`
 * naming what was wrong. */
int wav_parse(const unsigned char *b, size_t n, wav_info *w, const char **why);

/* Read a host file whole (malloc'd into *buf, the caller frees) and parse it. */
int wav_read_file(const char *host_path, unsigned char **buf, wav_info *w,
                  const char **why);

/* The game's spelling in, a sample out: resolve through the VFS, read, parse,
 * hand the PCM to plat_audio_sample_load (which copies it), free the file.
 * PLAT_SAMPLE_NONE, with a line on stderr naming the file and the reason, on
 * any failure. */
plat_sample wav_load_sample(const vfs *fs, const char *game_path);
/* The same, and the file's format into `*info` when non-NULL (its `pcm` is
 * NULL: the file's buffer is freed): the co-driver times each sample
 * by its length. */
plat_sample wav_load_sample_info(const vfs *fs, const char *game_path,
                                 wav_info *info);

#endif
