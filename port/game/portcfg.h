/* The port's own settings: a remaster's, never the original's.
 * They live in rally.cfg beside var\ (the --user-dir, else the working
 * directory), one "key=value" a line, and are never written into the
 * original's save formats (var\*.dat). */
#ifndef PORTCFG_H
#define PORTCFG_H

typedef struct portcfg {
    int hd_textures;            /* HD TEXTURES: 0 (default) or 1             */
    int pad_curve;              /* steering curve, % exponent, 100 linear */
} portcfg;

void portcfg_defaults(portcfg *c);
/* Unknown keys and a missing file leave the defaults; 0 read, -1 none. */
int  portcfg_load(portcfg *c, const char *dir);
int  portcfg_save(const portcfg *c, const char *dir);

#endif
