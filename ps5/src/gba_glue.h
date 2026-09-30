#ifndef GBA_GLUE_H
#define GBA_GLUE_H

#include "core.h"

/* The host layer the mGBA GBA core expects.
 *
 * mGBA is built with MINIMAL_CORE=1 + M_CORE_GBA=1 + DISABLE_THREADING=1, so
 * only the GBA core exists and mCoreFindVF always returns it (or NULL). This
 * file hides the whole mCore lifecycle -- VFile ownership, AV stream
 * callbacks, audio resampling, battery saves -- behind the small API main.c
 * uses. It mirrors LuaGB's glue.h shape so the runtime stays core-agnostic.
 *
 * Button bits equal (1u << GBA_KEY_*), so the mask feeds mCore setKeys
 * directly. Bit 15/16 are NOT buttons: they are the wire commands shared with
 * the web controller (lua/gba.lua.in documents the same numbers). */

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

/* Commands on the same wire (never ORed with buttons). */
#define GBA_CMD_MENU   0xFFFE   /* open the pause menu */

struct gba_audio_iface {
    void *gadget;
    void *audio_out;    /* sceAudioOutOutput */
    s32   handle;
};

/* Called once from _start, after shim_init. */
void gba_glue_init(const struct gba_audio_iface *audio);

/* Load a .gba from an absolute path. Returns 1 on success.
   Stores the outgoing game's save, then tears down any previous game and
   rewinds the shim arena. */
int  gba_load_rom(const char *path);

/* Unloads the current game if one is loaded. Teardown only -- it does NOT
   save; saving is the caller's job (gba_load_rom, menu, quit). Safe to call
   otherwise. */
void gba_unload(void);

/* Runs the emulated machine for one frame (1/60s). The finished picture is
   in gba_framebuffer (240x160 ARGB). */
void gba_run_frame(void);

/* Resamples the frame's 32768Hz audio to 48kHz and hands 256-frame batches
   to sceAudioOutOutput. Blocking, and that is deliberate: it is the
   emulator's master clock, exactly like LuaGB's gb_audio_flush. */
void gba_audio_flush(void);
void gba_audio_discard(void);
void gba_audio_ff_flush(int factor);

/* Applies a bitmask of GBA_BTN_* to the emulated pad. */
void gba_set_input(u16 mask);

/* Writes the battery save back out through the savedata write window.
   Idempotent per game session, and skipped entirely when the game never
   touched save memory (compared against the load-time snapshot).
   Safe to call when the cartridge has no save or no ROM is loaded. */
void gba_save_store(void);

/* Menu audio drain (v1.1.1): submits at most 4 grains (1024 frames) per
   call so queued click tones play across frames instead of stalling input
   and rendering. Call once per menu frame; a no-op when the ring is
   empty. */
void gba_audio_flush_menu(void);

/* UI click tones for the pause menu (v1.1.0): kind is CLICK_TICK (move),
   CLICK_CONFIRM (select/open) or CLICK_BACK (close). Soft sine bursts with
   an exponential decay, synthesised at 48kHz and queued into the output
   ring; the menu loop drains them via gba_audio_flush_menu(). Silent
   no-op when audio is down. */
#define CLICK_TICK    0
#define CLICK_CONFIRM 1
#define CLICK_BACK    2
void gba_ui_click(int kind);

/* Non-zero makes gba_run_frame narrate its phases, like LuaGB's gb_trace. */
extern int gba_trace;

/* True once a ROM is running. */
int  gba_is_loaded(void);

/* ROM filename stem, for the log and the save name. */
const char *gba_title(void);

/* Raw core access for save states (savestate.c). */
struct mCore;
struct mCore *gba_core(void);

/* The finished frame. Defined in gba_glue.c. */
extern u32 gba_framebuffer[GBA_W * GBA_H];

#endif
