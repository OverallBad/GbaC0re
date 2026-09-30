/* POSIX-shaped stubs for the handful of hosted-OS functions mGBA's core
 * references but the payload never meaningfully uses.
 *
 * The freestanding build has no current directory, no environment, no
 * mkdir, and no directory iteration: the core's portable-config path
 * probing (mCoreConfig*) and save autoload-by-path are dead code on the
 * console -- the glue passes explicit absolute paths everywhere. These
 * stubs exist so the link succeeds; each one fails in the obvious safe way
 * if it is ever actually reached.
 *
 * strncat is real (trivially implementable); binaryName is the program
 * name mGBA's CMake normally generates into version.c.
 */

#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <mgba-util/vfs.h>

/* mGBA's CMake generates these into version.c; the values only surface in
   config-file paths and savestate metadata we never build. */
const char *const binaryName = "gba_emu";
const char *const projectName = "mGBA";
const char *const projectVersion = "0.0-freestanding";

/* errno itself is defined in gba_runtime.c (declared extern in errno.h). */

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    /* Only reached by the SharkPort save-export path, which has no UI in
       this payload and is never called. */
    (void)s; (void)max; (void)fmt; (void)tm;
    return 0;
}

/* getcwd/mkdir/getenv stubs: only for PS5 (no libc). Windows provides these
 * natively, and redefining them conflicts with <io.h>. */
#ifndef _WIN32
char *getcwd(char *buf, size_t size) {
    (void)buf; (void)size;
    return 0;
}

char *getenv(const char *name) {
    (void)name;
    return 0;
}

int mkdir(const char *path, unsigned mode) {
    (void)path; (void)mode;
    return -1;
}
#endif

struct VDir *VDirOpen(const char *path) {
    (void)path;
    return 0;
}

bool VDirCreate(const char *path) {
    (void)path;
    return false;
}

char *strncat(char *d, const char *s, size_t n) {
    char *p = d;
    while (*p) p++;
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}

/* Cheat engine compiled out (saves ~60KB, most of it the GBK table the cheat
 * parser's string layer drags in). The UI has no cheat interface and the core
 * only reaches these through core->cheatDevice(), which nothing calls, so they
 * are link-only stubs that fail in the obvious safe way if ever reached. */
struct mCheatDevice;
struct mCheatSet;
struct mCheatSets;

struct mCheatDevice *GBACheatDeviceCreate(void) { return 0; }
void mCheatDeviceDestroy(struct mCheatDevice *d) { (void) d; }
void mCheatDeviceClear(struct mCheatDevice *d) { (void) d; }
bool mCheatParseFile(struct mCheatDevice *d, struct VFile *vf) {
    (void) d; (void) vf;
    return false;
}
bool mCheatSaveFile(struct mCheatDevice *d, struct VFile *vf) {
    (void) d; (void) vf;
    return false;
}
void mCheatRefresh(struct mCheatDevice *d, struct mCheatSet *s) {
    (void) d; (void) s;
}
size_t mCheatSetsSize(const struct mCheatSets *v) { (void) v; return 0; }
struct mCheatSet **mCheatSetsGetPointer(struct mCheatSets *v, size_t loc) {
    (void) v; (void) loc;
    return 0;
}
