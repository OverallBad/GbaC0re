# GbaC0re — Game Boy Advance Emulator for PS5

Game Boy Advance emulation on a PlayStation 5, delivered through the LuaC0re loader. Built on the mGBA emulation core.

## Requirements

- **PS5** on firmware **14.00 or lower**
- **Star Wars Racer Revenge** (SWRR) — the host game for the LuaC0re exploit
- **SWRR LuaC0re exploit** files installed (see Setup below)
- A **PC on the same local network** to build and send the payload
- Your own `.gba` ROM files (not provided)

## Setup Guide

### 1. Prepare the PS5

1. Ensure your PS5 is on firmware **14.00 or lower**. Do not update past this.
2. Install **Star Wars Racer Revenge** on the PS5.
3. Install the **LuaC0re exploit** files for SWRR (the Lua loader that runs
   custom payloads through the game's Hall of Fame entry point).

### 2. Build the payload (on your PC)

```bash
cd ps5
python3 build.py
```

This produces two files:
- `gba_emu.bin` — the compiled emulator payload
- `lua/gba.lua` — the LuaC0re loader script that delivers it

### 3. Find your PS5's IP address

On the PS5, go to **Settings → Network → View Connection Status** and note
the IP address (e.g. `192.168.0.152`). Your PC must be on the same network.

### 4. Launch the exploit on the PS5

1. Launch **Star Wars Racer Revenge**
2. Go to **OPTIONS → HALL OF FAME** — this arms the LuaC0re loader and it
   will wait for a payload from your PC

### 5. Send the payload and ROMs

From the `ps5/` directory on your PC, run the launcher:

```bash
# Send payload + all ROMs in a folder (replace with your PS5's IP)
python3 gba_launcher.py 192.168.0.152 --roms /path/to/your/roms --log
```

What each flag does:
- `--roms /path/to/your/roms` — uploads every `.gba` file in that folder
- `--log` — prints the payload's UDP debug log (port 9027). **Use this when
  anything goes wrong** — it shows exactly what the PS5 is doing.

To upload a single ROM later without resending the payload:

```bash
python3 upload_rom.py 192.168.0.152 mygame.gba
```

Or upload from your phone/tablet: open `http://<PS5_IP>:9030` in a browser
and tap **⬇ ROM**.

### 6. Play

After all uploads complete, the main menu appears on the PS5:

```
NO ROM LOADED
LOAD ROM
SAVE STATE
LOAD STATE
OPTIONS
LINK
QUIT
```

- **LOAD ROM** opens the ROM picker — select a game and it starts immediately
- The game's proper title (e.g. *Pokémon FireRed*) briefly displays on load
- Once a game is running, the first menu item becomes **RESUME**
- Press **L1** in-game to open the pause menu

## Placing ROMs

### Where ROMs go

Uploaded ROMs land in `/temp0/roms/` on the PS5. **This directory is wiped
every time the console reboots** — you must re-upload ROMs after a restart.

### ROM file rules

- Filenames may only contain: `A-Z a-z 0-9 . _ -`
- Max 64 characters, must end in `.gba` (case-insensitive)
- Max ROM size: 32 MB
- One upload at a time (the server returns HTTP 409 if busy)

Invalid filenames are rejected — the launcher filters them before announcing
the upload count, so the PS5 never waits for a ROM that won't arrive.

### Battery saves

In-game saves (SRAM/Flash) are written to `/savedata0/saves/<game>.sav`
through the payload's own savedata handling. **These survive reboots** —
only the ROMs in `/temp0` need re-uploading. No manual resigning required.

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

A web controller mirroring this layout is available at `http://<PS5_IP>:9030`
while the payload is running.

## Features

- **mGBA core** with HLE BIOS — no BIOS file needed
- 240×160 integer-scaled ×6 to 1440×960 centered on 1080p
- Battery saves (per-game `.sav` files, survive reboots)
- Save states
- Fast-forward (R1 cycles OFF → 2X → 4X → OFF) with pitch-corrected audio
- Customizable accent color (OPTIONS menu)
- Screen aspect toggle: 3:2 or 16:9 (OPTIONS menu)
- Web-based ROM uploader and controller

## Link-Cable Multiplayer 🚧 WIP

> **Work in progress.** The link-cable bridge is implemented but has not yet
> been verified working between PS5 and PC. Expect issues.

### How it's supposed to work

GBA link-cable transfers are tunneled over UDP between the two systems:

- **PS5 hosts**, PC joins
- Default UDP port: `43879`
- Both sides must run the **exact same ROM**
- Only link transfers are synchronized — not ROMs, saves, video, or audio

### Setup steps (when working)

1. **On PS5:** open the pause menu (L1) → select **LINK**. The PS5 enters
   hosting mode and listens on UDP port 43879.
2. **On PC:** open the GbaC0re PC port's link dialog, enter the PS5's IP
   address (e.g. `192.168.0.152`), and connect.
3. **On both sides:** load the same ROM and use the game's link feature
   (e.g. Pokémon trading, multiplayer modes).

### Known limitations

- The PS5 link socket initialization is still being debugged — clicking LINK
  may fail silently. Run the launcher with `--log` and report the output.
- The PC port is not yet included in this release.
- Multiplayer timing beyond basic transfers is untested.
- Only 2-player link is supported.

## Not Yet Implemented

The following are planned but not in this beta:

- **Cheat code support** (deferred)
- **Rewind / fast-save slots** (single save-state slot only)
- **Screen filters** (no smoothing/scanlines yet)
- **Button remapping** (fixed layout)
- **Recent-ROM list / auto-resume** (ROMs must be re-uploaded after reboot)
- **Turbo / frame advance**

## Troubleshooting

- **Launcher can't reach the PS5:** confirm the IP in PS5 Settings → Network.
  The launcher has no auto-discovery — a stale IP fails silently.
- **Nothing happens after upload:** re-run with `--log`. The UDP log on port
  9027 shows each stage (payload received, blob written, ROM uploads, menu).
  On Windows, listen with PowerShell (WSL2 is NAT'd and won't see it):
  ```powershell
  $u = New-Object Net.Sockets.UdpClient(9027); $e = $null
  while ($true) { [Text.Encoding]::ASCII.GetString($u.Receive([ref]$e)) }
  ```
- **ROM picker is empty:** filenames must match `[A-Za-z0-9._-]` and end in
  `.gba`. Check `--log` output for rejected names.
- **Only some ROMs appear:** the picker rescans every time it opens — if a
  batch upload was interrupted, re-upload the missing files.
- **No sound / garbled sound:** check OPTIONS → VOLUME. Audio resampling is
  32768 Hz → 48 kHz; report persistent issues with a `--log` capture.
- **LINK does nothing:** known WIP issue, see above.

See `ps5/TESTING.md` for the static verification checklist.

## Credits

See [CREDITS.md](CREDITS.md) for full attribution to the developers whose work made this possible.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
