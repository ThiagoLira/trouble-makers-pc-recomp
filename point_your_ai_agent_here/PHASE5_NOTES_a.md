# Phase 5 Notes (lane a) — the 0x801B9900 overlay crash: root cause & fix

## TL;DR

At ~frame 770 the game DMA'd a world/level overlay's object code (raw,
uncompressed) from ROM `0x7C6AE0` into RDRAM `0x801B9900` and then jumped
there — but the runtime's `func_map` had no entry for `0x801B9900`, so
`get_function()` aborted with `Failed to find function at 0x801B9900`.

Root cause: **N64Recomp emits no per-call-site overlay-load hooks for this
title** (grep `RecompiledFuncs/` for `load_overlays`/`load_overlay_by_id` →
zero hits; the only caller in the whole runtime is `recomp.cpp`'s init, which
pre-loads the boot DMA). So when the game streams overlay code at runtime via a
PI DMA, the bytes are copied into RDRAM (by `recomp::do_rom_read`) but no
section is ever registered in `func_map`. The first indirect jump into the
overlay then fails to resolve.

Fix (one runtime hook): after `recomp::do_rom_read` copies a ROM→RDRAM DMA,
call `load_overlays(rom_offset, ram_addr, size)`. This is the symmetric
counterpart to the `load_overlays()` call the init path makes for the boot DMA,
generalized to *every* ROM read. `load_overlays()` is a no-op for DMAs that
don't overlap a code section (assets, dispatch tables, saves), so it is safe
to run on every read.

**Result:** headless run goes from crashing at frame ~770 (`Failed to find
function at 0x801B9900`) to running the full 60 s timeout with **0** lookup
failures, **send_dl #3500** gfx frames, and **aspMain 3580/3580 Broke**.

## What lives at 0x801B9900

`RecompiledFuncs/recomp_overlays.inl`'s `section_table` (177 entries). The
entries with `ram_addr = 0x801B9900` are the **overlay_4 slot** — the game's
per-world/per-level object-code overlays (one per world). The first is
section index 144:

```
{ rom_addr=0x007C6AE0, ram_addr=0x801B9900, size=0x00007580,
  funcs=section_144_overlay_7C6AB0_funcs, index=144 }
```

and its funcs array begins:

```
{ .func = func_801B9900_7E7F74, .offset = 0x00000000, ... }  // offset 0 → vram 0x801B9900
```

So the function at `0x801B9900` **is recompiled already** (`func_801B9900_*`);
its section just was never marked loaded. Confirmed by the decomp splat
(`versions/us1/troublemakers.yaml` line 1322+: "Overlay slot 4: dispatch
table -> 0x801B9800, object -> 0x801B9900"; `overlay_7C6AB0` start `0x7C6AE0`
vram `0x801B9900`).

## How the game loads that code at runtime

The loader is `func_80026174(u16 index)` (decomp
`asm/nonmatchings/26A00/func_80026174.s`), one of five per-slot overlay loaders
(`func_80025EC4/80025F70/8002601C/800260C8/80026174`, slots 0–4, RAM bases
`0x80192100/0x8019B100/0x801A6900/0x801B0900/0x801B9900`). It indexes
`D_800CE68C[index]` (an `OverlayLoadEntry` table in `src/26A00.c`: dispatch
table + object, each as a `(rom_start, rom_end)` pair) and:

1. `DMA_ReadSync(dispatch_table_rom_start, 0x801B9800, dispatch_end-start)` —
   the 0x30-byte dispatch table.
2. `DMA_ReadSync(object_rom_start, 0x801B9900, object_end-start)` — the
   object code, raw (uncompressed).

`DMA_ReadSync` (`asm/nonmatchings/dma/DMA_ReadSync.s`, `0x800011F0`) calls
`osPiStartDma` then `osRecvMesg`. `osPiStartDma` → `osPiStartDma_recomp`
(`librecomp/src/pi.cpp`) → `do_dma` → `recomp::do_rom_read(rdram, ram_address,
physical_addr, num_bytes)`. So the overlay object code DMA flows through
`do_rom_read` with `physical_addr = rom_base | 0x7C6AE0 = 0x107C6AE0`,
`ram_address = 0x801B9900`, `num_bytes = 0x7580` — exactly the `(rom, ram,
size)` triple that matches section 144. The bytes get copied; the section
never gets registered. (The mission's "compressed DMA" hypothesis does not
apply to *this* overlay — it is a raw DMA — but the principle is identical:
nothing observes the code load to register it. Other overlays in this game
may also stream through the Trouble RLE path; the `do_rom_read` hook covers
both, because whatever lands code bytes in RDRAM via a PI read gets registered.)

## The fix

`lib/N64ModernRuntime` was privatized into this worktree first
(`rm lib/N64ModernRuntime && cp -rL …/lib/N64ModernRuntime lib/`) so the
change is local. Then in `librecomp/src/pi.cpp`, `recomp::do_rom_read`:

```diff
+#include "librecomp/overlays.hpp"   // load_overlays (extern "C")
…
     uint8_t* rom_addr = rom.data() + physical_addr - recomp::rom_base;
     for (size_t i = 0; i < num_bytes; i++) {
         MEM_B(i, ram_address) = *rom_addr;
         rom_addr++;
     }
+
+    // Register any recompiled code sections whose ROM range this DMA just
+    // landed into RDRAM. … (symmetric counterpart to the init-path
+    // load_overlays() call; no-op for non-code DMAs).
+    load_overlays(physical_addr - recomp::rom_base, (int32_t)ram_address,
+                  (uint32_t)num_bytes);
 }
```

That is the entire fix: one include + one call.

### Why runtime-side, not game-side (justification)

The mission prefers game-side fixes; a runtime change is "if truly needed."
It is truly needed here:

- N64Recomp emits **no** overlay-load hooks for this title, so the runtime is
  the only layer that can uniformly observe ROM→RAM code DMAs.
- A game-side wrapper would have to wrap **all five** per-slot loaders
  (`func_80025EC4 … func_80026174`) and re-derive each `(rom, ram, size)`
  from the `OverlayLoadEntry` tables in rdram — fragile, partial (misses any
  other code-DMA path, e.g. RLE-streamed slots), and duplicative of loader
  internals.
- `do_rom_read` is the single chokepoint for every ROM read. Hooking it
  registers **every** overlay slot automatically with one line. The
  empirical trace (below) confirms slots 0, 1, and the boot `.main` section
  all load through this same hook — not just slot 4.
- It is the natural generalization of what the init path already does
  (`recomp.cpp`: `load_overlays(0x1000, entrypoint, 1MB)` then
  `do_rom_read(…)`). Making `do_rom_read` itself call `load_overlays` simply
  extends that to runtime DMAs. Upstream-worthy, like the bug-#10 delivery
  fix in `ultramodern/src/mesgqueue.cpp`.

### Why the dispatch-table DMA doesn't mis-register

`load_overlays` uses `lower_bound`/`upper_bound` on sections sorted by
`rom_addr`. The dispatch DMA covers `[0x7C6AB0, 0x7C6AE0)` — adjacent to, but
not overlapping, section 144 `[0x7C6AE0, 0x7CDD60)`. The bounds yield an empty
range for the dispatch DMA and exactly `{144}` for the object DMA (the
following section 147's `rom+size` exceeds the DMA end, so 144 is included).
Empirically confirmed: no spurious registrations, no wrong addresses.

## Evidence

Temp trace (now removed) printed `load_overlay`'s first 8 calls during boot:

```
[ovl] section index=3  rom=0x00001000 ram=0x80000400 size=0x000EF0D0 funcs=2076  # .main, via boot DMA
[ovl] section index=5  rom=0x000F00D0 ram=0x800EF4D0 size=0x00000030 funcs=6
[ovl] section index=57 rom=0x00713630 ram=0x80192100 size=0x000060D0 funcs=50    # overlay slot 0
[ovl] section index=69 rom=0x00731E60 ram=0x8019B100 size=0x0000B210 funcs=94    # overlay slot 1
```

Multiple overlay slots register through the same hook — the fix is general.

### Success gate (headless, `MM_HEADLESS_GFX=1`, 60 s)

| metric | before fix | after fix |
|---|---|---|
| `Failed to find function` count | 1 (`0x801B9900`) | **0** |
| gfx frames (`[gfx] send_dl #N`) | crashed at ~#770 | **#3500** (>2000 ✓) |
| audio | aspMain 760/760 Broke then crash | **aspMain 3580/3580 Broke** |
| run outcome | core dump at boot progress 0x801B9900 | ran full 60 s timeout, 0 asserts/aborts |

Repro:
```
cmake -B build -DMM_BUILD_GRAPHICS=OFF && cmake --build build --target mm_game -j8
cp ~/repos/trouble-makers-ai-recomp/baserom.us1.z64 ./rom.z64
MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout -k 5 60 ./build/src/game/mm_game ./rom.z64
```

## What changed (files)

1. `lib/N64ModernRuntime/librecomp/src/pi.cpp` — **the fix**: include
   `librecomp/overlays.hpp`; call `load_overlays(rom_offset, ram, size)` at
   the end of `recomp::do_rom_read`. (Runtime privatized into the worktree
   first; contains the tm-fixes/bug-#10 changes already.)
2. `src/game/main.cpp` — **TEMP diagnostic** (headless only): `StubRendererContext::
   send_dl` now prints a `[gfx] send_dl #N` counter (every 100), mirroring the
   existing `[mm_rsp] aspMain` counter and the RT64 build's `send_dl` counter
   in `src/graphics/rt64_render_context.cpp`. Needed only because the headless
   stub's `send_dl` was a no-op, so the gate's ">2000 gfx frames" metric had
   no observable. Marked TEMP; the real RT64 build is unaffected.

No changes to `RecompiledFuncs/`, `input/`, `reference/`, `src/rsp/generated/`,
or the toml — none were needed (the function at `0x801B9900` was already
recompiled; only its section needed registering).

## What remains

- **Pixels.** Headless confirms the frame loop now flows past the world load
  (3500 gfx frames, audio clean). Whether the *real* RT64 build (`-DMM_BUILD_
  GRAPHICS=ON`) actually renders the world — vs. hitting the known Phase-3
  present-backpressure park, or a new renderer issue — is the next frontier.
  The overlay fix is renderer-independent and correct under either build.
- **Overlay unload symmetry.** `unload_overlays` is never called by this
  game's code (N64Recomp emits no unload hooks either), so when the game
  swaps world overlays at the same RAM base (e.g. `0x801B9900` reused for a
  different world), `load_overlays` just overwrites the prior `func_map`
  entries for that RAM range — correct, because the new overlay's funcs are
  registered at the same addresses. `loaded_sections` accumulates stale
  `LoadedSection` records (never erased), which is benign for execution but
  could be cleaned up if unload semantics are ever needed. No issue observed
  across 3500 frames / multiple overlay swaps.
- The `do_rom_read` hook runs `load_overlays` (a sorted-range binary search +
  map inserts) on every ROM read. At ~58 fps headless (3500 frames / 60 s)
  there is no measurable cost; noted for completeness.
