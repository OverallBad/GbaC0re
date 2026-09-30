#ifndef GBAC0RE_ERRNO_H
#define GBAC0RE_ERRNO_H

/* Freestanding errno: a plain global, no thread-local magic. mGBA's GBA core
   only ever assigns and tests it (see src/gba/core.c). */

extern int errno;

#define EPERM   1
#define ENOENT  2
#define EIO     5
#define EACCES  13
#define EEXIST  17
#define EINVAL  22
#define EROFS   30

#endif
