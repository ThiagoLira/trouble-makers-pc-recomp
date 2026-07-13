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

YOUR MISSION (a — overlay streaming, THE critical path): make the game survive past 0x801B9900.
1. Identify what lives at 0x801B9900: search the decomp's symbol_addrs and splat config (versions/us1/) for the overlay/segment containing it; check RecompiledFuncs/recomp_overlays.inl's section_table (177 entries, rom/ram addresses) — grep for the section covering 0x801B9900. The function almost certainly IS recompiled already; its section just isn't marked loaded.
2. Understand how the game loads that code at runtime: the decomp has `Trouble_RLE_Type2` (0x80098B50) and related streaming routines; find the loader path (boot.c / game state code) that decompresses or DMAs overlay code into RAM, and determine the (rom_addr, ram_addr, size) triple it produces. Note librecomp's existing hook surface: `recomp::overlays::load_overlays(rom, ram, size)` (see librecomp/src/overlays.cpp + how recomp.cpp init calls it, and whether librecomp's pi.cpp do_rom_read path auto-registers code sections on ROM DMA — if the game DMAs COMPRESSED data then CPU-decompresses, that hook never sees the real code load, which is presumably the bug).
3. Fix game-side in src/game/ where possible: e.g. a native wrapper around the game's decompress-into-RAM routine (the add_loaded_function pattern used for rmonMain — see register_overlays.cpp) that calls the recompiled original THEN registers the target section; or hook at a higher-level loader function with a known symbol. Runtime changes allowed via privatized copy if truly needed — justify.
GATE PASSED = headless run gets past 0x801B9900 (no 'Failed to find function' there) and either runs >2000 gfx frames or reaches a NEW distinct failure, documented with the same rigor.
