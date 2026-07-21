# Phase 8 — Rotation-stage wall and 3D platform regression

Status: **resolved and regression-tested on 2026-07-21**. This note supersedes
the earlier clear/LOD experiments. None of the old renderer-wide texture rules
or rotation-stage framebuffer clears belong in the final implementation.

## User-visible failure

Two campaign entries share a rotating authored room in the Migen's Shrine
overlay:

| Progression stage | Runtime scene | Name |
|---:|---:|---|
| 13 | 69 | Vertigo |
| 21 | 13 | Seasick Climb |

The regression had three related presentations:

- moving sprites remained in old framebuffer positions, producing the
  “Windows Solitaire” trail;
- the screen-covering stone/portrait wall disappeared or became a flat color,
  so the playfield behind the actors looked empty;
- a moving 3D platform became a large neutral gray slab or showed stretched
  edge texels.

The symptoms were transient. Short spawn captures often looked correct, while
the bad platform appeared hundreds of motion frames later. A persisted
`widescreen=1` setting also made apparently identical command lines exercise
different presentation paths. All automated captures now use an isolated
config directory and sustained, non-jet `walk-right` input.

## Rendering path and exact ownership

The room is not a scrolling tile background. The scene rotates a 320x240
camera canvas containing ordinary 3D actors. Two actor families own the
affected display lists:

- actor type `0x0508`: thirteen screen-covering wall animation lists;
- actor type `0x0509`: seven textured moving-platform lists.

Both are drawn by `func_80009BE8`. The translated hook runs immediately before
the function appends `actor->dlist_17C`, at `0x8000D388`. The complete source
allowlist is:

| Family | Original display-list addresses | Bytes | Texture loads |
|---|---|---:|---:|
| wall A | `801B6F28`, `7020`, `7118`, `7210`, `7308`, `7400`, `74F8` | `0x0F8` | 2 |
| wall B | `801B75F0`, `7738`, `7880`, `79C8`, `7B10`, `7C58` | `0x148` | 3 |
| platform | `801B8808`, `8950`, `8A98`, `8BE0`, `8D28`, `8E70`, `8FB8` | `0x148` | 3 |

The abbreviated addresses in the table retain the common `0x801B` prefix.
Runtime traces confirmed all seven platform variants and both actor types;
the earlier wall-only fix missed `0x0509`, which is why the gray slab survived
after the wall first looked repaired.

## Root cause

The shared actor setup selects a two-cycle TRILERP combiner. These private
Fast3D lists then load 32x32 CI8 textures into render tile 0, but they have two
legacy assumptions that RT64 cannot infer safely:

1. mip tile 1 is never configured even though cycle one can blend TEXEL0 and
   TEXEL1 through `LOD_FRAC`;
2. the render tile declares `maskS=maskT=0` while the live vertex coordinates
   extend far outside the 0..31 texture range and visibly repeat on the
   original presentation.

The resulting undefined/clamped samples explain all three symptoms. A wall
panel can become transparent or flat, exposing persistent framebuffer content
and therefore sprite trails. The same state on actor `0x0509` stretches a
single platform edge texel across a large polygon, producing the gray slab.
The geometry was present; its material made it look absent.

This is source data with a known intended presentation, not a general RT64
rule. Changing RT64's global LOD fraction, mask-zero addressing, sampler, or
texgen behavior repaired this one family only by changing unrelated display
lists and caused regressions elsewhere.

## Final fix

`mm_fix_rotation_material()` creates corrected private copies in unused
expansion RDRAM (`0x80455000..0x80456DFF`). The expanded actor storage ends at
`0x80454CE8`; relocated frame arenas begin at `0x80460000`. Each of the twenty
allowlisted sources owns a `0x180`-byte slot, and compile-time bounds checks
keep the region and marker outside the copied command range.

For an exact source match, the copy does two things:

1. prepends `FCFFFE04 FF10F3FF`, a draw-local two-cycle combiner that passes
   TEXEL0 through cycle one and retains the setup's shade modulation in cycle
   two;
2. changes only matching CI8 tile-0 commands from
   `F5480800 00000000` to `F5480800 00014050`, explicitly declaring the
   32x32 wrap period (`maskS=maskT=5`).

The builder verifies the expected two or three tile commands and the original
EndDL at the exact source length before redirecting the actor. A source-layout
change fails closed and draws the original list. A per-slot marker avoids
recopying stable data, while combiner/EndDL checks and marker invalidation make
a damaged or failed rebuild safe. The correction lives inside its private
display list, so it does not consume the game's per-frame display-list arena.

The two scenes are also in `scene_requires_original()`. Even when the player
selects widescreen, they remain centered 4:3 because their wall actors and
camera composition author exactly a 320x240 rotating canvas. Ordinary stages
still expand. This is an aspect-policy boundary, not a stretch or blur fill.

With the opaque wall material restored, the wall naturally overwrites prior
pixels. The final code contains no scene-colored full-frame clear, entry seed
clear, `mm_widescreen_force_frame_clear`, or
`mm_widescreen_rotation_wall_wrap`. Generic RT64 wing clearing remains scoped
to genuinely expanded scenes, and its per-render-target bookkeeping is reset
each submission so render-target recreation cannot grow the map over time.

## Why this is intentionally narrow

The compatibility correction requires all three semantic identities:

1. actor type is exactly `0x0508` or `0x0509`;
2. `dlist_17C` is exactly one of twenty verified overlay sources, or its
   corresponding private copy;
3. the list contains the expected command count and EndDL.

There is no stage-name check inside the material code, no texture-address
heuristic, no global shader behavior change, and no arbitrary clear color.
This prevents the repaired material from changing other 3D platforms, bosses,
sprites, or stages.

## Removed experiments and what they taught us

- **Per-frame burgundy clear:** hid trails but also hid/raced the real wall,
  producing missing geometry and color flicker.
- **Entry-only seed clear:** left trails as soon as any wall panel resolved
  transparent; it treated feedback as the cause instead of a symptom.
- **Global `LOD_FRAC=0`:** made the wall less random but changed every
  TRILERP material using tile LOD mode.
- **Global mask-zero wrap override:** made some wall samples repeat but changed
  normal mask-zero clamp semantics throughout RT64.
- **Forced texgen:** removed or flattened unrelated geometry. The lists carry
  live, CPU-updated texture coordinates and do not need host-forced texgen.
- **Wall actor only:** repaired actor `0x0508` but missed the gray
  `0x0509` platform. Actor traces identified the missing family.
- **Expanded projection for these rooms:** exposes space for which the overlay
  never spawns wall coverage. The authored canvas must remain centered 4:3.

These experiments are useful negative evidence, but none remains in the
source or RT64 patch series.

## Regression suite

The dedicated matrix captures both stages at native 60 and display-rate
interpolation, with both a widescreen preference and explicit 4:3:

```sh
tools/test_rotation_regressions.sh \
    ./build-profile/src/game/troublemakers \
    input/troublemakers.us1.z64 \
    build/visual-tests/rotation-final
```

Defaults are 64 dense frames per case at 0.08-second intervals, for eight
stage/aspect/rate runs. The controller settles and then holds right without a
double-tap. Each run must:

- reach the exact runtime scene and authoritative gameplay-ready state;
- report `cinematic-4:3` presentation for these fixed canvases;
- finish without a fatal/assert/crash signature;
- remain below the neutral-flat-pixel threshold that detects the large gray
  slab (the broken reference scored about `0.118`; corrected references are
  below `0.006`);
- produce a contact sheet for manual wall, texture, geometry, and trail review.

The final matrix passed **8/8** configurations and **512/512** captured frames.
Maximum neutral-flat scores ranged from `0.0008` to `0.00534`, well below the
`0.05` failure threshold. Manual review found continuous textured walls and
platforms with no trails, missing geometry, or color dropout.

The harness writes `results.tsv`, per-frame PNGs, `capture-index.tsv`, and a
contact sheet per configuration. Build artifacts are intentionally ignored by
Git. The exact former failure at Seasick motion frame 675 is textured in
`build/visual-tests/final-material-seasick-wide-60`; a 64-frame Vertigo run is
clean in `build/visual-tests/final-material-vertigo-wide-60`.

Dense sheets over 32 frames use eight columns. During final testing, a UI image
viewer mis-rendered a 1616x6870 four-column sheet as repeated horizontal image
strips even though the corresponding 1600x900 source PNGs and cropped sheet
cells were clean. That viewer artifact looks exactly like Solitaire trails.
Treat individual frame files as ground truth for any suspicious sheet cell;
the wider layout avoids the known false positive without hiding real frames.

The broad stage gates remain:

```sh
MM_WIDESCREEN=1 tools/test_widescreen_playable.sh BIN ROM OUT-WIDE
MM_WIDESCREEN=0 tools/test_widescreen_playable.sh BIN ROM OUT-4X3
```

Those sweeps cover all 52 progression stages. They are complementary: the
rotation matrix samples transient frames densely, while the campaign sweep
detects crashes, scene-mode mistakes, tile-layer regressions, and unrelated
geometry changes.

The final broad passes completed **52/52** stages with a widescreen preference
and **52/52** in explicit 4:3 (312 additional captured frames total). Manual
review of both aggregate sheets found no unrelated geometry, texture, color,
aspect-transition, or left-edge regression.

## Review checklist

- Regenerate `RecompiledFuncs` after changing the TOML hook.
- Apply every runtime patch with `git am` and every RT64 patch with `git apply`
  from the pinned clean bases before accepting the series.
- Inspect contact sheets, not only the first spawn frame.
- Test a normal expanded stage after the fixed-canvas stages to prove aspect
  mode returns to expanded gameplay.
- Test 4:3 left-edge walking separately; its signed texture-rectangle fix is
  independent of this material correction.
