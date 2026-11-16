/* plat_audio_ma.h - the SDL2 backends' mixer, seen from the device side.
 *
 *
 * NOT part of plat.h.  plat_audio_ma.c implements every plat_audio_* entry
 * point on miniaudio, in its no-device mode; this header is the three calls
 * its OWNER makes - plat_sdl2.c, which opens SDL2's audio device and pulls
 * frames from the callback, and audio_mix_check.c, the audio-mix gate, which
 * pulls them with no device at all.
 *
 * Until plat_audio_ma_init succeeds every plat_audio_* call is accepted and
 * nothing plays (the voice pool still counts, as the original's does):
 * a machine with no sound device behaves like plat_null, never fails. */
#ifndef PLAT_AUDIO_MA_H
#define PLAT_AUDIO_MA_H

/* 0 on success.  `rate` is the device's, `channels` 1 or 2. */
int  plat_audio_ma_init(unsigned rate, unsigned channels);
void plat_audio_ma_shutdown(void);

/* Interleaved 32-bit float, `frames` frames.  Called on the AUDIO THREAD:
 * it reads only the mixer's own state, never the game's.  Writes silence
 * before init. */
void plat_audio_ma_render(float *out, unsigned frames);

#endif
