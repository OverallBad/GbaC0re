# GbaC0re — Game Boy Advance Emulator

Game Boy Advance emulation for PS5 and PC, built on the mGBA core.

## Components

| Directory | Description |
|-----------|-------------|
| `ps5/` | PS5 payload for the LuaC0re loader (primary) |
| `pc/` | Native PC port (SDL2, Windows/Linux) |
| `ps5-native/` | Native PS5 homebrew (Prospero SDK, experimental) |
| `docs/` | Technical documentation |

## PS5 (LuaC0re Payload)

The primary PS5 build. Runs through the LuaC0re exploit loader on a jailbroken PS5 (firmware ≤13.60).

**Features:**
- mGBA core with HLE BIOS (no BIOS file needed)
- 240×160 integer-scaled to 1440×960 on 1080p
- Battery saves and save states
- Fast-forward (R1: OFF → 2X → 4X → OFF)
- Link-cable bridge (PC ↔ PS5 multiplayer over UDP)
- Web-based ROM uploader

**Quick start:**
1. See `ps5/README.md` for full setup instructions
2. Upload ROMs: `python gba_launcher.py <PS5_IP> --roms /path/to/roms`

## PC Port

Native SDL2 application for Windows and Linux. Same mGBA core, same UI.

See `pc/README.md` for build instructions.

## Native PS5 Homebrew

Experimental native port using the Prospero SDK. Requires etaHEN homebrew launcher.

See `ps5-native/INSTALL.md`. **Not tested on hardware.**

## Link-Cable Bridge

UDP-based GBA link-cable tunneling for PC ↔ PS5 multiplayer. Shared protocol in `ps5/link/`.

Default port: `43879`. PS5 hosts, PC joins.

## Credits

- **mGBA** (Jeffrey Pfau) — MPL-2.0 emulation core
- **LuaPSX** (soniciso1) — GPL-2.0 PS5 payload runtime
- See `CREDITS.md` for full attribution

## License

GPL-2.0-or-later. See `LICENSE`.
