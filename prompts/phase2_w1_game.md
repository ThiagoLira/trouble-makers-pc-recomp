You are Phase 2 worker "w1 — game core" on trouble-makers-pc-recomp (Mischief Makers N64 -> PC static recomp). Work ONLY in /home/thiago/repos/trouble-makers-pc-recomp. Read README.md and PHASE1_NOTES.md first — they explain the pipeline and exactly where Phase 1 left off.

GROUND RULES (all workers):
- You own ONLY `src/game/` (create it, including its own CMakeLists.txt) and your notes file `PHASE2_NOTES_w1.md`. Everything else is READ-ONLY: never edit the root CMakeLists.txt (it already `add_subdirectory(src/game)`s when your CMakeLists.txt exists), never touch RecompiledFuncs/ (regenerated output), include/, src/probe_*.cpp, other src/<component>/ dirs, or submodules.
- Build ONLY with your own build dir: `cmake -B build_w1 && cmake --build build_w1 --target <your-target> -j4`. Three sibling workers are running concurrently — stay in your lane, use -j4.
- reference/Zelda64Recomp is STUDY ONLY (licensed, different game): learn integration patterns, never copy files wholesale.
- Commit NOTHING (the driver commits). Do not run destructive git commands.
- RecompiledFuncs/funcs_*.c are megabytes each — never cat whole files; use grep / sed -n ranges.

YOUR MISSION — the core runtime wiring (items 1, 2, 7 of the PHASE1_NOTES.md Phase 2 plan):

1. `src/game/register_overlays.cpp` — a TU that `#include`s `RecompiledFuncs/recomp_overlays.inl` (section_table[], num_sections=177, overlay_sections_by_index[]) and registers it with librecomp. Model on reference/Zelda64Recomp/src/main/register_overlays.cpp and the librecomp API in lib/N64ModernRuntime/librecomp (recomp::overlays::register_overlays etc.).

2. `src/game/os_stubs.cpp` — real implementations of the ONLY two missing OS wrappers (see PHASE1_NOTES.md): `rmonPrintf_recomp` (decode the N64-side format string/args from rdram+ctx and forward to host printf if feasible; otherwise a logging no-op — document the choice) and `__osGetCause_recomp` (ctx->r2 = 0). Standard wrapper signature: `extern "C" void name(uint8_t* rdram, recomp_context* ctx)`.

3. `src/game/main.cpp` — the host entry. Model on reference/Zelda64Recomp/src/main/main.cpp: build the `recomp::start(...)` / ultramodern configuration with the REQUIRED callback structs (rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks, events_callbacks, error_handling_callbacks, threads_callbacks, ...) as MINIMAL STUBS — sibling workers are building the real RSP/graphics/audio/input pieces in parallel; yours must compile and link without them. Define clean seams: put each stub group in its own small function/struct so the sibling components can replace them later. Game entry address is 0x80000400 (RecompiledFuncs/lookup.cpp); ROM name "troublemakers.z64"; save type is EEPROM (verify what librecomp expects for that).

4. `src/game/CMakeLists.txt` — target `mm_game` (executable) linking mm_recompiled + librecomp + ultramodern + SDL2. The root CMakeLists already provides RUNTIME_INCLUDES, SDL2, and the runtime subdirectory when your dir exists.

SUCCESS GATE (must reach): `cmake -B build_w1 && cmake --build build_w1 --target mm_game -j4` builds and links with zero errors. STRETCH: run ./build_w1/... mm_game with a copy of the ROM if it needs one (ROM read-only at ~/repos/trouble-makers-ai-recomp/baserom.us1.z64 — copy, never move) and document in your notes how far startup gets before it needs the real RSP/renderer (a clean early abort is expected and fine).

Write PHASE2_NOTES_w1.md as you go: librecomp API surface you used, stub seams the sibling components must fill, decisions, blockers. End your final message with:
RESULT
gate: <PASSED or FAILED>
stretch: <attempted/passed/skipped>
notes: <three lines max>
