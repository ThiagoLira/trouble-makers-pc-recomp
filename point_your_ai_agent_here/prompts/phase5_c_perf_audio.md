You are one of THREE parallel agents on the Mischief Makers PC recomp, each with an INDEPENDENT mission (not a race — all three deliver). Repo state: commit "Bug #10 fixed" + headless mode. The game now runs its frame loop continuously (~770 gfx frames, 760/760 audio tasks clean) and crashes at boot progress point: `Failed to find function at 0x801B9900` — code above the static .main section, loaded at runtime by the game's own streaming.

REQUIRED READING (in order): PHASE2_NOTES.md, PHASE3_NOTES.md, PHASE4_NOTES_a.md (esp. both driver appendices), src/game/main.cpp, src/rsp/rsp_callbacks.cpp.

REPRO HARNESS (headless, fast, no display): build once with
  `cmake -B build -DMM_BUILD_GRAPHICS=OFF && cmake --build build --target mm_game -j8`
  `cp ~/repos/trouble-makers-ai-recomp/baserom.us1.z64 ./rom.z64`
then `MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -k 5 60 ./build/src/game/mm_game ./rom.z64` — reaches the 0x801B9900 crash in <60s. `MM_EVENT_TRACE=1` adds runtime message-flow tracing. stderr already counts `[gfx] send_dl #N` (every 10) and `[mm_rsp] aspMain N/N Broke` (every 20).

RULES: work ONLY in your worktree (you are cd'd there). RecompiledFuncs/, input/, reference/, src/rsp/generated/, lib/N64ModernRuntime, lib/rt64, tools/N64Recomp are symlinks SHARED with other agents — READ-ONLY. To modify the runtime: `rm lib/N64ModernRuntime && cp -rL /home/thiago/repos/trouble-makers-pc-recomp/lib/N64ModernRuntime lib/` first (it already contains the tm-fixes branch changes). Same pattern for lib/rt64 if needed. Commit NOTHING. Never pkill -f a pattern matching your own command line; always `timeout -k`. gdb: attach is blocked, launch under gdb (`timeout --foreground -s INT N gdb -batch -x script.gdb --args ...`). rdram reads in gdb: aligned words at `x/xw rdram + (vaddr - 0x80000000)` read back correct values. The decomp at ~/repos/trouble-makers-ai-recomp is READ-ONLY reference (source + symbol_addrs + asm).

DELIVERABLE: PHASE5_NOTES_<lane>.md (root cause/design, evidence, what you changed, what remains). End your final message with:
RESULT
gate: <PASSED|PARTIAL|FAILED>
summary: <two lines max>

YOUR MISSION (c — release build + real AI_LEN pacing): two independent tasks.
1. RELEASE BUILD: make `cmake -B build_rel -DCMAKE_BUILD_TYPE=Release -DMM_BUILD_GRAPHICS=OFF` build and run cleanly (watch for -O2 UB in the generated C: -fno-strict-aliasing is already set for mm_recompiled — verify equivalent flags apply everywhere needed; fix any new warnings-as-errors or miscompiles). Measure headless frames/sec vs the debug build over a 45s run (send_dl counter deltas) and report both numbers.
2. AI_LEN MIRROR: Sound_Update reads AI_LEN_REG raw at 0xA4500004 (rdram offset 0x24500004 — currently a zero-filled page mprotect'd in src/game/register_overlays.cpp on_game_init; reads always 0 = "DMA drained", so the game's audio pacing logic free-runs). Make that value REAL: mm_audio_input (src/audio_input/audio.cpp) knows the remaining audio (get_frames_remaining); implement a mirror so the game reads a plausible AI_LEN: e.g. a small host thread started in on_game_init (rdram pointer available there) writing remaining-bytes (frames*4, big-endian ^3-swizzled aligned u32 — aligned word writes with plain host stores are correct) into rdram+0x24500004 every ~1ms; expose whatever accessor you need from mm_audio_input's public header. Keep it clean: this is permanent code, not a diagnostic — comment the hardware semantics (AI_LEN = bytes remaining in the current DMA).
GATE PASSED = both: release build runs headless to the known 0x801B9900 crash with measured fps reported, AND the AI mirror is implemented + game still reaches the same crash point (no regression) with the mirror active.
