/* GbaC0re PS5 native — mGBA link shims.
 *
 * Native homebrew has the SDK's real libc, so the freestanding stubs for
 * getcwd/getenv/mkdir/VDir/strncat are GONE — the real ones are used.
 * What remains:
 *  - binaryName/projectName/projectVersion (mGBA's CMake normally generates
 *    these into version.c; they only surface in config paths/metadata).
 *  - strftime: only reached by the SharkPort save-export path, which has no
 *    UI here and is never called.
 *  - Cheat engine compiled out (saves ~60KB). The UI has no cheat interface
 *    and the core only reaches these through core->cheatDevice(), which
 *    nothing calls. Link-only stubs that fail safe if ever reached.
 */

#include <stddef.h>
#include <mgba-util/vfs.h>  /* struct VFile for the cheat stubs */
#include <stdbool.h>
#include <time.h>

/* mGBA's CMake generates these into version.c. */
const char *const binaryName = "gbac0re_ps5";
const char *const projectName = "mGBA";
const char *const projectVersion = "0.0-ps5native";

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    (void)s; (void)max; (void)fmt; (void)tm;
    return 0;
}

/* Cheat engine compiled out. */
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
