/* objload.h - build the object-collision tables (sim_objects) from a level's
 * own files.  A TOOL-SIDE helper, not part of rally_sim: it opens files.
 **/
#ifndef RALLY_SIM_OBJLOAD_H
#define RALLY_SIM_OBJLOAD_H

#include "physics.h"

typedef struct simobj_stats {
    int  named;        /* types 1..0x4F with a non-zero model number         */
    int  loaded;       /* ... whose .ro3 parsed and were registered          */
    int  missing;      /* ... named, no file on disk                         */
    int  badparse;     /* ... present, would not parse                       */
    long points;       /* collision points registered, all types             */
    int  have_info;    /* OBJSINFO / CARSINFO / FRNTINFO / EXTRINFO found    */
} simobj_stats;

/* FUN_004247CD + FUN_004248B9 + FUN_004252A5 for the object types
 * the 63 model numbers LOCALDAT holds at
 * +0x500 (entry 0 is not copied), the eight code constants over types
 * 0x38..0x3F, each model's .ro3 by the 400/600/800 ladder, the class byte
 * OBJSINFO[model] + 0x1C, and every point of the file in file order.  A named
 * type with no readable model is registered UNLOADED (model -1, no points),
 * which is what FUN_00424997's early return leaves; its class is taken as 0,
 * unobservable because a type with no points cannot be hit.
 *
 *   ld / ld_len   the LOCALDAT file as read from disk
 *   files_root    the assets' `files` directory
 *
 * `o` must be sim_objects_init'ed.  Returns 0, or < 0 when LOCALDAT is too
 * short or no info file could be read (then nothing is registered). */
int simobj_load_level(sim_objects *o, const sim_consts *k, const uint8_t *ld,
                      size_t ld_len, const char *files_root,
                      simobj_stats *st);

#endif
