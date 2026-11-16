/* menu3d.h - THE MENUS' 3D MODELS: the carousels, car select's cars and REVIEW
 * OPTIONS' arrow...
 *
 * The split, and it is the: the original's INTEGER code runs in the
 * interpreter (x86vm.h) over the image's data - the carousel kinds
 * (FUN_0048C1A2's twenty setups), the slide (0x004664C9 -> FUN_0048C114) and
 * the placement of every slot (0x0048C30E / 0x0048C39A, 0x0048BD44) - and
 * every model draw it reaches, 0x0048C4AC, is a HOOK that lands here.  What is
 * native is what the interpreter does not run: the x87 matrix (0x00426422),
 * the light (FUN_00425F36) and the model path (objlit.h).
 *
 * What is the original's, read at run time and never checked in: the
 * frontend's type table, parsed out of FUN_004713BA's own MOV run
 * (0x004715EC..0x004717AE); the car models, [0x006CEF1C] (FUN_0046102B, the
 * loop at 0x004717B3); the models and plates, files\g\frnt and files\b.
*/
#ifndef RALLY_GAME_MENU3D_H
#define RALLY_GAME_MENU3D_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "objlit.h"

typedef struct menu3d menu3d;

/* Parses the type table out of the image and loads the models (no GL needed;
 * the plates load on the first draw).  NULL and `err` on failure. */
menu3d *menu3d_open(const ral_image *im, const char *files_root,
                    char *err, size_t n);
void    menu3d_close(menu3d *m);

/* [0x00581BC4 + type*4] as FUN_004713BA leaves it: the model number of every
 * type, 0 for none.  The caller writes it into the interpreter's
 * [0x00581FC4] (FUN_00424997's store, 0x004249A6) so FUN_00420F8A finds it. */
const uint32_t *menu3d_models(const menu3d *m, int *ntypes);

/* A screen body's projection: [0x005D6E44], [0x005D6E48] and [0x005D6F14],
 * then 0x0046C45F (800 x 600) -> FUN_0044C516. */
void menu3d_view(menu3d *m, int32_t off_x, int32_t off_y, int32_t zoom);

/* One draw of 0x0048C4AC: the type ([0x005D422C]) and the six globals it
 * reads.  `win_w` x `win_h` is the window, which the 800 x 600 frame scales
 * into. */
void menu3d_draw(menu3d *m, int type, const objlit_place *p,
                 int win_w, int win_h);

/* The draw-list bracket around the models: FUN_0044C942's states, and
 * 0x0044CA08's Z clear (the canvas has written no depth, so the port's only
 * job is to start the models on an empty buffer). */
void menu3d_begin(menu3d *m);
void menu3d_end(menu3d *m);

/* For the check: what the last frame drew, in total and draw by draw (the
 * draw list's own order, at most MENU3D_MAXDRAW).  Without a GL context the
 * draws still run - plat.h drops them - so the record is the same headless. */
#define MENU3D_MAXDRAW 8
typedef struct {
    int          type;
    uint32_t     model;           /* [0x00581BC4 + type*4]                    */
    objlit_place place;
    objlit_stats st;
} menu3d_rec;

const objlit_stats *menu3d_stats(const menu3d *m);
int  menu3d_records(const menu3d *m, const menu3d_rec **out);
void menu3d_stats_reset(menu3d *m);

#endif
