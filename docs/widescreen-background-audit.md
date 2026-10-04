# Widescreen background audit — 2026-10-04

## Findings and corrections

The reported square filler in Lunar (3-8) and black rectangles in The Day
Before (3-9) were reproduced and visually confirmed. The World 3 sweep
found additional instances of backgrounds authored only for the native
safe area, rather than usable scenery beyond it.

| Level | Scene | Result |
| --- | ---: | --- |
| 3-1 Clanpot Shake | 72 | Static panorama expanded; filler removed |
| 3-2 Clance War | 12 | Static panorama expanded; filler removed |
| 3-3 Missile Surf | 35 | Background panels expanded together; opaque sky base removes the brightness boundary while retaining translucent lights |
| 3-4 Clanball Lift | 71 | Existing intentional 4:3 presentation retained |
| 3-5 Go Marzen 64 | 32 | Static panorama expanded; filler removed |
| 3-6 Chilly Dog! | 31 | Existing correction restored by regenerating missing hook calls |
| 3-7 Snowstorm Maze | 36 | Existing correction restored by regenerating missing hook calls |
| 3-8 Lunar | 9 | Static panorama expanded; square filler removed |
| 3-9 The Day Before | 33 | Background panels expanded together; uncovered rectangles removed |
| 3-10 The Day Of | 18 | No matching background defect in sampled frames |
| 3-11 Cat-astrophe | 29 | No matching background defect in sampled frames |
| 3-12 CERBERUS | 19 | **Unresolved:** brighter native center against darker expanded background |

Local generated C lacked the existing `mm_ws_stretch_backdrop_rect` call.
The host implementation alone therefore could not apply earlier snow-stage
corrections. CMake now rejects generated sources missing this and three
other critical widescreen calls. Regeneration fixes the local build; no
generated sources are edited or committed.

Background actor changes use renderer-owned records and preserve both
in-flight matrices. The draw list reserves capacity for every remaining
original actor before inserting extra copies. Gameplay controllers,
collision, and native 4:3 rendering retain their existing paths.

## Whole-game sweep

All **52 playable progression rows** launched and produced three captures
each. Contact sheets were inspected in addition to automatic black-region
and native-border scores. The scores are review hints: they flag dark cave
art, foreground silhouettes, and intentional 4:3 views, and can miss patterned
filler. A harness `pass` means capture/runtime success, not visual correctness.

Additional findings:

- PHOENIX (progression 54, scene 26) has separated background strips and
  black gaps in the expanded view. **Unresolved.** Simple actor scaling and
  opacity changes did not correct it; those experiments were removed.
- Migen Brawl (progression 22, scene 5) retains a centered arena in the
  sampled boss sequence. Native captures show the same arena composition;
  it is not counted as successful full-width coverage.
- Automatic flags in progression rows 6, 8, 18, and 37–40 correspond to
  foreground edges or dark scenery in the inspected samples, without the
  reported square filler pattern.
- Intentionally fixed progression rows 11, 27, 47, 52, 56, and 57 were kept
  distinct from expanded-view defects.

## Verification and evidence

- Complete 52-level run: `/tmp/mm-background-final-sweep/` (156 frames,
  `results.tsv`, `background-audit.tsv`, seven contact sheets).
- Native 4:3 comparison: `/tmp/mm-background-native/` (11 selected levels).
- Lunar and The Day Before: 16:10 and ultrawide bursts under
  `/tmp/mm-background-aspects/`, with display-rate interpolation enabled.
- Final Missile Surf sky composition supersedes its full-sweep sample:
  `/tmp/mm-missile-seeded/` (16:9),
  `/tmp/mm-missile-seeded-16x10/`, and
  `/tmp/mm-missile-seeded-ultrawide/`.
- `mm_presentation` CTest passes. The generated-hook guard accepts current
  generated sources and rejects a temporary copy with the panorama call removed.

These are short gameplay samples, not complete playthroughs or coverage of
every camera position, boss phase, transition, and resize. Capture artifacts
are local temporary files. Reproduce the sweep and review with:

```sh
tools/test_widescreen_playable.sh build/src/game/troublemakers \
    input/troublemakers.us1.z64 /tmp/mm-background-review
python3 tools/audit_widescreen_backgrounds.py /tmp/mm-background-review
```
