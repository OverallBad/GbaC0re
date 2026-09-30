/* Freestanding stand-in for <stdlib.h>. Arena allocator in src/shim.c. */
#ifndef LUAGB_STDLIB_H
#define LUAGB_STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
int   abs(int v);

long               strtol(const char *s, char **end, int base);
unsigned long      strtoul(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
float              strtof(const char *s, char **end);
double             strtod(const char *s, char **end);

void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *));
char *getenv(const char *name);
int  rand(void);
void srand(unsigned seed);
void abort(void);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t  div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);

#endif
