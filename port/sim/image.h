/* image.h - read a PE32 image by virtual address.
 *
 * WHY THIS EXISTS NOW AND NOT IN
 * -----------------------------------------
 * car_params.c and drivetrain.c each carry their own small PE walk, and
 * drivetrain.h explains why: keeping the constants table's reader separate
 * from the handling record's reader stops the constants looking like part of
 * the record.  That argument holds for two.  physics.c would have made three
 * copies of the same twenty lines, which is where the argument stops being
 * about clarity and starts being about nobody having factored it.
 *
 * So the walk is here, physics.c uses it, and THE OTHER TWO ARE LEFT ALONE.
 * They are gated (`drivetrain-diff`, 32 records x 5 slider positions, exact)
 * and rewriting working gated code to satisfy a tidiness argument is the kind
 * of unverified edit standing rule 6 exists to prevent.  Folding them in is a
 * separate, testable change; it is noted in rather
 * than done in passing.
 *
 * is the policy this serves: the numbers live in the user's own RAL.EXE
 * and are read at runtime, never transcribed into a checked-in array.
*/

#ifndef RALLY_SIM_IMAGE_H
#define RALLY_SIM_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct ral_image {
    unsigned char *blob;
    size_t         len;
    uint32_t       base;
    const unsigned char *sec;
    unsigned       nsec;
} ral_image;

/* 0 on success.  -1 the file could not be opened or read (caller should SKIP),
 * -2 it is not a PE32 image this reader understands. */
int  ral_image_open(ral_image *im, const char *path, char *err, size_t errlen);
void ral_image_close(ral_image *im);

/* NULL when the address is not backed by raw data in the file - which is the
 * normal answer for anything in a section's zero-fill tail, and is exactly how
 * the 5,120-entry sine table at 0x009C3EE4 hides from a dump. */
const unsigned char *ral_image_at(const ral_image *im, uint32_t va, size_t n);

float   ral_image_f32(const unsigned char *p);
int32_t ral_image_i32(const unsigned char *p);

#endif /* RALLY_SIM_IMAGE_H */
