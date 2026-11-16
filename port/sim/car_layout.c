/* car_layout.c - compile car.h.
 *
 * car.h is header-only and its whole safety story is 63 file-scope offset
 * assertions plus a sizeof() check against the 1,668-byte replay prefix.  A
 * header nobody includes is a header nobody checks, and the step function that
 * will include it is not written yet - so this translation unit exists purely
 * to make the build compile it.
 *
 * It is not a test: there is nothing to run.  If it links, every offset in
 * car.h agreed with when this was built, on THIS
 * target - which is also why it is built for aarch64 as well.  A struct whose
 * padding is right on x86-64 and wrong on the handheld is exactly the class of
 * bug asked to be built for both targets from the start to avoid.
*/

#include "car.h"

/* One definition so the object file is not empty - some toolchains warn, and
 * this tree is required to stay warning-free at -Wall -Wextra -Wpedantic.
 * The value is the size the assertions above already proved. */
const unsigned rally_car_state_size = (unsigned)sizeof(car_state);
