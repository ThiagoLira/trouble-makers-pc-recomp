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

YOUR MISSION (b — pixels verdict): the window renders black through 770 frames. Determine WHY, with evidence — no fix required if the verdict is "legitimately black until assets stream" (bug #11 blocks assets), but a wrong-wiring finding must come with the fix.
1. Decode the actual display lists: the boot DLs live at 0x8012AF40 / 0x801310C0 (sizes 384-1560 bytes, printed by [gfx] send_dl). Add a temp hex-dump in the stub renderer path (src/game/main.cpp StubRendererContext/send_dl seam) or dump rdram in gdb at those addresses; hand-decode the Fast3D/RDP command stream (reference: gbi.h in the decomp's ultralib/include/PR/, F3D opcode encodings). Answer: do these DLs contain any draw commands (fill rects, tri draws, texture loads) or only state setup + full-screen black clears?
2. Statically audit RT64's ucode identification: RT64 hashes the ucode text+data from RDRAM (lib/rt64/src/gbi/rt64_gbi.cpp getGBIForUCode, GBISegment hashLength/hashValue tables). Compute XXH3-64 of this game's gspFast3D text (ROM 0xBB6B0, game DMAs 0x1000 bytes to RAM) and data (ROM 0xEF610, 0x800 bytes) at the hashLengths RT64's F3D-family entries use (python3 xxhash or the vendored xxHash), and check whether ANY database entry matches. If none: RT64 returns nullptr and silently renders nothing — document the exact GBIInstance/GBISegment entry that WOULD be needed (hash values, sizes, closest F3D variant) as the fix.
3. Verify the VI scanout chain end-to-end once: VI_ORIGIN cycles through 0x1DAA80/0x3DAA80 (framebuffers) — confirm those match the game's FRAMEBUFFER0/1 symbols in the decomp, and inspect a few framebuffer pixels in rdram after frames ran (all zeros = nothing ever drew, consistent with either verdict — note which).
GATE PASSED = a definitive, evidence-backed verdict (legit-black vs RT64-mismatch vs scanout-bug) + the concrete fix if it's not legit-black.
