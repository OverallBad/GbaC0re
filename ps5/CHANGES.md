# CHANGES — GbaC0re v1.2.6

## v1.2.6 — arena-backed state buffer (2026-09-28)

- **VM fix.** v1.2.5's boot-time 1MB mmap crowded the process VM ceiling so
  the 624KB roms-array mmap failed and the scan was skipped (0 ROMs). The
  state buffer is now a 1MB malloc from the 36MB arena at boot: no new VM
  mapping, 35MB remains for the game (FireRed: 34.33MB).

# CHANGES — GbaC0re v1.2.5

## v1.2.5 — boot-time state buffer (2026-09-28)

- **Alloc fix.** v1.2.4's per-save 1MB mmap is refused by the PS5 during
  gameplay (process near VM ceiling with the 34MB game loaded). The buffer
  is now mmap'd once at boot in savestate_init (VM pressure low) and reused.
  Not a static .bss (v1.2.0 regression), not per-save (v1.2.4 regression).

# CHANGES — GbaC0re v1.2.3

## v1.2.3 — toast fix + ROM titles via post-scan (2026-09-28)

- **Toast fix.** The save-state toasts never appeared: v1.2.0 moved the
  toast to y=270 when enlarging the menu panel, but the UI buffer is only
  270px tall (y=0-269) -- the toast rendered off-screen. Now drawn inside
  the panel at y=py+ph-44. (v1.1.7 had it at y=238, visible.)
- **ROM titles.** Internal 12-byte titles (header offset 0xA0, uppercased)
  are back, read in a post-scan pass after the directory fds close --
  never interleaved with getdents iteration (the v1.2.0 regression).
  Filename fallback preserved.

# CHANGES — GbaC0re v1.2.2

## v1.2.2 — transient state buffer (2026-09-28)

- **Regression fix.** v1.2.0/v1.2.1 broke ROM discovery on hardware even
  with the scan reverted: the 1MB static state buffer in .bss grew the
  boot-time data region from ~1.3MB to ~2.3MB, which the PS5 refused to
  tolerate (scan returned 0). The buffer is now mmap'd transiently for
  the duration of each save/load and freed after; .bss is back to 24
  bytes and the boot layout matches v1.1.7. Save states fully kept.

# CHANGES — GbaC0re v1.2.1

## v1.2.1 — revert ROM-title header reading (2026-09-28)

- **Regression fix.** v1.2.0's ROM-title scan (opening each ROM via
  sceKernelOpen during directory iteration) broke ROM discovery on
  hardware: the picker reported no ROMs. The scan path is reverted to the
  v1.1.7-stable form (filename-based display); save states are untouched.
  ROM titles will return via a safer design (lazy read in the picker, not
  during the scan).

# CHANGES — GbaC0re v1.2.0

## v1.2.0 — save states + ROM titles (2026-09-28)

- **Save states.** Pause menu gains SAVE STATE and LOAD STATE (one slot per
  game, `/savedata0/states/<stem>.ss0`). The state is a full machine
  snapshot via mGBA's `saveState`/`loadState` -- no in-game save required.
  Staged to `/av_contents/content_tmp` then copied in under the savedata
  write window, same pattern as the battery save; the 1MB serialization
  buffer is static (the arena is too tight at runtime). Toasts confirm:
  STATE SAVED / STATE LOADED / NO STATE / STATE FAILED.
- **ROM titles in the picker.** The scanner now reads the 12-byte game
  title at ROM header offset 0xA0 and shows it instead of the filename
  (uppercased for the caps-only font); falls back to the filename when the
  header is unreadable or the title isn't printable ASCII.
- Menu panel grows to six items (RESUME / SAVE GAME / SAVE STATE /
  LOAD STATE / CHANGE ROM / QUIT); toast text is now dynamic.

# CHANGES — GbaC0re v1.1.7

## v1.1.7 — picker auto-selecting top ROM, for real this time (2026-09-28)

- **Root cause:** v1.1.6's picker fix was incomplete. It held the previous
  buttons on a failed poll, but `prev_btn = btn` still ran unconditionally
  at the end of the loop -- so a failed first picker poll wrote a fake 0
  into `prev_btn`, wiping the 0xFFFF "everything held" entry guard, and the
  next good read of the still-held X looked like a brand-new press.
- **Fix:** the picker now keeps a separate `last_btn` (last successfully
  polled buttons, starts at 0). A failed poll reuses `last_btn` and leaves
  `prev_btn` untouched, so the edge detector only ever advances on real
  poll data. A held X coming back from the pause menu can no longer fake
  a fresh press regardless of poll timing.

# CHANGES — GbaC0re v1.1.6

## v1.1.6 — picker auto-selecting top ROM (2026-09-28)

- **Root cause:** same class as the v1.1.5 menu bug. The picker's per-frame
  `btn` reset to 0, so a single failed `scePadReadState` poll in the middle
  of a held X fabricated a fake release-then-press: the next good poll of
  the still-held X read as a fresh `pressed` edge and instantly selected
  the top ROM. A light/quick tap worked only because the hold was too short
  for a poll glitch to interleave.
- **Fix:** a failed poll in the picker now holds the previous frame's
  buttons instead of faking "all released" (the 0xFFFF "all held" entry
  sentinel is explicitly not treated as QUIT). Menu, release-wait, play
  loop, and picker now all hold-last on poll failure.

# CHANGES — GbaC0re v1.1.5

## v1.1.5 — pause menu would not stay open (2026-09-28)

- **Root cause (proven by v1.1.4-dbg's log):** `scePadReadState` fails when
  polled too soon after the previous poll. The play loop polls successfully
  (sees L1, opens the menu), then the menu's frame-1 poll fires microseconds
  later and returns failure. The menu read that as "all buttons released",
  which cleared the `menu_held` guard, so frame 2's good L1 read looked like
  a fresh press and closed the menu instantly (`menu: close by L1 toggle`
  at f=2, every open). The post-menu release-wait had the same flaw: its
  first poll also failed, so it broke early while L1 was still down and the
  play loop re-opened the menu -- an open/close flicker loop (8 cycles in
  the log while L1 was held). This is why the menu "didn't stay on screen
  unless L1 was held" and couldn't be navigated.
- **Why v1.1.0 worked:** its click tone was a blocking 24-53ms audio
  submit at menu entry, which accidentally delayed frame 1's poll past the
  pad's refractory period. v1.1.1's queued (non-blocking) clicks removed
  that delay and exposed the bug.
- **Fix:** a failed poll is now "no new information", never input. The
  menu seeds its button word with the known-true entry state (it only opens
  on an L1 edge, so L1 is down) and holds the last good word across failed
  polls; the release-wait only breaks on a confirmed not-held read. The
  play loop and ROM picker already held-last on failure and are unchanged.
- **Kept from v1.1.3:** the write-only menu render (no video-memory reads).

# CHANGES — GbaC0re v1.1.4-dbg

## v1.1.4-dbg — instrumented menu-debug build (2026-09-28)

- **Why:** v1.1.3's pause menu will not stay open on hardware (menu
  disappears unless L1 is held; no navigation while held). Static review
  cannot explain it -- the input logic is byte-identical to v1.1.2 -- so
  this build instruments instead of guessing.
- **Instrumentation:** logs `menu: open (input_src=..)` on entry, every
  input-state change (`f= nb= btn= held= cursor= action=`), pad-poll
  failures, `menu: close by L1 toggle`, `menu: action=.. at f=..`, and
  `game: opening pause menu` (detects open/close flicker loops).
- **Defensive fix included:** a failed `scePadReadState` poll (`nb < 0`) no
  longer fakes an all-buttons-released frame; the menu holds the last good
  button word instead. A single failed poll previously cleared `menu_held`,
  turning the next good L1 read into a spurious close. If the log shows
  `pad poll failed` lines, this was the bug.
- **How to test:** run the PS5 UDP log listener, press L1 once (quick tap),
  wait 3 seconds, then hold L1 for 3 seconds and release. Paste the
  `menu:`/`game:` log lines.

# CHANGES — GbaC0re v1.1.3

## v1.1.3 — pause-menu lag, the real fix (2026-09-28)

- **Root cause:** v1.1.2 cut CPU work but the menu stayed laggy, because
  the stall was never CPU math -- it was *reads* from the write-combining
  video mapping. The framebuffers come from `map_dm` direct memory: writes
  are fast, reads stall hard. Every slow menu version read video memory
  per frame (`dim_fb`'s read-modify-write in v1.1.0/v1.1.1; v1.1.2's 8MB
  backdrop `memcpy` on every cursor move and `dim_fb` during the fade --
  which is why the fade felt like part of it). The ROM picker never lagged
  because its blit path is write-only. That was the tell.
- **Fix:** nothing in the menu path reads video memory anymore. The frozen
  game frame is dimmed at 240x160 in cached RAM (`menu_dim`, 150KB .bss,
  NOLOAD so the blob is byte-identical in size); dirty frames do clear +
  `blit_scale` up + menu overlay, all writes. The menu is now strictly
  opaque-or-transparent (drop shadow removed, panel/pill/toast fills made
  fully opaque), so `blit_ui_blend` never takes its read-modify-write path.
  Verified with a host render: 0 translucent pixels emitted, look
  otherwise unchanged.
- Banner: `=== GbaC0re v1.1.3`.

## v1.1.2 — pause-menu lag fix, second attempt (2026-09-28)

- **Fixed (for real this time):** v1.1.1 queued the click tones but the
  menu still felt laggy on hardware, because the actual dominant stall was
  never the audio. The menu re-ran three full-screen software passes every
  frame: `blit_scale` (614k writes), `dim_fb` over the whole 1920x1080
  (2M pixels x 3 multiplies), and `blit_ui_blend` (2M pixel visits, with
  per-channel integer divisions for every blended panel/shadow pixel) --
  tens of ms of CPU per frame, plus up to 4 blocking audio grains (21ms)
  after vsync on click frames.
- **Fix:** the dimmed game frame is now rendered once into a private third
  direct-memory buffer (the game frame is frozen while paused) and the menu
  is composited on demand -- only when the cursor, toast, or fade changes.
  Idle menu frames do zero pixel work: poll input, re-flip, vsync.
- **Audio:** click tones shortened to a single 256-frame grain each (5.3ms;
  still clearly audible, pitches kept distinct), menu drain cap 4 -> 2
  grains. A press can never backlog audio across frames now.
- **Fixed:** stale native banner now prints `=== GbaC0re v1.1.2`
  (was v0.7.8 while the Lua banner said v1.1.1).

## v1.1.1 — pause-menu input lag fix, first attempt (2026-09-28)

- **Fixed:** the pause menu felt very slow/laggy on hardware. Root cause:
  `gba_ui_click()` submitted each tone with 5-10 back-to-back blocking
  `sceAudioOutOutput` calls, freezing input polling and rendering for the
  tone's full 24-53ms on every press (worse when scrolling, as each
  auto-repeat re-triggered the stall).
- **Fix:** clicks are now synthesised into the 48kHz output ring and the
  menu loop drains at most 4 grains per frame via `gba_audio_flush_menu()`,
  so tones play across frames and the menu stays at full rate. The
  release-wait after closing also drains, so the back blip no longer leaks
  into resumed game audio. Gameplay audio path (`gba_audio_flush()`)
  unchanged.

## v1.1.0 — pause menu + UI refresh (2026-09-28)

- **Pause menu (L1):** opens an overlay without resetting the game. The core
  stays loaded; emulation just pauses. Items: RESUME, SAVE GAME (writes the
  battery save with a "SAVED" toast, stays in the menu), CHANGE ROM (the old
  L1 save-and-picker behaviour), QUIT (the old R1-hold behaviour).
- **Menu look:** dimmed game frame with a fade-in, floating rounded panel
  with a soft drop shadow, accent selection pill, footer button hints.
- **Click tones:** soft synthesized UI clicks -- a quiet tick on navigation,
  a warmer blip on select/open, a lower one on back/close -- through the
  existing 48kHz audio path.
- **Picker restyle:** same design language (dark panel, pill selection,
  "..." overflow markers, updated footer hints).
- L1 is edge-triggered now and the menu waits for release, so one press can
  never toggle it open-shut-open. R1-hold quit still works inside the menu.

# CHANGES — GbaC0re v1.0.0

## v1.0.0 — first stable release (2026-09-28)

Consolidates the full v0.7 line, hardware-verified on PS5 OFW via LuaC0re:

- **Core:** mGBA mCore rebased onto soniciso1's LuaPSX runtime; dlsym via
  syscall 0x24F; TCP payload streaming; JIT pool fix (3 mappings, cheat
  engine compiled out); 36MB arena ladder.
- **ROMs:** OFW upload via POST /rom (`/temp0/roms/`), picker scans
  `/savedata0/roms/` then `/temp0/roms/`; PC launcher auto-upload
  (`--roms`, filename validation, retry on abort).
- **Saves:** per-ROM `.sav` in `/savedata0/saves/`, L1 save + return to
  picker, 128KB flash saves verified (Pokemon FireRed).
- **Audio:** fixed-point linear resample 65,536 Hz → 48,000 Hz, 256-frame
  grains via blocking `sceAudioOutOutput`; masterVolume/forceDisable
  fixup permanent.
- **Memory:** `_pristineCow` allocates `toPow2(romSize)` instead of the
  32MB worst case (fixes Pokemon FireRed arena exhaustion).
- **Diagnostics:** arena usage after load + periodic, large-alloc tracer,
  allocation size on exhaustion failure.

# CHANGES — GbaC0re v0.7.27

## New in v0.7.27

- **Copy-on-write fix.** mGBA's `_pristineCow` allocated the full 32MB
  `GBA_SIZE_ROM0` when a game writes to the ROM region (Pokemon FireRed
  does this during flash/save init). With a 16MB ROM already loaded, the
  32MB CoW blew the ~36MB arena. Now allocates `toPow2(romSize)` instead
  (16MB for 16MB ROMs); the address mask already wraps correctly.

# CHANGES — GbaC0re v0.7.25

## New in v0.7.25

- **Arena diagnostics (no behavior change).** `malloc` failure now logs the
  requested size and current usage (`shim: arena exhausted (want N, used
  U/S)`); `gba_load_rom` logs arena usage after load; main loop logs usage
  every 600 frames. Built to diagnose the Pokemon FireRed arena-exhaustion
  crash on game switch.

# CHANGES — GbaC0re v0.7.24

## New in v0.7.24

- **Auto-upload fixes.** `upload_rom.py` now validates filenames locally
  against the payload's `[A-Za-z0-9._-]` gate *before* sending -- a bad name
  used to make the server RST mid-body, surfacing as WinError 10053 instead
  of a clean rejection. `gba_launcher.py --roms` dedupes the glob (Windows
  case-insensitive FS matched `*.GBA` twice), skips bad names with a clear
  message, and retries once after a connection abort. No payload change.

# CHANGES — GbaC0re v0.7.23

## New in v0.7.23

- **Auto ROM upload.** `gba_launcher.py --roms DIR` uploads every `.gba` in
  DIR to `/temp0/roms/` after the payload boots (waits for `:9030`, then
  one at a time with progress). `upload_rom.py` refactored so the launcher
  imports its `upload_rom()` instead of duplicating the POST logic. No
  payload change — the binary is identical to v0.7.22.

# CHANGES — GbaC0re v0.7.22

## New in v0.7.22

- **Clean audio.** Removed all temporary audio diagnostics: the `audio dbg:`,
  `audio state:`, `audio post:`, `audio tmr:`, `audio win:`, `audio mix:`,
  `audio clk:`, `audio loop:` log lines, the `mixer test:` self-test, and the
  vendor patches in `mgba/src/gba/audio.c`. Removed the 440 Hz startup
  self-test tone. Kept the v0.7.21 audio fixup (`masterVolume=0x100`,
  `forceDisableChA/B=false`) which makes game audio work.

# CHANGES — GbaC0re v0.7.21

## New in v0.7.21

- **Audio FIXED.** The v0.7.20 mixer self-test revealed the root cause:
  `GBAAudioInit`'s `masterVolume=0x100` / `forceDisableChA/B=false` were not
  taking effect in this build, so `_applyBias` multiplied by zero and the
  mixer output silence. The boot fixup now writes these fields explicitly.
  Game audio (DMA + PSG) flows: `nonzero` mixes, nonzero `currentSamples`,
  nonzero `maxin`/`maxout`.

# CHANGES — GbaC0re v0.7.20

## New in v0.7.20

- **Audio: mixer self-test.** At boot (after ROM load, before first frame),
  stuffs known values into `chB.samples[]` and calls `GBAAudioSample`
  directly. The `mixer test:` line reports `currentSamples`. Nonzero =
  mixer function works, fault is in event timing. Zero = mixer broken in
  this build.

# CHANGES — GbaC0re v0.7.19

## New in v0.7.19

- **Audio: mix-loop counters.** The `audio loop:` line reports
  `GBAAudioSample` calls, loop iterations (samples actually mixed), and how
  many mixed nonzero — distinguishing "the loop never runs" from "the loop
  runs but computes zero".

# CHANGES — GbaC0re v0.7.18

## New in v0.7.18

- **Audio: mixer clock.** The `audio clk:` line dumps `sampleInterval`,
  `lastSample`, and `sampleIndex`. If `sampleInterval` is wrong, the mix
  loop's `timestamp >= sampleInterval` gate never opens and the mixer
  silently produces nothing.

# CHANGES — GbaC0re v0.7.17

## New in v0.7.17

- **Audio: raw mixer input.** The `audio mix:` line dumps `chA/chB.samples[]`
  (the int8_t arrays the mixer reads) plus volume fields, to see whether the
  FIFO fill is reaching the array the mixer reads.

# CHANGES — GbaC0re v0.7.16

## New in v0.7.16

- **Audio: fill-window distribution.** The `audio win:` line reports the
  min/max/zero-count of the `until` fill window in `GBAAudioSampleFIFO`.
  If `until` is always ≤ 0, the FIFO-to-mixer fill loop never writes and
  the FIFO data is consumed without ever reaching the mixer.

# CHANGES — GbaC0re v0.7.15

## New in v0.7.15

- **Audio: timer/FIFO instrumentation.** The mixer runs but mixes zero
  while both DMA FIFOs hold data, so the prime suspect is the DMA sound
  timer. A vendor patch counts `GBAAudioSampleFIFO` calls, and the
  `audio tmr:` line reports TM0CNT/TM1CNT raw registers, the FIFO pump
  count, and the first FIFO words with read positions — answering whether
  the timer runs, whether the FIFO is pumped, and whether the FIFO data is
  real.

# CHANGES — GbaC0re v0.7.14

## New in v0.7.14

- **Audio: capture what `_sample` actually posts.** v0.7.13 showed a
  paradox: the mixer's `currentSamples[0]` is nonzero at dump time, yet
  every `postAudioFrame` delivers zero. A vendor patch in
  `mgba-test/src/gba/audio.c` now snapshots `currentSamples[0..3]` and the
  sample index inside `_sample` right before posting, plus the
  `audio`/`audio->p` pointers. The `audio post:` line prints the snapshot
  and pointer identity (`board` vs `audio` vs `audio->p`) to catch either a
  two-GBA mixup or a `_sample` that posts stale slots.

# CHANGES — GbaC0re v0.7.13

## New in v0.7.13

- **Fixed the `audio state:` report.** v0.7.12 read the mixer through a bad
  cast (`struct GBA*` is not at offset 0 of the mCore; `mCore->board` is the
  correct pointer). The state line now reports real values.

# CHANGES — GbaC0re v0.7.12

## New in v0.7.12

- **Audio: mixer-state report.** v0.7.11 proved the core delivers digital
  silence (`maxin=0`) while its sample engine runs. The `audio dbg:` report
  now adds an `audio state:` line reading the live mixer: SOUNDCNT_X enable,
  master volume, DMA-sound A/B routing + timers, all four PSG channel
  routings, SOUNDBIAS, the current mix value, and whether each DMA FIFO
  holds data. This distinguishes "the game never enabled/routed audio" (a
  register-write problem) from "the game drives the mixer but it outputs
  zero" (a core emulation problem).
- **Corrected `sceAudioOutOutput` return handling.** The v0.7.11 log showed
  `ret=256` -- that is the frames-consumed success value (the audible
  self-test tone returns it too), not an error. Only negative returns are
  now flagged.

# CHANGES — GbaC0re v0.7.11

## New in v0.7.11

- **Audio: silence isolated to the last step.** The v0.7.10 report showed
  the full pipeline moving (3.2M samples in, 9244 blocks submitted) yet no
  sound. New counters track the loudest absolute sample at core delivery
  (`maxin`) and at PS5 submission (`maxout`), plus the first nonzero
  `sceAudioOutOutput` return (`outerr`, also logged immediately). Reading
  the report: maxin=0 means the core delivers digital silence; maxout=0
  with maxin>0 means the resampler zeroes it; both nonzero means the PS5
  swallows live data. (Side finding: `last_rate=65536` is correct, not a
  bug -- the game writes SOUNDBIAS with resolution 1, so mGBA raises its
  rate via `audioRateChanged` and the resampler tracks it.)
- **Resampler overflow fixed.** The linear interpolation multiplied two
  s32s whose product exceeds 2^31 (65534*65535), wrapping into distortion;
  the product is now s64. This caused distortion, not silence, but it was
  wrong.

# CHANGES — GbaC0re v0.7.10

## New in v0.7.10

- **Audio diagnostic made deterministic.** The frame-600 one-shot could be
  missed if the log was copied mid-session. The pipeline counters are now
  reset at each game launch and the `audio dbg:` report prints on every
  game exit (L1 back to picker, R1 quit) as well as at frame 600 -- the
  report for the session just played is always in the log.

# CHANGES — GbaC0re v0.7.9

## New in v0.7.9

- **Color flicker fixed.** v0.7.8's R/B swap was applied in place on the
  buffer the renderer draws into -- but the software renderer skips
  scanlines it considers clean, so on static screens the in-place swap
  toggled pixels back every frame (red/blue flicker). The renderer now
  draws into a private `gba_render_target` buffer and av_video copies with
  the swap into `gba_framebuffer`, which is idempotent.
- **Save actually fixed.** The boot-time `mkdir /savedata0/saves/` runs
  against the read-only savedata mount and never takes; the write window
  then remounts the container read-write, but nobody created the directory
  inside the window, so the save open died with ENOENT. `shim_mkdir()` now
  creates SAVE_DIR inside the write window (return value logged).
- **Audio pipeline diagnostic.** The boot tone proved the PS5 path, but
  games stayed silent. `gba_audio_dbg()` logs one line at frame 600 that
  pins the fault to one stage: `rate_calls` (setAVStream negotiation),
  `mgba_sample_events` (core _sample timer -- 3-line vendor patch in
  mgba/src/gba/audio.c), `av_samples` (postAudioFrame delivery),
  `blocks` (resampler submissions), `maxsrc` (deepest source ring level).
- **Log filter tightened.** "Invalid video register" warnings (the game
  poking unimplemented video registers, e.g. Shining Soul 2 writing
  WINOUT) are benign and repeat forever; they no longer reach the log.

# CHANGES — GbaC0re v0.7.8

## New in v0.7.8

- **Color fix.** mGBA emits 32-bit pixels as 0x00BBGGRR but the PS5
  video-out plane is A8R8G8B8, so red and blue were swapped in-game. The
  swap now happens in av_video (once per source pixel).
- **Audio fix.** The port was opened but never initialized and never had
  its volume set -- following the hardware-proven PS5 sequence
  (sceAudioOutInit, then sceAudioOutOpen, then sceAudioOutSetVolume with
  eight 0x8000/0dB entries, flags 3 for L+R). A 0.3 s 440 Hz self-test
  tone now plays at boot: if it is heard but games stay silent, the PS5
  path is proven and the fault is upstream.
- **Log spam removed.** mGBA's per-SWI (DEBUG) and per-DMA (INFO) traces
  cost thousands of UDP packets a second; a custom mLogger now forwards
  only WARN and worse (plus GAME_ERROR) to the log.
- **Save diagnostics.** The mkdir return values for the rom/save/tmp
  dirs are now logged, so a save open failing with ENOENT despite O_CREAT
  can be traced to its directory.

# CHANGES — GbaC0re v0.7.7

## New in v0.7.7

- **First-ROM bootstrap fixed.** The /rom upload endpoint only ran inside
  the ROM picker, which requires at least one ROM on disk -- so the very
  first upload over HTTP was impossible: with zero ROMs the payload showed
  "NO ROMS" for 5 seconds and quit. Now the no-ROMs screen waits
  indefinitely, pumping web_handle every frame and rescanning the ROM dirs
  about once a second; the picker opens as soon as an upload lands in
  /temp0/roms/. R1-hold quits from the wait screen.

# CHANGES — GbaC0re v0.7.6

## New in v0.7.6

- **Arena ceiling measured: 32MB.** First v0.7.5 hardware run walked the
  ladder and landed on 32MB (64/48/40 refused, 32 accepted), then ran the
  full pipeline cleanly: shim up, fault trap armed, video/audio/pad up,
  savedata ready, "ROMs found: 0", clean exit status=0 step=99.
- **ROM allocation sized to the file.** `gba_load_rom` used to malloc the
  full 32MB worst case, which would have consumed the entire 32MB arena and
  starved every later allocation. It now probes the file size with
  fseek/ftell and mallocs only that (falls back to 32MB if the probe fails).
- **Ladder refined:** 36MB and 34MB rungs added between 40 and 32, since the
  ceiling sits somewhere in (32, 40].

# CHANGES — GbaC0re v0.7.5

## New in v0.7.5

- **Arena size ladder.** First v0.7.4 hardware run got all the way through
  `_start` (JIT 3/3, data region mapped, 10,688 relocations applied) but died
  with `status=-2 step=3`: the single 64MB anonymous mmap for the bump arena
  was refused inside ps2emu. `_start` now walks a 64/48/40/32/24/16MB ladder
  and takes the largest that fits, logging each attempt; below 16MB it gives
  up as before. ROM loading needs ~32MB+overhead, so sizes under ~40MB will
  fail large ROMs gracefully in `gba_load_rom`.

# CHANGES — GbaC0re v0.7.4

## New in v0.7.4

- **Fits the real JIT pool.** On OFW hardware the first run died at
  `sceKernelJitCreateSharedMemory` with ENOMEM (`ret=0x8002000C`) on mapping
  4 of 4: inside ps2emu the per-process JIT pool holds ~768KB, but the
  reservation was a fixed 1MB (`__data_start = 0x100000`) even though the
  blob is far smaller. Two changes:
  - `tools/mklua.py` now reserves exactly the page-aligned blob size instead
    of `__data_start` (the writable .data/.bss region was never part of the
    JIT mapping anyway -- `_start` maps it separately as anonymous memory).
  - The Lua loader allocates the final mapping at exactly the remaining size
    instead of a full 256KB chunk, and the adjacency check uses each
    mapping's real size.
- **Blob diet (~94KB).** The cheat engine is compiled out
  (`src/core/cheats.c`, `src/gba/cheats.c`, the three cheat parsers): the UI
  has no cheat interface, so the eight now-undefined cheat symbols are
  link-only no-op stubs in `src/mgba_stubs.c`. This also drops the 48KB GBK
  table the cheat parser's string layer dragged in. `-ffunction-sections`
  / `-fdata-sections` + `-Wl,--gc-sections` trims the rest of the dead code.
  Blob: 800KB -> 706KB; reservation: 1MB (4 mappings) -> 0xB0000 (3 mappings,
  last one partial).

# CHANGES — GbaC0re v0.7.3

## New in v0.7.3

- **OFW ROM upload.** On OFW there is no save manager, so ROMs can no longer
  be assumed present in `/savedata0/roms/`. The web server in `src/main.c`
  now accepts `POST /rom?name=<file>` and streams the body into
  `/temp0/roms/` (created at boot). The picker scans both locations on every
  open, savedata first. `/temp0` is wiped on reboot -- re-upload after one
  (same tradeoff as LuaPSX's `/temp0` discs); cartridge saves are unaffected
  since the payload writes `/savedata0/saves` itself.
- Upload protocol details: one upload at a time (HTTP 409 while busy);
  filename gate `[A-Za-z0-9._-]`, <=64 chars, must end `.gba`
  (case-insensitive), else HTTP 400; Content-Length 1..32MB required, else
  400. `web_handle` does bounded work per call (4 x 64KB recvs) so a 32MB
  transfer can't starve pad input; a ~30s stall aborts and the partial file
  is unlinked (or truncated to zero if `unlink` didn't resolve).
- Controller page (`lua/gba.lua.in`) gains an **upload ROM** picker
  (`<input type=file accept=".gba">` + status line); new `upload_rom.py`
  (`python3 upload_rom.py <PS5_IP> <rom.gba>`, stdlib only) for command-line
  uploads with progress.
- Docs: README gains a "ROMs on OFW" section; TESTING lists the new upload
  test items.

---

# CHANGES — GbaC0re v0.7.2 (previous)

## New in v0.7.2

- **GCC 14+ build fix.** Newer GCC turns implicit function declarations
  into hard errors. Three functions mGBA references were implemented in
  `src/mgba_stubs.c` but never declared in the freestanding headers, so
  the build died in `mgba/src/core/config.c` (`strncat`, `getenv`) and
  `mgba/src/gba/sharkport.c` (`strftime`). Added the declarations to
  `src/libc/string.h`, `src/libc/stdlib.h`, `src/libc/time.h`. A strict
  `-Werror=implicit-function-declaration` sweep over all 101 sources
  confirms nothing else is missing.

---

# CHANGES — GbaC0re v0.7.1 (previous)

## New in v0.7.1

- **`build.py` replaces the `make` dependency.** `python3 build.py` performs
  the exact same build (verified byte-identical `gba_emu.bin` and
  `lua/gba.lua` against `make`), `python3 build.py check` adds the
  relocation/image-integrity checks, `clean`/`distclean` remove products.
  The two shell checks are reimplemented in pure Python so the build works
  on Windows without `sh`/MSYS2. The Makefile is kept as an alternative.
  Python 3 was already required (launcher, mklua), so this removes a
  dependency rather than adding one.

---

# CHANGES — GbaC0re v0.7 (previous)

v0.7 rebases the GbaC0re payload onto **soniciso1's LuaPSX runtime** (the
proven LuaPSX/LuaGB/LuaMD/PSN64 runtime family), replacing the v0.6
hand-rolled bring-up. The mGBA `mCore` core is unchanged in behavior.

## New in v0.7

- **LuaPSX runtime bring-up**: dlsym resolved via syscall `0x24F`
  (`sys_dynlib_dlsym`) with measured per-firmware fallback deltas, instead of
  a fixed-offset guess. Fixes silent loader death on firmwares 2.xx–4.xx.
- **Streamed delivery**: the payload blob travels as raw binary over TCP
  9028–9045 (receiver proves itself with a `K` ACK); the Lua script stays far
  under Luac0re's 500 KiB remote-loader cap. Multi-mapping JIT loader for
  payloads over 256 KiB.
- **`.rela.dyn` kept and applied at boot**: mGBA's vtables and dispatch tables
  are initialized pointer data; `_start` now applies `R_X86_64_RELATIVE`
  relocations like a real loader. (LuaGB's discard rule would have required
  hand-rebuilding mGBA's tables.)
- **Web controller** on port 9030 with a GBA-specific page: D-pad, A/B, L/R
  shoulders, Select/Start, MENU and EXIT chords, plus keyboard and generic
  gamepad support.
- **UDP logging** on port 9027 with automatic subnet-broadcast destination;
  fault trap reports faulting address and RIP offset for `addr2line`.
- **ROM picker** over `/savedata0/roms/` scanning `.gba` only (case-insensitive),
  with the savedata unmount→RW→write→commit→RO dance.
- **Launcher** `gba_launcher.py`: sends the script to loader port 9026,
  streams the blob with ACK-based live-receiver scan, optional `--log`.

## Preserved from v0.6

- `mCoreFindVF → init → setVideoBuffer → loadROM → setAVStream → setKeys →
  reset → runFrame` lifecycle.
- GBA 240×160 output, integer-scaled ×6 to 1440×960 on 1080p.
- Dedicated 128-byte native pad buffer; `scePadRead` length quirk handled.
- VFile ownership rule: close only when `loadROM`/`loadSave` rejects the
  VFile, never after a successful load.
- Exact 256-stereo-frame audio submissions; blocking `sceAudioOutOutput`
  paces the emulator. GBA source rate 32768 Hz → 48 kHz resample.
- Per-game saves as `/savedata0/saves/<game-stem>.sav`, written only when the
  game touched save memory.
- GBA-only extension scanning.

## Input changes

- L1/R1 now follow the LuaPSX runtime convention: **L1** returns to the ROM
  picker (saving first), **R1 held ~1 s** quits the payload.
- GBA **L/R shoulders moved to L2/R2** so every game button remains reachable.
- Web and native paths speak a shared **16-bit** mask (`GBA_BTN_*`,
  `GBA_CMD_MENU = 0xFFFE`, `GBA_CMD_QUIT = 0xFFFF`).

## Removed

- FTP server (ports 1337/1338): dropped, as in LuaGB — ROM management goes
  through the on-console picker.
- Hex-embedded blob delivery (LuaGB style): superseded by TCP streaming.
- v0.6's zig-based freestanding build scripts: replaced by direct host-gcc
  compilation of a curated mGBA source list (no CMake, no patching upstream).

## License notes

- Runtime-derived files: **GPL-2.0-or-later** (LuaPSX inheritance).
- mGBA core: **MPL-2.0** (unchanged).
