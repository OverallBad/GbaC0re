/* GbaC0re PS5 native — libc function shims.
 *
 * These symbols are referenced by mGBA or GbaC0re code but have no usable
 * import on a native title:
 *
 * - strtof_l / newlocale / freelocale: the SDK headers declare the latter
 *   two but the ps5link NID catalog has entries for none of the three.
 *   A native title never calls setlocale (process locale is already C),
 *   so newlocale("C") is a no-op returning NULL and strtof_l degenerates
 *   to atof (which IS catalogued and hardware-confirmed).
 * - div: trivial; not in the catalog. Called by mGBA's HLE BIOS.
 * - mktime: not in the catalog. Called by mGBA's RTC (savedata.c).
 *   Implemented with Howard Hinnant's days_from_civil algorithm
 *   (proleptic Gregorian calendar, UTC) — exact for all valid dates.
 * - vprintf: not in the catalog (vfprintf is). Called by mGBA's default
 *   log handler. Routes through the diag log file instead of stdout,
 *   which has nowhere to go on a native title.
 *
 * Defining the symbols locally means the linker emits no imports for them.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xlocale.h>

#include "ps5_diag.h"

float strtof_l(const char *str, char **end, locale_t locale) {
    (void)locale;
    (void)end;
    return (float)atof(str);
}

locale_t newlocale(int mask, const char *locale, locale_t base) {
    (void)mask;
    (void)locale;
    (void)base;
    return (locale_t)0;
}

int freelocale(locale_t loc) {
    (void)loc;
    return 0;
}

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

/* Days since 1970-01-01 (civil date -> day count). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = (int64_t)y - era * 400;             /* [0, 399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

time_t mktime(struct tm *tm) {
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1,
                                   tm->tm_mday);
    int64_t secs = days * 86400 + tm->tm_hour * 3600 +
                   tm->tm_min * 60 + tm->tm_sec;
    return (time_t)secs;
}

int vprintf(const char *format, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    ps5_diag_log(buf);
    return n;
}
