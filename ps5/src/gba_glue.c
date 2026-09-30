/* GbaC0re v0.7 -- mGBA GBA core adapter.
 *
 * Owns the mCore lifecycle the way LuaGB's glue.c owns fixGB: find, init,
 * loadROM, AV stream, runFrame, saves. main.c only sees the small API in
 * gba_glue.h and never touches an mCore pointer.
 *
 * VFile ownership rule (kept from v0.6, which debugged this the hard way):
 * after a successful loadROM/loadSave the core OWNS the VFile -- do not
 * close it; unloadROM closes both. Closing early is a use-after-free
 * followed by a double-close inside the core.
 *
 * Audio: mGBA's GBA core renders at 32768Hz (GBA_ARM7TDMI_FREQUENCY /
 * sampleInterval, see mgba/src/gba/audio.c). sceAudioOut is opened at
 * 48kHz, so postAudioFrame samples accumulate in a source ring and
 * gba_audio_flush resamples them with a fixed-point linear interpolator
 * before submitting whole 256-frame batches. No floating point, no libm. */

/* NOTE: must come before our local "core.h", which #defines GBA_H to 160
   (screen height) -- that would trip this header's include guard. */
#include <mgba/internal/gba/gba.h>

#include "core.h"
#include "shim.h"
#include "gba_glue.h"
#include "savedata.h"
#include "menu.h"  /* g_volume */
#include "bridge.h"  /* link-cable bridge: attached to every loaded core */

#include <stdarg.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

#define GBA_ROM_MAX   (32 * 1024 * 1024)   /* largest possible GBA cartridge */
#define GBA_SAVE_MAX  (128 * 1024)        /* largest GBA save (Flash 1Mbit) */

/* Audio rings, in stereo frames. One emulated frame yields ~546 source
   samples at 32768Hz and ~800 output samples at 48kHz; both rings hold
   several frames so a late flush never overflows. */
#define SRC_RING_FRAMES 4096
#define OUT_RING_FRAMES 4096

u32 gba_framebuffer[GBA_W * GBA_H];
int gba_trace = 0;

/* The software renderer draws DIRECTLY into the buffer handed to
   setVideoBuffer, and it SKIPS scanlines it considers clean (scanlineDirty
   in video-software.c) -- on a static screen most scanlines are skipped
   every frame. av_video must therefore never transform that buffer in
   place: an in-place R/B swap would toggle already-swapped pixels back on
   every frame the renderer skips, i.e. a full-frame red/blue flicker
   (observed on hardware in v0.7.8). The renderer draws unswapped
   0x00BBGGRR here; av_video copies with the swap into gba_framebuffer,
   which is idempotent no matter which scanlines the renderer skipped. */
static u32 gba_render_target[GBA_W * GBA_H];

static struct mCore *core;
static char rom_title[MAX_NAME + 8];
static char rom_save_path[160];

static struct gba_audio_iface au;

/* Save staging lives in .bss, not the arena: the clone's malloc'd block is
   copied here, so repeated saves don't grow the arena, and the buffer
   survives arena_reset() across game switches. save_base snapshots what was
   loaded so gba_save_store() can skip untouched games; save_stored makes
   the store idempotent no matter how many call sites fire per transition. */
static u8 save_buf[GBA_SAVE_MAX];
static u8 save_base[GBA_SAVE_MAX];
static size_t save_base_size;
static int save_stored;

/* 32768Hz source samples from postAudioFrame. */
static s16 src_ring[SRC_RING_FRAMES * 2];
static int src_frames;

/* Fixed-point resampler state. phase advances by step per OUTPUT sample;
   step = src_rate * 65536 / 48000. src_keep holds the last source sample of
   the previous block so interpolation never reads past the block end. */
static u32 resample_phase;
static u32 resample_step;
static s16 src_keep[2];
static int src_rate = 32768;

static const s16 click_sine[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

/* 48kHz output samples waiting for sceAudioOutOutput. Declared up here so
   the UI click tones can queue into it; the menu drains it a few grains
   per frame instead of submitting synchronously. */
static s16 out_ring[OUT_RING_FRAMES * 2];
static int out_frames;

/* Menu click tones, v1.1.2: each tone is exactly one 256-frame grain
   (5.3ms). The v1.1.0 24-50ms tones stalled the menu on blocking audio
   submits; v1.1.1 queued them but still drained up to 4 grains (21ms) per
   menu frame, and scrolling kept every frame at max drain. A 5ms tick is
   still clearly audible as UI feedback, and one grain can never backlog:
   the per-frame drain always clears a press within one frame. Pitches stay
   distinct (tick high, confirm mid, back low). Amplitudes deliberately
   quiet. */
#define CLICK_TICK    0   /* menu navigation */
#define CLICK_CONFIRM 1   /* select / open */
#define CLICK_BACK    2   /* back / close */

static const struct { int freq; int frames; int peak; int decay_q16; } click_cfg[3] = {
    { 1500, 256, 2300, 64632 },   /* tick:    5.3ms, tau 1.5ms */
    {  840, 256, 3600, 64857 },   /* confirm: 5.3ms, tau 2ms */
    {  600, 256, 2900, 64857 },   /* back:    5.3ms, tau 2ms */
};

void gba_ui_click(int kind) {
    if (!au.audio_out || au.handle < 0) return;
    if (kind < 0 || kind > 2) kind = CLICK_TICK;

    int freq  = click_cfg[kind].freq;
    int n     = click_cfg[kind].frames;
    int peak  = click_cfg[kind].peak;
    int decay = click_cfg[kind].decay_q16;

    /* Pad to whole 256-frame grains so the menu flush can always drain the
       tone completely. v1.1.2 tones are exactly one grain; the +256 below is
       belt and braces for a sub-grain game-audio tail ahead of the tone.
       512 < 4096 OUT_RING_FRAMES, so this cannot overrun; the check stays.
       A click that does not fit is dropped, never corrupting queued audio. */
    int total = (n + 255) & ~255;
    if (out_frames + total > OUT_RING_FRAMES) return;

    /* 8.24 fixed-point phase into the 256-entry sine table. */
    u32 phase = 0;
    u32 step = (u32)(((u64)(u32)freq << 24) / 48000);
    s32 amp = (s32)peak << 16;   /* 16.16 envelope */

    for (int i = 0; i < n; i++) {
        int s = click_sine[(phase >> 24) & 255];
        phase += step;
        s32 v = ((s32)s * (amp >> 16)) >> 15;
        out_ring[(out_frames + i) * 2]     = (s16)v;
        out_ring[(out_frames + i) * 2 + 1] = (s16)v;
        /* s64 intermediate: amp*decay peaks around 1.5e13, far past s32. */
        amp = (s32)(((s64)amp * (s64)decay) >> 16);
    }
    for (int i = n; i < total; i++)
        out_ring[(out_frames + i) * 2] = out_ring[(out_frames + i) * 2 + 1] = 0;
    out_frames += total;
}

/* ------------------------------------------------------------ audio in -- */

static void av_vsize(struct mAVStream *s, unsigned w, unsigned h) {
    (void)s; (void)w; (void)h;   /* fixed 240x160 */
}

static void av_rate(struct mAVStream *s, unsigned rate) {
    (void)s;
    if (rate) {
        src_rate = (int)rate;
        resample_step = (u32)(((u64)rate << 16) / SAMPLE_RATE);
    }
}

static void av_video(struct mAVStream *s, const mColor *buf, size_t stride) {
    (void)s;
    if (!buf) return;
    /* buf is gba_render_target (the renderer's own unswapped 0x00BBGGRR
       buffer); dst is gba_framebuffer. They never alias, so this copy is
       idempotent -- safe against the renderer's clean-scanline skipping. */
    for (unsigned y = 0; y < GBA_H; y++) {
        const u32 *row = (const u32 *)buf + y * stride;
        u32 *dst = &gba_framebuffer[y * GBA_W];
        for (unsigned x = 0; x < GBA_W; x++) {
            u32 c = row[x];
            /* mGBA packs 32-bit pixels as 0x00BBGGRR; gba_framebuffer is
               documented ARGB (the video-out plane is A8R8G8B8), so red and
               blue trade places here -- once per source pixel, not once
               per scaled output pixel in blit_scale. */
            dst[x] = (c & 0x0000FF00u) | ((c & 0x000000FFu) << 16) |
                     ((c & 0x00FF0000u) >> 16);
        }
    }
}

/* One source sample per call. If the ring is full the oldest samples are
   dropped -- better a glitch than a stall, and the flush runs every frame. */
static void av_sample(struct mAVStream *s, int16_t l, int16_t r) {
    (void)s;
    if (src_frames >= SRC_RING_FRAMES) return;
    src_ring[src_frames * 2]     = l;
    src_ring[src_frames * 2 + 1] = r;
    src_frames++;
}

static void av_buffer(struct mAVStream *s, struct mAudioBuffer *b) {
    (void)s; (void)b;   /* the GBA core uses per-sample delivery */
}

/* ---------------------------------------------------------- audio out -- */

/* Resample all pending source frames to 48kHz into the output ring, keeping
   the last source sample as the interpolation overlap for the next block. */
static void resample_pending(void) {
    if (src_frames <= 0) return;

    /* src_keep holds sample[-1]; block samples are sample[0..n-1]. */
    int n = src_frames;
    for (;;) {
        u32 idx = resample_phase >> 16;
        if (idx >= (u32)n) break;
        u32 frac = resample_phase & 0xFFFF;

        s16 s0l = (idx == 0) ? src_keep[0] : src_ring[(idx - 1) * 2];
        s16 s0r = (idx == 0) ? src_keep[1] : src_ring[(idx - 1) * 2 + 1];
        s16 s1l = src_ring[idx * 2];
        s16 s1r = src_ring[idx * 2 + 1];

        if (out_frames >= OUT_RING_FRAMES) break;
        /* s64 product: s32 would overflow (65534 * 65535 > 2^31), which
           wraps into distortion instead of clipping. */
        s64 dl = (s64)s1l - (s64)s0l;
        s64 dr = (s64)s1r - (s64)s0r;
        out_ring[out_frames * 2]     = (s16)(s0l + ((dl * (s64)frac) >> 16));
        out_ring[out_frames * 2 + 1] = (s16)(s0r + ((dr * (s64)frac) >> 16));
        out_frames++;

        resample_phase += resample_step;
    }

    /* Carry: the last source sample becomes sample[-1] of the next block,
       and the phase is rebased so idx 0 is the first NEW sample. */
    src_keep[0] = src_ring[(n - 1) * 2];
    src_keep[1] = src_ring[(n - 1) * 2 + 1];
    resample_phase -= (u32)n << 16;
    src_frames = 0;
}

/* Shared submit loop. max_grains bounds how many 256-frame grains one call
   may push: the play loop drains everything (audio is the master clock),
   the menu drains at most 2 grains per frame. v1.1.2 click tones are a
   single grain each, so one press clears within one frame and input never
   stalls on audio. */
static void audio_flush_n(int max_grains) {
    resample_pending();
    if (!au.audio_out || au.handle < 0) { out_frames = 0; return; }

    /* Master volume: scale the 16-bit samples. 10 = full, 0 = mute. */
    if (g_volume < 10) {
        for (int i = 0; i < out_frames * 2; i++) {
            out_ring[i] = (s16)((out_ring[i] * g_volume) / 10);
        }
    }

    /* sceAudioOutOutput consumes exactly the open-time granularity
       (256 stereo frames). Submit only whole buffers and keep the
       remainder -- never a partial buffer padded with stale ring data. */
    int n = 0;
    while (out_frames >= SAMPLES_PER_BUF && n < max_grains) {
        s32 ret = (s32)NC(au.gadget, au.audio_out, (u64)(u32)au.handle,
                          (u64)out_ring, 0, 0, 0, 0);
        /* sceAudioOutOutput returns the frames consumed (256) on success;
           only a negative return is an error. */
        if (ret < 0) {
            printf("audio: sceAudioOutOutput failed ret=%ld\n", (long)ret);
        }
        int rem = out_frames - SAMPLES_PER_BUF;
        for (int i = 0; i < rem * 2; i++)
            out_ring[i] = out_ring[SAMPLES_PER_BUF * 2 + i];
        out_frames = rem;
        n++;
    }
}

void gba_audio_flush(void) {
    audio_flush_n(0x7fffffff);
}

void gba_audio_discard(void) {
    out_frames = 0;
}

/* Fast-forward chipmunk: the N emulation frames leave ~Nx audio in out_ring.
   Decimate by N (every Nth stereo frame) and flush at the normal 48kHz rate:
   the pitch rises Nx (chipmunk) and the flush blocks only 1x time, so the
   Nx speedup survives. */
void gba_audio_ff_flush(int factor) {
    resample_pending();
    int m = out_frames / factor;
    for (int i = 0; i < m; i++) {
        out_ring[i * 2]     = out_ring[i * 2 * factor];
        out_ring[i * 2 + 1] = out_ring[i * 2 * factor + 1];
    }
    out_frames = m;
    audio_flush_n(0x7fffffff);
}

void gba_audio_flush_menu(void) {
    audio_flush_n(2);
}


/* ------------------------------------------------------------------ -- */

/* mGBA's own trace (per-SWI at DEBUG, per-DMA at INFO) costs thousands of
 * formatted UDP packets a second and buries real diagnostics. Only actual
 * problems come through. */
static void gba_log_fn(struct mLogger *log, int category, enum mLogLevel level,
                       const char *fmt, va_list args) {
    (void)log; (void)category;
    if (level & (mLOG_INFO | mLOG_DEBUG | mLOG_STUB)) return;
    char buf[256];
    vsnprintf(buf, sizeof buf, fmt, args);
    int n = 0;
    while (buf[n]) n++;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    /* "Invalid video register" is the game poking video registers the
       software renderer doesn't implement (e.g. Shining Soul 2 writing
       WINOUT at 0x4E) -- benign, repeats forever, buries real warnings. */
    if (strstr(buf, "Invalid video register")) return;
    printf("mgba: %s\n", buf);
}

static struct mLogger gba_logger = { gba_log_fn, 0 };

void gba_glue_init(const struct gba_audio_iface *audio) {
    au = *audio;
    resample_step = (u32)(((u64)(u32)src_rate << 16) / SAMPLE_RATE);
    src_keep[0] = src_keep[1] = 0;
    mLogSetDefaultLogger(&gba_logger);
}

static void stem_of(const char *path, char *out, int max) {
    int n = 0;
    while (path[n]) n++;
    int start = 0, end = n;
    for (int i = 0; i < n; i++)
        if (path[i] == '/' || path[i] == '\\') start = i + 1;
    for (int i = n - 1; i >= start; i--)
        if (path[i] == '.') { end = i; break; }
    int k = 0;
    for (int i = start; i < end && k < max - 1; i++) out[k++] = path[i];
    out[k] = 0;
}

static void build_save_path(const char *rom_path, char *out, int max) {
    int n = 0;
    while (rom_path[n] && n < max - 1) { out[n] = rom_path[n]; n++; }
    out[n] = 0;
    /* ROMs live in ROM_DIR; saves live in SAVE_DIR under the same stem. */
    const char *base = rom_path;
    for (int i = 0; rom_path[i]; i++)
        if (rom_path[i] == '/') base = rom_path + i + 1;
    int k = 0;
    while (SAVE_DIR[k] && k < max - 1) { out[k] = SAVE_DIR[k]; k++; }
    int j = 0;
    while (base[j] && base[j] != '.' && k < max - 1) { out[k++] = base[j++]; }
    const char *ext = ".sav";
    while (*ext && k < max - 1) out[k++] = *ext++;
    out[k] = 0;
}

/* Teardown only. Saving is the caller's job (gba_load_rom saves the outgoing
   game first; main.c saves on menu/quit) -- an implicit save here made every
   transition store twice. */
void gba_unload(void) {
    if (!core) return;
    bridge_detach();  /* drop the SIO driver before the GBA struct is freed */
    core->unloadROM(core);
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    core = NULL;
    rom_title[0] = 0;
    rom_save_path[0] = 0;
    save_stored = 0;
}

int gba_load_rom(const char *path) {
    gba_save_store();          /* outgoing game, before its arena is rewound */
    gba_unload();              /* tears down the previous game */
    arena_reset();             /* then rewinds every allocation it made */

    src_frames = 0;
    out_frames = 0;
    resample_phase = 0;
    src_keep[0] = src_keep[1] = 0;

    /* ROM image: read whole, wrapped read-only. GBA ROMs are at most 32MB,
     * but the bump arena on PS5 hardware is only ~32MB itself, so size the
     * allocation to the actual file instead of the 32MB worst case. Falls
     * back to the full 32MB if the size probe fails. */
    FILE *f = fopen(path, "rb");
    if (!f) { printf("GBA: cannot open %s\n", path); return 0; }
    size_t want = GBA_ROM_MAX;
    if (fseek(f, 0, SEEK_END) == 0) {
        long fsz = ftell(f);
        if (fsz > 0 && (u64)fsz <= GBA_ROM_MAX) want = (size_t)fsz;
        fseek(f, 0, SEEK_SET);
    }
    u8 *rom = malloc(want);
    if (!rom) { printf("GBA: no arena for ROM\n"); fclose(f); return 0; }
    size_t n = fread(rom, 1, want, f);
    fclose(f);
    if (n < 192) { printf("GBA: %s too small (%d)\n", path, (int)n); return 0; }
    /* Sanity: a GBA ROM starts with a branch into the header (or zeroes for
       a homebrew that still boots). Reject obvious non-ROMs early. */
    printf("GBA: %s, %d bytes\n", path, (int)n);

    struct VFile *vf = VFileFromConstMemory(rom, n);
    if (!vf) { printf("GBA: VFile failed\n"); return 0; }

    core = mCoreFindVF(vf);
    if (!core) { vf->close(vf); printf("GBA: no core for this file\n"); return 0; }

    mCoreConfigInit(&core->config, NULL);
    if (!core->init(core)) {
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        vf->close(vf);
        core = NULL;
        printf("GBA: core init failed\n");
        return 0;
    }

    /* The renderer draws into gba_render_target, never into gba_framebuffer
       directly -- see the note at its declaration. av_video copies with the
       R/B swap into gba_framebuffer once per frame. */
    core->setVideoBuffer(core, (mColor *)gba_render_target, GBA_W);

    if (!core->loadROM(core, vf)) {
        /* loadROM failed: the core did NOT take the VFile, close it here. */
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        vf->close(vf);
        core = NULL;
        printf("GBA: loadROM failed\n");
        return 0;
    }
    /* loadROM succeeded: the core owns vf now. Do NOT close it --
       unloadROM will. (v0.6 use-after-free fix, kept.) */

    mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
    mCoreLoadConfig(core);

    /* Audio fixup (v0.7.21): GBAAudioInit should set masterVolume=0x100 and
       forceDisableChA/B=false, but on this build the mixer outputs silence
       until these fields are written explicitly. Root cause TBD. */
    {
        struct GBA* gba = core->board;
        gba->audio.masterVolume = GBA_AUDIO_VOLUME_MAX;
        gba->audio.forceDisableChA = false;
        gba->audio.forceDisableChB = false;
        /* Link-cable bridge: install the SIO driver now. It no-ops until
           a session is hosted from the pause menu, and re-attaching here
           keeps it live across ROM changes. */
        bridge_attach(gba);
    }

    /* Battery save from the previous session, if any. The core owns sv
       after a successful loadSave -- same rule as the ROM VFile. */
    char save_path[160];
    build_save_path(path, save_path, sizeof(save_path));
    {
        int k = 0;
        while (save_path[k] && k < (int)sizeof(rom_save_path) - 1) {
            rom_save_path[k] = save_path[k];
            k++;
        }
        rom_save_path[k] = 0;
    }
    u8 *savebuf = save_buf;
    {
        FILE *sf = fopen(save_path, "rb");
        if (sf) {
            size_t sn = fread(savebuf, 1, GBA_SAVE_MAX, sf);
            fclose(sf);
            if (sn > 0) {
                struct VFile *sv = VFileFromConstMemory(savebuf, sn);
                if (sv && !core->loadSave(core, sv))
                    sv->close(sv);   /* rejected: core didn't take it */
                printf("GBA: save %s (%d bytes)\n", save_path, (int)sn);
            }
        }
    }
    /* Baseline for the dirtiness check. Whatever the clone returns later is
       compared against this; an untouched game never touches the savedata. */
    {
        void *cur = 0;
        size_t n = 0;
        if (core->savedataClone)
            n = core->savedataClone(core, &cur);
        if (n > GBA_SAVE_MAX) n = GBA_SAVE_MAX;
        if (n && cur) {
            memcpy(save_base, cur, n);
            save_base_size = n;
        } else {
            save_base_size = 0;
        }
    }

    struct mAVStream *st = malloc(sizeof(struct mAVStream));
    if (!st) { gba_unload(); return 0; }
    st->videoDimensionsChanged = av_vsize;
    st->audioRateChanged       = av_rate;
    st->postVideoFrame         = av_video;
    st->postAudioFrame         = av_sample;
    st->postAudioBuffer        = av_buffer;
    core->setAVStream(core, st);

    core->reset(core);

    stem_of(path, rom_title, sizeof(rom_title));
    printf("GBA: core running: %s\n", rom_title);
    {
        char ab[64];
        snprintf(ab, sizeof ab, "GBA: arena used %lu/%lu after load\n",
                 (unsigned long)arena_used(),
                 (unsigned long)arena_size_get());
        printf("%s", ab);
    }
    return 1;
}

void gba_run_frame(void) {
    if (!core) return;
    core->runFrame(core);
}

void gba_set_input(u16 mask) {
    if (!core) return;
    core->setKeys(core, (u32)(mask & 0x3FF));
}

/* Idempotent: every transition path (menu, quit, game switch, shutdown) may
   call this, but the bytes are written at most once per game session.
   save_stored is set only once the bytes are actually committed (or proven
   untouched) -- a failed stage or write window leaves it clear so a later
   call retries instead of silently dropping the save. */
void gba_save_store(void) {
    if (!core || !rom_title[0] || save_stored) return;

    void *saved = 0;
    size_t save_size = 0;
    if (core->savedataClone)
        save_size = core->savedataClone(core, &saved);
    if (!save_size || !saved) return;
    if (save_size > GBA_SAVE_MAX) save_size = GBA_SAVE_MAX;

    /* Dirtiness check: skip games that never touched save memory. */
    if (save_size == save_base_size && memcmp(saved, save_base, save_size) == 0) {
        printf("GBA: save untouched, skipping write\n");
        save_stored = 1;
        return;
    }
    memcpy(save_buf, saved, save_size);

    /* Build the save outside the container first. The read-write window
       unmounts and remounts savedata, so hold it open as briefly as
       possible -- stage, then copy in. (LuaGB's pattern, kept.) */
    const char *staging = "/av_contents/content_tmp/gba_tmp.sav";
    FILE *t = fopen(staging, "wb");
    if (!t) { printf("GBA: cannot stage save\n"); return; }
    fwrite(save_buf, 1, save_size, t);
    fclose(t);

    if (savedata_begin_write() != 0) {
        printf("GBA: no write window, save left at %s\n", staging);
        return;
    }

    /* The write window is a FRESH REMOUNT of the savedata container: the
       boot-time mkdir ran against the read-only mount and never took, so
       /savedata0/saves/ does not exist here until it is created inside the
       window. Without this the save open below dies with ENOENT (v0.7.8). */
    {
        int mr = shim_mkdir(SAVE_DIR);
        printf("GBA: save mkdir -> %d\n", mr);
    }

    char save_path[160];
    {
        int k = 0;
        while (rom_save_path[k] && k < (int)sizeof(save_path) - 1) {
            save_path[k] = rom_save_path[k];
            k++;
        }
        save_path[k] = 0;
    }
    int ok = 0;
    {
        FILE *in = fopen(staging, "rb");
        FILE *out = fopen(save_path, "wb");
        if (in && out) {
            u8 chunk[4096];
            size_t r;
            ok = 1;
            while ((r = fread(chunk, 1, sizeof(chunk), in)) > 0) {
                if (fwrite(chunk, 1, r, out) != r) { ok = 0; break; }
            }
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }

    /* The unmount inside this call is what actually commits the bytes. */
    savedata_end_write();

    if (ok) {
        /* Only now is the new baseline valid: an earlier snapshot would make
           a retry look "untouched" and skip the write it still owes. */
        memcpy(save_base, saved, save_size);
        save_base_size = save_size;
        save_stored = 1;
        printf("GBA: saved %s (%d bytes)\n", save_path, (int)save_size);
    } else {
        printf("GBA: save copy failed for %s\n", save_path);
    }
}

int gba_is_loaded(void) { return core != 0; }

const char *gba_title(void) { return rom_title; }

struct mCore *gba_core(void) { return core; }
