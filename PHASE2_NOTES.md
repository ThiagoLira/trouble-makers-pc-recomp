# Phase 2 Notes — driver integration & first boot

Driver-level notes. Component-level detail lives in `PHASE2_NOTES_w1..w4.md`
(one per worker of the Phase 2 wave). This file covers what happened when the
four components were wired together and the game was booted for real —
the part no single worker owned.

## Wave outcome (all four gates passed independently)

| component | target | delivered by |
|---|---|---|
| `src/game/` | `mm_game` host entry, overlay registration, OS stubs | w1 |
| `src/rsp/` | `mm_rsp` (RSPRecomp'd aspMain + rsp_callbacks) | w2 |
| `src/graphics/` | `mm_graphics` (RT64-backed RendererContext) | w3 |
| `src/audio_input/` | `mm_audio_input` (SDL2 audio/input, EEPROM config) | w4 |

Driver review fixes on top of the workers' output: `SDL_Init(...) > 0` dead
error branch (SDL returns negative on failure); README/comment corrections
(the game uses **gspFast3D**, not F3DEX — w2's evidence); include-path
contract mismatch in `mm_rsp.hpp`; stub seams in `main.cpp` replaced with the
real component APIs.

## Integration decisions

- `main.cpp` wires `mm_rsp::make_callbacks()`, `mm_audio_input::*`, and
  (when built) `mm::graphics::create_render_context` via `MM_HAS_GRAPHICS`.
  Renderer/gfx fall back to stubs when RT64 is absent.
- Components are added before `src/game` in the root CMakeLists so
  `if(TARGET mm_graphics)` works.
- **Auto-start**: with no launcher UI, the game auto-starts when a ROM is
  passed on argv. The trigger lives in the `vi_callback` (NOT
  `gfx_init_callback`) — see boot bug #1.

## The first-boot debugging chain (each fixed at its proper layer)

1. **VI null-mode crash.** Starting the game before the first VI tick means
   `set_dummy_vi` never seeds `ViState::mode`, and ultramodern's `update_vi`
   dereferences it unconditionally (events.cpp:74). Fix: auto-start from the
   end of a VI tick (`vi_callback`), by which point the mode is seeded.
2. **`rmonMain` unresolvable.** The game's boot.c passes rmonMain (in
   N64Recomp's `ignored_funcs`) to `osCreateThread`; the runtime resolves
   thread entries by address at run time. Fix: host no-op registered with
   `recomp::overlays::add_loaded_function` from `GameEntry::on_init_callback`
   (must be there — `init_overlays()` clears `func_map` at game start, wiping
   anything registered earlier).
3. **45 static functions missing from the ELF.** The decomp compiles many
   libultra internals (`static` linkage), and the linked ELF drops their
   symbols; the recompiler skips symbol-table holes, but the game installs
   several of them as function POINTERS (libaudio sequencer handlers:
   `__CSPVoiceHandler` & family). Fix: `manual_funcs` in
   `troublemakers.us1.toml` (names/addrs from the decomp's symbol_addrs,
   sizes gap-derived). Regeneration emits them (30,260 → 30,305 functions)
   and auto-covers further jal-reachable holes (`static_3_800AD6F4`).
   Note: regeneration created a new `funcs_91.c` — the CMake file(GLOB) is
   configure-time, so reconfigure after regenerating RecompiledFuncs.
4. **Undefined `*_recomp` wrappers.** The newly translated rmon statics call
   ignored-function wrappers N64Recomp never emits. Fix: loud
   link-satisfaction stubs in `os_stubs.cpp` (`MM_UNREACHABLE_OS_STUB`) —
   unreachable because the rmon thread entry no-ops.
5. **libc interposition by translated code.** A translated rmon static named
   `send` was exported from the executable's dynamic symbol table (linked
   shared libs reference `send`), so libdbus's `send(2)` call executed MIPS
   code. Fix: `-fvisibility=hidden` on `mm_recompiled` — 30k flat extern-C
   symbols must never be exported.
6. **Raw MMIO from game code.** `Sound_Update` reads AI_LEN_REG directly
   (`lui 0xA450`) — the game's ONLY raw RCP register access (all other
   `D_A4*` uses live in replaced libultra). Fix: commit the AI mirror page
   (rdram offset 0x24500000) as zero-fill in `on_game_init`; AI_LEN=0 reads
   as "drained". TODO(phase3): trap-based MMIO forwarding to ultramodern's
   AI state for correct audio pacing.
7. **aspMain jump tables.** Exactly as PHASE2_NOTES_w2.md predicted: the
   recompiled audio ucode aborts on runtime-computed `jr` targets;
   `extra_indirect_branch_targets` in `aspMain.us1.rsp.toml` is grown by the
   run→abort→add loop (driver automated it).

## Licensing (resolved: repo code is MIT)

The wave's original `rt64_render_context.cpp` (w3) was derived from
Zelda64Recomp's GPLv3 file. It was REPLACED with an original implementation
written against the two MIT-licensed interfaces it bridges (ultramodern's
renderer_context.hpp, RT64's rt64_application.h) — verified behaviorally
equivalent (full RT64 init, same thread census). Repo code is MIT (see
LICENSE); distributed `mm_game` binaries still carry GPLv3 obligations from
statically linking N64ModernRuntime (librecomp/ultramodern) — see README.

## Where Phase 2 stands

Boot progression verified: ROM validation → runtime init → game thread →
`Thread_MainProc` → `Sound_Update` → a real audio RSP task through the
recompiled aspMain (jump-table target 0x1118 discovered & registered; more
will surface once audio pumps steadily). Current frontier: after full
runtime + RT64 + audio-device init (69 threads, renderer live), the game's
threads park on a message queue with zero CPU — one earlier run instead
spun hot, so there is a timing-dependent wait in early boot. Phase 3 entry
point: trace which `osRecvMesg` the game blocks in (likely tied to the
zero-page AI_LEN read in Sound_Update, item 6 above) and model that signal
properly. Then: first rendered frame.
