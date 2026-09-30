#ifndef SHIM_H
#define SHIM_H

#include "core.h"

/* The freestanding libc that lets the fixGB core compile unmodified.
 *
 * fixGB includes <stdio.h>, <string.h>, <malloc.h> and <time.h>; src/libc/
 * shadows those with headers that declare exactly what the core touches, and
 * this translation unit implements them on top of PS5 sceKernel calls. */

struct shim_ps5 {
    void *gadget;
    void *open;
    void *read;
    void *write;
    void *close;
    void *lseek;
    void *mkdir;
    void *sendto;
    void *gettimeofday;
    s32   log_fd;
    u8   *log_sa;      /* sockaddr_in of the PC listening for UDP logs */
};

/* Must be the first thing _start does -- every other shim entry point
   assumes these pointers are live. */
void shim_init(const struct shim_ps5 *ps5, void *arena, u64 arena_size);

/* sceKernelMkdir through the saved gadget table. Returns the raw s32
   (0 on success, EEXIST if already there, EROFS on a read-only mount). */
int shim_mkdir(const char *path);

/* Raw log line, bypassing printf formatting. Safe before shim_init. */
void klog(const char *msg);

/* Set to 0 to compile every core printf down to nothing. Diagnostics from
   the emulation core are chatty at load time but silent during play. */
#ifndef GB_VERBOSE
#define GB_VERBOSE 1
#endif

/* The arena backs malloc(). fixGB allocates the ROM buffer and the APU output
   buffer per game and frees them in its deinit path, so the whole arena is
   simply rewound between games instead of maintaining a real free list. */
void  arena_reset(void);
u64   arena_used(void);
u64   arena_size_get(void);

#endif
