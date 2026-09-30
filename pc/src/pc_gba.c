/* GbaC0re PC — mGBA GBA core adapter.
 *
 * Ports the PS5 gba_glue.c to PC/SDL. Keeps the proven pieces:
 * mGBA core lifecycle, the 32768Hz -> 48kHz fixed-point resampler,
 * the 240x160 ARGB framebuffer, battery-save snapshot logic.
 * Drops: PS5 shim, arena, savedata mount, web server, Lua bootstrap.
 * Audio output is a callback (SDL_QueueAudio in pc_main.c).
 * Saves go to saves/<stem>.sav next to the binary. */

#include <mgba/internal/gba/gba.h>

#include "pc_core.h"
#include "pc_gba.h"
#include "menu.h"  /* g_volume */
#include "bridge.h"  /* link-cable bridge */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>

#define GBA_ROM_MAX (32 * 1024 * 1024)

u32 pc_gba_framebuffer[GBA_W * GBA_H];
int pc_gba_trace = 0;

/* ------------------------------------------------------------------ */
/* Core state                                                          */
/* ------------------------------------------------------------------ */

static struct mCore *core = NULL;
static char rom_title[128] = "";
static char rom_stem[128] = "";

/* Battery save snapshot (for change detection). */
static u8 *save_snapshot = NULL;
static size_t save_snapshot_size = 0;

/* ------------------------------------------------------------------ */
/* Audio: 32768Hz source -> 48kHz output resampler (from PS5, portable) */
/* ------------------------------------------------------------------ */

#define SRC_RING_FRAMES 4096
#define OUT_RING_FRAMES 4096

static s16 src_ring[SRC_RING_FRAMES * 2];
static int src_frames = 0;
static s16 src_keep[2] = {0, 0};

static s16 out_ring[OUT_RING_FRAMES * 2];
static int out_frames = 0;

static u32 resample_phase = 0;
/* Updated dynamically from core->audioSampleRate() each frame. */
static u32 resample_step = (u32)(((u64)GBA_APU_RATE << 16) / SAMPLE_RATE);
static unsigned current_apu_rate = GBA_APU_RATE;

static pc_audio_submit_fn audio_submit = NULL;

void pc_gba_set_audio(pc_audio_submit_fn fn) {
    audio_submit = fn;
}

/* postAudioFrame: push model. Rate is queried dynamically in
   pc_gba_audio_flush() via core->audioSampleRate(), no callback needed. */
static void av_sample(struct mAVStream *s, int16_t l, int16_t r) {
    (void)s;
    if (src_frames >= SRC_RING_FRAMES) return;
    src_ring[src_frames * 2]     = l;
    src_ring[src_frames * 2 + 1] = r;
    src_frames++;
}

static void av_buffer(struct mAVStream *s, struct mAudioBuffer *b) {
    (void)s; (void)b;
}

static void av_video(struct mAVStream *s, const mColor *buf, size_t stride) {
    (void)s;
    for (unsigned y = 0; y < GBA_H; y++) {
        const u32 *row = (const u32 *)buf + y * stride;
        u32 *dst = &pc_gba_framebuffer[y * GBA_W];
        for (unsigned x = 0; x < GBA_W; x++) {
            u32 c = row[x];
            dst[x] = (c & 0x0000FF00u) | ((c & 0x000000FFu) << 16) |
                     ((c & 0x00FF0000u) >> 16);
        }
    }
}

static double resample_pos = 0;
static int resample_ff_factor = 1; /* 1=normal, 2=2x chipmunk, 4=4x chipmunk */

static void resample_pending(void) {
    if (src_frames <= 0) return;
    int n = src_frames;
    /* Chipmunk: during FF, step by factor * rate_ratio to raise pitch. */
    double step = (double)current_apu_rate / SAMPLE_RATE * resample_ff_factor;
    for (;;) {
        int idx = (int)resample_pos;
        if (idx >= n) break;
        if (out_frames >= OUT_RING_FRAMES) break;
        out_ring[out_frames * 2]     = src_ring[idx * 2];
        out_ring[out_frames * 2 + 1] = src_ring[idx * 2 + 1];
        out_frames++;
        resample_pos += step;
    }
    resample_pos -= n;
    if (resample_pos < 0) resample_pos = 0;
    src_frames = 0;
}

static void audio_flush_n(int max_grains) {
    resample_pending();
    if (!audio_submit) { out_frames = 0; return; }

    if (g_volume < 10) {
        for (int i = 0; i < out_frames * 2; i++) {
            out_ring[i] = (s16)((out_ring[i] * g_volume) / 10);
        }
    }

    int n = 0;
    while (out_frames >= SAMPLES_PER_BUF && n < max_grains) {
        audio_submit(out_ring, SAMPLES_PER_BUF);
        int rem = out_frames - SAMPLES_PER_BUF;
        for (int i = 0; i < rem * 2; i++)
            out_ring[i] = out_ring[SAMPLES_PER_BUF * 2 + i];
        out_frames = rem;
        n++;
    }
    /* Drop overflow rather than stall. */
    if (out_frames >= OUT_RING_FRAMES) out_frames = 0;
}

void pc_gba_audio_flush(void) {
    if (core) {
        /* Query live APU rate each frame; update resampler if changed.
           No callback needed — avoids the silence bug from audioRateChanged. */
        unsigned rate = core->audioSampleRate(core);
        if (rate != 0 && rate != current_apu_rate) {
            current_apu_rate = rate;
            resample_step = (u32)(((u64)rate << 16) / SAMPLE_RATE);
        }
    }
    resample_ff_factor = 1;
    audio_flush_n(1000);  /* drain everything; audio is the master clock */
}

void pc_gba_audio_flush_chipmunk(int factor) {
    if (core) {
        unsigned rate = core->audioSampleRate(core);
        if (rate != 0 && rate != current_apu_rate) {
            current_apu_rate = rate;
        }
    }
    resample_ff_factor = factor;
    audio_flush_n(1000);
    resample_ff_factor = 1;
}

void pc_gba_audio_flush_menu(void) {
    audio_flush_n(2);
}

void pc_gba_audio_discard(void) {
    src_frames = 0;
    out_frames = 0;
    resample_pos = 0;
}

/* UI click tones (from PS5, portable synthesis). */
static void click_tone(int freq, int ms) {
    int frames = (SAMPLE_RATE * ms) / 1000;
    for (int i = 0; i < frames; i++) {
        if (out_frames >= OUT_RING_FRAMES) break;
        /* Sine via integer approximation; exponential decay. */
        int phase = (i * freq * 256) / SAMPLE_RATE;
        int s = 0;
        /* Simple triangle-ish wave to avoid libm. */
        int p = phase & 0xFF;
        s = (p < 128) ? (p * 256 / 128 - 128) : (128 - (p - 128) * 256 / 128);
        s = (s * 12000) / 128;
        /* Decay. */
        s = (s * (frames - i)) / frames;
        out_ring[out_frames * 2]     = (s16)s;
        out_ring[out_frames * 2 + 1] = (s16)s;
        out_frames++;
    }
}

void pc_gba_ui_click(int kind) {
    switch (kind) {
    case CLICK_TICK:    click_tone(880, 30); break;
    case CLICK_CONFIRM: click_tone(1320, 50); break;
    case CLICK_BACK:    click_tone(440, 50); break;
    }
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

/* Quiet mGBA logging: the per-frame BIOS/DMA/serial chatter costs
   significant console I/O on Windows and tanks the frame rate.
   Errors and fatals still print. */
static struct mStandardLogger quiet_logger;
static bool quiet_logger_done = false;

static void pc_quiet_logging(void) {
    if (quiet_logger_done) return;
    quiet_logger_done = true;
    mStandardLoggerInit(&quiet_logger);
    quiet_logger.d.filter->defaultLevels = mLOG_ERROR | mLOG_FATAL;
    quiet_logger.logToStdout = true;
    mLogSetDefaultLogger(&quiet_logger.d);
}

int pc_gba_is_loaded(void) { return core != NULL; }

const char *pc_gba_title(void) { return rom_title; }

struct mCore *pc_gba_core(void) { return core; }

void pc_gba_set_input(u16 mask) {
    if (core) core->setKeys(core, mask);
}

void pc_gba_run_frame(void) {
    if (core) core->runFrame(core);
}

static void make_stem(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(rom_stem)) len = sizeof(rom_stem) - 1;
    memcpy(rom_stem, base, len);
    rom_stem[len] = 0;
    /* Sanitize for filename. */
    for (size_t i = 0; i < len; i++) {
        char c = rom_stem[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            rom_stem[i] = '_';
    }
}

int pc_gba_load_rom(const char *path) {
    pc_gba_unload();

    FILE *f = fopen(path, "rb");
    if (!f) { printf("PC: cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 192 || fsz > GBA_ROM_MAX) {
        printf("PC: bad ROM size %ld\n", fsz);
        fclose(f);
        return 0;
    }

    u8 *rom = malloc(fsz);
    if (!rom) { fclose(f); return 0; }
    if (fread(rom, 1, fsz, f) != (size_t)fsz) {
        free(rom); fclose(f); return 0;
    }
    fclose(f);
    printf("PC: %s, %ld bytes\n", path, fsz);

    struct VFile *vf = VFileFromConstMemory(rom, fsz);
    if (!vf) { printf("PC: VFileFromConstMemory failed\n"); free(rom); return 0; }

    core = mCoreFindVF(vf);
    if (!core) { printf("PC: mCoreFindVF failed\n"); vf->close(vf); return 0; }

    mCoreConfigInit(&core->config, NULL);
    pc_quiet_logging();
    core->opts.volume = 0x100;  /* full volume; default 0 = silence */
    core->opts.mute = false;
    if (!core->init(core)) {
        printf("PC: core->init failed\n");
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        vf->close(vf);
        core = NULL;
        return 0;
    }

    /* Render target: mGBA draws here, av_video copies to pc_gba_framebuffer. */
    static u32 render_target[GBA_W * GBA_H];
    core->setVideoBuffer(core, (mColor *)render_target, GBA_W);

    if (!core->loadROM(core, vf)) {
        printf("PC: core->loadROM failed\n");
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        vf->close(vf);
        core = NULL;
        return 0;
    }

    mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "detect");
    mCoreLoadConfig(core);

    /* AV stream. */
    static struct mAVStream st;
    memset(&st, 0, sizeof(st));
    st.postVideoFrame = av_video;
    st.postAudioFrame = av_sample;
    st.postAudioBuffer = av_buffer;
    /* audioRateChanged disabled: dynamic switching caused silence */
    core->setAVStream(core, &st);

    core->reset(core);

    make_stem(path);
    snprintf(rom_title, sizeof(rom_title), "%s", rom_stem);

    /* Load battery save if present. */
    {
        char save_path[256];
        snprintf(save_path, sizeof(save_path), PC_SAVE_DIR "%s.sav", rom_stem);
        FILE *sf = fopen(save_path, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long ssz = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            if (ssz > 0) {
                u8 *sbuf = malloc(ssz);
                if (sbuf && fread(sbuf, 1, ssz, sf) == (size_t)ssz) {
                    struct VFile *svf = VFileFromConstMemory(sbuf, ssz);
                    if (svf) {
                        if (core->loadSave(core, svf)) {
                            printf("PC: loaded save %s (%ld bytes)\n", save_path, ssz);
                        } else {
                            svf->close(svf);
                        }
                    } else {
                        free(sbuf);
                    }
                } else {
                    free(sbuf);
                }
            }
            fclose(sf);
        }
    }

    /* Snapshot the save for change detection. */
    {
        void *saved = 0;
        size_t sz = 0;
        if (core->savedataClone)
            sz = core->savedataClone(core, &saved);
        if (sz > 0 && saved) {
            save_snapshot = malloc(sz);
            if (save_snapshot) {
                memcpy(save_snapshot, saved, sz);
                save_snapshot_size = sz;
            }
        }
    }

    printf("PC: ROM loaded: %s\n", rom_title);
    return 1;
}

/* Reset the loaded ROM to power-on state (battery save is kept,
   like a real console reset). */
void pc_gba_reset(void) {
    if (!core) return;
    core->reset(core);  /* fires the bridge driver's reset hook too */
    pc_gba_audio_discard();
    memset(pc_gba_framebuffer, 0, sizeof(pc_gba_framebuffer));
}

/* Attach the link-cable bridge driver to the running core (no-op if no core). */
void pc_gba_link_attach(void) {
    if (!core || !core->board) return;
    bridge_attach((struct GBA*)core->board);
}

void pc_gba_unload(void) {
    if (!core) return;
    bridge_detach();  /* link driver references core->board */
    pc_gba_save_store();
    core->unloadROM(core);
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    core = NULL;
    free(save_snapshot);
    save_snapshot = NULL;
    save_snapshot_size = 0;
    rom_title[0] = 0;
    rom_stem[0] = 0;
}

void pc_gba_save_store(void) {
    if (!core || !rom_stem[0]) return;

    void *saved = 0;
    size_t save_size = 0;
    if (core->savedataClone)
        save_size = core->savedataClone(core, &saved);
    if (!save_size || !saved) return;

    /* Skip if untouched (compare against snapshot). */
    if (save_snapshot && save_size == save_snapshot_size &&
        memcmp(saved, save_snapshot, save_size) == 0) {
        printf("PC: save untouched, skipping\n");
        return;
    }

    /* mkdir: two args on POSIX, one on Windows. */
#ifdef _WIN32
    mkdir(PC_SAVE_DIR);
#else
    mkdir(PC_SAVE_DIR, 0755);
#endif
    char save_path[256];
    snprintf(save_path, sizeof(save_path), PC_SAVE_DIR "%s.sav", rom_stem);

    FILE *f = fopen(save_path, "wb");
    if (f) {
        fwrite(saved, 1, save_size, f);
        fclose(f);
        printf("PC: saved %s (%zu bytes)\n", save_path, save_size);
        /* Update snapshot. */
        free(save_snapshot);
        save_snapshot = malloc(save_size);
        if (save_snapshot) {
            memcpy(save_snapshot, saved, save_size);
            save_snapshot_size = save_size;
        }
    } else {
        printf("PC: cannot write %s\n", save_path);
    }
}
