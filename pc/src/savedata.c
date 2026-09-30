#include "core.h"
#include "shim.h"
#include "savedata.h"

/* Read-write access to the game's savedata container.
 *
 * /savedata0 is mounted read-only for the ps2emu sandbox, so cartridge saves
 * cannot simply be written to it. The container has to be remounted
 * read-write, written, and unmounted again -- and the unmount is what commits.
 *
 * Three things here are not guessable and come from a known-working
 * implementation rather than from reading libSceSaveData:
 *
 *   1. sceSaveDataMount resolved through dlsym refuses every mount. Only the
 *      function pointer sitting in the host eboot's import table works, so the
 *      address is read straight out of it at EBOOT_SAVEDATA_MOUNT_GOT.
 *
 *   2. The parameter block is sceSaveDataMount's, not sceSaveDataMount2's:
 *      userId at 0x00, dirName at 0x10, blocks at 0x20, mountMode at 0x28.
 *      titleId is left NULL. (An earlier version of this file modelled the
 *      Mount2 layout instead -- mountMode at 0x18, no blocks -- and every
 *      mount was rejected.)
 *
 *   3. SD_CREATE must never be set. With the wrong dirName it silently builds
 *      a second, empty container and mounts that, so the write appears to
 *      succeed and goes nowhere.
 *
 * The dirName is the savedata directory without the sdimg_ container prefix,
 * and it is region specific: SLUS-20268 for the USA title, SLES-50366 for EU.
 */

#define EBOOT_SAVEDATA_MOUNT_GOT 0x3893F0

#define SD_RO      1
#define SD_RW      2
#define SD_BLOCKS  32768

/* sceSaveDataMount parameter block. */
#define SD_P_USERID    0x00
#define SD_P_DIRNAME   0x10
#define SD_P_BLOCKS    0x20
#define SD_P_MOUNTMODE 0x28
#define SD_P_SIZE      128
#define SD_R_SIZE      64

static void *S_G;
static void *mount_fn, *umount_fn, *stat_fn, *usleep_fn;
static s32   user_id;
static s32   sd_ready;
static s32   depth;
static char  save_dir[32];

static void logret(const char *what, s64 v) {
    char b[128];
    int p = 0;
    while (*what && p < 80) b[p++] = *what++;
    const char *sep = " ret 0x";
    while (*sep) b[p++] = *sep++;
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((((u64)v) >> i) & 0xF);
        if (d || started || i == 0) {
            b[p++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10);
            started = 1;
        }
    }
    b[p++] = '\n';
    b[p] = 0;
    klog(b);
}

static void copy_str(char *dst, int size, const char *src) {
    int i = 0;
    for (; i < size; i++) dst[i] = 0;
    for (i = 0; src[i] && i < size - 1; i++) dst[i] = src[i];
}

int savedata_init(void *G, void *D, void *load_mod, u64 eboot_base, s32 userId) {
    S_G = G;
    user_id = userId;

    /* The import-table entry, not dlsym -- see note 1 above. */
    mount_fn = (void *)*(u64 *)(eboot_base + EBOOT_SAVEDATA_MOUNT_GOT);
    if (!mount_fn) { klog("savedata: eboot mount import is null\n"); return 0; }

    s32 mod = (s32)NC(G, load_mod, (u64)"libSceSaveData.sprx", 0, 0, 0, 0, 0);
    if (mod < 0) { klog("savedata: libSceSaveData.sprx failed to load\n"); return 0; }

    umount_fn = SYM(G, D, mod, "sceSaveDataUmount");
    stat_fn   = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelStat");
    usleep_fn = SYM(G, D, LIBKERNEL_HANDLE, "sceKernelUsleep");

    void *init = SYM(G, D, mod, "sceSaveDataInitialize3");
    if (!init) init = SYM(G, D, mod, "sceSaveDataInitialize2");
    if (init) logret("savedata: initialize", (s64)NC(G, init, 0, 0, 0, 0, 0, 0));

    if (!umount_fn) { klog("savedata: umount not resolvable\n"); return 0; }

    copy_str(save_dir, sizeof(save_dir), SAVE_DIR_NAME);
    sd_ready = 1;
    klog("savedata: ready (eboot mount import)\n");
    return 1;
}

static s32 mount_dir(const char *dir, u32 mode) {
    u8 params[SD_P_SIZE];
    u8 result[SD_R_SIZE];
    char name[32];

    for (int i = 0; i < SD_P_SIZE; i++) params[i] = 0;
    for (int i = 0; i < SD_R_SIZE; i++) result[i] = 0;
    copy_str(name, sizeof(name), dir);

    *(u32 *)(params + SD_P_USERID)    = (u32)user_id;
    *(u64 *)(params + SD_P_DIRNAME)   = (u64)name;
    *(u64 *)(params + SD_P_BLOCKS)    = SD_BLOCKS;
    *(u32 *)(params + SD_P_MOUNTMODE) = mode;   /* never SD_CREATE */

    return (s32)NC(S_G, mount_fn, (u64)params, (u64)result, 0, 0, 0, 0);
}

/* Tries the configured region first, then the other one. Safe only because
   SD_CREATE is never set: a wrong name simply fails instead of fabricating an
   empty container that would swallow the write. */
static int sd_mount(u32 mode) {
    static const char *both[2] = { 0, 0 };
    const char *a = SAVE_DIR_NAME_US;
    const char *b = SAVE_DIR_NAME_EU;
    both[0] = a; both[1] = b;

    if (save_dir[0] && mount_dir(save_dir, mode) == 0) return 1;

    for (int i = 0; i < 2; i++) {
        if (mount_dir(both[i], mode) == 0) {
            copy_str(save_dir, sizeof(save_dir), both[i]);
            return 1;
        }
    }
    return 0;
}

int savedata_begin_write(void) {
    if (!sd_ready) return -1;
    if (depth++ > 0) return 0;              /* already open */

    /* A mount cannot be upgraded in place, so drop the read-only one first. */
    NC(S_G, umount_fn, (u64)SAVEDATA_MOUNT_POINT, 0, 0, 0, 0, 0);

    if (!sd_mount(SD_RW)) {
        klog("savedata: RW mount refused, restoring read-only\n");
        sd_mount(SD_RO);                    /* leave it as we found it */
        depth = 0;
        return -1;
    }
    return 0;
}

void savedata_end_write(void) {
    if (!sd_ready) return;
    if (--depth > 0) return;
    if (depth < 0) { depth = 0; return; }

    /* The unmount is the commit -- bytes reach the container here. */
    NC(S_G, umount_fn, (u64)SAVEDATA_MOUNT_POINT, 0, 0, 0, 0, 0);

    /* ...and it is asynchronous, so wait for the mount point to disappear. */
    if (stat_fn) {
        u8 st[256];
        for (int i = 0; i < 20; i++) {
            if ((s32)NC(S_G, stat_fn, (u64)SAVEDATA_MOUNT_POINT, (u64)st, 0, 0, 0, 0) != 0)
                break;
            if (usleep_fn) NC(S_G, usleep_fn, 250000, 0, 0, 0, 0, 0);
        }
    }

    /* Restoring the read-only mount is mandatory: without it the game cannot
       be closed from the PS menu. */
    if (!sd_mount(SD_RO))
        klog("savedata: WARNING could not restore read-only mount\n");
}

int savedata_is_ready(void) { return sd_ready; }
