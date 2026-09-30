# TESTING — GbaC0re v0.7.3

**Scope: static validation only.** This package has not been run on PS5
hardware. The checks below verify the code compiles under the freestanding
model, links into a genuine position-independent flat image, and that the
delivery tooling is well-formed. On-console testing remains the user's step.

## What was checked

| Check | Command | Result |
|---|---|---|
| Full build | `python3 build.py` against a real mGBA checkout: 94 mGBA sources + 7 runtime sources compiled freestanding, linked `-static-pie` into `gba_emu.elf`, `objcopy` to `gba_emu.bin` (800,040 B), `mklua.py` folds it into `lua/gba.lua` (25,652 B) | pass |
| C syntax, freestanding | every `src/*.c` under `BASEFLAGS` (`-ffreestanding -fno-builtin -fpie -mno-red-zone …`) with `-Wall` | pass |
| Strict implicit-declaration sweep | every `src/*.c` + all 94 mGBA sources with `-Werror=implicit-function-declaration`, no `-w` (the GCC 14 failure class from v0.7.2) | pass, zero findings |
| mGBA API surface | `gba_glue.c` compiled against real mGBA headers (`-DMINIMAL_CORE=1 -DM_CORE_GBA=1 -DDISABLE_THREADING=1`) | pass |
| `python3 build.py check` | `check_relocs` + `check_image` (see below) | pass |
| Python tooling | `python3 -m py_compile tools/mklua.py gba_launcher.py upload_rom.py` | pass |
| `upload_rom.py` protocol | against a local HTTP test server: correct `POST /rom?name=…`, `Content-Length`, streamed body intact; missing-file and connection-failure paths exit with clear messages | pass |
| Lua template | structural review: balanced `[=[ ]=]` HTML block, upload block present in generated `lua/gba.lua`, single `@@JIT_SIZE@@`, 16-bit GBA masks, no MD/GB copy-paste residue | pass |

`make check` runs:

- `tools/check_relocs.sh gba_emu.elf` — all 10,705 dynamic relocations are
  `R_X86_64_RELATIVE`, the one type `_start` applies. Anything else would
  never be fixed up at boot.
- `tools/check_image.sh gba_emu.elf` — requires an `ET_DYN` image (guards the
  `-static`-vs-`-static-pie` GOT-relaxation trap), rejects absolute function
  immediates, and verifies the GOT bounds `_start` walks.

Also verified by inspection of the linked image:

- `.data` VMA is the RW address (`__data_start` = 0x100000); its LMA
  (`__data_load` = 0xAC5C0) sits in the blob right after the code. `_start`
  copies LMA→VMA, then applies the 10,705 `R_X86_64_RELATIVE` relocations,
  whose offsets now target the RW copy. (An earlier layout gave `.data` the
  blob VMA, which would have left every global and every relocation pointing
  at the read-only image.)
- `ASSERT` in `linker.ld` proves the `.data` init image ends before
  `__data_start`; the fixed 1 MiB gap replaces the old derived placement.
- `.bss` follows the RW data with no overlap.
- The JIT reservation (4 × 256 KiB mappings) covers the whole blob.

## Build notes (failures hit and fixed)

- mGBA's CMake forces `ENABLE_EXTRA ON` for the Generic target, which pulls
  in `src/feature/video-logger.c` — unbuildable against the minimal core.
  Fixed by compiling mGBA directly from source (no CMake) with a curated
  94-file list; `src/feature/*` and `src/util/sfo.c` (Vita SFO parsing,
  unreferenced) are excluded.
- `PATH_MAX` is undefined freestanding → `-DPATH_MAX=4096`.
- mGBA's CMake enables no VFS backend for a freestanding target and `vfs.c`
  fails with `#error` → `-DENABLE_VFS_FILE=1`, compiling
  `src/util/vfs/vfs-file.c` (needs only the six stdio calls `src/shim.c`
  implements).
- mGBA sources are built against `src/libc/` (our freestanding headers take
  precedence); gaps found this way and filled: `vsnprintf`/`vprintf`,
  `fflush`, `fgets`, `div`/`ldiv`, `mktime` (days-from-civil, UTC),
  `gettimeofday` (real, via the syscall gadget), `PRI*` macros (LP64),
  `strftime` (stub — only the never-called SharkPort export path needs it),
  and POSIX-shaped stubs (`getcwd`, `getenv`, `mkdir`, `VDirOpen`,
  `VDirCreate`, `binaryName`/`projectName`/`projectVersion`;
  `__snprintf_chk`-family defeated with `-U_FORTIFY_SOURCE`).
- `HAVE_POPCOUNT32`/`HAVE_LOCALTIME_R` must NOT be defined (nothing provides
  them); mGBA then uses its own inline/fallback versions.

## What was NOT checked

- No PS5 hardware run. Frame pacing, pad behavior, audio, saves, the
  savedata dance, and the whole `/rom` upload path are unverified on
  console.
- `luac` was unavailable, so the Lua template got structural review only —
  run `luac -p lua/gba.lua` if you have it before sending.

## On-hardware upload test items (for the console session)

1. Small ROM (~1 MB): `python3 upload_rom.py <PS5_IP> game.gba` → 200 OK,
   UDP log shows `upload: game.gba -> /temp0/roms/` then `upload: complete`;
   open the picker (MENU) — the ROM is listed and boots.
2. Same via the controller page: `http://<PS5_IP>:9030` → **⬇ ROM** — status
   line reports success.
3. 32 MB ROM uploads without dropping pad input: wiggle the web D-pad
   mid-upload, confirm the game keeps responding and the log shows no pad
   gaps attributable to the upload.
4. Bad filename (`../x.gba`, `game.zip`, 70-char name) → HTTP 400, no file
   created in `/temp0/roms/`.
5. Second upload while one is in flight → HTTP 409.
6. Kill the client mid-upload → ~30 s later the log shows
   `upload: aborted (stall)` and no partial `.gba` remains.
7. Reboot the console → `/temp0/roms/` is empty (expected); re-upload works.
8. A save written for an uploaded ROM persists across a game relaunch and
   the ROM still lists afterwards (save lives in `/savedata0/saves`).

## Reproduce

```
# 1. fetch mGBA (not shipped)
git clone https://github.com/mgba-emu/mgba.git mgba

# 2. full build + checks
make && make check

# 3. python tooling
python3 -m py_compile tools/mklua.py gba_launcher.py
```

## Known risks (for the hardware session)

1. **Arena sizing** — 64 MiB arena + whole-ROM buffer + save buffers. Large
   homebrew ROMs with big save types are the stress case.
2. **JIT pool depletion** — each payload send in one game session consumes
   mappings; a used session may only yield 2. Relaunch the game on
   `CreateSharedMemory` failure.
3. **First-run `.data` copy** — if the image hangs before the first log line,
   suspect `.rela.dyn` application (check the `boot: relocations applied`
   count against `readelf -r` on the ELF).
