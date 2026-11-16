/* corpus.h - the file-system half of the format tools, kept strictly out of
 * the parsers. ro3.c and lwo.c take buffers; everything that knows what a
 * path is lives here.
 *
 * POSIX (dirent, stat). The port targets aarch64 Linux, and these are
 * development tools, not shipped code - the parsers are the part that lifts.
 */
#ifndef RAL_CORPUS_H
#define RAL_CORPUS_H

#include <stddef.h>

typedef struct {
    char **path;
    size_t n;
} corpus_list;

/* Every *.ro3 under `root`, case-insensitively, sorted by path. */
int  corpus_find(const char *root, const char *ext, corpus_list *out);
void corpus_free(corpus_list *l);

/* Read a whole file. Returns NULL on failure; *len is set on success. The
 * caller frees. */
void *corpus_slurp(const char *path, size_t *len);

/* The .lwo beside a .ro3, matched case-insensitively on the stem. Returns a
 * malloc'd path, or NULL if there is none. */
char *corpus_paired(const char *ro3_path, const char *ext);

#endif /* RAL_CORPUS_H */
