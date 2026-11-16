/* corpus.c - see corpus.h. */

#include "corpus.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    size_t i;
    if (lf > ls) return 0;
    for (i = 0; i < lf; i++)
        if (tolower((unsigned char)s[ls - lf + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    return 1;
}

static int push(corpus_list *l, size_t *cap, char *p)
{
    if (l->n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        char **q = realloc(l->path, nc * sizeof *q);
        if (!q) return -1;
        l->path = q; *cap = nc;
    }
    l->path[l->n++] = p;
    return 0;
}

static int walk(const char *dir, const char *ext, corpus_list *l, size_t *cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return 0;   /* an unreadable directory contributes nothing */
    while ((e = readdir(d)) != NULL) {
        char *full;
        struct stat st;
        size_t n;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n = strlen(dir) + 1 + strlen(e->d_name) + 1;
        full = malloc(n);
        if (!full) { closedir(d); return -1; }
        snprintf(full, n, "%s/%s", dir, e->d_name);
        if (stat(full, &st) != 0) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) {
            int r = walk(full, ext, l, cap);
            free(full);
            if (r != 0) { closedir(d); return r; }
        } else if (ends_with_ci(e->d_name, ext)) {
            if (push(l, cap, full) != 0) { free(full); closedir(d); return -1; }
        } else {
            free(full);
        }
    }
    closedir(d);
    return 0;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int corpus_find(const char *root, const char *ext, corpus_list *out)
{
    size_t cap = 0;
    memset(out, 0, sizeof(*out));
    if (walk(root, ext, out, &cap) != 0) { corpus_free(out); return -1; }
    qsort(out->path, out->n, sizeof *out->path, cmp_path);
    return 0;
}

void corpus_free(corpus_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) free(l->path[i]);
    free(l->path);
    memset(l, 0, sizeof(*l));
}

void *corpus_slurp(const char *path, size_t *len)
{
    FILE *fh = fopen(path, "rb");
    long sz;
    void *buf;
    if (!fh) return NULL;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return NULL; }
    sz = ftell(fh);
    if (sz < 0) { fclose(fh); return NULL; }
    rewind(fh);
    buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fh); return NULL; }
    if (fread(buf, 1, (size_t)sz, fh) != (size_t)sz) { free(buf); fclose(fh); return NULL; }
    fclose(fh);
    *len = (size_t)sz;
    return buf;
}

char *corpus_paired(const char *ro3_path, const char *ext)
{
    char *dir, *want, *found = NULL;
    const char *slash = strrchr(ro3_path, '/');
    const char *base = slash ? slash + 1 : ro3_path;
    const char *dot = strrchr(base, '.');
    size_t dirlen = slash ? (size_t)(slash - ro3_path) : 1;
    size_t stemlen = dot ? (size_t)(dot - base) : strlen(base);
    DIR *d;
    struct dirent *e;

    dir = malloc(dirlen + 1);
    if (!dir) return NULL;
    memcpy(dir, slash ? ro3_path : ".", dirlen);
    dir[dirlen] = 0;

    want = malloc(stemlen + strlen(ext) + 1);
    if (!want) { free(dir); return NULL; }
    memcpy(want, base, stemlen);
    strcpy(want + stemlen, ext);

    d = opendir(dir);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (strlen(e->d_name) != strlen(want)) continue;
            {
                size_t i, n = strlen(want);
                int same = 1;
                for (i = 0; i < n; i++)
                    if (tolower((unsigned char)e->d_name[i]) != tolower((unsigned char)want[i]))
                        { same = 0; break; }
                if (!same) continue;
            }
            {
                size_t n = dirlen + 1 + strlen(e->d_name) + 1;
                found = malloc(n);
                if (found) snprintf(found, n, "%s/%s", dir, e->d_name);
            }
            break;
        }
        closedir(d);
    }
    free(dir);
    free(want);
    return found;
}
