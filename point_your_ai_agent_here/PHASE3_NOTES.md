# Phase 3 Notes — the boot park, root-caused

## Symptom (from PHASE2_NOTES.md frontier)

After full init (RT64 + audio + 69 threads), the game's threads park on a
message queue with ~0 CPU; one run instead spun hot. Timing-dependent.

## Root cause (confirmed on the real-renderer build)

`Thread_MainProc`'s frame loop (decomp boot.c) is:
submit gfx task via `osSpTaskStartGo` → audio → **`osRecvMesg(&gDisplayProcessorMessageQueue, NULL, 1)`**.
That queue (rdram `0x8012ABF0`, registered for `OS_EVENT_DP` at boot.c:258)
is posted ONLY by ultramodern's gfx thread: the `SpTaskAction` handler calls
`sp_complete()` then, after the renderer processes the display list,
`dp_complete()` (events.cpp:365/375).

gdb of the parked state shows the `[Game]` thread in
`osRecvMesg(mq_=-2146259984 = 0x8012ABF0)` — the DP wait — and the Gfx
Thread blocked in `pthread_cond_timedwait` **inside libnvidia-glcore**: the
Vulkan present stalled because the screen was locked/off (Wayland
compositors starve occluded surfaces). No present → `send_dl`/present path
never returns → `dp_complete` never posts → the game waits forever. With
the display awake, the same binary runs the frame loop continuously
(~2 cores, window "Mischief Makers: Recompiled" opens; framebuffer still
black — see Next).

The park-vs-spin nondeterminism: parked = screen off (present stalls
immediately); hot = screen on (frame loop actually pumping). The recompiler
turns branch-to-self into cooperative `pause_self`, so parks are silent
zero-CPU states.

No code fix required for the park itself — it is present-backpressure by
design. A robustness TODO remains: decide behavior when the window is
occluded (frame-skip / fake dp_complete / pause the game clock), otherwise
the game "freezes" whenever the window is hidden.

Credit: this was raced by three GLM agents (prompts/phase3_race_*.md) in
worktrees; all three died mid-flight on OpenRouter credit exhaustion, but
their partial forensics (queue map incl. 0x8012ABF0 = gDisplayProcessor
MessageQueue; dp_complete only in gfx_thread_func; pause_self semantics)
pointed exactly here and were verified by the driver on the live process.

## Next milestone: pixels

The window renders black while the game simulates. Suspects, in order:
1. VI scanout wiring — the game runs its own framebuffer swap
   (`osViSwapBuffer` → ultramodern VI regs → RT64 scanout); verify
   VI_ORIGIN/WIDTH values RT64 sees match the game's framebuffers.
2. RT64 vs gspFast3D — this game uses original Fast3D (not F3DEX; see
   PHASE2_NOTES_w2.md). Verify RT64's ucode detection handles the F3D
   variant the game loads (`loadUCodeGBI` auto-detect) and that the
   display lists aren't being dropped.
3. Legitimate black — early boot may render black frames until assets
   stream in (Trouble RLE streaming quirks are a known Phase 3 item);
   let it run longer / add a processed-display-list counter in send_dl.
