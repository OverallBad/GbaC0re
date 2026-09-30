#ifndef PS5_GBA_H
#define PS5_GBA_H

#include "ps5_core.h"

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

/* PC audio output: called with 256-frame (512-sample) stereo s16 batches
   at 48kHz. Implemented by ps5_main.c via SDL_QueueAudio. */
typedef void (*ps5_audio_submit_fn)(const s16 *samples, int frames);

void ps5_gba_set_audio(ps5_audio_submit_fn fn);

/* Load a .gba from a filesystem path. Returns 1 on success. */
int  ps5_gba_load_rom(const char *path);

/* Unload the current game (saves battery first). */
void ps5_gba_unload(void);

/* Reset the loaded ROM to power-on state (keeps battery save). */
void ps5_gba_reset(void);

/* Attach the link-cable bridge driver to the running core (no-op if none). */
void ps5_gba_link_attach(void);

/* Run one frame (1/60s). Picture lands in ps5_gba_framebuffer (240x160 ARGB). */
void ps5_gba_run_frame(void);

/* Resample the frame's audio to 48kHz and submit via the audio callback. */
void ps5_gba_audio_flush(void);
void ps5_gba_audio_flush_chipmunk(int factor);
void ps5_gba_audio_discard(void);

/* Apply a GBA_BTN_* bitmask to the emulated pad. */
void ps5_gba_set_input(u16 mask);

/* Write the battery save to saves/<stem>.sav. Idempotent per session. */
void ps5_gba_save_store(void);

/* UI click tones (ported from PS5). */
#define CLICK_TICK    0
#define CLICK_CONFIRM 1
#define CLICK_BACK    2
void ps5_gba_ui_click(int kind);
void ps5_gba_audio_flush_menu(void);

extern int ps5_gba_trace;
int  ps5_gba_is_loaded(void);
const char *ps5_gba_title(void);

/* Raw core for save states. */
struct mCore;
struct mCore *ps5_gba_core(void);

/* The finished frame. */
extern u32 ps5_gba_framebuffer[GBACORE_W * GBACORE_H];

#endif
