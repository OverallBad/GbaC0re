/* GbaC0re PS5 native — Sce* platform backend for real native titles.
 *
 * Video: SceVideoOut, 1920x1080, double-buffered. The display hardware
 *   reads tiled memory, so each frame is composed into a linear scratch
 *   buffer (with scaling + ARGB->ABGR swizzle) and scattered through
 *   ps5_tilemap.c (algorithm from ps5-payload-dev/SDL). Sequence mirrors
 *   that SDL fork's PS5_VideoInit, which is the hardware-proven reference.
 * Audio: SceAudioOut, 48kHz S16 stereo, 256-frame grains, blocking submit.
 * Pad: ScePad, DualSense button map carried over from the v1.x payload
 *   (hardware-verified on 2026-09-28).
 *
 * All imports are hand-declared externs, ps5link-example style; the linker
 * resolves them by NID from the system modules. No libc startup beyond
 * what crt1 provides.
 */

#include "ps5_platform.h"
#include "ps5_gba.h"
#include "ps5_tilemap.h"
#include "menu.h"  /* g_fullscreen */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* ------------------------- hand-declared imports ------------------- */

extern int sceUserServiceInitialize(void *initParams);
extern int sceUserServiceGetInitialUser(int *userId);

extern int sceSystemServiceHideSplashScreen(void);

extern int scePadInit(void);
extern int scePadOpen(int userId, int type, int index, void *param);
extern int scePadReadState(int handle, void *data);
extern int scePadClose(int handle);

extern int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment,
                                             int type, intptr_t *paddr);
extern int sceKernelMapDirectMemory(void **vaddr, size_t len, int prot,
                                    int flags, intptr_t paddr, size_t alignment);
extern int sceKernelReleaseDirectMemory(intptr_t paddr, size_t len);
extern int sceKernelUsleep(unsigned int microseconds);

typedef struct {
    void *data;
    uint64_t pad[3];
} VideoBuf;

typedef struct {
    uint8_t bytes[80];
} VideoAttr;

extern int sceVideoOutOpen(int userId, int busType, int index, const void *attr);
extern void sceVideoOutClose(int handle);
extern int sceVideoOutSetFlipRate(int handle, int rate);
extern int sceVideoOutSubmitFlip(int handle, int bufIdx, uint32_t flipMode,
                                 int64_t flipArg);
/* In the ps5link NID catalog (Add/DeleteFlipEvent are not). */
extern int sceVideoOutIsFlipPending(int handle, int *isPending);
extern void sceVideoOutSetBufferAttribute2(VideoAttr *attr, uint64_t pixelFormat,
                                           uint32_t tilingMode, uint32_t w,
                                           uint32_t h, uint64_t u1, uint32_t u2,
                                           uint64_t u3);
extern int sceVideoOutRegisterBuffers2(int handle, int startIndex, int flags,
                                       VideoBuf *bufs, int numBufs,
                                       VideoAttr *attr, int unk, void *reserved);

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int userId, int portType, int index, int len,
                           int freq, int format);
extern int sceAudioOutOutput(int handle, const void *ptr);
extern int sceAudioOutClose(int handle);

/* ------------------------- video ----------------------------------- */

#define OUT_W 1920
#define OUT_H 1080

static int video_handle = -1;
static intptr_t video_paddr = 0;
static size_t video_memsize = 0;
static void *video_vaddr = NULL;
static VideoBuf video_bufs[2];
static GbaTilemap *video_tmap = NULL;
static uint32_t *video_scratch = NULL;  /* linear 1920x1080 ABGR */
static int video_buf_idx = 0;
static int64_t video_frame_id = 0;

/* ARGB (our buffers) -> ABGR (video out): swap R and B. */
static inline uint32_t argb_to_abgr(uint32_t c) {
    return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
}

int plat_video_init(void) {
    VideoAttr vattr;

    memset(video_bufs, 0, sizeof(video_bufs));
    memset(&vattr, 0, sizeof(vattr));

    sceSystemServiceHideSplashScreen();

    video_handle = sceVideoOutOpen(0xFF, 0, 0, NULL);
    if (video_handle < 0) {
        ps5_diag_logf("PS5N: sceVideoOutOpen failed: %d\n", video_handle);
        return 0;
    }

    /* 32MB direct memory: two 16MB halves, each holds one 1080p tiled
       buffer (~8.9MB). Mirrors the SDL PS5 fork's proven sequence. */
    video_memsize = 0x2000000;
    if (sceKernelAllocateMainDirectMemory(video_memsize, 0x20000, 3,
                                          &video_paddr) != 0) {
        ps5_diag_log("PS5N: sceKernelAllocateMainDirectMemory failed\n");
        return 0;
    }
    if (sceKernelMapDirectMemory(&video_vaddr, video_memsize, 0x33, 0,
                                 video_paddr, 0x20000) != 0) {
        ps5_diag_log("PS5N: sceKernelMapDirectMemory failed\n");
        return 0;
    }
    video_bufs[0].data = video_vaddr;
    video_bufs[1].data = (uint8_t *)video_vaddr + video_memsize / 2;

    sceVideoOutSetFlipRate(video_handle, 0);

    sceVideoOutSetBufferAttribute2(&vattr, 0x8000000022000000UL, 0,
                                   OUT_W, OUT_H, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(video_handle, 0, 0, video_bufs, 2,
                                    &vattr, 0, NULL) != 0) {
        ps5_diag_log("PS5N: sceVideoOutRegisterBuffers2 failed\n");
        return 0;
    }

    video_tmap = gba_tilemap_create(OUT_W, OUT_H);
    video_scratch = (uint32_t *)malloc((size_t)OUT_W * OUT_H * 4);
    if (!video_tmap || !video_scratch) {
        ps5_diag_log("PS5N: video scratch alloc failed\n");
        return 0;
    }

    ps5_diag_logf("PS5N: video: handle %d, buffers %p/%p\n",
                  video_handle, video_bufs[0].data, video_bufs[1].data);
    return 1;
}

/* 240x160 -> 1080p. 16:9 stretches; 3:2 is 6x integer centered on black. */
void plat_video_compose_game(void) {
    const uint32_t *src = ps5_gba_framebuffer;
    uint32_t *dst = video_scratch;

    if (g_fullscreen) {
        for (int dy = 0; dy < OUT_H; dy++) {
            int sy = dy * GBACORE_H / OUT_H;
            const uint32_t *srow = &src[sy * GBACORE_W];
            uint32_t *drow = &dst[dy * OUT_W];
            for (int dx = 0; dx < OUT_W; dx++)
                drow[dx] = argb_to_abgr(srow[dx * GBACORE_W / OUT_W]);
        }
    } else {
        const int scale = 6;  /* 240*6=1440, 160*6=960 */
        const int ox = (OUT_W - GBACORE_W * scale) / 2;
        const int oy = (OUT_H - GBACORE_H * scale) / 2;
        for (int i = 0; i < OUT_W * OUT_H; i++) dst[i] = 0xFF000000u;
        for (int dy = 0; dy < GBACORE_H * scale; dy++) {
            int sy = dy / scale;
            const uint32_t *srow = &src[sy * GBACORE_W];
            uint32_t *drow = &dst[(oy + dy) * OUT_W + ox];
            for (int dx = 0; dx < GBACORE_W * scale; dx++)
                drow[dx] = argb_to_abgr(srow[dx / scale]);
        }
    }
}

/* 480x270 ARGB -> 1080p ABGR, exact 4x. */
void plat_video_compose_ui(const u32 *ui) {
    uint32_t *dst = video_scratch;
    for (int dy = 0; dy < OUT_H; dy++) {
        int sy = dy / 4;
        const uint32_t *srow = &ui[sy * UI_W];
        uint32_t *drow = &dst[dy * OUT_W];
        for (int dx = 0; dx < OUT_W; dx++)
            drow[dx] = argb_to_abgr(srow[dx / 4]);
    }
}

void plat_video_submit(void) {
    gba_tilemap_blit_full(video_tmap, video_scratch, OUT_W,
                           (uint32_t *)video_bufs[video_buf_idx].data);
    sceVideoOutSubmitFlip(video_handle, video_buf_idx, 1, video_frame_id++);
}

void plat_video_sync(void) {
    /* Wait until the submitted flip has been consumed by the display.
       sceVideoOutAdd/DeleteFlipEvent have no NID catalog entries, so poll
       IsFlipPending instead of waiting on a flip event queue. */
    int pending = 1;
    int spins = 0;
    while (pending && spins < 1000000) {
        if (sceVideoOutIsFlipPending(video_handle, &pending) != 0)
            break;
        spins++;
    }
    video_buf_idx ^= 1;
}

void plat_video_shutdown(void) {
    if (video_handle >= 0) {
        sceVideoOutClose(video_handle);
        video_handle = -1;
    }
    if (video_paddr) {
        sceKernelReleaseDirectMemory(video_paddr, video_memsize);
        video_paddr = 0;
        video_vaddr = NULL;
    }
    gba_tilemap_destroy(video_tmap);
    video_tmap = NULL;
    free(video_scratch);
    video_scratch = NULL;
}

/* ------------------------- audio ----------------------------------- */

static int audio_handle = -1;

int plat_audio_init(void) {
    if (sceAudioOutInit() != 0) {
        ps5_diag_log("PS5N: sceAudioOutInit failed\n");
        return 0;
    }
    /* userId 0xFF, MAIN port, 256-frame grains, 48kHz, S16 stereo. */
    audio_handle = sceAudioOutOpen(0xFF, 0, 0, SAMPLES_PER_BUF,
                                   SAMPLE_RATE, 1);
    if (audio_handle < 0) {
        ps5_diag_logf("PS5N: sceAudioOutOpen failed: %d\n", audio_handle);
        return 0;
    }
    ps5_diag_logf("PS5N: audio: handle %d\n", audio_handle);
    return 1;
}

void plat_audio_submit(const s16 *samples, int frames) {
    /* Blocking: each 256-frame grain takes 256/48000 s. This paces the
       emulator — see the frame loop ordering in ps5_main.c. */
    (void)frames;
    if (audio_handle >= 0)
        sceAudioOutOutput(audio_handle, samples);
}

u32 plat_audio_queued(void) {
    return 0;  /* blocking backend: the submit itself is the clock */
}

void plat_audio_shutdown(void) {
    if (audio_handle >= 0) {
        sceAudioOutClose(audio_handle);
        audio_handle = -1;
    }
}

/* ------------------------- pad ------------------------------------- */

/* DualSense button bits in the scePadReadState word, hardware-verified
   in the v1.x payload lineage. */
#define PAD_CROSS   0x00004000u
#define PAD_CIRCLE  0x00002000u
#define PAD_CREATE  0x00000004u
#define PAD_OPTIONS 0x00000008u
#define PAD_UP      0x00000010u
#define PAD_DOWN    0x00000040u
#define PAD_LEFT    0x00000080u
#define PAD_RIGHT   0x00000020u
#define PAD_L2      0x00000100u
#define PAD_R2      0x00000200u
#define PAD_L1      0x00000400u
#define PAD_R1      0x00000800u
#define PAD_TAKEN   0x80000000u

static int pad_handle = -1;

int plat_pad_init(void) {
    int userId = 0;

    sceUserServiceInitialize(0);
    if (sceUserServiceGetInitialUser(&userId) < 0) {
        ps5_diag_log("PS5N: sceUserServiceGetInitialUser failed\n");
        return 0;
    }
    if (scePadInit() < 0) {
        ps5_diag_log("PS5N: scePadInit failed\n");
        return 0;
    }
    pad_handle = scePadOpen(userId, 0, 0, 0);
    if (pad_handle < 0) {
        ps5_diag_logf("PS5N: scePadOpen failed: %d\n", pad_handle);
        return 0;
    }
    ps5_diag_logf("PS5N: pad: user %d, handle %d\n", userId, pad_handle);
    return 1;
}

u16 plat_pad_buttons(int *out_l1, int *out_r1) {
    uint8_t buf[1024];
    uint32_t buttons;
    u16 b = 0;
    int l1 = 0, r1 = 0;

    if (pad_handle < 0) goto out;

    memset(buf, 0, sizeof(buf));
    if (scePadReadState(pad_handle, buf) < 0) goto out;

    buttons = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (buttons & PAD_TAKEN) goto out;  /* system has the controller */

    if (buttons & PAD_CROSS)   b |= GBA_BTN_A;
    if (buttons & PAD_CIRCLE)  b |= GBA_BTN_B;
    if (buttons & PAD_CREATE)  b |= GBA_BTN_SELECT;
    if (buttons & PAD_OPTIONS) b |= GBA_BTN_START;
    if (buttons & PAD_UP)      b |= GBA_BTN_UP;
    if (buttons & PAD_DOWN)    b |= GBA_BTN_DOWN;
    if (buttons & PAD_LEFT)    b |= GBA_BTN_LEFT;
    if (buttons & PAD_RIGHT)   b |= GBA_BTN_RIGHT;
    if (buttons & PAD_L2)      b |= GBA_BTN_L;
    if (buttons & PAD_R2)      b |= GBA_BTN_R;
    l1 = (buttons & PAD_L1) != 0;
    r1 = (buttons & PAD_R1) != 0;

    /* Left stick as d-pad (u8, center ~128). */
    {
        int lx = (int)buf[4] - 128;
        int ly = (int)buf[5] - 128;
        if (lx > 48) b |= GBA_BTN_RIGHT;
        if (lx < -48) b |= GBA_BTN_LEFT;
        if (ly > 48) b |= GBA_BTN_DOWN;
        if (ly < -48) b |= GBA_BTN_UP;
    }

out:
    if (out_l1) *out_l1 = l1;
    if (out_r1) *out_r1 = r1;
    return b;
}

void plat_pad_shutdown(void) {
    if (pad_handle >= 0) {
        scePadClose(pad_handle);
        pad_handle = -1;
    }
}

/* ------------------------- misc ------------------------------------ */

void plat_delay_ms(int ms) {
    sceKernelUsleep((unsigned int)ms * 1000u);
}
