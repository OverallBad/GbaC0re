# mGBA ARM Dynarec → GbaC0re PS5 Port: Feasibility Assessment

**Date:** 2026-09-28
**Status:** Research only — no code changed.
**Verdict: Not feasible as a port. The x86-64 backend does not exist and would have to be written from scratch, and there is no executable memory left in the PS5 JIT pool for a code cache.**

---

## 1. What upstream mGBA actually has

**There is no dynarec in any mGBA release.** Checked 0.6.3, 0.8.4, 0.9.0, and current
master (0.10.5-era): `src/arm/` contains only the interpreter
(`arm.c`, `isa-arm.c`, `isa-thumb.c`, `decoder*.c`). No `dynarec/` directory.

The only dynarec ever written for mGBA lives on the abandoned **`feature/dynarec`**
branch (endrift, 2013–2016, last commit 2016-11-03). Key facts about it:

| Aspect | Detail |
|---|---|
| Guest | ARM7TDMI (ARMv4T), same as GBA |
| **Host** | **ARM only** (`src/arm/dynarec-arm/`, `emitter-arm.h`). **No x86-64 backend was ever written.** |
| Merge status | Never merged to master; experimental |
| Completeness | Partial — emitter contains `ILL` (illegal/unimplemented) fallbacks for many instruction forms; trace compiler (`dynarec-impl.c`, 188 lines) covers a subset |
| Why abandoned | Correctness issues; the interpreter got fast enough that the maintenance burden wasn't justified |

The 2016 branch hardcodes `cpu->executor = ARM_DYNAREC` in `ARMInit` — there is no
runtime interpreter/dynarec selection, only compile-time inclusion.

**Bottom line for item 1:** "porting mGBA's x86-64 dynarec" is a misnomer — there is
nothing to port. This would be a from-scratch x86-64 emitter plus a port of the
2016 trace compiler.

## 2. Code cache allocation (the critical seam)

In the dynarec branch (`src/arm/dynarec.c`):

```c
cpu->dynarec.buffer = executableMemoryMap(0x200000);   /* 2 MB code cache */
cpu->dynarec.temporaryMemory = anonymousMemoryMap(0x2000);
```

`executableMemoryMap` (posix: `src/platform/posix/memory.c`) does **not** use
`mmap(PROT_EXEC)`. It bumps a pointer through a **statically-linked executable
buffer** (`_execMem`, sized by the linker script). I.e., even upstream, the
dynarec assumed link-time-reserved executable memory — there is no
RW→RX dual-mapping dance in mGBA itself.

For GbaC0re this seam must be reimplemented against the PS5 JIT API
(`sceKernelJitCreateSharedMemory` + `CreateAliasOfSharedMemory` +
`JitMapSharedMemory`), which the C payload *can* reach via dlsym (syscall 0x24F
is plumbed through `resolve_sym`/`native_call`).

**The pool math kills it.** From `lua/gba.lua.in` comments and boot logs:

- One JIT mapping caps at 256 KB (`CHUNK = 0x40000`).
- "The old 1 MB reservation died with ENOMEM on chunk 4" → the per-process JIT
  pool ceiling is ~768 KB (3 mappings), possibly less in a used session ("the pool
  DEPLETES within a game session").
- The payload blob itself consumes all 3 mappings (705,216 bytes → 3 mappings).

A 2 MB code cache needs **8 additional 256 KB mappings**. The pool cannot provide
them. A smaller cache (e.g., 256–512 KB = 1–2 mappings) *might* squeeze in on a
fresh boot, but: (a) it competes with the payload's own mappings — the blob would
have to shrink to make room; (b) a tiny cache means constant eviction/recompile
churn, erasing the dynarec's speed advantage; (c) in a non-fresh session even one
extra mapping may fail.

There is no `mprotect`-style fallback: on PS5, arbitrary anonymous memory cannot
gain `PROT_EXEC` — that is precisely why the JIT shared-memory API exists.

## 3. Our runtime executable-memory primitives

- **At load (Lua):** `gba.lua.in` ~510–600 creates the RW+RX dual mappings,
  copies the blob, and jumps to RX. After `func_wrap(rx)(...)` the Lua side is done.
- **At runtime (C):** the payload receives only `eboot_base`, `dlsym_addr`, `ext`.
  It has **no retained handles** to the JIT shared-memory fds and no RW alias —
  only the RX mapping it executes from.
- **What C *could* do:** resolve `sceKernelJitCreateSharedMemory` et al. via
  dlsym and create fresh mappings at runtime (the Lua `jit_*` helpers prove the
  call sequences work). This is technically available but subject to the pool
  ceiling in §2.
- **Arena:** the 36 MB bump allocator in `shim.c` is plain RW anonymous memory —
  usable for dynarec *metadata* (trace tables, `BumpAllocator`, `Table`) but
  **not** for emitted code.

## 4. Dependencies of the dynarec

Good news — the 2016 dynarec is dependency-light:

- **No signals / fault handlers.** No `sigaction`, no `mprotect`-based SMC
  trapping. (This also means its self-modifying-code story is weak — see risks.)
- **No pthreads.** Single-threaded trace compilation.
- **Only `__clear_cache`** (`dynarec-arm/dynarec-impl.c:186`) — ARM-only icache
  flush. On x86-64 icache is coherent so the equivalent is a no-op, but the call
  sites assume the ARM emitter layout.
- Needs `util/bump-allocator.h` + `util/table.h` (both small, portable, already
  have freestanding-compatible implementations).
- The **decoder API is stable**: `ARMInstructionInfo`, `ARMDecodeARM`,
  `ARMDecodeThumb` exist with the same shape in the 2016 branch and in our
  vendored core (`include/mgba/internal/arm/decoder.h`). The trace compiler's
  input side would port with modest churn.
- The **core integration is not**: modern `ARMCore` has no `dynarec` field and no
  `executor` enum (dynarec-branch-only). Re-adding them means touching
  `arm.h`/`arm.c` and the run-loop dispatch — doable, but every future core
  resync re-opens the wound.

## 5. What a port would actually require (scope)

| # | Work item | Est. size |
|---|---|---|
| 1 | **x86-64 emitter from scratch** — every ARMv4T/Thumb ALU, load/store, branch, multiply, and flag-setting form lowered to x86-64 machine code. (Reference scale: the *partial* ARM emitter is 245 lines + 335 lines of table macros; x86-64 encoding is denser and less regular — realistic 1,000–1,500 lines for full coverage.) | 1,000–1,500 LOC, new |
| 2 | Port `dynarec.c` / `dynarec-impl.c` trace compiler to modern core; adapt to x86-64 emitter API; replace `__clear_cache` | ~300 LOC ported + rewritten |
| 3 | Re-add `ARMDynarec` to `ARMCore`, `executor` dispatch, init/deinit hooks in `arm.c`/`arm.h` | ~100 LOC touched |
| 4 | `executableMemoryMap` reimplementation on PS5 JIT API via dlsym (create SHM, dual-alias RW/RX, map both, adjacency not required for cache) | ~150 LOC, new |
| 5 | Dual-mapping write discipline: emit via RW alias, execute via RX alias (never W+X simultaneously — matches PS5 constraints) | design constraint |
| 6 | Vendoring `bump-allocator`, `table` utils into the freestanding build | small |
| 7 | Bring-up & correctness: the 2016 base was never correct; expect game-specific miscompilations, flag bugs, and cycle-count drift. No hardware debugger — diagnosis via UDP log only. | **dominant cost** |

Realistic estimate: **4–8 weeks of focused work** by someone fluent in x86-64
encoding *and* the mGBA core, with the debugging phase (item 7) being unbounded.
For reference, PCSX2's x86-64 recompiler is ~35,000 LOC — mature, but indicative
of the problem's true size.

## 6. Top 3 risks

1. **No executable memory for the code cache (hard blocker).** The JIT pool
   ceiling (~768 KB, 3×256 KB mappings) is fully consumed by the payload blob.
   mGBA's default 2 MB cache needs 8 more mappings. Shrinking the cache to fit
   1–2 spare mappings (if they even exist on a fresh boot) trades away the
   performance win to eviction churn. This alone makes the project not viable
   without first shrinking the payload or finding pool headroom that the ENOMEM
   history says isn't there.
2. **The x86-64 backend must be invented, not ported.** There is zero existing
   code — no emitter, no calling-convention bridge, no flag emulation. Every
   ARM instruction form is a new chance for a subtle miscompile, and GBA games
   are brutal differential testers (a wrong carry flag = a crash 20 minutes in).
   The 2016 ARM→ARM base doesn't de-risk this at all.
3. **The foundation was abandoned as incorrect.** The 2016 branch has `ILL`
   fallbacks, a weak SMC/invalidation story (no fault-based invalidation; GBA
   games *do* write to code regions — e.g., self-modifying overlays), and 10
   years of core drift. Porting it means inheriting its bugs, then debugging
   them over a UDP log on a 60-fps real-time target.

## 7. Alternatives worth noting

- **Interpreter optimization.** The current bottleneck is unknown — no profiling
  has been done on hardware. The frame pipeline (emulation + blit + audio +
  vsync) may be dominated by PPU/audio, not CPU decode. Profile first; the
  interpreter may be fast enough on a Zen 2 (GBA is 16.7 MHz; even a 20×
  interpreter overhead is ~335 MIPS-equivalent, trivial for Zen 2).
- **Static recompilation (ahead-of-time).** Projects like `mstan/gbarecomp`
  lift a GBA ROM to C++ on PC at build time. That moves the recompiler *off*
  the PS5 entirely — but it is per-game, PC-side, and a different product, not
  a runtime dynarec.
- **Cached interpreter** (mGBA's other old branch, `feature/cached-interp`):
  decode-once, execute-many threaded interpreter. Smaller win than a dynarec,
  but needs no executable memory — could live entirely in the RW arena. If CPU
  decode ever proves to be the bottleneck, this is the proportionate response.

## 8. Recommendation

**Do not pursue.** The combination of (a) no existing x86-64 backend, (b) an
abandoned/incorrect 2016 foundation, and (c) the PS5 JIT pool ceiling leaving no
room for a code cache makes this a high-cost, low-probability project. If GBA
performance becomes a measured problem on hardware, the correct next step is to
profile the interpreter build first, then consider a cached interpreter — not a
dynarec.
