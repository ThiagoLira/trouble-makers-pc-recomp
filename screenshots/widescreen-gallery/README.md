# Widescreen capture gallery

All ten GIFs were recorded from the current build at 640×288. The automated
test harness advances dialogue and drives Marina during each capture, making
these useful as both a visual showcase and a quick record of moving-scene
coverage.

| Stage | Scene | Capture |
|---:|---:|---|
| 2 | 0 — Meet Marina | [meet-marina.gif](meet-marina.gif) |
| 3 | 68 — Meet Calina | [meet-calina.gif](meet-calina.gif) |
| 13 | 69 — Vertigo | [vertigo.gif](vertigo.gif) |
| 21 | 13 — Seasick Climb | [seasick-climb.gif](seasick-climb.gif) |
| 30 | 36 — Snowstorm Maze | [snowstorm-maze.gif](snowstorm-maze.gif) |
| 37 | 42 — Rolling Rock | [rolling-rock.gif](rolling-rock.gif) |
| 41 | 41 — Rescue! Act 2 | [rescue-act-2.gif](rescue-act-2.gif) |
| 42 | 10 — Taurus | [taurus.gif](taurus.gif) |
| 49 | 77 — ClanCe War 2 | [clance-war-2.gif](clance-war-2.gif) |
| 53 | 22 — Trapped | [trapped.gif](trapped.gif) |

Capture source frames are intentionally not committed. Recreate a moving
sequence with `tools/test_render_burst.sh`, `MM_TEST_MOVE=jet-right`, and the
stage index listed above.
