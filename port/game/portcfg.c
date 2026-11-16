/* the port's settings file.  See portcfg.h. */
#include "portcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cfg_path(char *out, size_t n, const char *dir)
{
    snprintf(out, n, "%s%srally.cfg", dir ? dir : "",
             dir && *dir && !strchr("/\\", dir[strlen(dir) - 1]) ? "/" : "");
}

void portcfg_defaults(portcfg *c)
{
    c->hd_textures = 0;
    c->pad_curve = 100;
}

int portcfg_load(portcfg *c, const char *dir)
{
    char path[1100], line[256];
    FILE *f;
    portcfg_defaults(c);
    cfg_path(path, sizeof path, dir);
    if (!(f = fopen(path, "r"))) return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        if (strcmp(line, "hd_textures") == 0) c->hd_textures = atoi(eq + 1) != 0;
        if (strcmp(line, "pad_curve") == 0) c->pad_curve = atoi(eq + 1);
    }
    fclose(f);
    return 0;
}

int portcfg_save(const portcfg *c, const char *dir)
{
    char path[1100];
    FILE *f;
    cfg_path(path, sizeof path, dir);
    if (!(f = fopen(path, "w"))) return -1;
    fprintf(f, "# rally: the port's own settings (not the original's)\n");
    fprintf(f, "hd_textures=%d\n", c->hd_textures);
    fprintf(f, "pad_curve=%d\n", c->pad_curve);
    return fclose(f) == 0 ? 0 : -1;
}
