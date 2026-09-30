# GbaC0re PS5 Native v2.0.0 — Installation

Native PS5 homebrew title (installed app, signed `eboot.bin`). No LuaC0re,
no payload uploader, no JIT pool — this is a real installed title.

## Requirements

- PS5 on firmware **13.60** (do not update past 13.60)
- Jailbreak applied for this boot: Relapse chain + kstuff + etaHEN
  (tethered — re-run after every reboot)
- ShadowMountPlus **v1.6beta16** for registering the title
  (v1.7alpha13fix1 is known to fail parsing `param.json`)
- FTP access to the PS5 (etaHEN provides FTP on port 2121)

## 1. Provide your own `libc.prx`

Sony's `libc.prx` is **not included** and cannot be redistributed.
You must supply a signed copy that matches firmware **13.60 or older**
(a newer-firmware copy will not work).

Place it at:

```
package/sce_module/libc.prx
```

i.e. next to `eboot.bin`, inside `sce_module/`, before copying to the PS5.

## 2. Copy the title to the PS5

Copy the whole `package/` folder contents to **one** of:

- Internal: `/data/homebrew/GBAC00001/`
- USB: `homebrew/GBAC00001/` (on a USB drive the PS5 can read)

Resulting layout on the PS5:

```
/data/homebrew/GBAC00001/
├── eboot.bin
├── sce_module/
│   └── libc.prx          <- your own signed copy
└── sce_sys/
    ├── icon0.png
    └── param.json
```

## 3. Register with ShadowMountPlus

1. Open ShadowMountPlus v1.6beta16.
2. Register/mount the title `GBAC00001`.
3. The title **GbaC0re** should appear on the home screen.

## 4. Add ROMs

Create a `roms/` folder inside the title folder and copy your `.gba`
files there:

```
/data/homebrew/GBAC00001/roms/your_game.gba
```

Saves and save states are written to `saves/` and `states/` next to it;
a debug log is appended to `gbac0re.log`.

> The exact writable working directory for native titles on 13.60 is
> hardware-unverified. If the title boots but finds no ROMs, check
> `gbac0re.log` — it records the ROM scan result — and try the alternate
> install location (internal vs USB).

## 5. Launch

Launch **GbaC0re** from the home screen. The ROM picker lists every
`.gba` in `roms/`.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | GBA directions |
| Cross | GBA A |
| Circle | GBA B |
| L2 / R2 | GBA L / R |
| Create | GBA Select |
| Options | GBA Start |
| L1 | Pause menu |
| R1 | Fast-forward: OFF → 2× → 4× → OFF |

Pause menu: RESUME, SAVE STATE, LOAD STATE, CHANGE ROM, OPTIONS, LINK, QUIT.

## Link-cable (PS5 ↔ PS5)

Both consoles on the same LAN, both running GbaC0re v2.0.0:

1. Host: pause menu → LINK → HOST.
2. Joiner: pause menu → LINK → JOIN → enter host IP.
3. Same ROM must be loaded on both sides.

UDP port 43881. Host learns the joiner's address from the first HELLO.
See TESTING.md for the unverified-import caveats.

## Uninstall

Delete `/data/homebrew/GBAC00001/` (or the USB folder) and unregister
the title in ShadowMountPlus.
