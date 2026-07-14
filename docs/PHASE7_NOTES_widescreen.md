# Phase 7 — Real widescreen (branch `real-widescreen`)

The longest hunt in the project so far. This note exists so nobody —
human or agent — ever re-derives (or re-falls-into) any of it. Ships at
`f1b0a85` on branch `real-widescreen`; main stays clean 4:3 by default.

**Ground rule (user verdict, permanent): cosmetic fills are REJECTED.**
Mirror wings, edge stretch, blur fills — all built, all demonstrated, all
deleted at the user's demand (`f11e871` on main reverts every trace). Only
two acceptable states exist: genuine game content to the window edges, or
clean 4:3. Do not propose cosmetic fills again.

## 0. Research phase — how everyone else does 2D widescreen

Surveyed Castlevania/SotN RecompOne, Sonic 3 A.I.R., SMW widescreen hack,
and others. Verdict: **nobody generates wing content automatically.**
S3AIR = ~49 hand-authored fixes across 90 files over years; the SMW hack =
months of elite ASM work plus helpers; SotN's widescreen code isn't public.
There is no trick to steal — real 2D widescreen is per-layer engineering.
One free win: this game's intro cinematics are ortho-triangle scenes that
RT64 already widens natively.

## 1. The rendering architecture (verified against the community decomp)

Community decomp: github.com/Drahsid/mischief-makers — **same US 1.1
vaddrs**, trustworthy ground truth alongside `../trouble-makers-ai-recomp`.

Three tile layers flow through the shared static draw `func_80082380`
(exactly three callers, all 7×10 grids — see §4 for why we once believed
otherwise):

| Layer | dst buffer | wrapper | notes |
|---|---|---|---|
| midground | `D_80180930` | `func_80082CFC` | map `0x80108DE8` direct; addend `*0x80137474`; masks `*0x800BE64C/650/654`; storage vertically REVERSED; gated `md == *0x800BE588 == 0`; FP parallax modes 1–3 never derived |
| env | `D_80180B60` | `func_80082E04` | map `*0x8013746C`, addend `0x80137476`; variant A (`*0x800BE58C==1`): 128-wide (mask 0x7F), rowblk `((t0i+r*32)<<2)&0x380`, base `(*0x800BE578-2)&0xFFF` |
| static-bg | `D_80180D90` | `func_80082F10` | also the sole caller of the scrolling path `func_80082820` (when `D_800BE6FC != 0`) |

Each dst = 140 s32 slots (7×20 — headroom already in the game!). Filled per
frame by `func_8001107C` from wrapping tilemaps. tile→texptr:
`(id<<10) + *0x80180FC0` (= `0x80380600`).

Scrolling backdrop map: 16×16 bytes, wraps both axes
(`col=(srcX>>5)&0xF`, `rowblk=(srcY>>1)&0xF0`); map ptr `*0x80137470`,
addend `*0x80137478`; per-row parallax s16 `D_8011D3B0[8][2]`;
`sy0 = (-0xC - *0x800BE584) & 0x1FF`.

The horizontal draw extent is gated by the per-frame **view cull rect**
`D_800BE568/56C/570/574` — not SCREEN_WIDTH, not scissor (see
`phase6-widescreen-cull-lever` memory / PHASE6 notes).

## 2. THE ROOT CAUSE — walking pointers (read this before touching the draws)

**Both** draw loops read their tile buffer via a *walking pointer*:
`lw $t1, 0(\$s0)` / `addiu \$s0, \$s0, 4` every iteration. The column
counter only drives screen X — it is NOT an index into the buffer.

- `func_80082380` (static): buffer arg in `$a3` = `ctx->r7`
- `func_80082820` (scrolling): buffer arg in `$a1` = `ctx->r5`

Consequence: when a hook repacks into scratch and repoints the arg, the
repoint must be the scratch **base** (`ctx->rN = s`). The parked branch's
"+20 off-by-five" repoint (`s + 20`, imagining the widened loop starts at
column −5 of an indexed buffer) **shifted every layer by 5 tiles and pulled
5 junk slots into view**. That single wrong assumption was the source of
ALL historical garbage: the Attempt-2 sprite flicker, the rainbow noise,
and the DO⁶⁴/N64-boot-logo tiles that survived three separate exorcisms
(env-formula gating, overlay-caller census, framebuffer clears, canvas
zeroing — each falsified in turn before the real cause surfaced).

## 3. What shipped (`f1b0a85`), all in `troublemakers.us1.toml` + `patches/rt64/`

1. **Cull-rect widening** (unchanged from Phase 6): `Camera_UpdateViewBounds`
   `0x80046370→0x2443FEE0`, `0x80046398→0x24440120`; `func_800463C0`
   `0x800463E0→0x25CFFEE0`.
2. **Both draw loops widened 10→20 columns** (start −5, stop 15, row stride
   doubled, left-edge skip threshold 0x20→0xC0, `max(0,x)` clamp dropped so
   negative-FB-X tiles land in the left wing). Exact instruction words are
   commented per-line in the toml.
3. **Catch-all scratch repack hooks** (entry hooks on both funcs): every
   buffer reaching the draw is rebuilt at stride 20 in scratch
   (0x9FFFC000–0x9FFFF000 region), center 10 columns copied verbatim from
   the game's own fill (byte-identical center), wings computed for known
   layers / zeroed for anything else, then `ctx->r7 = s` / `ctx->r5 = s`
   (BASE — see §2). No caller can ever misread stride-10 data as stride-20.
4. **Real-content wings, opt-in per layer** (env vars):
   - `MM_BAND_WINGS=1` — scrolling backdrop from the wrapping 16×16 map
     (the showcase: gorgeous in ground scenes)
   - `MM_ENV_WINGS=1` — env layer variant-A formula (clean in scene 0x16)
   - `MM_MID_WINGS=1` — midground (known-bad: FP parallax modes underived)
   Default (all off) = center-copy widening — verified clean everywhere
   including the flying-high attract camera that breaks band/env formulas.
5. **RT64 wing clear made robust** (`patches/rt64/0001`, regenerated as the
   full combined lib/rt64 diff): fbPair 0 is rendered by **two** submission
   paths — the workload queue AND a mid-frame path in `rt64_state.cpp`
   (~line 1251) that hard-coded `clearWings = false`. Both now honor it;
   wing rects derive from `contentWidth = fbWidth * resolutionScale.y` vs
   `colorTarget->width` (the aspect param is zeroed in the state path — do
   not trust it); cleared once per `submissionFrame` per target via a
   `wingClearFrame` map so later passes don't erase wing content.
6. **Debug warp** (`MM_WARP=<scene>` or exact-row `MM_WARP_STAGE=<index>` on
   the `func_80001670` dispatcher hook;
   `MM_WARP_AT`/`MM_WARP_DELAY` optional): resolves the requested scene through
   `gStageScenes`, selects the corresponding progression row (`gCurrentStage`,
   stage id, and unlock index), sets `gCurrentScene 0x800BE5D0`,
   `gSkipStageIntro 0x800D2908=1`, `gGameState 0x800BE4F0=LOADING(5)`, and
   **zeroes the canvas RAM at 0x8027CEE8/0x8027EEE8** that normal boot
   would have initialized (skip that and you get warp-only stale-canvas
   artifacts). Selecting only `gCurrentScene` is invalid: it silently reuses
   the previous row's map and overlay assets. Scene ids in decomp
   `include/Scene.h`: 0=MEETMARINA intro, 0x16=TRAPPED metal level,
   0x25=3D dungeon.
7. **Window targeting** (`src/game/main.cpp`): `MM_DISPLAY=<n>` →
   `SDL_WINDOWPOS_CENTERED_DISPLAY`, `MM_WIN_POS="x,y"`. Wayland/KWin
   ignores plain positioning — fullscreen-on-target-display is the only
   reliable placement.

Run recipe: `MM_WARP=22 MM_DISPLAY=1 ./build/src/game/mm_game <rom>
--fullscreen --widescreen` (`MM_WARP`'s current debug parser expects decimal).

### Gameplay-only widescreen and final 52-level suite

Widescreen presentation is now stateful rather than a process-lifetime flag.
`mm_widescreen_sync_mode` switches RT64 to Expand only when all of these are
true: game state is gameplay, `gStageTime` is actively advancing, and the game
permits pausing. Cinema/dialogue freezes the timer and/or asserts
`gCannotPause`, returning the renderer and every tile-wing hook to centered
4:3. Entry to 4:3 is fast; return to widescreen requires 30 stable frames so
one-frame stage-script pulses cannot thrash renderer configuration.

Scenes 36, 57, and 71 are fixed-canvas playable compositions and are always
4:3. This is deliberate: widening their renderer reveals compositing buffers,
not additional authored world content.

`tools/test_widescreen_playable.sh` enumerates the 52 player-controlled rows in
`gStageScenes` (excluding opening, demo, attract, ending, and extra rows), uses
`MM_WARP_STAGE` to avoid duplicate-scene ambiguity, and injects L/Z only under
the test-only `MM_TEST_AUTO_ADVANCE=1` flag. It waits for stable gameplay before
capturing. The final run completed **52 pass / 0 fail**; evidence is in
`screenshots/widescreen-playable-suite.png` and
`screenshots/widescreen-cutscenes-4x3.png`.

### 2026-07-11 completion pass

The remaining layer work is now integrated into `--widescreen` by default:

- Band, environment, and midground wings are enabled together; the midground
  modes use the original horizontal/parallax formulas rather than guessed
  offsets.
- Static backgrounds fill their side columns from the same wrapping 16×16 map
  used by the original ten-column buffer.
- Vertical `Gfx_DrawBorderRect` strips are suppressed only while widescreen is
  active; the original top/bottom letterbox behavior is retained.
- RT64 expands full-frame rectangle scissors and sign-extends negative 12-bit
  texture-rectangle X coordinates only on the widescreen wing path. This lets
  genuine left-side tiles render instead of being decoded as large positive X.

The production host automatically enables the layer hooks; the `MM_*_WINGS`
variables remain useful only for translated-code diagnostics. A successful
1600×900 live run is captured in `screenshots/widescreen-scene-22.png`.

The corrected warp harness was used to sweep scenes 0, 10, 13, 20, 22, 23, 24,
36, 37, 40, 42, 46, 47, 48, 52, 53, 55, 57, 65, 68, 69, and 77. Isolating each
layer showed that several scene layouts send different data through the shared
buffers or contain off-frame tile ids whose texture slots are never loaded.
Those layers initially got per-scene blacklists (band/static: 21 scenes; env:
6; mid: 5) that fell back to black wings.

### Second pass (same day) — blacklists replaced by ground truth

Decomp research (community repo: `func_8001107C`, `func_800255B4`,
`func_80026220`) established that **every scene fills every layer with the
same generic wrapping-map formulas** — per-scene differences are entirely
captured by readable globals (mode selectors `*0x800BE588/58C/6FC`, masks
`*0x800BE64C/650/654`, scroll registers, map pointers, addends). The
garbage that motivated the blacklists was **texture residency**: wing
columns can reference tile ids whose textures lie past the RLE-decompressed
end of the map bank, recorded at `*0x80137728` by `func_80026220`. That was
the forest sky noise.

The layer hooks now live in `src/game/widescreen.cpp` (thin toml hooks call
`mm_ws_band_repack` / `mm_ws_static_repack`): exact per-variant fill
formulas for all parallax modes (mid modes 0–3 incl. the /1.55 factors, env
16×16 vs 128×8, band per-row vs single-scroll) and a per-tile residency
check `pool + ((tile+addend)<<10) + 0x400 <= bankEnd` replacing every scene
blacklist. Two companion pieces:

- `func_80026428` (scenes 10, 24, 46–51) reloads the bank without updating
  the recorded extent; a hook at vram 0x80026484 captures
  `Trouble_RLE_Type1`'s returned size. The corrected bound is kept
  **host-side only** (`g_host_bank_end`, cleared on `func_80026220`) — do
  NOT write 0x80137724/28 from the host.

### THE display-list arena overflow (root cause of the intermittent noise)

Extending the wings surfaced a hard capacity bug: each layer's DL output
arena (six of them, double-buffered mid/env/band: D_80178470, D_80179A90,
D_8017B0B0, D_8017C6D0, D_8017DCF0, D_8017F310) is 0x1620 bytes = exactly
70 tiles × 80 bytes + G_ENDDL + 0x38 slack. The widened 7×20 loops emit up
to 140 tiles (0x2BC8), and the draw loop has **no bounds check**. Overrun
from the last arena (D_8017F310) stomps the s32 layer grids at 0x80180930+
and can reach the texture-pool base ptr at 0x80180FC0 → intermittent
full-frame tile noise (center included, since the center columns are copied
from the stomped grids). This was ALSO the real reason several scenes ended
up on the original blacklists — heavy scenes overflowed, light scenes
didn't.

Fix: the arena base is parameter-borne ($a0); the draw funcs keep the
cursor in a register and emit both the G_ENDDL and the gSPDisplayList
submission from that same base. `remap_arena()` in the two entry hooks
substitutes six 0x4000 arenas at 0x80400000..0x80418000 — real low RDRAM
the 4MB game never touches (runtime extended allocations start at
0x80800000), so RT64's display-list walker reads them without issues.
With capacity fixed, wing tiles keep the game's exact tile-0 semantics
(map byte 0 draws pool slot addend<<10 — skipping it leaves holes).

### Entity pop-in (same pass)

The view-cull rect is only one of the game's visibility systems. Three more
windows used 4:3-sized X margins, so objects appeared/vanished inside the
16:9 wings (~±214 px needed). All X-only, Y untouched, instruction words
verified against the decomp asm:

- `func_80016D94` (per-frame despawner): kills `ACTOR_FLAG_ONSCREEN_ONLY`
  actors (projectiles, particles, drops) outside camX ±0xD0 → ±0x150
  (48 px slack over the ±0x120 draw cull; pure despawn, no spawn
  hysteresis to break).
- `func_800451E4` (gem/prop sprite windowing): items only get one of 64
  sprite slots inside camX ±0xA8 → ±0xE8. Rebuilt statelessly every frame,
  risk-free; the wide-mode window (`D_800BE710`, ±0x100) already covered
  16:9 and is untouched. Watch the 64-slot cap in gem-dense rooms.
- `Actor_IsOutsideRegion` (shared `|x| > 0x90+len`, 19 call sites:
  positional SFX radius + behavior gates): 0x90 → 0xD8.

Remaining long tail (not addressed): ~69 per-actor-type asm sites compare
camera-relative `posX.whole` against private constants (±0x70…±0x100);
densest in `overlay_0/6EE580`, `overlay_3/7C0B20`, `overlay_2/766110`.
Patch individually if a specific actor type still pops.

Older evidence sheets (`widescreen-forest-fix.png`,
`widescreen-regression-scenes.png`, `widescreen-coverage-scenes.png`)
predate the second pass.

## 4. Falsified conclusions — do not re-believe these

- ~~"func_80082380 serves callers whose buffers are not 7×10 grids; the
  catch-all repack corrupts them"~~ (Attempt 2 / commit `6a54fcf`) —
  **WRONG.** The flicker was the +20 repoint bug (§2). Community decomp
  proves exactly three callers, all 7×10. The catch-all repack is safe.
- ~~"The DO⁶⁴-logo garbage is stale framebuffer / uncleaned wings"~~ —
  the wing-clear gap was real and fixed (§3.5) but was NOT the cause.
- ~~"…is the warp skipping canvas init"~~ — also real, also fixed (§3.6),
  also not the cause. All roads led back to §2.

## 5. Gotchas that burned hours

- **display.cfg persistence**: every run writes
  `~/.config/troublemakers-recomp/display.cfg`; a `--widescreen` test run
  leaves `widescreen=1` sticky for flagless runs and contaminates 4:3
  baselines. Reset it before baseline comparisons.
- **Attract-timing bisects**: the attract demo cycles scenes, so
  same-timestamp screenshots across runs can be different camera moments.
  Compare matching moments, not matching timestamps.
- Hook C has no stdio — `extern int dprintf(int, const char*, ...)`.
  `MEM_B/BU` carry the ^3 big-endian XOR, `MEM_H/HU` carry ^2; `MEM_W` is
  the canonical aligned-word accessor.
- hlslpp vector members passed to varargs = UB garbage; cast to `float()`.
- The game draws its own letterbox every frame (`Gfx_DrawLetterbox`
  0x800218FC / `Gfx_DrawBorderRect` 0x80021690, visible area x 14–306,
  y 20–212) — that's the thin seam at the 4:3 edges in widescreen.

## 6. Original remaining tail (completed by the 2026-07-11 pass)

1. Band/environment formulas derived and enabled by the host.
2. Midground parallax modes 0–3 derived from the original assembly.
3. `Gfx_DrawBorderRect` side bars suppressed in widescreen only.
4. Temporary tracing stripped and the user-facing README updated. The
   intentionally gated debug warp remains useful for stage coverage runs.
5. User playtest verdict remains the final coverage check across every stage.
