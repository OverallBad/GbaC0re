/* GbaC0re PS5 native — libc prototype fixups.
 *
 * Forced into every TU with -include. The SDK libc headers omit the
 * prototype for strtof_l (declared nowhere under target/include), which
 * mGBA's util/formatting.c calls under HAVE_STRTOF_L. The definition
 * lives in src/ps5_locale_shim.c.
 */

#ifndef PS5_LIBC_FIXUPS_H
#define PS5_LIBC_FIXUPS_H

#include <xlocale.h>

float strtof_l(const char *str, char **end, locale_t locale);

#endif
