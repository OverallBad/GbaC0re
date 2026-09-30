#ifndef PC_CORE_H
#define PC_CORE_H

/* GbaC0re PC port — shared types and defines.
 * Stripped of PS5/LuaC0re specifics; uses the real libc. */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

#define VERSION_STR "GbaC0re PC v1.2"

/* GBA native resolution. */
#define GBA_W   240
#define GBA_H   160

/* UI framebuffer (menu). 480x270, scaled by SDL. */
#define UI_W      480
#define UI_H      270

/* Audio: 48kHz stereo, matching the PS5 build's resampler output. */
#define SAMPLE_RATE      48000
#define SAMPLES_PER_BUF  256

/* mGBA GBA audio native rate. */
#define GBA_APU_RATE 32768

/* Local directories (relative to working dir). */
#define PC_SAVE_DIR  "saves/"
#define PC_STATE_DIR "states/"

/* Simple log to stderr. */
#include <stdio.h>
#define klog(msg) fprintf(stderr, "%s", msg)

#endif
