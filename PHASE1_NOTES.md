# Phase 1 Notes — Mischief Makers static recomp

Engineer: Phase 1 groundwork pass. Status: **gate PASSED, stretch PASSED.**
Nothing committed by this pass — the driver commits. All hand-written files
live at the repo root (`CMakeLists.txt`, `include/`, `src/`, this file).

## What was done

1. **Runtime submodule** — `git submodule add …N64ModernRuntime.git lib/N64ModernRuntime`,
   `git submodule update --init --recursive` (pulls N64Recomp, miniz, o1heap,
   xxHash + the recompiler's own ELFIO/fmt/rabbitizer/sljit/tomlplusplus).
   Layout: `librecomp/` (libultra reimpl + recomp support, C++20) and
   `ultramodern/` (platform/threading/audio/input layer). The header the
   generated C includes — **`recomp.h`** — lives at
   `lib/N64ModernRuntime/N64Recomp/include/recomp.h` (self-contained: stdlib,
   stdint, math, fenv, assert; defines `recomp_context`, `gpr`, `fpr`,
   `RECOMP_FUNC`, and the `MEM_*`/`ADD32`/`MUL_*` macros).
2. **Reference clone** — `reference/Zelda64Recomp` (depth 1, gitignored, study
   only). Modeled the integration on its `CMakeLists.txt` `RecompiledFuncs`
   library and `src/main/register_overlays.cpp`.
3. **CMakeLists.txt** — `mm_recompiled` STATIC lib, globs `RecompiledFuncs/*.c`
   + `*.cpp`. Include paths: `RecompiledFuncs/` (for `funcs.h`), `N64Recomp/include`
   (`recomp.h`), `librecomp/include` + `ultramodern/include` (forward use).
   Compile flags mirror the reference: `-fno-strict-aliasing` (the `MEM_*`
   macros type-pun `rdram` bytes) plus `-Wno-unused-variable` etc.; the
   C-only `-Wno-` flags are scoped to C via `$<$<COMPILE_LANGUAGE:C>:…>` so the
   C++ `lookup.cpp` TU stays clean.
4. **Shim** — `include/recomp_config.h` (empty placeholder). The generated `.c`
   need only `recomp.h` + `funcs.h`, so no shim content is required yet; it
   exists as the future home for game config, matching the reference's
   game-private `include/`.

## Generated output layout (RecompiledFuncs/, from the toml)

- `funcs_NN.c` — 91 translation units; each `#include "recomp.h"` + `"funcs.h"`.
- `funcs.h` — prototypes for every recompiled function (`extern "C"` guarded).
- `lookup.cpp` — `get_entrypoint_address()` → `0x80000400`, `get_rom_name()` →
  `"troublemakers.z64"`. Consumed by librecomp.
- `recomp_overlays.inl` — `section_table[]` + `num_sections` (177) +
  `overlay_sections_by_index[]`. **Not compiled here** — it is `#include`d by a
  runtime TU that calls `recomp::overlays::register_overlays(...)` (Phase 2;
  see `reference/…/register_overlays.cpp`).

## SUCCESS GATE — PASSED

```
cmake -B build && cmake --build build --target mm_recompiled -j8
```
Zero errors, **zero warnings**. 92 objects (91 `.c` + `lookup.cpp.o`) →
`build/lib/libmm_recompiled.a` (8.0 MB). No hand-edits to `RecompiledFuncs/`
were needed; the generated C compiled as-is.

## STRETCH — PASSED (`mm_runtime_probe`, off by default: `-DMM_BUILD_PROBE=ON`)

A `mm_runtime_probe` executable that whole-archive-links **mm_recompiled +
librecomp + ultramodern** under `-Wl,--no-undefined`, with SDL2 (the only
runtime dep, needed by `ultramodern/src/audio.cpp`). It links cleanly and runs.

### Genuine link-time gaps: exactly 2

Enumerated by diffing the game's per-object undefined symbols against what the
game + runtime archives define (libm symbols `sqrtf/lrint/fegetround/…`
resolve via `-lm`, which g++ pulls in). Only **two** `_recomp` libultra
OS-function wrappers are referenced by the game and **not** implemented by
librecomp:

| symbol | called from | meaning | probe stub |
|---|---|---|---|
| `rmonPrintf_recomp` | `funcs_72.c` | N64 rmon (debug monitor) printf | no-op |
| `__osGetCause_recomp` | `funcs_90.c` | read MIPS CP0 `Cause` reg (exc code + IP bits) | `ctx->r2 = 0` |

Both are `extern "C" void name(uint8_t* rdram, recomp_context* ctx)` (the
standard wrapper signature; return value goes in `ctx->r2` = `$v0`). The stubs
live in `src/probe_glue.cpp` (host-side — **never** in `RecompiledFuncs/`).
Phase 2 should implement them for real inside librecomp alongside
`ultra_stubs.cpp`/`ultra_translation.cpp`.

### Why graphics/audio/RSP/entry do NOT surface as link errors

`recomp::start(const Configuration&)` (`librecomp/include/librecomp/game.hpp`)
requires `rsp_callbacks` and `renderer_callbacks` as **abstract callback
structs** the host app fills in — they are pure-virtual/function-pointer
interfaces, not link-time symbols. So RT64 (RDP/F3DEX), RSPRecomp (the RSP
microcode), the audio driver, controller/save, and the game entry are not
*unresolved*; they are simply **not registered**. The probe links but would not
render or run the game. Link-success ≠ a running game.

## Phase 2 work plan (derived from the stretch)

1. **Overlay/section registration** — a `register_overlays()` TU that
   `#include "RecompiledFuncs/recomp_overlays.inl"` and calls
   `recomp::overlays::register_overlays({section_table, …}, {overlay_sections_by_index, …})`,
   mirroring `reference/…/register_overlays.cpp`. `num_sections` is 177.
2. **Game entry + `recomp::start`** — wire `recomp::start(cfg)` with a
   `Configuration` (`window_handle`, `rsp_callbacks`, `renderer_callbacks`,
   `audio_callbacks`, `input_callbacks`, `events_callbacks`,
   `error_handling_callbacks`, `threads_callbacks`, `message_queue_control`),
   then `recomp::start_game(game_id)`. Entry address is `0x80000400` (from
   `lookup.cpp`).
3. **Graphics (RDP)** — RT64 renderer as the concrete `renderer_callbacks`
   (F3DEX microcode; see README Phase 2). Pull RT64 as a submodule.
4. **RSP microcode** — RSPRecomp for the game's F3DEX ucode → `rsp_callbacks`.
5. **Audio** — `audio_callbacks` (librecomp's `ai.cpp`/`ultramodern`'s
   `audio.cpp` already use SDL2; needs a concrete driver).
6. **Controller/save** — EEPROM save (`recomp::get_save_type` → EEPROM),
   `input_callbacks`; Trouble RLE asset streaming quirks (Phase 3 per README).
7. **2 missing OS wrappers** — implement `rmonPrintf_recomp` and
   `__osGetCause_recomp` in librecomp (remove the probe stubs).

## Decisions / notes

- `reference/` and `build_probe/` are gitignored (study-only / build artifact).
- Toolchain: gcc/g++ 16.1.1, cmake 4.3.4, SDL2 2.32.70 (all system-installed).
- The probe uses `--whole-archive` so the *entire* game + runtime are linked
  (a naive link only pulls the transitive closure of `main()`, which touches
  almost nothing and hides the real symbol surface).
- The first whole-archive link under `--no-undefined` failed on exactly
  `rmonPrintf_recomp` / `__osGetCause_recomp`; after the two stubs it links
  clean. Final executable's only `nm U` entries are libc/libstdc++/libgcc/dl
  runtime symbols resolved at load.
- No blockers for Phase 1. Phase 2 entry point: item 1 above (overlay
  registration) is the cheapest first step and unblocks everything else.
