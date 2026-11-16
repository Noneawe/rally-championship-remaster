/* lexicon.c - see lexicon.h. */
#include "lexicon.h"
#include "plat.h"

#include <stdlib.h>
#include <string.h>

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One message, from just after the label's whitespace to the end of the
 * line, into `out` (which has room: the result is never longer). */
static void message(const char *p, const char *end, char *out)
{
    char *o = out, *last = out;     /* `last` = one past the last kept byte */
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == ';') break;                         /* a comment */
        if (c == '{' || c == '}') continue;          /* a group */
        if (c == '\\' && p < end) {
            unsigned char e = (unsigned char)*p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 's') c = ' ';
            else if (e >= '1' && e <= '8') c = (unsigned char)(e - '0');
            else if (e == '#' && p + 1 < end && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) {
                c = (unsigned char)(hexv(p[0]) * 16 + hexv(p[1]));
                p += 2;
            } else c = e;                            /* \\ \{ \} \; */
            *o++ = (char)c;
            last = o;                                /* an escape is kept */
            continue;
        }
        if (c == '\t') c = ' ';                      /* TENTATIVE, lexicon.h */
        *o++ = (char)c;
        if (c != ' ') last = o;
    }
    *last = 0;                                       /* trailing blanks off */
}

/* The port's own wording where the original names its platform: the quit
 * screens say "exit to windows", which would be wrong on any other port.  The
 * install's ENGLISH.TXT is not ours to edit, so the text is replaced here, by
 * label, keeping each token's case.  The strings are static: lexicon_free
 * frees only the blob. */
static void patch_platform_text(lexicon *lx)
{
    static const struct { const char *name, *text; } patch[] = {
        { "tokExit",  "exit game" },
        { "tokExitT", "EXIT GAME" },
    };
    int k, i;
    for (k = 0; k < lx->n; k++)
        for (i = 0; i < (int)(sizeof patch / sizeof *patch); i++)
            if (strcmp(lx->name[k], patch[i].name) == 0)
                lx->text[k] = (char *)patch[i].text;
}

int lexicon_load(lexicon *lx, const char *path)
{
    plat_file *f;
    int64_t sz;
    char *src, *p, *end;
    int cap = 0, k;

    memset(lx, 0, sizeof *lx);
    f = plat_file_open(path, 0);
    if (!f) return -1;
    sz = plat_file_size(f);
    if (sz <= 0 || sz > (64 << 20)) { plat_file_close(f); return -1; }
    src = (char *)malloc((size_t)sz + 1);
    lx->blob = (char *)malloc((size_t)sz * 2 + 16);   /* two NULs a line */
    if (!src || !lx->blob ||
        plat_file_read(f, src, (size_t)sz) != (size_t)sz) {
        plat_file_close(f);
        free(src);
        lexicon_free(lx);
        return -1;
    }
    plat_file_close(f);
    src[sz] = 0;

    /* Pass 1: count.  Pass 2: fill.  A token line is one that starts with
     * `tok` (frontend.py's rule, the one the anchors were checked with). */
    for (k = 0; k < 2; k++) {
        char *o = lx->blob;
        int n = 0;
        p = src;
        end = src + sz;
        while (p < end) {
            char *eol = p;
            while (eol < end && *eol != '\n') eol++;
            if (eol - p >= 3 && p[0] == 't' && p[1] == 'o' && p[2] == 'k') {
                if (k == 1) {
                    char *q = p, *le = eol;
                    if (le > p && le[-1] == '\r') le--;
                    while (q < le && *q != ' ' && *q != '\t') q++;
                    lx->name[n] = o;
                    memcpy(o, p, (size_t)(q - p));
                    o += q - p;
                    *o++ = 0;
                    while (q < le && (*q == ' ' || *q == '\t')) q++;
                    lx->text[n] = o;
                    message(q, le, o);
                    o += strlen(o) + 1;
                }
                n++;
            }
            p = eol + 1;
        }
        if (k == 0) {
            cap = n;
            /* the names and texts together are never longer than the file */
            lx->text = (char **)calloc((size_t)(cap ? cap : 1), sizeof *lx->text);
            lx->name = (char **)calloc((size_t)(cap ? cap : 1), sizeof *lx->name);
            if (!lx->text || !lx->name) { free(src); lexicon_free(lx); return -1; }
        }
        lx->n = n;
    }
    free(src);
    patch_platform_text(lx);
    return 0;
}

void lexicon_free(lexicon *lx)
{
    free(lx->text);
    free(lx->name);
    free(lx->blob);
    memset(lx, 0, sizeof *lx);
}

const char *lexicon_text(const lexicon *lx, int id)
{
    if (id < 0 || id >= lx->n) return "";
    return lx->text[id];
}
