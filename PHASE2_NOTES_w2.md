# Phase 2 Notes — Worker w2 (RSP microcode)

Engineer: Phase 2 RSP worker. Status: **gate PASSED, stretch skipped
(determined unnecessary — documented).** Nothing committed by this pass.

## Mission recap

Statically recompile the game's RSP audio microcode (aspMain) with RSPRecomp,
emit a `mm_rsp` static lib + `recomp::rsp::callbacks_t` glue that worker w1's
main() adopts. Graphics ucode handled by RT64, not RSPRecomp (see Stretch).

## 1. RSPRecomp built

`tools/N64Recomp` already had an `RSPRecomp` target; built clean:

```
cmake --build tools/N64Recomp/build --target RSPRecomp -j4   # -> tools/N64Recomp/build/RSPRecomp
```

The toml format is read by `tools/N64Recomp/RSPRecomp/src/rsp_recomp.cpp`.
Required keys: `text_offset`, `text_size`, `text_address`, `rom_file_path`,
`output_file_path`, `output_function_name`. Optional: `extra_indirect_branch_targets`,
`unsupported_instructions`, `overlay_slots`. The generated function signature is
`RspExitReason <name>(uint8_t* rdram, uint32_t ucode_addr)` — exactly
`RspUcodeFunc` (librecomp/include/librecomp/rsp.hpp:35).

## 2. Ucode location + evidence

The game ships **three** RSP ucode blobs, all standard Nintendo libultra.
Located via the decomp repo (`~/repos/trouble-makers-ai-recomp`):

- `versions/us1/symbol_addrs_libultra.txt` lines 550-554 give the RAM addresses,
  sizes, and (for data) `rom:` annotations.
- `asm/data/rsp/{rspboot,gspFast3D,aspMain}.s` `.incbin` the per-ucode text/data
  binaries from `assets/rsp/*.bin`.
- `src/boot.c:230-239` shows the gfx task loads `gspFast3DTextStart` with
  `ucode_size = 0x1000`, `ucode_data_size = 0x800`.

ROM offsets proven by byte-exact `rom.find(textbin[:32])` against
`baserom.us1.z64` (8 MiB, US 1.1):

| ucode            | RAM (text)   | size    | ROM offset (text) | data ROM | data size |
|------------------|--------------|---------|-------------------|----------|-----------|
| rspboot          | 0x800BA9E0   | 0xD0    | 0xBB5E0           | —        | —         |
| gspFast3D (F3D)  | 0x800BAAB0   | 0x1400* | 0xBB6B0           | 0xEF610  | 0x800     |
| aspMain (audio)  | 0x800BBEB0   | 0xE20   | 0xBCAB0           | 0xEFE10  | 0x2C0     |

\* gspFast3D textbin is 0x1400 but the game DMA-loads only `ucode_size=0x1000`
  into IMEM (4 KiB). The data `rom:` offsets (0xEF610, 0xEFE10) match the
  decomp's symbol_addrs annotations exactly, cross-validating the text offsets.

**README says "F3DEX" but the game actually uses gspFast3D (Fast3D)** — the
original F3D microcode, not F3DEX. Evidence: the loaded ucode symbol is
`gspFast3DTextStart` (boot.c:234) and the `gspF3DEX_*` symbols in
`ultralib/include/PR/ucode.h` are not referenced by this game.

## 3. toml config + generated output

Config: `aspMain.us1.rsp.toml` (repo root). Pointed `rom_file_path` at the
canonical US 1.1 ROM in the sibling decomp
(`../trouble-makers-ai-recomp/baserom.us1.z64`; `*.z64` is gitignored). Used
the **ROM offset** form (`text_offset=0xBCAB0`) rather than the textbin, so the
config documents the in-ROM location directly.

- `text_offset = 0xBCAB0`, `text_size = 0xE20`, `text_address = 0x04001000`
  (conventional IMEM base; RSPRecomp masks to 0x1FFF → labels at 0x1000+).
- **No `overlay_slots`** — aspMain is a single flat IMEM image (unlike naudio).
- `extra_indirect_branch_targets` **deliberately omitted** (see "Jump-table
  targets" below).

Generate: `tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml`
→ `src/rsp/generated/aspMain.cpp` (2245 lines, 72 KiB). Generated with **no
"Unhandled instruction" / "Unhandled mfc0" errors** — every instruction in this
aspMain is in RSPRecomp's supported set. `src/rsp/generated/` is build output
(not committed; the driver may gitignore it).

### Jump-table targets (runtime follow-up, NOT a build concern)

aspMain is data-driven: a few `jr` instructions branch to addresses computed at
runtime from small DMEM jump tables. RSPRecomp only auto-collects indirect
targets from *linking* branches (jal/jalr return addresses), so unlisted
targets print `"Unhandled jump target"` and abort the task **at runtime** —
compilation succeeds regardless. Supplying *wrong* targets is worse than
omitting them: any target that doesn't coincide with a real instruction vram
emits a `goto L_XXXX` with no matching label → **compile error**.

The reference (Zelda64Recomp/aspMain.us.rev1.toml) lists 24 targets, but those
are for the **0x1000-byte** MM aspMain; this game's aspMain is **0xE20 bytes**
(vram span 0x1000-0x1E20), so several of MM's targets (0x1F80, 0x1EF8, 0x1E7C…)
fall outside the text and must NOT be reused. Deriving the correct set for
*this* binary is a Phase-3 runtime-correctness task: run the ucode under the
recomp, hit the abort, read the printed `jump_target`, add it, repeat.

## 4. mm_rsp library + rsp_callbacks glue

Files (all under `src/rsp/`, my owned dir):

- `src/rsp/CMakeLists.txt` — `add_library(mm_rsp STATIC generated/aspMain.cpp
  rsp_callbacks.cpp)`. Includes `${RUNTIME_INCLUDES}` (provided by root
  CMakeLists); **PUBLIC**-links `librecomp` (the generated ucode + glue call
  into librecomp: `dmem[]`, `rspReciprocals[]`, `recomp::rsp::run_task`,
  `set_callbacks`, `constants_init` — all defined in `librecomp/src/rsp.cpp`).
  Adds `-msse4.1` (see "SSE flag" below) and the same permissive `-Wno-` flags
  as `mm_recompiled`.
- `src/rsp/rsp_callbacks.cpp` — implements `get_rsp_microcode`: returns `&aspMain`
  for `M_AUDTASK`, `nullptr` for `M_GFXTASK` (with a stderr shout — RT64 should
  consume gfx tasks via `renderer_callbacks` before RSP dispatch) and unknown
  types. Exports `mm_rsp::make_callbacks()` (fills `Configuration.rsp_callbacks`)
  and `mm_rsp::register_callbacks()` (set_callbacks + constants_init, for the
  non-Configuration path).
- `src/rsp/mm_rsp.hpp` — public API header. **Contract for w1**: add `src/` to
  its include path, `#include "rsp/mm_rsp.hpp"`, then
  `cfg.rsp_callbacks = mm_rsp::make_callbacks();` before `recomp::start(cfg)`.
  `recomp::Configuration.rsp_callbacks` is mandatory (game.hpp:88-94).

### SSE flag

`rsp_vu.hpp`/`rsp_vu_impl.hpp` (included by the generated ucode) use SSE4.1 /
SSSE3 intrinsics (`_mm_shuffle_epi8`, `pcmpgtq`, …) on x86_64. The TU must be
built with the ISA enabled or the `always_inline` intrinsics fail with *"target
specific option mismatch"*. librecomp's own `rsp.cpp` doesn't hit this — it
never includes `rsp_vu_impl.hpp`; only the generated ucode does. The reference
fixes it with `-march=nehalem` on the exe that compiles aspMain.cpp; here
`-msse4.1` (implies SSSE3 on gcc) on the `mm_rsp` target suffices.

## SUCCESS GATE — PASSED

```
cmake -B build_w2 -S . && cmake --build build_w2 --target mm_rsp -j4
```

Zero errors, **zero warnings** in both `mm_rsp` TUs (`generated/aspMain.cpp.o`,
`rsp_callbacks.cpp.o`). Output: `build_w2/lib/libmm_rsp.a` (119 KiB).
Symbols verified: `aspMain` defined (T) and resolved within the lib;
`mm_rsp::make_callbacks` / `register_callbacks` exported; `dmem`,
`recomp::rsp::set_callbacks`/`constants_init` undefined here and resolve via the
PUBLIC `librecomp` link dep at w1's link time. (Upstream librecomp TUs emit
cosmetic `UNUSED`-redefinition / `Wreturn-type` warnings — not mine.)

## STRETCH — skipped (determined unnecessary)

The graphics ucode (gspFast3D / F3D) is **not** statically recompiled, by
design — matching Zelda64Recomp, which does NOT recompile F3DEX either:

- RSPRecomp's own source (`rsp_recomp.cpp:134-135`) hardcodes `mfc0 DPC_STATUS`
  → 0 with the comment *"Good enough for the microcodes that would be recompiled
  (i.e. non-graphics ones)"*. Graphics ucode reads DPC/DPC status to drive the
  RDP pipeline; RSPRecomp can't model it.
- RT64 (the `renderer_callbacks` impl, worker w3's domain) interprets the
  **display list** directly — the RDP command stream the game builds in RDRAM
  (`task->t.data_ptr`), not the RSP ucode. So RT64 needs the display-list
  buffer + the F3D opcodes the game emits, **not** the gspFast3D ucode binary.
- librecomp's SP dispatch (`librecomp/src/sp.cpp`, `ultramodern/src/events.cpp`)
  special-cases `M_GFXTASK` to route to the renderer; `get_rsp_microcode` is
  only consulted for non-gfx tasks.

So: no F3D toml, no `gspFast3D.*.rsp.toml`. Documented here for the driver /
w3. (If a future need ever arises to recompile gspFast3D, the text ROM offset
is 0xBB6B0, size 0x1000, IMEM base 0x04001000 — but RSPRecomp would reject its
DPC-touching instructions.)

## Seams handed to w1 (main())

- `cfg.rsp_callbacks = mm_rsp::make_callbacks();` — mandatory, non-empty.
- Link `mm_rsp` (it PUBLIC-pulls `librecomp`, so just `mm_rsp` on the link line
  suffices for RSP symbols).
- Do **not** route `M_GFXTASK` through RSP — leave that to w3's RT64
  `renderer_callbacks`. If `get_rsp_microcode` is ever called for a gfx task,
  `rsp_callbacks.cpp` prints to stderr and returns nullptr (loud failure).

## Blockers / follow-ups

- None for the build gate.
- Phase-3 runtime: derive aspMain's `extra_indirect_branch_targets` for *this*
  0xE20 binary (run → read abort → add → repeat) so audio tasks don't abort on
  jump-table branches.
- The two Phase-1 OS-wrapper gaps (`rmonPrintf_recomp`, `__osGetCause_recomp`)
  are unrelated to RSP and remain for librecomp-side implementation.
