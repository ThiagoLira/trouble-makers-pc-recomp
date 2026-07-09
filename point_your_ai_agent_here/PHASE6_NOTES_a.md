# Phase 6 Notes (lane a) — find the horizontal-width levers in the game

## TL;DR

The game bounds horizontal drawing with a **per-frame view cull rectangle** in
Q16.16 fixed-point, recomputed from the live camera position and read by every
actor/sprite draw function as a straight draw-cull test. The lever is the four
`FixedCoord` outputs `D_800BE568/56C/570/574` (left/right/top/bottom bounds),
*not* a baked `SCREEN_WIDTH` and *not* a hard scissor (the game draws past
x=320 — it just stops *updating* past the cull edge, which is why off-stage
sprites freeze in the wings instead of disappearing).

`MM_WIDE=1` patch delivered: a detached ~1ms host thread (mirroring the Phase-5
AI_LEN mirror pattern) recomputes the horizontal cull bounds from the live
`gScreenPosCurrentX` with a widened half-extent (default 256px vs the game's
0x90=144px). Headless-verified crash-free: `send_dl` climbs to #1490 in ~24s
(~62fps, identical to the bug#11-fixed baseline), zero `Failed to find
function`/assert/abort.

GATE: PASSED.

## The width lever — variable map

All addresses are N64 vram (KSEG0). rdram offset = `vaddr - 0x80000000`.
Types from decomp `include/common_structs.h` + `include/globalData.h`:
`FixedCoord` is a Q16.16 union (`s32 raw; struct { s16 whole; s16 frac; }`),
`.whole` = integer pixels (bits 16..31 on the host), `.frac` = sub-pixel.

| vaddr | rdram off | symbol | type | meaning | evidence |
|---|---|---|---|---|---|
| 0x800BE558 | 0xBE558 | `gScreenPosCurrentX` | FixedCoord | current camera X (center of view) | globalData.h:107 "current x-position of camera in stage" |
| 0x800BE55C | 0xBE55C | `gScreenPosCurrentY` | FixedCoord | current camera Y | globalData.h:108 |
| **0x800BE568** | 0xBE568 | `D_800BE568` | FixedCoord | **LEFT cull bound** = camX.whole − 0x90 | src/438E0.c:290 `D_800BE568.whole = gScreenPosCurrentX.whole - 0x90`; Camera_UpdateViewBounds.s |
| **0x800BE56C** | 0xBE56C | `D_800BE56C` | FixedCoord | **RIGHT cull bound** = camX.whole + 0x90 | Camera_UpdateViewBounds.s `addiu $a0,$v0,0x90; sh $a0,D_800BE56C` |
| 0x800BE570 | 0xBE570 | `D_800BE570` | FixedCoord | TOP cull bound = camY.whole − 0x70 | Camera_UpdateViewBounds.s |
| 0x800BE574 | 0xBE574 | `D_800BE574` | FixedCoord | BOTTOM cull bound = camY.whole + 0x70 | Camera_UpdateViewBounds.s |
| 0x800462F0 | — | `Camera_UpdateViewBounds` | func | per-frame cull-rect writer (±0x90 X / ±0x70 Y) | asm/nonmatchings/438E0/Camera_UpdateViewBounds.s |
| 0x800463C0 | — | `func_800463C0` | func | calls Camera_UpdateViewBounds; then re-sets LEFT = camX−0x90 | src/438E0.c:288-291 (matched C) |

### Why these are the lever (not SCREEN_WIDTH, not a scissor)

1. **The cull test is a draw skip.** Overlay actor-draw functions read the
   bounds and branch around the draw. Example, `func_8019B314` (overlay slot 1):
   ```asm
   lh  $t7, D_800BE568      # left bound (.whole, sign-extended)
   slt $at, $a0, $t7        # at = (actorX < leftBound)
   beqz $at, .L...          # actor left of bound -> skip draw (culled)
   ...
   lh  $t1, D_800BE56C      # right bound
   slt $at, $t1, $a0        # at = (rightBound < actorX)
   beqz $at, .L...          # actor right of bound -> skip draw (culled)
   ```
   So an actor whose X is outside `[left, right]` is **not drawn**. Widen the
   rectangle and previously-culled off-stage actors get drawn — live, every
   frame — into the wings. That is exactly the requested effect ("trees now
   draw live in the wings during the intro pan").

2. **The bounds track the camera, so they're the view rectangle, not a
   stage-edge camera clamp.** `Camera_UpdateViewBounds` sets them =
   `gScreenPosCurrentX ± 0x90` every frame (and `func_800463C0` re-asserts
   LEFT = camX−0x90 right after). A stage-edge clamp would be scene-constant;
   a per-frame, camera-relative rectangle is a draw-cull region. (Some layer
   setup funcs — `func_800255B4`, `func_80023C18` — also write these with
   scene-specific values at scene load, but the per-frame camera update
   overwrites them before draw, so during gameplay/intro the bounds are
   camera-relative.)

3. **No hard scissor at 320.** The mission's own evidence — frozen stale
   sprites in the wings — proves the game *does* emit draw geometry past
   x=320 (a sprite straddling the edge is drawn into the wing once), it just
   stops redrawing once the sprite is fully past the cull edge. There is no
   `G_SETSCISSOR` clipping at 320; the cull rectangle is the gate. Confirmed:
   no scissor command found in the layer draw loops (`func_80082CFC/E04/F10`);
   they iterate a tile list bounded by a runtime count, not a pixel scissor.

4. **The half-extent 0x90 (144) is baked as an `addiu` immediate** in
   `Camera_UpdateViewBounds`/`func_800463C0` (and 0x70 for Y), so — per the
   mission's "SCREEN_WIDTH is baked" warning — the lever is *not* editing that
   immediate; it is the **output variables** `D_800BE568/56C/570/574`, which
   are plain rdram words the runtime can rewrite. The game refreshes them
   every frame, so the rewrite must recur (hence the thread, not a one-shot
   poke).

### Secondary lever (background/midground/env tile layers — noted, not poked)

The three layer draw loops `func_80082CFC` (Midground) / `func_80082E04`
(EnvLayer) / `func_80082F10` (Background) — gated by the `gDrawMidground`/
`gDrawEnvLayer`/`gDrawBackground` flags at 0x800BE6E4/E8/EC — iterate a tile
list at `D_80137614` with a **runtime** count/end-pointer stored in
`D_8013769C` (0x8013769C, set per-scene by `func_800255B4` from an 11-way
switch). So the *number* of background tiles drawn is runtime-modifiable, but
the tile *content* array (`D_80137614` … `D_8013769C`) only holds the visible
set the game built — drawing past it reads uninitialized tiles. Widening the
cull bounds (the primary lever) lets actor/sprite scenery fill the wings; the
*background tile* layer would need its own visible-tile rebuild to fill, which
is riskier and out of scope for this lane. Flagged for lane b / follow-up.

## The MM_WIDE=1 experiment patch

`src/game/register_overlays.cpp`, in `on_game_init` (the
`on_init_callback`, which receives `rdram`), right after the AI_LEN mirror
thread. Env-gated, mirrors that thread's teardown safety (`exited` is set by
`ultramodern::quit()` and rdram is `munmap`'d only after the event threads
join, so the thread never outlives rdram).

What it does, every ~1ms while `!exited`:
- read `gScreenPosCurrentX.whole` (the live camera center, integer px);
- write `D_800BE568.whole = camX − MM_WIDE_HALF` (LEFT, widened);
- write `D_800BE56C.whole = camX + MM_WIDE_HALF` (RIGHT, widened);
- optionally (if `MM_WIDE_Y_HALF>0`) do the same for TOP/BOTTOM from
  `gScreenPosCurrentY`.

FixedCoord write detail: only `.whole` (bits 16..31) is rewritten; the low
16 (`.frac`) are preserved (`*p = (*p & 0xFFFF) | (whole<<16)`). The cull test
reads `.whole` via a sign-extended halfword load, so the frac is irrelevant.
`MEM_W` is a plain aligned host word load (no swizzle), so a host s32 store is
read back verbatim by the recompiled `lw`/`sh`. `volatile` so the store issues
every tick under -O2.

At 60 fps (16 ms/frame) several 1 ms pokes land inside each frame's draw
window, so the widened bound reaches the cull test on the vast majority of
frames. Not perfectly synchronized (racy vs the game's per-frame bound
rewrite) but sufficient for a visual experiment — and the only insertion
point that doesn't require a runtime code patch.

### Env vars

| var | default | meaning |
|---|---|---|
| `MM_WIDE` | unset | presence enables the experiment |
| `MM_WIDE_HALF` | `0x100` (256) | horizontal half-extent in px. Game default 0x90=144; screen half is 0xA0=160, so >160 fills the 4:3 edge strip + wings. Clamped to ≥0xA0. |
| `MM_WIDE_Y_HALF` | `0` (off) | vertical half-extent; 0 leaves Y to the game |

### Headless verification (no crash)

```
cmake -B build -DMM_BUILD_GRAPHICS=OFF && cmake --build build --target mm_game -j8
cp ~/repos/trouble-makers-ai-recomp/baserom.us1.z64 ./rom.z64
MM_WIDE=1 MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout -k 5 25 ./build/src/game/mm_game ./rom.z64
```
Result:
```
[mm_wide] on: half=256px (game 0x90=144), y_half=0px
[gfx] send_dl #1490          # steadily climbing
[mm_rsp] aspMain 1480/1480 Broke
```
- `[mm_wide]` banner fires → experiment thread started.
- `send_dl` reaches #1490 in ~24 s (~62 fps) — equal to the bug#11-fixed
  baseline; **no regression**.
- Zero `Failed to find function` / `assert` / `Abort`. Exit 124 is the
  `timeout` kill, not a game crash (a crash would exit 134 and leave a marker).
- Confirms writing the cull-bound rdram words every 1 ms is safe: the offsets
  are committed `.data`, aligned, and the values are plausible FixedCoord
  integers.

## Driver visual verification (this is the real gate — headless can't see pixels)

Build the real renderer and run widescreen with the experiment on, then
toggle it to compare:

```
cmake -B build_gfx -DCMAKE_BUILD_TYPE=Release   # MM_BUILD_GRAPHICS=ON (default)
cmake --build build_gfx --target mm_game -j8

# Baseline (wings stale, as today):
./build_gfx/src/game/mm_game rom.z64 --widescreen --window 1920x1080

# Experiment (wings should fill live):
MM_WIDE=1 MM_WIDE_HALF=256 \
  ./build_gfx/src/game/mm_game rom.z64 --widescreen --window 1920x1080
```

What to look for (intro pan is the clearest test — camera pans horizontally
past trees/scenery):

1. **Wings go live, not frozen.** In baseline `--widescreen`, scenery that
   crosses the 4:3 edge freezes in the side wings (stale). With `MM_WIDE=1`,
   that scenery should **continue to animate/pan** in the wings — trees slide
   live instead of sticking. This is the primary success signal: the cull no
   longer kills off-stage sprites, so they redraw every frame.
2. **Sweep the half-extent.** Try `MM_WIDE_HALF=160` (just past screen half —
   should fix the ~16 px edge over-cull and give a sliver of wing), then
   `256`, then `320`/`426` (16:9 half-width). The live wing area should grow
   with the value. If wings stay frozen at all values, the cull isn't the
   gate for this scene — re-examine (see "If it doesn't visual-verify").
3. **No new glitches in the 4:3 center.** The center region should be
   identical to baseline (we only widened, symmetric about the same camera
   center). If center actors vanish or jitter, the 1 ms thread is racing the
   game's bound rewrite badly — try `MM_WIDE_HALF=160` first (smallest safe
   widen) and/or a faster poke.
4. **Toggle at runtime** by killing and relaunching with/without `MM_WIDE`
   (the env is read once at `on_game_init`). Compare the same intro frame.

### If it doesn't visual-verify (what to check next)

- **Background tiles still gap in wings:** expected for the *tile* layers
  (Background/Midground/Env). The primary lever fixes *actor/sprite* scenery
  (trees, props, characters). If the wing gap is the tile background, the
  secondary lever (`D_8013769C` tile count + the `D_80137614` tile-list
  rebuild in `func_800255B4`) is the next target — see lane b / follow-up.
- **Sprites draw but at wrong X:** the draw position is `worldX − camX`; if
  an off-stage sprite is within the widened bound but the game also has a
  separate position clamp, it may render clipped. Check `Actor_IsOutsideRegion`
  / the `gPlatforms*` arrays (0x8011D3B0 region) for a second cull.
- **Certain scenes unaffected:** layer-setup funcs (`func_800255B4`,
  `func_80023C18`) rewrite the bounds with scene-specific values that may
  not be camera-relative in e.g. menus/codec screens. The thread overrides
  them with camX-relative values — fine for gameplay, may over-draw in menus.
  Gate by `gGameState` (0x800BE5xx) to gameplay-only if needed.

## What changed (files)

1. `src/game/register_overlays.cpp` — the `MM_WIDE` experiment: `<cstdlib>`
   include + a detached ~1ms thread in `on_game_init` (env-gated) that
   rewrites `D_800BE568/56C` (and optionally `570/574`) `.whole` from the live
   `gScreenPosCurrentX/Y` with a widened half-extent. No runtime
   (lib/N64ModernRuntime) changes — `register_overlays.cpp` is in `src/game/`,
   owned by this worktree, so no privatization was needed.

No changes to `RecompiledFuncs/`, `input/`, `reference/`, the toml, or the
runtime. Nothing committed (per the lane rules).

## What remains (for the driver / lane b)

- **Pixels.** Headless proves the cull-bound writes are crash-free and the
  frame loop is unaffected. Whether widening actually fills the wings live is
  a visual check only the driver can do (above).
- **Synchronization.** The 1 ms thread is racy vs the game's per-frame bound
  rewrite. If the visual result flickers, a proper fix hooks the bound
  *after* `func_800463C0` returns (a runtime patch on the call site, or a
  post-camera-update host hook) rather than a free-running thread.
- **Background tile fill** (secondary lever `D_8013769C` + tile-list rebuild)
  for the non-actor layers.
- **Per-scene gating** so menus/codec screens keep their scene-specific bounds.
