# Point your AI agent here

Everything an agent (or a curious human) needs to work on this repo without
re-deriving two days of hard-won context. Read in this order:

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

## 2. Mission prompts (`prompts/`)

Every brief given to a worker/agent, phases 2–5. Useful as templates: they
encode the ground rules (worktree isolation, read-only shared dirs, commit
nothing, RESULT blocks) and the repro recipes that worked.

## 3. Facts that will save you hours

- **Headless harness**: `cmake -B build_headless -DMM_BUILD_GRAPHICS=OFF`,
  then `MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  ./build_headless/src/game/mm_game rom.z64` — full game loop, no GPU.
- **Event tracing**: `MM_EVENT_TRACE=1` (runtime patch) traces sp/dp
  completions and the interesting message queues on stderr.
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
- The decomp at `../trouble-makers-ai-recomp` is READ-ONLY ground truth:
  source, `versions/us1/symbol_addrs*.txt`, asm. The game is **natively
  60fps**, uses **gspFast3D** (not F3DEX) + **aspMain** (RSPRecomp'd, IMEM
  base **0x04001080**), saves to **4Kbit EEPROM**, and pokes exactly one
  hardware register raw (AI_LEN — mirrored, see PHASE5_NOTES_c.md).
- Temp diagnostics still in tree (marked TEMP): `[gfx]`/`[mm_rsp]` stderr
  counters, MM_EVENT_TRACE. Strip before a proper release.

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
