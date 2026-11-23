/* fopen_ci.h - fopen that finds a file whatever the case of its path.
 *
 * The game's data comes from a Windows CD, and the code spells paths in
 * several cases ("FILES/g/frnt/FRNTINFO").  Windows and FAT do not care;
 * Linux and Android's app storage do.  Force-included into every port
 * source on non-Windows builds (CMakeLists.txt), it replaces fopen: the
 * exact path is tried first, and only a read that failed with ENOENT walks
 * the path component by component, matching names case-insensitively. */
#ifndef RALLY_FOPEN_CI_H
#define RALLY_FOPEN_CI_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* lstat, opendir under -std=c99 */
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static inline FILE *rally_fopen_ci(const char *path, const char *mode)
{
    char buf[1024], dir[1024];
    char *c;
    FILE *f = fopen(path, mode);

    if (f || errno != ENOENT || mode[0] != 'r' || strchr(mode, '+') ||
        strlen(path) >= sizeof buf) {
        return f;
    }
    strcpy(buf, path);
    for (c = buf + (buf[0] == '/'); *c; ) {
        char *end = strchr(c, '/'), save = 0;
        struct stat st;
        DIR *d;
        struct dirent *e;

        if (end) { save = *end; *end = '\0'; }
        if (lstat(buf, &st) != 0) {                  /* this component is wrong */
            size_t dl = (size_t)(c - buf);
            if (dl == 0) strcpy(dir, ".");
            else { memcpy(dir, buf, dl); dir[dl > 1 ? dl - 1 : dl] = '\0'; }
            d = opendir(dir);
            if (!d) return NULL;
            while ((e = readdir(d)) != NULL)
                if (strlen(e->d_name) == strlen(c) && strcasecmp(e->d_name, c) == 0) {
                    memcpy(c, e->d_name, strlen(c));
                    break;
                }
            closedir(d);
        }
        if (!end) break;
        *end = save;
        c = end + 1;
    }
    return fopen(buf, mode);
}
#define fopen rally_fopen_ci

#endif
