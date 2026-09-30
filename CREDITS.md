# CREDITS — GbaC0re

## GbaC0re

- **OverallBad** — GbaC0re developer. Ported the emulator to the PS5 LuaC0re
  payload runtime, wrote the GBA glue layer, menu system, ROM uploader,
  save-state support, and the PC↔PS5 link-cable bridge.

## Stepping stones

Without these projects, GbaC0re would not exist.

- **Gezine** — creator of **LuaC0re**, the Lua loader that delivers payloads
  to the PS5. GbaC0re runs on top of it.

- **egycnq / EgyDevTeam** — creator of **EmuC0re**, the original LuaC0re NES
  payload. Its PS5 runtime pattern (shellcode bring-up, blob delivery,
  savedata handling) is the foundation the GBA port builds on.

- **BrinooTk** — creator of **SnesC0re**, the LuaC0re SNES payload. Its
  launcher, UI, and savedata shapes directly informed the GBA adaptation.

- **soniciso1** — creator of **LuaPSX**, whose proven PS5 payload runtime
  GbaC0re is built on: shellcode bring-up, `sys_dynlib_dlsym` (0x24F) fix,
  streamed blob delivery, multi-mapping JIT loader, savedata dance, web
  controller pattern, fault trap, `boot.inc`/`linker.ld` relocation model.

- **Jeffrey Pfau (endrift)** and contributors — creators of **mGBA**
  (https://mgba.io), the Game Boy Advance emulation core at the heart of
  GbaC0re, including its HLE BIOS (no BIOS file needed).

## Emulation core

- **mGBA** — Copyright (c) Jeffrey Pfau and contributors. **MPL-2.0.**
  https://mgba.io — `mCore` GBA emulation, HLE BIOS.
  (Not shipped; fetched at build time.)

## Runtime (adapted for this port — GPL-2.0-or-later)

- **LuaPSX** — soniciso1 (GPL-2.0-or-later). `LICENSE` carries the license text.

## GbaC0re port

- GBA glue (`ps5/src/gba_glue.*`), menu system, ROM picker, web-based ROM
  uploader, save states, link-cable bridge (`ps5/link/`) — written by
  OverallBad for this project, same GPL-2.0-or-later terms as the runtime.

## License summary

| Component | License |
|---|---|
| LuaPSX-derived runtime files | GPL-2.0-or-later (`LICENSE`) |
| GbaC0re glue/docs/launcher/link bridge | GPL-2.0-or-later |
| mGBA | MPL-2.0 (its own license, in its source tree) |
