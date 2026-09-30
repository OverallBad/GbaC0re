# CREDITS — GbaC0re v0.7

## Emulation core

- **mGBA** — Copyright (c) Jeffrey Pfau and contributors. **MPL-2.0.**
  https://mgba.io — `mCore` GBA emulation, HLE BIOS.
  (Not shipped; fetched at build time.)

## Runtime (adapted for this port — GPL-2.0-or-later)

- **LuaPSX** — soniciso1. The PS5 payload runtime this port is built on:
  shellcode bring-up, `sys_dynlib_dlsym` (0x24F) fix, streamed blob delivery,
  multi-mapping JIT loader, savedata dance, web controller pattern, fault
  trap, `boot.inc`/`linker.ld` relocation model. `COPYING` carries the license.
- **EmuC0re** — egycnq / EgyDevTeam. The PS5 runtime pattern LuaPSX derives from.
- **LuaGB** — fixGB core (MIT) as the studied reference for the runtime's
  Game Boy port; its launcher/UI/savedata shapes informed the GBA adaptation.
- **Luac0re** — Gezine. The Lua loader that delivers the payload.

## GbaC0re port

- GBA glue (`src/gba_glue.*`), 16-bit input policy, GBA web controller page,
  GBA ROM picker policy, docs — written for this port, same GPL-2.0-or-later
  terms as the runtime.

## License summary

| Component | License |
|---|---|
| LuaPSX-derived runtime files | GPL-2.0-or-later (`COPYING`) |
| GbaC0re glue/docs/launcher | GPL-2.0-or-later |
| mGBA | MPL-2.0 (its own `LICENSE`, in its source tree) |
