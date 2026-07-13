# Mischief Makers: Recompiled

**A native PC port of Mischief Makers (N64, 1997) via static recompilation —
playable, 60 fps, high-resolution, with correct sound.**

![The intro cutscene rendering at 1920x1440](screenshots/intro-1920x1440.png)

The whole game's MIPS code is translated to C once, ahead of time, by
[N64Recomp](https://github.com/N64Recomp/N64Recomp), compiled natively, and
linked against [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
(a libultra re-implementation) with [RT64](https://github.com/rt64/rt64)
rendering the display lists on Vulkan. Same approach as
[Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp); this project
covers a very different, very Treasure-shaped game.

Sibling project: [trouble-makers-ai-recomp](../trouble-makers-ai-recomp) — the
byte-perfect decompilation whose symbol-rich ELF makes this translation
legible (every function arrives named).

**No game assets, ROM contents, or recompiler output are in this repository.**
Everything runs locally from your own legally dumped ROM.

## Status

- ✅ Boots, plays the full intro with correct music, title screen, menus
- ✅ Gameplay: first level verified playable (controller + keyboard)
- ✅ Natively 60 fps (the game's own rate), correct audio pacing
- ✅ High-resolution rendering (window-integer-scale via RT64), fullscreen, MSAA/SSAA
- ✅ EEPROM saves persist to disk
- 🧪 Experimental widescreen (opt-in), F11 fullscreen, Tab fast-forward,
  persistent display config
- 🚧 Automated widescreen sweep passes all 52 playable progression rows;
  a full controller-driven playthrough is still pending. Window
  occlusion freezes the game (present-paced); minor dither artifacts remain in
  translucent overlays at high res.
- 🗺️ Next: full-playthrough verification, mod hooks, upstreaming runtime patches

## Experimental widescreen

`--widescreen` expands playable scenes beyond the N64's original 4:3 frame.
This is real scene rendering: actors, 3D geometry, and the game's scrolling
background, environment, and midground tile maps can draw into both side
wings. It is still experimental while level-specific masks, framebuffer
effects, and off-map boundaries are tested across the full game.

| Original 4:3 (`--no-widescreen`) | Experimental 16:9 (`--widescreen`) |
|:--:|:--:|
| ![Marina idling in the first level at the original 4:3 aspect ratio](screenshots/gameplay-vanilla-idle.gif) | ![Marina idling in the first level with the scene expanded to 16:9](screenshots/gameplay-widescreen-idle.gif) |

Both GIFs were captured from this build in the first playable level at the
same idle sequence. Cinematics automatically use the original presentation;
the renderer reopens the wings only after stable player control is detected.

### Widescreen gallery

These live captures use the automated controller driver, so the camera,
actors, geometry, and tile layers are moving throughout each shot.

| Meet Marina | Snowstorm Maze |
|:--:|:--:|
| ![Meet Marina running through the expanded forest](screenshots/widescreen-gallery/meet-marina.gif) | ![Marina traversing Snowstorm Maze in widescreen](screenshots/widescreen-gallery/snowstorm-maze.gif) |
| Rolling Rock | ClanCe War 2 |
| ![The Rolling Rock corridor extending across the widescreen frame](screenshots/widescreen-gallery/rolling-rock.gif) | ![The ClanCe War 2 battle filling a widescreen city road](screenshots/widescreen-gallery/clance-war-2.gif) |
| Trapped | More captures |
| ![Marina running across the widescreen rooftops in Trapped](screenshots/widescreen-gallery/trapped.gif) | Five more animated scenes are preserved in the [full widescreen gallery](screenshots/widescreen-gallery/README.md). |

## Building and running

### Prerequisites

- A legally dumped **Mischief Makers (US 1.1)** ROM (`.z64`, big-endian)
- The sibling decomp built once (its `./trouble build` produces `troublemakers.elf`)
- Linux: gcc/g++ (C++20), CMake ≥ 3.24, SDL2, a Vulkan-capable GPU + loader
  (no Vulkan SDK needed — RT64 bundles headers and its shader compiler)
- Windows (experimental, untested): Visual Studio 2022 with the "Desktop
  development with C++", "C++ Clang Compiler" and "C++ CMake tools"
  components — build with **clang-cl** (`cmake -G Ninja
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`); MSVC's cl is
  not supported for the generated C. SDL2 is fetched automatically and
  SDL2.dll + the DXC DLLs are copied next to mm_game.exe. Saves/config go to
  `%LOCALAPPDATA%\troublemakers-recomp` (or next to the exe with a
  `portable.txt`).

### One-time setup

```sh
git clone --recurse-submodules https://github.com/ThiagoLira/trouble-makers-pc-recomp
cd trouble-makers-pc-recomp

# Runtime fixes not yet upstreamed (message delivery, overlay registration,
# EEPROM semantics):
git -C lib/N64ModernRuntime am "$(pwd)"/patches/N64ModernRuntime/*.patch

# RT64 (renderer), pinned to the fork/commit Zelda64Recomp uses, plus the
# widescreen wing-clear patch:
git clone https://github.com/rt64/rt64 lib/rt64
git -C lib/rt64 checkout 23cab603
git -C lib/rt64 submodule update --init --recursive
git -C lib/rt64 apply "$(pwd)"/patches/rt64/0001-widescreen-wing-clear.patch

# Translate the game + audio microcode:
cp ../trouble-makers-ai-recomp/build/troublemakers.elf input/
cmake -B tools/N64Recomp/build tools/N64Recomp
cmake --build tools/N64Recomp/build --target N64Recomp RSPRecomp -j
tools/N64Recomp/build/N64Recomp troublemakers.us1.toml
tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml
```

### Build and play

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mm_game -j8

./build/src/game/mm_game                                 # no args: launcher
#   (splash screen: pick your ROM with a native file dialog, set resolution /
#   fullscreen / widescreen, Start Game; remembers the ROM for next time)
./build/src/game/mm_game path/to/your.z64                # windowed 1280x960
./build/src/game/mm_game rom.z64 --fullscreen
./build/src/game/mm_game rom.z64 --window 1920x1440 --msaa 4
./build/src/game/mm_game rom.z64 --widescreen    # real 16:9 scene rendering:
#   entities, foreground, scrolling backdrops, environment and midground
#   tile maps are drawn beyond the original 4:3 frame. Plain launches retain
#   the original presentation.
./build/src/game/mm_game rom.z64 --no-widescreen # force original 4:3
```

Widescreen uses the game's own wrapping maps and scene formulas—there is no
mirroring, stretching, blur fill, or other cosmetic substitute. Every layer
in ordinary scrolling scenes is eligible to extend; individual off-frame
tiles whose art a scene never loads are validated against the loaded texture
bank and left at a clean authored boundary instead of displaying uninitialized
texture memory.
Entity spawn/despawn windows are widened to match, so objects no longer pop
in at the wing edges.
Opening/in-stage cinematics automatically switch back to centered 4:3 and
return to widescreen only after player control is stable. A small remaining
fallback set currently stays 4:3 while its fixed-canvas composition is being
validated (scenes 25, 27, 57, 71, 79, and 85). Vertigo and Seasick Climb now
render their rotating textured walls in widescreen without framebuffer trails.
See the live [scene 22 capture](screenshots/widescreen-scene-22.png), the
[forest artifact comparison](screenshots/widescreen-forest-fix.png), and the
labeled [coverage](screenshots/widescreen-coverage-scenes.png) and
[regression](screenshots/widescreen-regression-scenes.png) sheets. The final
[playable-level sample](screenshots/widescreen-playable-suite.png) and
[4:3 cinematic sample](screenshots/widescreen-cutscenes-4x3.png) show the
automatic mode boundary.

Run the complete playable-level screenshot/crash suite with:

```sh
tools/test_widescreen_playable.sh ./build/src/game/mm_game path/to/rom.z64 /tmp/mm-widescreen-suite
```

The suite targets exact progression-table stage indices, advances dialogue,
waits for authoritative player-control state, moves Marina in short alternating
bursts, audits expanded tile layers for wing coverage, and writes a TSV manifest
plus a multi-frame contact sheet and log for every level. For transient 3D
problems, capture a sustained frame sequence with `tools/test_render_burst.sh`.

Options persist to `~/.config/troublemakers-recomp/display.cfg` (CLI
overrides). In game: **F11** toggles fullscreen, **hold Tab** fast-forwards 3x.

The ROM is hash-validated (US 1.1 only), stored under
`~/.config/troublemakers-recomp/` along with saves, and the game auto-starts.
The scene renders at window-integer-scale: a bigger window IS higher internal
resolution. Keep the window visible — a fully occluded window pauses the
present and the game with it.

### AppImage

```sh
./.github/linux/appimage.sh          # after building mm_game; NO_STRIP=1 on Arch-likes
```

Produces `MischiefMakersRecompiled-x86_64.AppImage` (launcher included — no
CLI needed; the ROM is picked in the splash screen). Build it on the oldest
distro you want to support: the AppImage requires the build machine's glibc
or newer. Put a `portable.txt` next to the AppImage to keep config/saves in
that folder instead of `~/.config/troublemakers-recomp`. CI scaffolding for
Linux/AppImage and Windows artifacts lives in `.github/workflows/build.yml`
(untested; needs a `TM_ASSETS_REPO` secret providing `troublemakers.elf`).

### Controls

| N64        | Key        | N64      | Key         |
|------------|------------|----------|-------------|
| Stick      | W A S D    | A        | X           |
| D-pad      | Arrows     | B        | C           |
| C-buttons  | I J K L    | Z        | Left Shift  |
| L / R      | Q / E      | Start    | Enter       |

The first SDL game controller is picked up automatically (rumble included).

## How it works / hacking on it

Source layout: `src/game/` (host entry, overlay registration, OS shims),
`src/rsp/` (recompiled aspMain audio microcode + dispatch),
`src/graphics/` (RT64 renderer glue), `src/audio_input/` (SDL2 audio, input,
save config), `patches/` (runtime patches pending upstream), `tools/` (the
recompiler submodule + agent-workflow scripts).

The complete engineering history — twelve root-caused bugs from "parks before
boot" to "playable", every mission brief, and the debugging recipes — lives in
[`point_your_ai_agent_here/`](point_your_ai_agent_here/). It is written to
onboard an AI agent (or you) in one sitting. Headless dev harness:
`MM_HEADLESS_GFX=1` runs the full game loop with no GPU.

This port was built in ~2 days by AI agents: Claude (Fable 5) as
driver/reviewer with fleets of GLM 5.2 workers doing parallel
investigation and implementation — worktree-isolated, race-judged,
line-by-line reviewed. The receipts are in the phase notes.

## Licensing

- Code in this repository: **MIT** (see `LICENSE`)
- `N64ModernRuntime` (statically linked): **GPLv3** — distributed binaries
  are combined works and carry GPLv3 obligations
- `N64Recomp`, `RT64`: MIT
- No Nintendo/Treasure code or assets are included or distributed

## Credits

- [N64Recomp / N64ModernRuntime](https://github.com/N64Recomp) and
  [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) — the
  toolchain and the integration blueprint
- [RT64](https://github.com/rt64/rt64) — the renderer
- Treasure Co. Ltd — for the weirdest, most wonderful N64 game
