#ifndef LUAGB_SAVEDATA_H
#define LUAGB_SAVEDATA_H

#include "core.h"

/* The savedata directory inside the container, WITHOUT the sdimg_ prefix that
   the image file carries. It is region specific, so both are tried. */
#define SAVE_DIR_NAME_US     "SLUS-20268"      /* CUSA03474 */
#define SAVE_DIR_NAME_EU     "SLES-50366"      /* CUSA03492 */
#define SAVE_DIR_NAME        SAVE_DIR_NAME_US
#define SAVEDATA_MOUNT_POINT "/savedata0"

/* Resolves the mount primitive out of the host eboot's import table and
   initialises libSceSaveData. Must run before any write window. */
int savedata_init(void *G, void *D, void *load_mod, u64 eboot_base, s32 userId);

/* Opens a read-write window on the savedata container: unmounts the read-only
   mount and remounts read-write. Returns 0 on success. Nests, so callers can
   pair begin/end freely.
   Keep the window SHORT -- prepare the data outside it, then copy in. */
int savedata_begin_write(void);

/* Closes the window. The unmount is what commits the bytes, it completes
   asynchronously, and the read-only mount is restored afterwards -- skipping
   that leaves the game unable to close from the PS menu. */
void savedata_end_write(void);

/* True once the mount primitive and libSceSaveData are available. */
int savedata_is_ready(void);

#endif
