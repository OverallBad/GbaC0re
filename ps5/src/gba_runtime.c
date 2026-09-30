/* GbaC0re v0.7 -- freestanding libc extensions for mGBA.
 *
 * The LuaPSX shim (src/shim.c) already provides malloc/calloc/realloc/free,
 * the string routines, the printf family, FILE* and time. mGBA's GBA core
 * needs a little more: ctype, strto*, strdup, qsort, rand, errno, a minimal
 * sscanf, and float/double math. The math here is ported from v0.6's
 * gba_runtime.c, which proved it against this exact core.
 *
 * Everything is integer or bit-level; no libm, no FPU syscalls. With
 * -fno-builtin the compiler still emits its own helpers for 64-bit division
 * on some paths -- those come from compiler-rt style emission, not libc, and
 * the LuaPSX link model already accounts for that. */

#include "core.h"
#include "shim.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- errno -- */

int errno;

/* ---------------------------------------------------------------- ctype -- */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c)  { return isdigit(c) || isalpha(c); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }

/* --------------------------------------------------------------- strto* -- */

static unsigned long long strtoull_inner(const char *s, char **end, int base, int *neg) {
    while (isspace((unsigned char)*s)) s++;
    int n = 0;
    if (*s == '+') s++;
    else if (*s == '-') { n = 1; s++; }
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') { base = 16; s += 2; }
            else base = 8;
        } else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long long v = 0;
    while (*s) {
        int d;
        if (isdigit((unsigned char)*s)) d = *s - '0';
        else if (isalpha((unsigned char)*s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d;
        s++;
    }
    if (end) *end = (char *)s;
    if (neg) *neg = n;
    return v;
}

long strtol(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtoull_inner(s, end, base, &neg);
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtoull_inner(s, end, base, 0);
}

long long strtoll(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = strtoull_inner(s, end, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return strtoull_inner(s, end, base, 0);
}

float strtof(const char *s, char **end) {
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    double v = 0;
    while (isdigit((unsigned char)*s)) { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (isdigit((unsigned char)*s)) { v += (*s - '0') * f; f *= 0.1; s++; }
    }
    if (end) *end = (char *)s;
    return neg ? (float)-v : (float)v;
}

double strtod(const char *s, char **end) {
    return (double)strtof(s, end);
}

/* ----------------------------------------------------------------- misc -- */

/* NOTE: strdup/strndup are intentionally NOT defined here. mGBA's
 * src/util/string.c provides them (HAVE_STRDUP=OFF), and a second
 * definition would be a link-time multiple-definition error. */

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++; n--;
    }
    if (!n) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Insertion sort -- O(n^2) but tiny inputs (e-Reader anchor lists). */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (!base || nmemb < 2 || !size || !cmp) return;
    char *b = (char *)base;
    char *tmp = malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

static unsigned long rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 16) & 0x7FFF);
}

void srand(unsigned seed) { rand_state = seed ? seed : 1; }

void abort(void) {
    for (;;) { __asm__ volatile ("pause"); }
}

/* mGBA's configuration reader goes through inih; without a config file to
   parse there is nothing to do. Returning nonzero means "parse failed", which
   the caller already treats as "keep defaults". (Same stub v0.6 used.) */
int ini_parse_stream(char *(*reader)(char *, int, void *), void *stream,
                     int (*handler)(void *, const char *, const char *, const char *),
                     void *user) {
    (void)reader; (void)stream; (void)handler; (void)user;
    return -1;
}

/* Minimal sscanf: %d %u %x %s %c and %n, enough for the %u<%n> pattern
   mGBA's vfs uses when probing numbered filenames. */
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int assigned = 0;
    const char *p = str;

    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*p)) p++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*p != *fmt) break;
            p++; fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') { if (*p != '%') break; p++; fmt++; continue; }
        if (*fmt == 'n') {
            int *n = va_arg(ap, int *);
            *n = (int)(p - str);
            fmt++;
            continue;
        }
        while (isspace((unsigned char)*p)) p++;
        int neg = 0;
        if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
        if (*fmt == 'd' || *fmt == 'u' || *fmt == 'i') {
            unsigned long long v = 0;
            int any = 0;
            while (isdigit((unsigned char)*p)) { v = v * 10 + (unsigned)(*p - '0'); p++; any = 1; }
            if (!any) break;
            long long sv = neg ? -(long long)v : (long long)v;
            if (*fmt == 'd' || *fmt == 'i') *va_arg(ap, int *) = (int)sv;
            else *va_arg(ap, unsigned *) = (unsigned)v;
            assigned++; fmt++;
        } else if (*fmt == 'x' || *fmt == 'X') {
            unsigned long long v = 0;
            int any = 0;
            while (isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10;
                v = v * 16 + (unsigned)d; p++; any = 1;
            }
            if (!any) break;
            *va_arg(ap, unsigned *) = (unsigned)v;
            assigned++; fmt++;
        } else if (*fmt == 's') {
            char *d = va_arg(ap, char *);
            int any = 0;
            while (*p && !isspace((unsigned char)*p)) { *d++ = *p++; any = 1; }
            *d = 0;
            if (!any) break;
            assigned++; fmt++;
        } else if (*fmt == 'c') {
            *va_arg(ap, char *) = *p++;
            assigned++; fmt++;
        } else {
            break;
        }
    }

    va_end(ap);
    return assigned;
}

/* ----------------------------------------------------------------- math -- */
/* Ported from v0.6's gba_runtime.c, which proved these against mGBA. */

float fminf(float a, float b) { return a < b ? a : b; }
float fmaxf(float a, float b) { return a > b ? a : b; }
float fabsf(float x) { return x < 0 ? -x : x; }

static float ps5_wrap_pi(float x) {
    const float pi = 3.14159265358979323846f;
    const float tau = 6.28318530717958647692f;
    while (x > pi) x -= tau;
    while (x < -pi) x += tau;
    return x;
}

float sinf(float x) {
    x = ps5_wrap_pi(x);
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f))));
}

float cosf(float x) { return sinf(x + 1.5707963267948966192f); }

float exp2f(float x) {
    int n = (int)x;
    if (x < 0 && x != (float)n) n--;
    if (n >= 128) return 3.402823466e+38f;
    if (n < -149) return 0.0f;
    float f = x - (float)n;
    if (f > 1.0f) f = 1.0f;
    float y = 1.0f + f * 0.69314718f + f * f * 0.24022651f +
              f * f * f * 0.05550411f + f * f * f * f * 0.00961813f;
    union { unsigned u; float f; } u;
    u.u = (unsigned)(n + 127) << 23;
    return u.f * y;
}

float sqrtf(float x) {
    if (x <= 0) return 0;
    float g = x > 1 ? x : 1;
    for (int i = 0; i < 8; i++) g = 0.5f * (g + x / g);
    return g;
}

float hypotf(float x, float y) { return sqrtf(x * x + y * y); }

double floor(double x) {
    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

double ceil(double x) {
    long long i = (long long)x;
    if ((double)i < x) i++;
    return (double)i;
}

double sin(double x)   { return (double)sinf((float)x); }
double cos(double x)   { return (double)cosf((float)x); }
double sqrt(double x)  { return (double)sqrtf((float)x); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double exp2(double x)  { return (double)exp2f((float)x); }
double fabs(double x)  { return x < 0 ? -x : x; }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }
