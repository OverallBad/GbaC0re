#include "pc_core.h"
#include "savestate.h"
#include "pc_gba.h"
#include <mgba/core/core.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define STATE_DIR PC_STATE_DIR
#define STATE_EXT ".ss0"
#define STATE_STAGING "gba_tmp.ss0"

    /* PC mkdir (POSIX; single-arg on Windows). */
#ifdef _WIN32
static void pc_mkdir(const char *path) { mkdir(path); }
#else
static void pc_mkdir(const char *path) { mkdir(path, 0755); }
#endif
#define shim_mkdir pc_mkdir

/* A GBA state serializes ~384KB of RAM plus CPU/PPU/APU state -- comfortably
   under 1MB. The buffer is malloc'd from the arena ON DEMAND (during save/
   load, when the game is paused) and freed after. This avoids:
   - v1.2.0: 1MB static .bss (data region 1.3MB->2.3MB, ROM scan broke).
   - v1.2.4: 1MB mmap per-save refused during gameplay (new VM mapping hits
     the process VM ceiling).
   - v1.2.5: 1MB mmap at boot crowded VM so roms mmap failed (0 ROMs).
   - v1.2.6: 1MB arena malloc at boot starved Shining Soul 2 during gameplay
     (slowdown, audio cracking).
   Arena malloc does NOT create a new VM mapping (unlike mmap), so it does
   not hit the VM ceiling. During the paused save, ~2MB of the 36MB arena
   is free; the 1MB is returned immediately after. */
#define STATE_BUF_SIZE (1024 * 1024)

/* Returns a 1MB arena buffer, or 0 if the arena cannot grant it. */
static u8 *state_alloc(void) {
    u8 *p = (u8 *)malloc(STATE_BUF_SIZE);
    if (!p) printf("GBA: state buffer arena alloc failed\n");
    return p;
}

static void state_free(u8 *p) {
    free(p);
}

static void build_state_path(char *out, int max) {
    const char *stem = pc_gba_title();
    int k = 0;
    while (STATE_DIR[k] && k < max - 1) { out[k] = STATE_DIR[k]; k++; }
    int j = 0;
    while (stem[j] && k < max - 1) { out[k++] = stem[j++]; }
    const char *ext = STATE_EXT;
    while (*ext && k < max - 1) out[k++] = *ext++;
    out[k] = 0;
}

int gba_state_exists(void) {
    if (!pc_gba_core() || !pc_gba_title()[0]) return 0;
    char path[160];
    build_state_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int gba_state_save(void) {
    struct mCore *core = pc_gba_core();
    if (!core || !pc_gba_title()[0]) return -1;

    size_t sz = core->stateSize(core);
    if (sz == 0 || sz > STATE_BUF_SIZE) {
        printf("GBA: state size %lu out of range\n", (unsigned long)sz);
        return -1;
    }
    u8 *buf = state_alloc();
    if (!buf) { printf("GBA: state buffer unavailable (boot alloc failed)\n"); return -1; }
    int rc = -1;
    if (!core->saveState(core, buf)) {
        printf("GBA: saveState failed\n");
        goto out;
    }

    /* Stage outside the container first: the read-write window unmounts and
       remounts savedata, so hold it open as briefly as possible. */
    FILE *t = fopen(STATE_STAGING, "wb");
    if (!t) { printf("GBA: cannot stage state\n"); goto out; }
    size_t w = fwrite(buf, 1, sz, t);
    fclose(t);
    if (w != sz) goto out;

    /* PC: write directly (no PS5 mount window needed). */
    pc_mkdir(STATE_DIR);

    char path[160];
    build_state_path(path, sizeof path);
    int ok = 0;
    {
        FILE *in = fopen(STATE_STAGING, "rb");
        FILE *out = fopen(path, "wb");
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
    printf("PC: state save %lu bytes -> %s\n",
           (unsigned long)sz, ok ? "ok" : "FAIL");
    rc = ok ? 0 : -1;
out:
    state_free(buf);
    return rc;
}

int gba_state_load(void) {
    struct mCore *core = pc_gba_core();
    if (!core || !pc_gba_title()[0]) return -1;

    char path[160];
    build_state_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > (long)STATE_BUF_SIZE) { fclose(f); return -1; }
    u8 *buf = state_alloc();
    if (!buf) { fclose(f); return -1; }
    size_t sz = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    int rc = -1;
    if (sz != (size_t)fsz) goto out;

    /* A state from another core version or game would deserialize garbage;
       the size check catches the common cases. */
    if (sz != core->stateSize(core)) {
        printf("GBA: state size mismatch (%lu vs %lu)\n",
               (unsigned long)sz, (unsigned long)core->stateSize(core));
        goto out;
    }
    if (!core->loadState(core, buf)) {
        printf("GBA: loadState failed\n");
        goto out;
    }
    printf("GBA: state loaded (%lu bytes)\n", (unsigned long)sz);
    rc = 0;
out:
    state_free(buf);
    return rc;
}
