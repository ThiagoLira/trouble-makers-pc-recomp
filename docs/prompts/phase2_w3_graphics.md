You are Phase 2 worker "w3 — graphics/RT64" on trouble-makers-pc-recomp (Mischief Makers N64 -> PC static recomp). Work ONLY in /home/thiago/repos/trouble-makers-pc-recomp. Read README.md and PHASE1_NOTES.md first.

GROUND RULES (all workers):
- You own ONLY `src/graphics/` (create it, including its own CMakeLists.txt), `lib/rt64/` (your clone area, already gitignored), and your notes file `PHASE2_NOTES_w3.md`. Everything else is READ-ONLY: never edit the root CMakeLists.txt (it already `add_subdirectory(src/graphics)`s when your CMakeLists.txt exists), never touch RecompiledFuncs/, include/, other src/<component>/ dirs, or submodule sources.
- Build ONLY with your own build dir `build_w3` (RT64's own build may live in lib/rt64/build). Three sibling workers run concurrently — use -j4.
- reference/Zelda64Recomp is STUDY ONLY: learn integration patterns, never copy wholesale. NOTE: it was cloned depth-1 WITHOUT submodules, so its lib/rt64 dir is empty — read its .gitmodules to find the exact RT64 fork/branch it pins, then clone that fork yourself into lib/rt64 (shallow is fine).
- NO sudo / no system package installs. If a system dependency (Vulkan headers, etc.) is missing, document it precisely in your notes and make your CMake component optional (guard with an option defaulting to what currently builds) rather than breaking the build.
- Commit NOTHING. Never cat multi-MB files.

YOUR MISSION — RDP rendering via RT64 (item 3 of the PHASE1_NOTES.md Phase 2 plan):

1. Study how Zelda64Recomp integrates RT64: its .gitmodules (which RT64 fork+branch), its CMakeLists (how RT64 is added, required flags/defines), and its renderer glue (src/graphics/*, RT64Context / renderer_callbacks / ultramodern renderer interface — see lib/N64ModernRuntime/ultramodern for the abstract interface our side must implement).

2. Clone that RT64 fork into lib/rt64 and get the RT64 library target building on this system (Linux, gcc 16, SDL2 available; check for Vulkan SDK availability with pkg-config/find_package before assuming). Document every deviation needed from the reference's setup.

3. `src/graphics/` — the glue: implement the renderer_callbacks / ultramodern render-context interface that hands RDP display lists to RT64, modeled on the reference but written for our game (F3DEX microcode per README). Worker w1 is building main() with stub callbacks in parallel — export a clean API (e.g. `mm_graphics` static lib with an init/register function) it can adopt later; do NOT edit src/game/.

4. `src/graphics/CMakeLists.txt` — target `mm_graphics` linking RT64. If RT64 cannot build due to missing system deps, the target must degrade gracefully (option MM_BUILD_GRAPHICS defaulting OFF with a clear CMake message) so `cmake -B build_w3` never breaks the tree for others.

SUCCESS GATE (must reach): RT64 (the reference's pinned fork) builds as a library on this machine AND `cmake -B build_w3 && cmake --build build_w3 --target mm_graphics -j4` compiles the glue with zero errors. If a missing system dep makes RT64 unbuildable, the fallback gate is: dep documented precisely (exact package/component missing, evidence), glue written and compiling against RT64 headers with the link step guarded OFF.

Write PHASE2_NOTES_w3.md as you go: fork/branch pinned, build deviations, the exact callback surface w1 must call, blockers. End your final message with:
RESULT
gate: <PASSED, PASSED-FALLBACK, or FAILED>
notes: <three lines max>
