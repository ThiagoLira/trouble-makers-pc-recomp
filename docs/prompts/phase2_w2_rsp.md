You are Phase 2 worker "w2 — RSP microcode" on trouble-makers-pc-recomp (Mischief Makers N64 -> PC static recomp). Work ONLY in /home/thiago/repos/trouble-makers-pc-recomp. Read README.md and PHASE1_NOTES.md first.

GROUND RULES (all workers):
- You own ONLY `src/rsp/` (create it, including its own CMakeLists.txt), your notes file `PHASE2_NOTES_w2.md`, and new RSPRecomp toml configs at repo root (`*.rsp.toml` naming). Everything else is READ-ONLY: never edit the root CMakeLists.txt (it already `add_subdirectory(src/rsp)`s when your CMakeLists.txt exists), never touch RecompiledFuncs/, include/, other src/<component>/ dirs, or submodule sources.
- Build ONLY with your own build dir `build_w2` (and the recompiler's existing `tools/N64Recomp/build`). Three sibling workers run concurrently — use -j4.
- reference/Zelda64Recomp is STUDY ONLY: learn patterns (it has `aspMain.us.rev1.toml` at its root — the audio-ucode RSPRecomp config format), never copy wholesale.
- Commit NOTHING. The ROM at ~/repos/trouble-makers-ai-recomp/baserom.us1.z64 and that whole sibling repo are READ-ONLY reference (it is the decomp of THIS game — its symbols/asm can tell you where ucode blobs live).
- Never cat multi-MB files; use grep / xxd ranges / python slicing.

YOUR MISSION — RSP microcode recompilation (item 4 of the PHASE1_NOTES.md Phase 2 plan):

1. Build the RSP recompiler: the tools/N64Recomp CMake tree has an RSPRecomp target (`cmake --build tools/N64Recomp/build --target RSPRecomp -j4`; reconfigure if the target isn't generated).

2. Locate the game's RSP microcode in the ROM/decomp: the game uses F3DEX (graphics) plus an audio microcode (likely aspMain family). Use the decomp repo (~/repos/trouble-makers-ai-recomp: asm/, symbol names like gspF3DEX*/aspMain*, ucode data/text/data_size symbols) to find text/data offsets and sizes in the ROM. Document exact offsets + evidence in your notes.

3. Write RSPRecomp toml config(s) at repo root (model on reference/Zelda64Recomp/aspMain.us.rev1.toml and RSPRecomp's own docs/source in tools/N64Recomp) and generate the recompiled ucode C into `src/rsp/generated/` (add that dir to your notes; the driver will gitignore it if appropriate).

4. `src/rsp/CMakeLists.txt` — static lib target `mm_rsp` compiling the generated ucode C, plus `src/rsp/rsp_callbacks.cpp` glue implementing librecomp's rsp_callbacks interface (see lib/N64ModernRuntime/librecomp and how reference wires `get_rsp_microcode`/task dispatch). Worker w1 is building main() with stub callbacks in parallel — export a clean `mm_rsp` API it can adopt; do NOT edit src/game/.

SUCCESS GATE (must reach): the audio microcode (aspMain or whatever this game ships) is located, recompiled by RSPRecomp, and `cmake -B build_w2 && cmake --build build_w2 --target mm_rsp -j4` compiles it with zero errors. STRETCH: same for the F3DEX graphics ucode (Zelda64Recomp notably does NOT statically recompile F3DEX — RT64 interprets the display list — so first determine from the reference whether F3DEX even needs RSPRecomp here; if not, document that and what RT64 needs instead).

Write PHASE2_NOTES_w2.md as you go: ucode locations + evidence, toml configs, what rsp_callbacks needs from w1's seams, blockers. End your final message with:
RESULT
gate: <PASSED or FAILED>
stretch: <attempted/passed/skipped>
notes: <three lines max>
