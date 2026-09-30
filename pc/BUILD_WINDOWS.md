# Building GbaC0re PC on Windows

## 1. Install the toolchain (one time)

1. Download and install **MSYS2** from https://www.msys2.org/
   (accept all defaults — installs to `C:\msys64`).
2. Open **"MSYS2 MinGW x64"** from the Start menu.
   (Important: use the MinGW x64 terminal, not the plain "MSYS2" one.)
3. Update the package database (run twice if it tells you to restart):
   ```
   pacman -Syu
   ```
4. Install the compiler, SDL2, and build tools:
   ```
   pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 make zip
   ```
   Accept the defaults when it asks which packages (just press Enter).
5. Verify:
   ```
   gcc --version
   sdl2-config --version
   ```
   Both should print version numbers.

## 2. Get the source

Extract `GbaC0re_PC_build.zip` somewhere, e.g. `C:\gbac0re-pc`.
You should end up with:

```
C:\gbac0re-pc\
  pc\             <- the port (pc_main.c, pc_gba.c, menu, ui, savestate, Makefile)
  work\v07\mgba-test\   <- mGBA core source
  build.sh        <- build script
```

## 3. Build

In the **"MSYS2 MinGW x64"** terminal:

```
cd /c/gbac0re-pc
./build.sh
```

This compiles everything and produces `pc/gbac0re_pc.exe`.

To rebuild after changing code, just run `./build.sh` again.
To clean: `./build.sh clean`.

## 4. Run

```
cd /c/gbac0re-pc/pc
./gbac0re_pc.exe /c/path/to/game.gba
```

Or from Windows Explorer: drag a `.gba` file onto `gbac0re_pc.exe`.

You'll also need `SDL2.dll` next to the exe — the build script copies it
automatically from the MSYS2 installation.

## Controls

| Key / Button | GBA |
|---|---|
| Z / pad B | B |
| X / pad A | A |
| Enter / pad Start | Start |
| Shift / pad Back | Select |
| Arrows / d-pad / left stick | D-pad |
| A / pad LB | L |
| S / pad RB | R |
| ESC, or Start+Select | Pause menu |
| F1 / F2 | Save / load state |
| F3 | Fast-forward (off → 2x → 4x) |

Saves go to `saves/`, states to `states/` next to the exe.

## Troubleshooting

- **`sdl2-config: command not found`** — you installed the wrong SDL2
  (`mingw-w64-x86_64-SDL2`, not `mingw-w64-i686-SDL2`), or you're in the
  plain MSYS2 terminal instead of "MSYS2 MinGW x64".
- **`gcc: command not found`** — same cause; use the MinGW x64 terminal.
- **Missing `SDL2.dll` at runtime** — the build script copies it; if you
  moved the exe, copy `C:\msys64\mingw64\bin\SDL2.dll` next to it.
