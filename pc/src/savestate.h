#ifndef GBACORE_SAVESTATE_H
#define GBACORE_SAVESTATE_H

#include "pc_core.h"

/* Full-emulator save states, complementing the battery save (gba_save_store).
 *
 * A state is a snapshot of the entire machine -- CPU, RAM, PPU, APU -- taken
 * at any moment, with no in-game save required. One slot per game, stored as
 * /savedata0/states/<stem>.ss0. The write path reuses the battery save's
 * staging pattern: serialize to a temp file outside the container, then copy
 * it in under the savedata write window. */

/* Serializes the running core into its state file. Returns 0 on success,
   -1 when no ROM is loaded or any step fails. */
int gba_state_save(void);

/* Restores the state file into the running core. The read-only savedata
   mount is enough; no write window needed. Returns 0 on success. */
int gba_state_load(void);

/* True when a state file exists for the loaded ROM. */
int gba_state_exists(void);

#endif
