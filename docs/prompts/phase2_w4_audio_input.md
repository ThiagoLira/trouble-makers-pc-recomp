You are Phase 2 worker "w4 — audio/input/save" on trouble-makers-pc-recomp (Mischief Makers N64 -> PC static recomp). Work ONLY in /home/thiago/repos/trouble-makers-pc-recomp. Read README.md and PHASE1_NOTES.md first.

GROUND RULES (all workers):
- You own ONLY `src/audio_input/` (create it, including its own CMakeLists.txt) and your notes file `PHASE2_NOTES_w4.md`. Everything else is READ-ONLY: never edit the root CMakeLists.txt (it already `add_subdirectory(src/audio_input)`s when your CMakeLists.txt exists), never touch RecompiledFuncs/, include/, other src/<component>/ dirs, or submodule sources.
- Build ONLY with your own build dir: `cmake -B build_w4 && cmake --build build_w4 --target mm_audio_input -j4`. Three sibling workers run concurrently — use -j4.
- reference/Zelda64Recomp is STUDY ONLY: learn patterns, never copy wholesale. Keep dependencies to SDL2 only (already available; the reference may use extra input libs — do not add new third-party deps this wave).
- Commit NOTHING. Never cat multi-MB files.

YOUR MISSION — audio + controller + save (items 5, 6 of the PHASE1_NOTES.md Phase 2 plan):

1. Study the interfaces first: lib/N64ModernRuntime/ultramodern (audio_callbacks, input_callbacks — the abstract structs the host fills) and librecomp (save handling: recomp::get_save_type / EEPROM path — find how saves are persisted to disk). Then study how reference/Zelda64Recomp fills them (its src/game/ and src/main/ glue) — but implement with SDL2 primitives only.

2. `src/audio_input/audio.cpp` — audio_callbacks backed by SDL2 audio (queue/callback device, sample-rate handling as ultramodern expects).

3. `src/audio_input/input.cpp` — input_callbacks backed by SDL2 (keyboard mapping + SDL game controller if present). Mischief Makers is a 1-player game; a sane default keyboard map is enough — document it.

4. `src/audio_input/save.cpp` — whatever glue librecomp needs for EEPROM saves for this game (verify the game's save type: the decomp repo ~/repos/trouble-makers-ai-recomp is READ-ONLY reference; its src/ may reference osEeprom* calls as evidence). If librecomp handles EEPROM persistence internally and needs only configuration, document that and provide the config surface instead.

5. `src/audio_input/CMakeLists.txt` — static lib target `mm_audio_input`. Worker w1 is building main() with stub callbacks in parallel — export a clean API (header with init/register functions returning the filled callback structs) it can adopt later; do NOT edit src/game/.

SUCCESS GATE (must reach): `cmake -B build_w4 && cmake --build build_w4 --target mm_audio_input -j4` compiles with zero errors, and PHASE2_NOTES_w4.md documents the exact functions w1 should call to obtain your callback structs.

Write PHASE2_NOTES_w4.md as you go: interface findings, keyboard map, save-type evidence, blockers. End your final message with:
RESULT
gate: <PASSED or FAILED>
notes: <three lines max>
