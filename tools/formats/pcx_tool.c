/* pcx_tool.c - the CLI over pcx.c.
 *
 * Two subcommands, and both exist to produce output BYTE-IDENTICAL to
 * `python tools/plates.py`, so check.sh can be a plain `diff`:
 *
 *   pcx decode ROOT          decode every tx????.pcx under ROOT's six band
 *                            directories; one line per plate, CRC-32 of the
 *                            decoded pixels, first and last texel
 *   pcx decodefile PATH ...  the same, over named files.  This is the one the
 *                            MUTANTS go through: the shipped corpus is
 *                            entirely valid, so without it every rejection
 *                            string in pcx.c is part of the compared surface
 *                            and never compared.  tools/formats/pcxmutate.py
 *
 * Both take `--request WxH`, which is the size the CALLER asks the decoder
 * for.  It is not a tool convenience: FUN_0040509D asks for 256x256 on every
 * plate the game loads, and the decoder CLIPS rather than refusing, so
 * `--request 256x256` is the shipped configuration and the bare form is not.
 * The printed w x h is the DECODED size, which is the file's whenever nothing
 * clipped.  pcx.h, and.
 *
 * THE WALK IS THE BAND DIRECTORIES, NOT A RECURSIVE SWEEP, because that is
 * what plates.py does and the two have to agree on the corpus before they can
 * be made to agree on its contents.  `files/a` appears in the band table
 * twice (it is split around the 900..901 hole) and is walked once.
 *
 * The CRC is zlib's - the ordinary IEEE 802.3 polynomial, reflected - because
 * `binascii.crc32` is what the Python half has for free, and a digest that
 * both halves can compute at C speed is what makes a 942-file, 135 MB
 * comparison a gate rather than a chore.
*/

#include "pcx.h"
#include "corpus.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The band table, `FUN_00424672`, in the order plates.py walks it:
 * `sorted(set(band for band in BANDS) | {"b/gravel"})`. */
static const char *const BANDS[] = { "a", "b", "b/gravel", "g/frnt", "m", "s/extr" };
#define NBANDS ((int)(sizeof BANDS / sizeof BANDS[0]))

#define PLATE_MAX_PIXELS (1024u * 1024u)

static uint32_t crc_tab[256];
static int      crc_ready;

static void crc_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t   i;

    if (!crc_ready) {
        crc_init();
    }
    for (i = 0; i < n; i++) {
        c = crc_tab[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

/* `name.startswith("tx") and name.endswith(".pcx") and name[2:-4].isdigit()`,
 * on the LOWERCASED name, which is what index_dir keys on. */
static int is_plate(const char *lower)
{
    size_t n = strlen(lower), i;

    if (n < 7 || strncmp(lower, "tx", 2) != 0 ||
        strcmp(lower + n - 4, ".pcx") != 0) {
        return 0;
    }
    for (i = 2; i < n - 4; i++) {
        if (lower[i] < '0' || lower[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static void lower_of(const char *s, char *out, size_t outlen)
{
    size_t i;

    for (i = 0; i + 1 < outlen && s[i]; i++) {
        out[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    }
    out[i] = '\0';
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}


/* The `--request WxH` the caller asked for; 0,0 is "no request", which is the
 * bare form and is NOT what the game does.  See the file header. */
static unsigned req_w, req_h;

/* One `decode` row, or an ERROR row.  Shared by both subcommands so the two
 * cannot drift into printing different things about one file. */
static int print_row(const char *label, const void *data, size_t len,
                     uint32_t *px)
{
    char     err[256];
    pcx_info pi;
    size_t   npx;
    unsigned w = 0, h = 0;

    if (pcx_read_header(data, len, &pi, err, sizeof err) != 0 ||
        pcx_decode_request(data, len, px, PLATE_MAX_PIXELS, req_w, req_h,
                           &w, &h, err, sizeof err) != 0) {
        printf("%-28s ERROR: %s\n", label, err);
        return 0;
    }
    npx = (size_t)w * h;
    printf("%-28s %4ux%-4u planes=%u pal=%d crc=%08x px0=%08x pxN=%08x\n",
           label, w, h, pi.planes, pi.has_palette ? 1 : 0,
           (unsigned)crc32_of((const uint8_t *)px, npx * sizeof *px),
           (unsigned)px[0], (unsigned)px[npx - 1]);
    return 1;
}

static int cmd_decodefile(int argc, char **argv)
{
    uint32_t *px;
    int       i;

    px = (uint32_t *)malloc(PLATE_MAX_PIXELS * sizeof *px);
    if (!px) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    for (i = 0; i < argc; i++) {
        void  *data;
        size_t len;

        data = corpus_slurp(argv[i], &len);
        if (!data) {
            printf("%-28s ERROR: cannot read\n", argv[i]);
            continue;
        }
        print_row(argv[i], data, len, px);
        free(data);
    }
    free(px);
    return 0;
}

static int cmd_decode(int argc, char **argv)
{
    uint32_t *px;
    long      n = 0, ok = 0;
    int       band;

    if (argc < 1) {
        fprintf(stderr, "usage: pcx decode ROOT | pcx decodefile PATH ...\n");
        return 2;
    }
    px = (uint32_t *)malloc(PLATE_MAX_PIXELS * sizeof *px);
    if (!px) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }

    for (band = 0; band < NBANDS; band++) {
        char    dir[1024];
        DIR    *d;
        struct dirent *de;
        char  **names = NULL;
        size_t  nn = 0, cap = 0, i;

        snprintf(dir, sizeof dir, "%s/%s", argv[0], BANDS[band]);
        d = opendir(dir);
        if (!d) {
            continue;
        }
        /* NON-RECURSIVE, and it has to be: `files/b/gravel` sits inside
         * `files/b`, and a recursive sweep would count all 37 gravel plates
         * twice - once under each band - while plates.py's `glob` of `dir` + slash-star
         * counts them once.  The two walks must be the same walk before any
         * comparison of their contents means anything. */
        while ((de = readdir(d)) != NULL) {
            char low[512];

            lower_of(de->d_name, low, sizeof low);
            if (!is_plate(low)) {
                continue;
            }
            if (nn == cap) {
                char **grown;
                cap = cap ? cap * 2 : 64;
                grown = (char **)realloc(names, cap * sizeof *names);
                if (!grown) {
                    fprintf(stderr, "out of memory\n");
                    return 2;
                }
                names = grown;
            }
            names[nn] = (char *)malloc(strlen(low) + 1 + strlen(de->d_name) + 1);
            if (!names[nn]) {
                fprintf(stderr, "out of memory\n");
                return 2;
            }
            /* "<lowercase name>\0<on-disk name>" - one allocation, sorted on
             * the key plates.py sorts on. */
            strcpy(names[nn], low);
            strcpy(names[nn] + strlen(low) + 1, de->d_name);
            nn++;
        }
        closedir(d);
        qsort(names, nn, sizeof *names, cmp_str);

        for (i = 0; i < nn; i++) {
            const char *low = names[i];
            const char *on_disk = low + strlen(low) + 1;
            char        path[2048], rel[2048];
            void       *data;
            size_t      len;

            n++;
            snprintf(path, sizeof path, "%s/%s", dir, on_disk);
            snprintf(rel, sizeof rel, "files/%s/%s", BANDS[band], low);
            data = corpus_slurp(path, &len);
            if (!data) {
                printf("%-28s ERROR: cannot read\n", rel);
                continue;
            }
            ok += print_row(rel, data, len, px);
            free(data);
        }
        for (i = 0; i < nn; i++) {
            free(names[i]);
        }
        free(names);
    }
    free(px);

    if (n == 0) {
        /* plates.py's require_plates, word for word: a population of zero
         * prints in the same shape as a real answer. */
        fprintf(stderr, "plates.py: found no tx????.pcx under %s\n", argv[0]);
        return 1;
    }
    printf("\n");
    printf("%ld of %ld plate files decode, stream landing exactly.\n", ok, n);
    return 0;
}

/* Lift `--request WxH` out of argv wherever it sits, so the positional
 * arguments keep the shape both halves have always had. */
static int take_request(int *argc, char **argv)
{
    int i, j;

    for (i = 0; i < *argc; i++) {
        if (strcmp(argv[i], "--request") != 0) {
            continue;
        }
        if (i + 1 >= *argc ||
            sscanf(argv[i + 1], "%ux%u", &req_w, &req_h) != 2) {
            fprintf(stderr, "pcx: --request wants WxH, e.g. 256x256\n");
            return 1;
        }
        for (j = i; j + 2 < *argc; j++) {
            argv[j] = argv[j + 2];
        }
        *argc -= 2;
        return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int n;

    if (argc < 2) {
        fprintf(stderr, "usage: pcx decode ROOT [--request WxH]\n"
                        "       pcx decodefile PATH ... [--request WxH]\n");
        return 2;
    }
    n = argc - 2;
    if (take_request(&n, argv + 2) != 0) {
        return 2;
    }
    if (strcmp(argv[1], "decode") == 0) {
        return cmd_decode(n, argv + 2);
    }
    if (strcmp(argv[1], "decodefile") == 0) {
        return cmd_decodefile(n, argv + 2);
    }
    fprintf(stderr, "pcx: unknown subcommand %s\n", argv[1]);
    return 2;
}
