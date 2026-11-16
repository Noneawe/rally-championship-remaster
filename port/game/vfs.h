/* vfs.h - the game's asset paths, resolved against the local asset tree.
 *
 *
 * WHAT THE ORIGINAL DOES.  Every asset path goes through vfs_resolve_path
 * (0x00402FEB, 128 bytes, read in full -):
 *
 *   "var\" or "save..."   verbatim, no prefix
 *   ";4..."               the CD, unconditionally (only the CD check builds one)
 *   installation >= 3     local
 *   ";3..."               the CD
 *   installation == 2     local
 *   otherwise             the CD
 *   then the ";N" is stripped and each '%' expands to the language value,
 *   which carries a trailing separator.
 *
 * WHAT THE PORT DOES, AND WHY IT IS NOT A TRANSCRIPTION.  The user's decision
 * of 2026-10-05 (Non-goals): NO CD CHECK ON ANY TARGET, and every
 * ";3" / ";4" path resolves to the local tree.  That is a full install
 * (installation = 3) with the ";4" arm folded in too - so there is ONE prefix,
 * the asset root, and nothing here probes a drive type, reads jade.bun's token
 * dword or a volume serial, or waits for a disc.  "var\" and "save" stay
 * relative to the working directory, as the original's are.
 *
 * Separators become '/', and each component is matched CASE-INSENSITIVELY
 * when the exact spelling is absent: the original ran on a case-insensitive
 * file system with lower-case templates (`files\a\obj????.ro3`), and the
 * shipped tree is upper-case (`OBJ0001.RO3`).  A missing file is reported to
 * the caller, never waited for. */
#ifndef RALLY_GAME_VFS_H
#define RALLY_GAME_VFS_H

#include <stddef.h>

typedef struct {
    char root[1024];       /* the asset root, no trailing separator         */
    char language[64];     /* the `language` value; "" when there is none   */
    char user[1024];       /* where "var\" and "save..." are; "" = the
                              working directory, as the original's           */
} vfs;

void vfs_init(vfs *v, const char *assets_root, const char *language);
/* the directory "var\" and "save..." resolve under (NULL or "" = the
 * working directory) - the port's choice of where the saves go */
void vfs_set_user(vfs *v, const char *dir);

/* The game's spelling in, a host path out.  Returns 0 and the path of a file
 * that exists; -1 with `out` holding the path that was looked for (so the
 * caller's error names it); -2 if `out` is too small. */
int  vfs_resolve(const vfs *v, const char *game_path, char *out, size_t n);

/* As vfs_resolve, for a path whose existence is not required (a directory
 * root handed to a loader that builds its own file names).  Always fills
 * `out`; returns 0 if it exists, -1 if not. */
int  vfs_resolve_dir(const vfs *v, const char *game_path, char *out, size_t n);

#endif
