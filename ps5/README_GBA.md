# GbaC0re v0.7.3 — Game Boy Advance emulator for PS5

Game Boy Advance emulation on a PS5, delivered through the Luac0re loader.
The emulation core is **mGBA** (`mCore`, MPL-2.0); the PS5 runtime —
shellcode bring-up, video/audio/pad, ROM picker, UDP logging, web controller —
is **LuaPSX**'s proven runtime by soniciso1 (GPL-2.0-or-later), adapted for GBA.

> **Not tested on PS5 hardware.** Static checks only (see `TESTING.md`).
> On-console testing is the user's step.

## What it does

- Runs GBA ROMs at 240×160, integer-scaled ×6 to 1440×960 centered on 1080p.
- ROM picker: browse `/savedata0/roms/`, per-game battery saves in
  `/savedata0/saves/<game-stem>.sav`.
- Audio: GBA 32768 Hz → 48 kHz resample, submitted in exact 256-frame grains;
  the blocking `sceAudioOutOutput` call paces the emulator (~60 fps).
- Controls: DualSense **and** a web controller on port 9030.
- No BIOS file needed: mGBA's HLE BIOS is used (no copyrighted files ship).

## Controls

| Input            | GBA button            |
|------------------|-----------------------|
| Cross            | A                     |
| Circle           | B                     |
| D-pad            | D-pad                 |
| L2 / R2          | L / R                 |
| Create           | Select                |
| Options          | Start                 |
| **L1**           | back to ROM picker (saves first) |
| **R1 (hold ~1s)**| quit the payload      |

L1/R1 follow the LuaPSX runtime convention (menu / quit); the GBA shoulders
live on L2/R2 so no game input is lost. The web controller mirrors this layout
exactly, including the L1-menu / hold-R1-quit chords.

## Quick start

1. Build (see **Building** below): produces `gba_emu.bin` and `lua/gba.lua`.
2. Arm the Luac0re loader: on the PS5, launch the host game and open
   **OPTIONS → HALL OF FAME**.
3. From this directory:
   ```
   python3 gba_launcher.py <PS5_IP> --log
   ```
   `--log` prints the payload's UDP log (port 9027) — start here when anything
   goes wrong. The web controller is at `http://<PS5_IP>:9030`.
4. Get ROMs onto the console (see **ROMs on OFW** below): either upload them
   over the network into `/temp0/roms/`, or — on a jailbroken console — put
   `.gba` files in `/savedata0/roms/` with a save manager. The picker scans
   both locations every time it opens.

## ROMs on OFW

OFW has no save manager, so ROMs cannot be injected into the savedata
container — and the USB decrypt/resign dance is only practical for the
one-time LuaC0re install, not per ROM. Instead, the payload accepts ROM
uploads over the network into `/temp0/roms/`:

- **From the controller page:** open `http://<PS5_IP>:9030` on your phone/PC,
  tap **⬇ ROM**, pick a `.gba` file. A status line confirms the upload.
- **From the command line:**
  ```
  python3 upload_rom.py <PS5_IP> <rom.gba>
  ```

Then open the ROM picker in the payload (**MENU** / L1) — it rescans on every
open, so the new ROM appears immediately. No relaunch needed.

Rules and limits:

- Filenames: `[A-Za-z0-9._-]`, max 64 chars, must end `.gba`
  (case-insensitive). Anything else is rejected with HTTP 400.
- Max ROM size: 32 MB. One upload at a time (HTTP 409 while busy).
- **`/temp0` is wiped on console reboot** — re-upload after one. Same tradeoff
  as LuaPSX's `/temp0` discs.
- Cartridge saves are unaffected: the payload writes `/savedata0/saves`
  itself through its savedata dance, so no resigning is ever needed.

## Building

mGBA is **not** shipped — fetch it once (any recent checkout works):

```
git clone https://github.com/mgba-emu/mgba.git mgba
python3 build.py         # builds libmgba.a, links gba_emu.elf, emits gba_emu.bin + lua/gba.lua
python3 build.py check  # also runs the relocation / image integrity checks
```

`python3 build.py` needs no `make` and no `sh` — the checks are pure Python,
so this works on Windows with just a C toolchain (gcc/objcopy/readelf;
WSL's `build-essential` is the easy path). The `Makefile` remains as an
alternative (`make`, `make check`, `make clean`); both produce byte-identical
output.

`build.py` runs `tools/mklua.py`, which substitutes `@@JIT_SIZE@@` (the JIT
reservation, computed from `__data_start`) and rejects a blob that would
overrun it.

### Layout

| Path | What |
|---|---|
| `src/main.c` | PS5 bring-up, ROM picker, play loop, input, video/audio plumbing |
| `src/gba_glue.c/.h` | mGBA `mCore` lifecycle, VFiles, AV callbacks, saves |
| `src/gba_runtime.c` | freestanding libc extensions mGBA needs (strtol, sscanf, math, …) |
| `src/shim.c`, `src/ui.c`, `src/savedata.c` | syscall gadget calls, menu UI, savedata dance |
| `src/libc/` | freestanding headers (no glibc on the console) |
| `src/boot.inc`, `src/fault.inc` | data-region bootstrap + RELATIVE reloc application, fault trap |
| `linker.ld` | flat static-PIE layout; keeps `.rela.dyn` (see below) |
| `lua/gba.lua.in` | Luac0re-side template (dlsym fix, TCP blob, web page, JIT setup) |
| `tools/mklua.py` | folds the blob metadata into `lua/gba.lua` |
| `gba_launcher.py` | sends script to loader port 9026, streams blob with `K`-ACK scan |
| `upload_rom.py` | POSTs a `.gba` to the payload's `/rom` endpoint (OFW ROM upload) |

### Why `.rela.dyn` is kept

LuaGB's runtime discards dynamic relocations and forbids initialized pointer
data — fine for a Game Boy core with two offenders. mGBA is vtable-heavy
(core vtables, component tables, interpreter dispatch), so this build uses the
LuaPSX variant: `linker.ld` keeps `.rela.dyn` inside the blob and `_start`
applies every `R_X86_64_RELATIVE` entry at boot. `tools/check_relocs.sh`
verifies the final image contains no other relocation type, which no boot
fixup could repair.

## Network map

| Port | Direction | Use |
|---|---|---|
| 9026 TCP | PC → PS5 | Luac0re loader (Lua script) |
| 9027 UDP | PS5 → PC | debug log (`nc -u -l -p 9027`) |
| 9028–9045 TCP | PC → PS5 | payload blob, receiver proves itself with a `K` ACK |
| 9030 TCP | PC → PS5 | web controller page |

No FTP server in v0.7 (removed, as in LuaGB); ROM management goes through
the on-console picker.

## Saves

Battery saves are staged in `/av_contents/content_tmp/` while playing and
committed to `/savedata0/saves/<game-stem>.sav` through a short read-write
window on picker return, quit, and shutdown. A save is only written when the
emulated game actually touched SRAM/flash — exiting an untouched game leaves
no stray `.sav`.

## Troubleshooting

- **No log at all**: the script never ran — re-arm the loader (HALL OF FAME)
  and resend. A wrong `PC_IP` also looks like silence; the template defaults
  to `auto` (console's own subnet broadcast).
- **"blob: no live receiver"**: the script's TCP listener died or a previous
  run leaked one — relaunch the host game.
- **"JIT pool exhausted"**: the PS5 JIT pool depletes across payload sends in
  one game session — relaunch the game.
- **Hang after the picker**: check the UDP log for `FAULT:` lines; they print
  the faulting address and RIP offset for `addr2line`.

## Licenses

- Runtime files derived from LuaPSX: **GPL-2.0-or-later** (`COPYING`).
- mGBA: **MPL-2.0** (see mGBA's own `LICENSE` in its source tree).
- GbaC0re-specific glue: same terms as the runtime (GPL-2.0-or-later).

See `CREDITS.md` for attributions.
