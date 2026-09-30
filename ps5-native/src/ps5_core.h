#ifndef PS5_CORE_H
#define PS5_CORE_H

/* GbaC0re PS5 native — shared types and defines.
 * New lineage: native PS5 (Prospero) homebrew, NOT a LuaC0re payload.
 * Uses the SDK's real libc — no freestanding shims, no arenas. */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef long           s64;
typedef int            s32;
typedef short          s16;
typedef signed char    s8;

#define VERSION_STR "GbaC0re PS5 native v2.0.0"

/* GBA native resolution. */
#define GBACORE_W   240
#define GBACORE_H   160

/* UI framebuffer (menu). 480x270, scaled by SDL. */
#define UI_W      480
#define UI_H      270
#define UI_SCALE  4   /* 480*4 = 1920, 270*4 = 1080 */

/* Output resolution the UI blit helpers assume. Kept for the payload-era
   ui.c helpers (blit_ui/blit_scale/clear_fb); the SDL path scales through
   textures instead. */
#define SCR_W   1920
#define SCR_H   1080

/* Game integer scale and centering, for the payload-era ui.c helpers. */
#define SCALE   6
#define OFF_X   ((SCR_W - GBACORE_W * SCALE) / 2)
#define OFF_Y   ((SCR_H - GBACORE_H * SCALE) / 2)

/* Audio: 48kHz stereo, matching the resampler output. */
#define SAMPLE_RATE      48000
#define SAMPLES_PER_BUF  256

/* mGBA GBA audio native rate. */
#define GBA_APU_RATE 32768

/* Local directories, relative to the app's working directory.
 * KNOWN UNKNOWN: the homebrew sandbox's writable locations on 13.60/etaHEN
 * are unverified. These are centralized here so the path changes in one
 * place once Ty reports where files actually land. */
#define PS5_SAVE_DIR  "saves/"
#define PS5_STATE_DIR "states/"
#define PS5_ROM_DIR   "roms/"

/* GBA buttons, matching enum GBAKey in mgba/internal/gba/input.h. */
#define GBA_BTN_A      0x001
#define GBA_BTN_B      0x002
#define GBA_BTN_SELECT 0x004
#define GBA_BTN_START  0x008
#define GBA_BTN_RIGHT  0x010
#define GBA_BTN_LEFT   0x020
#define GBA_BTN_UP     0x040
#define GBA_BTN_DOWN   0x080
#define GBA_BTN_R      0x100
#define GBA_BTN_L      0x200

/* Out-of-band command: open the pause menu (DualSense L1). */
#define GBA_CMD_MENU  0x8000

/* Diagnostics: stderr (visible via the deploy tooling's stdout stream /
 * klogsrv) plus an on-screen notification where the SDK supports it.
 * See ps5_diag.h. */
#include "ps5_diag.h"
#define klog(msg) ps5_diag_log(msg)

#endif
