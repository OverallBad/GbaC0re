# GbaC0re v1.3.0 — Link-cable bridge (PC<->PS5 multiplayer)

Built 2026-09-29. Stable baseline v1.2.15 + link-cable bridge only
(no experimental swap behavior).

## Link menu (pause menu, before QUIT)
- New LINK item with live status: LINK: OFF / LINK: HOST (WAITING) /
  LINK: HOST (CONNECTED).
- Selecting LINK when off starts hosting (UDP 43879).
- Selecting LINK when active disconnects (keeps SIO driver attached).
- QUIT remains the final menu item.

## Protocol (shared ../link/, also used by the PC port)
- mGBA-verified SIO semantics: outgoing words from SIOMLT_SEND,
  connectedDevices=1 (other devices, not total).
- Reliable UDP: HELLO/ACK rendezvous, REQ retransmit every 100ms,
  duplicate-HELLO ACK resend, duplicate-REQ response caching,
  deferred RESP for slaves not yet at the Busy edge.
- Multiplayer + Normal 8/32 modes supported.
- 29 deterministic unit tests pass (link/test_link.c).

## Integration
- SIO driver attached at ROM load, detached before core unload.
- bridge_poll() in gameplay, pause menu, ROM picker, and no-ROM wait loop.
- PS5 hosts; PC joins (see GbaC0re PC v1.2).

## Build
- Blob 720,624 bytes; 272 byte margin under the 0xB0000 JIT reserve.
- 10,729 R_X86_64_RELATIVE relocs, ET_DYN, no absolute immediates.

## Batch ROM upload (2026-09-29)
- Launcher sends POST /roms_begin?count=N before uploading N ROMs.
- Payload shows "RECEIVING ROMS" with a progress bar, waits for all N
  uploads to complete before opening the ROM picker.
- Fixes the issue where the picker opened after the first ROM, missing
  subsequent uploads.
- Blob grew to 721,264 bytes; JIT reserve auto-sized to 0xB4000.
