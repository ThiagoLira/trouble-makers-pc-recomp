# Engineering notes & history

Everything an agent (or a curious human) needs to work on this repo without
re-deriving two days of hard-won context. For the operational quick-start
(build, gotchas, conventions) see [`../CLAUDE.md`](../CLAUDE.md) first; this
folder is the deep history. Read in this order:

## 1. The story so far — phase notes (chronological)

| File | What it covers |
|---|---|
| `PHASE1.md` | The original Phase 1 mission prompt (worker protocol example) |
| `PHASE1_NOTES.md` | Runtime groundwork: whole game compiles native; probe link; the 2 OS-wrapper gaps |
| `PHASE2_NOTES_w1..w4.md` | The four-component wave: game core, RSP/aspMain, RT64 glue, audio/input/save |
| `PHASE2_NOTES.md` | Driver integration + first-boot bug chain #1–#6 (VI race, rmonMain, ELF symbol holes, stale glob, libc interposition by translated code, raw AI MMIO) |
| `PHASE3_NOTES.md` | Bug #7: the boot park — DP message starved by a stalled Vulkan present (screen off!) |
| `PHASE4_NOTES_a.md` | Bugs #8–#10: aspMain IMEM base 0x1080 (not 0x1000!), lossy SP semantics, delivery starvation. Both driver appendices matter |
| `PHASE5_NOTES_a.md` | Bug #11: runtime overlay DMA registration (the fix that unlocked gameplay) |
| `PHASE5_NOTES_c.md` | Release build + the AI_LEN mirror (real audio pacing; fixed pacing 17→58fps) |
| `PHASE6_NOTES_a/b.md` | Widescreen groundwork: the view cull rect lever (D_800BE568..574); RT64 Expand + stale-wing analysis |
| `PHASE7_NOTES_widescreen.md` | **Real widescreen** (branch `real-widescreen` @ f1b0a85): walking-pointer root cause, catch-all repack, RT64 dual-path wing clear, MM_WARP debug tool, opt-in wing fills. Read §2 and §4 BEFORE touching the tile draws — §4 lists conclusions we falsified |
| `PHASE8_NOTES_rotation_wall.md` | Resolved Vertigo/Seasick wall and 3D-platform material failure: exact actor/list ownership, removed clear/LOD hacks, fixed-canvas policy, and dense regression matrix |
| `TERRAIN_POP_INVESTIGATION.md` | Current playable-widescreen terrain-pop investigation: proved layer ownership, falsified fixes, exact A/B commands, evidence, and the next experiment |
| `DIALOGUE_TOOLCHAIN_REGRESSION.md` | July 2026 release regression: GCC 11 corrupted dialogue/display-list code; compiler boundary, package A/B proof, fix, and Wayland validation gate |
| `PERFORMANCE.md` | Native profiling workflow, measured startup/long-session slowdown causes, runtime/launcher fixes, and ranked optimization opportunities |

## 2. Mission prompts (`prompts/`)

Every brief given to a worker/agent, phases 2–5. Useful as templates: they
encode the ground rules (worktree isolation, read-only shared dirs, commit
nothing, RESULT blocks) and the repro recipes that worked.

## 3. Facts that will save you hours

- **Headless harness**: `cmake -B build_headless -DMM_BUILD_GRAPHICS=OFF`,
  then `MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  ./build_headless/src/game/troublemakers rom.z64` — full game loop, no GPU.
- **Runtime diagnostics**: `logs/latest.log` records bounded five-second
  performance/audio/watchdog snapshots. Reproduce for at least 15 seconds,
  relaunch, and use **Support > Copy previous session**. The obsolete
  `MM_EVENT_TRACE` hot-path instrumentation is removed by runtime patch 0007.
- **The submodule rule**: `lib/N64ModernRuntime` is pinned to UPSTREAM; the
  local working tree runs branch `tm-fixes` = pin + `patches/N64ModernRuntime/`
  applied (`git -C lib/N64ModernRuntime am "$(pwd)"/patches/N64ModernRuntime/*.patch`).
  **Never commit the submodule gitlink** (.gitmodules ignore=all hides it).
- **Regenerating RecompiledFuncs** can add new `funcs_NN.c` files — rerun
  cmake configure (file GLOB is configure-time).
- **gdb**: ptrace attach is blocked (yama); launch under gdb instead:
  `timeout --foreground -s INT 20 gdb -batch -x script.gdb --args ...`.
  rdram: aligned-word reads at `x/xw rdram + (vaddr - 0x80000000)` are
  correct as-is (the ^3 swizzle only affects sub-word accesses).
- **Never** `pkill -f` a pattern that matches your own command line; always
  `timeout -k` (the game sometimes ignores plain SIGTERM).
- Current builds use the checked-in `symbols/troublemakers.us1.toml` map plus
  `input/troublemakers.us1.z64`; no decomp checkout or ELF is required. The
  historical phase notes may mention the maintainer's private reverse-
  engineering checkout, but do not substitute an unrelated third-party
  decompilation. The game is **natively 60fps**, uses **gspFast3D** (not
  F3DEX) + **aspMain** (RSPRecomp'd, IMEM
  base **0x04001080**), saves to **4Kbit EEPROM**, and pokes exactly one
  hardware register raw (AI_LEN — mirrored, see PHASE5_NOTES_c.md).
- Historical phase notes mention temporary `[gfx]`/`[mm_rsp]` counters and
  `MM_EVENT_TRACE`; current builds use the bounded session logger instead.

## 3.5 WORKSPACE RULE learned the hard way (Phase 7)

If a mission involves REGENERATING RecompiledFuncs (toml patch experiments),
the worktree MUST get its own COPY of RecompiledFuncs and its own
`output_func_path` — the default worktree setup symlinks it SHARED, and a
regeneration writes through the symlink into every checkout at once,
invalidating A/B tests and contaminating the main tree. (Recovery: regenerate
from main's toml.) Draft work parked in `drafts/backdrop_band_patches.toml.draft`
— NOTE: that draft is now OBSOLETE (its Attempt-2 conclusions were falsified);
the landed work and corrections live in `PHASE7_NOTES_widescreen.md`.

## 4. Worker orchestration (how the waves were run)

`tools/or_worker.sh` (headless claude-code on OpenRouter GLM),
`tools/spawn_worker.sh` (tmux window + live log view),
`tools/watch_worker.sh` (pretty-print the stream). Driver pattern: isolated
git worktrees per agent, lane-specific prompts, judge loop on the logs,
driver reviews every line and commits. Workers commit nothing.
