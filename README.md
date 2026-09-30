# GbaC0re — Game Boy Advance Emulator for PS5

Game Boy Advance emulation on a PlayStation 5, delivered through the LuaC0re loader. Built on the mGBA emulation core.

## Requirements

- **PS5** on firmware **13.60 or lower**, jailbroken (Relapse + etaHEN)
- **LuaC0re** loader installed (via the SWRR host game)
- A PC on the same local network for uploading ROMs
- Your own `.gba` ROM files (not provided)

## Quick Start

### 1. Build the payload

```bash
cd ps5
python3 build.py
```

This produces `gba_emu.bin` and `lua/gba.lua`.

### 2. Launch via LuaC0re

1. On the PS5, launch the SWRR host game
2. Open **OPTIONS → HALL OF FAME** to arm the LuaC0re loader

### 3. Upload the payload and ROMs

From the `ps5/` directory on your PC:

```bash
# Upload payload + ROMs in one go (replace with your PS5's IP)
python3 gba_launcher.py 192.168.0.152 --roms /path/to/your/roms --log
```

- `--roms` uploads all `.gba` files in the folder to the PS5
- `--log` shows the payload's UDP debug log (port 9027) — start here if anything goes wrong

Or upload a single ROM:

```bash
python3 upload_rom.py 192.168.0.152 mygame.gba
```

You can also upload from your phone: open `http://<PS5_IP>:9030` in a browser and tap **⬇ ROM**.

### 4. Play

After uploads complete, the main menu appears:

```
NO ROM LOADED / RESUME
LOAD ROM
SAVE STATE
LOAD STATE
OPTIONS
LINK
QUIT
```

- **LOAD ROM** opens the ROM picker — select a game to start playing immediately
- The game's proper title (e.g. *Pokémon FireRed*) briefly displays on load
- Press **L1** (or the menu button) in-game to open the pause menu

## Controls

| Input | GBA Button |
|-------|------------|
| Cross | A |
| Circle | B |
| D-pad | D-pad |
| L2 / R2 | L / R |
| Create | Select |
| Options | Start |
| L1 | Pause menu |
| R1 | Fast-forward: OFF → 2X → 4X → OFF |

A web controller mirroring this layout is available at `http://<PS5_IP>:9030`.

## Features

- **mGBA core** with HLE BIOS — no BIOS file needed
- 240×160 integer-scaled ×6 to 1440×960 centered on 1080p
- Battery saves (per-game `.sav` files, preserved across sessions)
- Save states
- Fast-forward with pitch-corrected audio
- Customizable accent color and screen aspect ratio
- **Link-cable multiplayer** (PC ↔ PS5 over UDP, see below)

## Link-Cable Multiplayer

Play link-cable games between PS5 and PC over your local network.

- **PS5 hosts**, PC joins
- Default port: `43879`
- Both sides must run the **same ROM**
- Only link transfers are synchronized (not video, audio, or save data)

1. On PS5: open the menu → **LINK** (puts the PS5 in hosting mode)
2. On PC: open the link dialog and enter the PS5's IP address
3. Start the same game on both sides and use the in-game link feature

## ROM Notes

- Filenames: `[A-Za-z0-9._-]`, max 64 characters, must end in `.gba`
- Max ROM size: 32 MB
- ROMs upload to `/temp0/roms/` — **this is wiped on console reboot**, so re-upload after restarting
- Battery saves are stored separately in `/savedata0/saves/` and survive reboots

## Troubleshooting

- **Nothing appears after upload:** run the launcher with `--log` and check the UDP output
- **PS5 unreachable:** confirm the IP with your router; the launcher defaults may be stale
- **Link fails:** ensure both sides use the same ROM and the PS5 shows the hosting state before the PC joins
- See `ps5/TESTING.md` for static verification checklists

## Credits

See [CREDITS.md](CREDITS.md) for full attribution to the developers whose work made this possible.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
