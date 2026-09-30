/* Freestanding stand-in for <stdio.h>.
 * Only what the fixGB core actually touches. Backed by PS5 sceKernel file
 * syscalls and a UDP log socket -- see src/shim.c. */
#ifndef LUAGB_STDIO_H
#define LUAGB_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _FILE FILE;

/* Modes honoured: "r"/"rb" read, "w"/"wb" create+truncate, and "r+"/"rb+"
   which opens an EXISTING file for writing without O_CREAT. That last one
   matters on PS5: creating a new file inside savedata is refused from the
   game sandbox, while writing one that already exists is not. */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);

int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    vprintf(const char *fmt, va_list ap);
int    puts(const char *s);
int    sprintf(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *out, size_t n, const char *fmt, va_list ap);
int    sscanf(const char *str, const char *fmt, ...);
int    fflush(FILE *f);
char  *fgets(char *s, int n, FILE *f);

#endif
