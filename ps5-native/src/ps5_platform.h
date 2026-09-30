#ifndef PS5_PLATFORM_H
#define PS5_PLATFORM_H

/* GbaC0re PS5 native — platform abstraction.
 *
 * The emulator core, UI, audio resampler, saves, and link bridge are pure C
 * and platform-agnostic. Everything that touches the console (display,
 * audio, pad, timing) goes through this interface. The native-title
 * backend (ps5_platform_sce.c) implements it against SceVideoOut,
 * SceAudioOut, and ScePad directly — no SDL: SDL2 has no supported path
 * into a ps5link-signed native title (thread-locals, unproven link).
 *
 * FRAME PACING (hardware-verified pattern from the v1.x payload lineage):
 * the video flip is SUBMITTED first (non-blocking), then the audio flush
 * blocks until its grains drain — audio is the master clock — and only
 * then do we wait for flip completion. The two waits overlap instead of
 * adding, holding ~60fps. Collapsing submit+wait into one call would
 * serialize them and halve the frame rate.
 */

#include "ps5_core.h"

/* ---- video: 1920x1080 output -------------------------------------- */

/* Opens the display. Returns 1 on success. */
int plat_video_init(void);

/* Compose the 240x160 ARGB game frame into the linear scratch frame. */
void plat_video_compose_game(void);

/* Compose a 480x270 ARGB UI buffer into the linear scratch frame. */
void plat_video_compose_ui(const u32 *ui);

/* Tile-blit the scratch frame into the current video buffer and submit
 * the flip. Non-blocking. */
void plat_video_submit(void);

/* Block until the submitted flip completes, then swap to the other
 * video buffer for the next frame. */
void plat_video_sync(void);

void plat_video_shutdown(void);

/* ---- audio: 48kHz stereo s16 -------------------------------------- */

/* Opens audio output (256-frame grains). Returns 1 on success. */
int plat_audio_init(void);

/* Blocking submit of one or more 256-frame grains. This is the frame
 * pacer: each grain takes 256/48000 s of real time. */
void plat_audio_submit(const s16 *samples, int frames);

/* Bytes currently queued in the output path. Always 0 for the blocking
 * backend (the submit itself paces); kept for interface symmetry. */
u32 plat_audio_queued(void);

void plat_audio_shutdown(void);

/* ---- pad: DualSense ------------------------------------------------ */

/* Opens the first controller. Returns 1 on success. */
int plat_pad_init(void);

/* Returns the GBA_BTN_* mask; out_l1/out_r1 report the raw L1/R1
 * shoulder states for the edge-triggered menu / fast-forward commands. */
u16 plat_pad_buttons(int *out_l1, int *out_r1);

void plat_pad_shutdown(void);

/* ---- misc ---------------------------------------------------------- */

void plat_delay_ms(int ms);

#endif
