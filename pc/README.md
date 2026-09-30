# GbaC0re PC

SDL2 port of the GbaC0re PS5 payload. Same mGBA core, same audio resampler,
same menu UI — native PC binary, no exploit needed.

## Build (Windows)

1. Install [SDL2](https://github.com/libsdl-org/SDL/releases) dev libraries
   (e.g. via vcpkg: `vcpkg install sdl2:x64-windows`).
2. Set `MGBA_SRC` to the mGBA source directory
   (`../work/v07/mgba-test` by default, relative to this folder).
3. Build with the provided Makefile (requires MinGW or MSVC with `make`),
   or compile manually:

```bat
gcc -O2 -I%MGBA_SRC%/include -I%MGBA_SRC%/src -Isrc ^
    -DHAVE_LOCALE -DHAVE_STRTOF_L ^
    src/pc_main.c src/pc_gba.c src/menu.c src/ui.c src/savestate.c ^
    src/mgba_stubs.c %MGBA_SRC%/src/*/*.c %MGBA_SRC%/src/*/*/*.c ^
    -I<SDL2>/include -L<SDL2>/lib -lSDL2 -o gbac0re_pc.exe
```

(See `Makefile` for the exact mGBA source list.)

## Build (Linux)

```bash
sudo apt-get install libsdl2-dev
make
./gbac0re_pc <rom.gba>
```

## Run

```bash
./gbac0re_pc /path/to/game.gba
```

## Controls

| Input | GBA |
|-------|-----|
| Z / gamepad B | B |
| X / gamepad A | A |
| Enter / gamepad Start | Start |
| Shift / gamepad Back | Select |
| Arrows / d-pad / left stick | D-pad |
| A / gamepad LB | L |
| S / gamepad RB | R |
| ESC / P, or Start+Select | Pause menu |
| F1 | Save state |
| F2 | Load state |
| F3 | Fast-forward toggle (off → 2x → 4x → off) |

## Files

- `saves/<rom>.sav` — battery saves (auto-loaded/stored).
- `states/<rom>.ss0` — save states (F1/F2 or via pause menu).

## Notes

- This is v1 of the PC port: single-player, no netplay yet.
- The pause menu is the same UI as the PS5 build (RESUME / SAVE STATE /
  LOAD STATE / OPTIONS / QUIT).
- Audio uses the same 32768Hz → 48kHz fixed-point resampler as the PS5.
- For netplay (PS5 ↔ PC), see the planned UDP link-cable driver
  (not yet implemented).
