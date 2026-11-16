/* vfs.c - see vfs.h. */

#include "vfs.h"
#include "plat.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void vfs_init(vfs *v, const char *assets_root, const char *language)
{
    size_t n;

    memset(v, 0, sizeof *v);
    snprintf(v->root, sizeof v->root, "%s", assets_root ? assets_root : ".");
    for (n = strlen(v->root); n > 1 && (v->root[n - 1] == '/'
                                        || v->root[n - 1] == '\\'); n--) {
        v->root[n - 1] = '\0';
    }
    snprintf(v->language, sizeof v->language, "%s", language ? language : "");
}

void vfs_set_user(vfs *v, const char *dir)
{
    size_t n;
    snprintf(v->user, sizeof v->user, "%s", dir ? dir : "");
    for (n = strlen(v->user); n > 1 && (v->user[n - 1] == '/'
                                        || v->user[n - 1] == '\\'); n--) {
        v->user[n - 1] = '\0';
    }
}

static int prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

static int exists_any(const char *path)
{
    plat_dir *d;
    if (plat_file_exists(path)) {
        return 1;
    }
    d = plat_dir_open(path);
    if (d) {
        plat_dir_close(d);
        return 1;
    }
    return 0;
}

/* Replace the last component of `path` (from `comp` on) with the directory
 * entry that matches it case-insensitively, if the exact spelling is absent.
 * `path` is NUL-terminated at the end of the component. */
static void fix_case(char *path, char *comp)
{
    char      dir[1024];
    plat_dir *d;
    const char *e;
    size_t    dl = (size_t)(comp - path);

    if (exists_any(path)) {
        return;
    }
    if (dl == 0) {
        snprintf(dir, sizeof dir, ".");
    } else {
        if (dl >= sizeof dir) {
            return;
        }
        memcpy(dir, path, dl - 1);      /* without the separator */
        dir[dl - 1] = '\0';
        if (dl == 1) {
            snprintf(dir, sizeof dir, "/");
        }
    }
    d = plat_dir_open(dir);
    if (!d) {
        return;
    }
    while ((e = plat_dir_next(d)) != NULL) {
        size_t i, n = strlen(comp);
        if (strlen(e) != n) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (tolower((unsigned char)e[i]) != tolower((unsigned char)comp[i])) {
                break;
            }
        }
        if (i == n) {
            memcpy(comp, e, n);
            break;
        }
    }
    plat_dir_close(d);
}

static int build(const vfs *v, const char *game_path, char *out, size_t n)
{
    const char *p = game_path;
    size_t      o = 0, start;
    int         verbatim;

    if (!game_path || n == 0) {
        return -2;
    }
    /* "var\" and "save" are exempt from every prefix (0x00402FEB). */
    verbatim = prefix_ci(p, "var\\") || prefix_ci(p, "var/")
            || prefix_ci(p, "save");
    if (!verbatim) {
        /* ";3" and ";4" are volume prefixes: stripped, and - the port's
         * decision, see vfs.h - both resolve to the local tree. */
        if (p[0] == ';' && (p[1] == '3' || p[1] == '4')) {
            p += 2;
        }
        o = (size_t)snprintf(out, n, "%s/", v->root);
        if (o >= n) {
            return -2;
        }
    } else if (v->user[0]) {
        o = (size_t)snprintf(out, n, "%s/", v->user);
        if (o >= n) {
            return -2;
        }
    }
    start = o;
    for (; *p; p++) {
        if (*p == '%') {
            /* The language value, with its trailing separator. */
            size_t ll = strlen(v->language);
            if (o + ll + 2 >= n) {
                return -2;
            }
            memcpy(out + o, v->language, ll);
            o += ll;
            if (ll && out[o - 1] != '/') {
                out[o++] = '/';
            }
            continue;
        }
        if (o + 2 >= n) {
            return -2;
        }
        out[o++] = (*p == '\\') ? '/' : *p;
    }
    out[o] = '\0';

    /* Case, one component at a time, below the root. */
    {
        char *c = out + start, *slash;
        for (;;) {
            char save;
            slash = strchr(c, '/');
            save = slash ? *slash : '\0';
            if (slash) {
                *slash = '\0';
            }
            if (*c) {
                fix_case(out, c);
            }
            if (!slash) {
                break;
            }
            *slash = save;
            c = slash + 1;
        }
    }
    return 0;
}

int vfs_resolve(const vfs *v, const char *game_path, char *out, size_t n)
{
    int rc = build(v, game_path, out, n);
    if (rc != 0) {
        return rc;
    }
    return plat_file_exists(out) ? 0 : -1;
}

int vfs_resolve_dir(const vfs *v, const char *game_path, char *out, size_t n)
{
    int rc = build(v, game_path, out, n);
    if (rc != 0) {
        return rc;
    }
    return exists_any(out) ? 0 : -1;
}
