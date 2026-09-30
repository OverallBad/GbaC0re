#ifndef PC_GBA_H
#define PC_GBA_H

#include "pc_core.h"

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
   at 48kHz. Implemented by pc_main.c via SDL_QueueAudio. */
typedef void (*pc_audio_submit_fn)(const s16 *samples, int frames);

void pc_gba_set_audio(pc_audio_submit_fn fn);

/* Load a .gba from a filesystem path. Returns 1 on success. */
int  pc_gba_load_rom(const char *path);

/* Unload the current game (saves battery first). */
void pc_gba_unload(void);

/* Reset the loaded ROM to power-on state (keeps battery save). */
void pc_gba_reset(void);

/* Attach the link-cable bridge driver to the running core (no-op if none). */
void pc_gba_link_attach(void);

/* Run one frame (1/60s). Picture lands in pc_gba_framebuffer (240x160 ARGB). */
void pc_gba_run_frame(void);

/* Resample the frame's audio to 48kHz and submit via the audio callback. */
void pc_gba_audio_flush(void);
void pc_gba_audio_flush_chipmunk(int factor);
void pc_gba_audio_discard(void);

/* Apply a GBA_BTN_* bitmask to the emulated pad. */
void pc_gba_set_input(u16 mask);

/* Write the battery save to saves/<stem>.sav. Idempotent per session. */
void pc_gba_save_store(void);

/* UI click tones (ported from PS5). */
#define CLICK_TICK    0
#define CLICK_CONFIRM 1
#define CLICK_BACK    2
void pc_gba_ui_click(int kind);
void pc_gba_audio_flush_menu(void);

extern int pc_gba_trace;
int  pc_gba_is_loaded(void);
const char *pc_gba_title(void);

/* Raw core for save states. */
struct mCore;
struct mCore *pc_gba_core(void);

/* The finished frame. */
extern u32 pc_gba_framebuffer[GBA_W * GBA_H];

#endif
