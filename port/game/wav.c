/* wav.c - see wav.h. */

#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 |
           (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

int wav_parse(const unsigned char *b, size_t n, wav_info *w, const char **why)
{
    size_t i = 12;
    int have_fmt = 0, have_data = 0;

    memset(w, 0, sizeof *w);
    if (n < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        *why = "not RIFF/WAVE";
        return -1;
    }
    /* chunks: id, size, body, padded to an even length */
    while (i + 8 <= n) {
        unsigned long sz = rd32(b + i + 4);
        const unsigned char *body = b + i + 8;
        if (memcmp(b + i, "fmt ", 4) == 0) {
            if (sz < 16 || sz > n - i - 8) { *why = "short fmt chunk"; return -1; }
            w->tag      = rd16(body);
            w->channels = rd16(body + 2);
            w->rate     = (unsigned)rd32(body + 4);
            w->bits     = rd16(body + 14);
            have_fmt = 1;
        } else if (memcmp(b + i, "data", 4) == 0) {
            if (sz > n - i - 8) { *why = "data chunk runs past the end of the file"; return -1; }
            w->pcm   = body;
            w->bytes = (unsigned)sz;
            have_data = 1;
        }
        if (sz > n - i - 8) break;      /* an unknown chunk overrunning: stop */
        i += 8 + sz + (sz & 1);
    }
    if (!have_fmt)  { *why = "no fmt chunk";  return -1; }
    if (!have_data) { *why = "no data chunk"; return -1; }
    if (w->tag != 1) { *why = "format tag is not 1 (PCM)"; return -1; }
    if (w->channels != 1 && w->channels != 2) { *why = "not mono or stereo"; return -1; }
    if (w->bits != 8 && w->bits != 16) { *why = "not 8- or 16-bit"; return -1; }
    if (w->rate == 0) { *why = "rate is 0"; return -1; }
    return 0;
}

int wav_read_file(const char *host_path, unsigned char **buf, wav_info *w,
                  const char **why)
{
    FILE *f = fopen(host_path, "rb");
    long n;

    *buf = NULL;
    if (!f) { *why = "cannot open"; return -1; }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        *why = "cannot size";
        return -1;
    }
    if (!(*buf = malloc(n ? (size_t)n : 1)) || fread(*buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(*buf);
        *buf = NULL;
        *why = "cannot read";
        return -1;
    }
    fclose(f);
    if (wav_parse(*buf, (size_t)n, w, why) != 0) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    return 0;
}

plat_sample wav_load_sample(const vfs *fs, const char *game_path)
{
    return wav_load_sample_info(fs, game_path, NULL);
}

plat_sample wav_load_sample_info(const vfs *fs, const char *game_path,
                                 wav_info *info)
{
    char host[1100];
    unsigned char *buf;
    wav_info w;
    const char *why = "";
    plat_sample s;

    if (vfs_resolve(fs, game_path, host, sizeof host) != 0) {
        fprintf(stderr, "rally: sound %s: not found (looked for %s)\n", game_path, host);
        return PLAT_SAMPLE_NONE;
    }
    if (wav_read_file(host, &buf, &w, &why) != 0) {
        fprintf(stderr, "rally: sound %s: REJECTED - %s\n", host, why);
        return PLAT_SAMPLE_NONE;
    }
    s = plat_audio_sample_load(w.pcm, w.bytes, w.rate, w.channels, w.bits);
    free(buf);
    if (info) {
        *info = w;
        info->pcm = NULL;          /* the buffer is gone */
    }
    return s;
}
