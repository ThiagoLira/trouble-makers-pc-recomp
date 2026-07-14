# Widescreen terrain-pop investigation

Status: resolved and regression-tested on 2026-07-13, branch
`streaming-pop-fix`.

This note records the complete investigation of terrain, actor, and
fixed-viewport-effect pop in expanded gameplay. It intentionally includes the
false leads: the visible scene is a composite of tile layers and actor-backed
landscape panels, so several individually correct A/B results initially
pointed at the wrong subsystem.

## Final result

The reported problem was not one bug. Four independent 4:3 assumptions were
visible while Marina moved through a 16:9 viewport:

1. Several actor loaders and draw-list builders still rejected entities near
   the old screen edge. Their horizontal windows now cover the expanded view
   with cleanup hysteresis beyond it.
2. Widescreen tile columns were sometimes calculated from scroll registers
   newer than the already-filled center grid. All three grids now use a
   snapshot taken when `func_8001107C` fills the center.
3. The early forest floor includes actor-backed 512px repeating landscape
   panels. The original controller teleports its sole copy from -256 to +255,
   briefly exposing the background in a wide wing. The renderer now gets an
   adjacent visual copy while simulation and collision keep the original.
4. The game's 8x8 death/room-transition portrait grid is a fixed 320x240
   effect. Expanded gameplay now skips only its 64 visual entries and still
   draws the two HUD portrait entries.

The adjacent landscape copies initially fixed coverage but flickered. That
flicker had a separate, precise cause: copying all `0x198` bytes of a source
actor into a synthetic record overwrote the clone's two renderer-owned
64-byte matrices at offsets `0x00..0x7f`. A display list for the other
framebuffer could still reference the overwritten matrix. Copying only the
controller-owned region at `0x80..0x197` made both rollovers stable.

This is systemic, not a per-level coordinate hack. The render-copy signatures
describe the shared landscape controllers and the cull patches modify shared
engine paths.

## Symptom and reference recording

- On the first two playable forest levels, walk or jet right.
- Terrain on the right can be missing at load, then appear after the camera
  moves. NPCs and props can vanish before reaching the widescreen edge.
- The clearest user recording was
  `/home/thlira/Videos/Screencasts/Screencast From 2026-07-13 11-07-11.mp4`.
- The user identified the decisive sampled pair as frames 17 to 18 near the
  beginning, while Marina falls from the first house.

Sparse screenshots complicated the diagnosis. The source video is about
27.2fps and early extractions were resampled, so generated frame numbers did
not identify adjacent source frames. A separate lossless 60fps capture was
used wherever one-frame behavior mattered.

## Root causes and fixes

### 1. Actor and world-record windows

The widened camera bounds were necessary but not sufficient. Later shared
paths performed their own horizontal rejection:

- `func_8001DC60` / `func_8001DE30`: map-authored actor prefetch rings;
- `func_800451E4`: gem/prop world-record window;
- `Actor_IsOutsideRegion`: behavior and positional-effect range;
- `func_8000FBF4`: the final actor-list builder, after `ACTOR_FLAG_DRAW` was
  already set;
- `func_801A78DC_7670EC`: an independent state-1 terrain draw toggle.

The important trace was an actor that remained active and draw-enabled but
never reached any depth-sorted list consumed by `func_80009BE8`. Widening the
four `func_8000FBF4` paths fixed the house NPC disappearing before the wing
edge. The other windows now use `0x170..0x1c0` extents selected according to
their role, rather than one indiscriminate global bound.

The 128-record relocated clan/prop capacity from the preceding
`streaming-pop-fix` commit remains required; dense scenes exceed the original
64 records after widening.

### 2. Tile-layer fill/draw skew

`func_8001107C` fills the original 7x10 center grids early in a game tick.
Parallax controllers can update scroll registers before the later band,
static, midground, and environment draw hooks add the wing columns. Combining
the old center with a new origin creates a whole-tile discontinuity.

`LayerFillState` now snapshots the map pointer, origin, addend, masks, shifts,
and per-row band scroll at fill time. Every widened repack uses that snapshot.
The center remains byte-for-byte the game's output; only the additional wing
columns are derived.

### 3. Native 512px landscape wrap

Two shared actor signatures implement repeating scenery by teleporting their
only copy across a 512px period:

- type `0x181c`, state `2`: foreground trees/grass decoration, including a
  horizontal flip;
- type `0x000d`, state `0x50`: four 128px panels that make up the large forest
  landscape/floor strip.

The controller for the second signature computes approximately:

```text
screen_x = ((world_x - camera_x) & 0x1ff) - 0x100
```

That covers a 320px viewport but not 16:9. At the -256/+255 rollover, part of
the wide actor should still be visible in one wing. Hiding all four
type-`000d`/state-`0x50` panels removed the green foreground floor and exposed
the blue environment beneath it, proving ownership of the missing chunk.

`mm_ws_repeat_wrapped_terrain` runs after `func_8000FBF4` builds a renderer
list. It injects a synthetic copy shifted by +/-512 immediately before each
matching source. The synthetic indices live in expansion RDRAM and are never
seen by controllers or collision.

Only offsets `0x80..0x197` are refreshed from the source. Offsets
`0x00..0x7f` are the clone's two double-buffered matrices and must survive
until their framebuffer is no longer in flight. This matrix lifetime rule is
what removed the last intermittent flicker.

### 4. Fixed 8x8 transition grid

The black checker holes caught in stage 14 / scene 37 were not terrain and
not `Gfx_DrawLetterbox`. `func_8000EA88` draws 66 `PortraitStruct` records:

- entries `0..63`: an 8x8 fixed-screen death/room-transition wipe;
- entries `64..65`: the real HUD portraits.

During expanded gameplay the hook starts the draw loop at entry 64. The wipe
controller continues to advance, deaths and respawns complete normally, and
the HUD stays visible. Cinematics and original-mode scenes still draw all 66
entries. `MM_TEST_KEEP_FIXED_VIEWPORT_EFFECTS=1` restores the effect for A/B
diagnostics.

This complements the existing top-actor filter for type `0x2700` fixed
color-grading rectangles.

## Falsified explanations

- **The state-4 `gfx2010` actor is the missing forest floor.** Removing it did
  not eliminate the later pop. The experiment was reverted.
- **The state-2 `0x181c` actor is the entire floor.** It supplies foreground
  decoration, but the large floor strip is type `0x000d`/state `0x50`.
- **The environment tile layer alone owns the missing chunk.** Disabling its
  wings removes a large green field, which made this look conclusive, but the
  scene is composited. Hiding the four landscape panels exposed the actual
  blue hole at the rollover.
- **A larger global camera bound fixes everything.** An earlier `+/-0x1c0`
  camera-bound experiment broke loading. The final work keeps the stable
  camera cull and widens only the downstream paths that require it.
- **Injecting only the nearest landscape clone prevents flicker.** It produced
  6 blue frames out of 40, worse than all four clones. The render list was
  populated every tick; the failure was matrix lifetime, not overlap.
- **The stage-14 checker is `Gfx_DrawLetterbox`.** Suppressing its nonzero
  modes changed nothing. Rectangle tracing showed only ordinary borders. The
  64 portrait entries were the actual wipe.

## Regression suite

Build and set the ROM path:

```bash
ROM="$HOME/.config/troublemakers-recomp/troublemakers.n64.us.1.z64"
cmake --build build_codex -j"$(nproc)"
```

Run the focused terrain regression:

```bash
DISPLAY=:0 tools/test_terrain_pop.sh \
  build_codex/src/game/mm_game "$ROM" /tmp/mm-terrain-pop
```

The test performs three checks:

1. a dense moving traversal of progression stage 3 / scene 68;
2. trace proof that a native type-`0x181c` state-2 actor crossed its
   -256/+255 wrap and that adjacent landscape copies were submitted;
3. fixed-camera captures at scene 0 camera X=512 and scene 68 camera X=1024.

The rollover check captures 12 consecutive frames per scene by default and
requires the lower-right wing pixel to equal an adjacent covered pixel in
every frame. `MM_TERRAIN_ROLLOVER_FRAMES` raises that stress count.

Final evidence from `/tmp/mm-terrain-pop-final`:

```text
scene-00 frames=12 inner=srgb(41,93,41) wing=srgb(41,93,41)
scene-68 frames=12 inner=srgb(0,0,16) wing=srgb(0,0,16)
```

Separate fixed-camera stress runs captured 60 frames per scene. Scene 0 was
60/60 green and scene 68 was 60/60 covered. Before preserving the matrix
slots, the same scene-0 test intermittently produced blue frames (3/40 and
5/20 in two runs).

Run a moving multi-scene burst:

```bash
DISPLAY=:0 MM_CAPTURE_FRAMES=8 MM_CAPTURE_INTERVAL=0.15 \
MM_TEST_MOVE=jet-right MM_TEST_STREAM_TRACE=1 \
tools/test_widescreen_playable.sh \
  build_codex/src/game/mm_game "$ROM" /tmp/mm-wide-sweep \
  2 3 4 5 14 21 37 41 42 43 49 51
```

That representative sweep passed all 12 scenes. It includes both forest
levels, scene 13, filter-heavy scenes 37/41/42, geometry-heavy caves/lava,
and late-game stages. Contact sheets were inspected, not only exit codes.

After the portrait-grid change, a final 12-frame sweep of progression stages
2, 3, 14, 37, 41, and 42 passed 6/6. Its combined contact sheet is
`/tmp/mm-effects-final/overview.png`; stage 14 had no black center samples.

The stage-14 portrait fix was additionally exercised for 50 moving frames.
Several death/respawn cycles completed, no sampled center pixel was black,
and the HUD remained present.

## Harness improvements

- `tools/test_render_burst.sh` can wait for a camera X target, capture early,
  warm up, disable fast-forward, and drive `jet-right`, `jet-hop-right`,
  `jet-left`, or `jet-bounce`.
- Screenshots target the actual game window when possible, avoiding unrelated
  desktop pixels and repeated permission prompts.
- `capture-index.tsv` records the last movement trace beside each image.
- `tools/test_widescreen_playable.sh` supports the same camera/warmup controls
  and captures multiple frames per playable stage.
- `MM_TEST_ACTOR_TRACE=1`, `MM_TEST_STREAM_TRACE=1`, and
  `MM_TEST_TERRAIN_REPEAT_TRACE=1` provide coverage evidence without changing
  normal builds.

## Temporary evidence paths

These may disappear after reboot, but identify the reviewed artifacts:

- final terrain suite: `/tmp/mm-terrain-pop-final/`;
- fixed-camera matrix stress: `/tmp/mm-panel-flicker-matrix/` and
  `/tmp/mm-panel-flicker-matrix-stage3/`;
- 12-scene sweep: `/tmp/mm-widescreen-matrix-regression/overview.png`;
- stage-14 before: `/tmp/mm-stage14-filter-moving/numbered.png`;
- stage-14 after: `/tmp/mm-stage14-portrait-skip/numbered.png`;
- lossless forest route: `/tmp/mm-true60/route.mkv`;
- original layer A/B: `/tmp/mm-layer-off-{mid,env,band,static}/`.

The success condition is stronger than nonempty wings: a stationary rollover
must remain covered on consecutive displayed frames, moving actors must stay
in renderer lists through the expanded edge, and fixed 4:3 effects must not
mask only the center of a wide gameplay frame.
