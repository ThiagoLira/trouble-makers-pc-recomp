# trouble-makers-pc-recomp

Static recompilation of **Mischief Makers** (N64, US 1.1) to portable C via
[N64Recomp](https://github.com/N64Recomp/N64Recomp) — the path to a native PC
build. Sibling project of
[trouble-makers-ai-recomp](../trouble-makers-ai-recomp), whose byte-perfect
decompilation build supplies the symbol-rich ELF that drives this recompiler
(every function the decomp names shows up named here).

## Pipeline

```
decomp repo:  ./trouble build            -> build/troublemakers.elf  (byte-perfect, symbol-rich)
this repo:    cp that ELF into input/
              tools/N64Recomp/build/N64Recomp troublemakers.us1.toml
              -> RecompiledFuncs/*.c     (whole game as portable C)
```

Build the recompiler once: `cmake -B tools/N64Recomp/build tools/N64Recomp && cmake --build tools/N64Recomp/build -j`

No game code, ROM contents, or recompiler output is committed — the
translation runs locally from your legally dumped ROM's decomp build.

## Building and running the game

**Current state: the game boots, plays its full intro cutscene with correct
music and graphics, and reaches the press-start screen at ~58 fps.**

### Prerequisites

- A legally dumped **Mischief Makers (US 1.1)** ROM (`.z64`, big-endian).
- The sibling decomp repo built once (`../trouble-makers-ai-recomp`, its
  `./trouble build` produces the symbol-rich `troublemakers.elf`).
- Linux with: gcc/g++ (C++20), CMake ≥ 3.20, SDL2, a Vulkan-capable GPU +
  loader (`libvulkan.so`). RT64 bundles its own Vulkan headers and shader
  compiler — no Vulkan SDK needed.

### One-time setup

```sh
git clone --recurse-submodules <this repo>
cd trouble-makers-pc-recomp

# The runtime needs two local fixes not yet upstreamed (message-delivery
# starvation + runtime overlay registration):
git -C lib/N64ModernRuntime am "$(pwd)"/patches/N64ModernRuntime/*.patch

# RT64 (renderer): clone the fork Zelda64Recomp pins into lib/rt64
git clone https://github.com/rt64/rt64 lib/rt64
git -C lib/rt64 checkout 23cab603
git -C lib/rt64 submodule update --init --recursive

# Build the recompilers, then translate the game + audio microcode
cp ../trouble-makers-ai-recomp/build/troublemakers.elf input/
cmake -B tools/N64Recomp/build tools/N64Recomp
cmake --build tools/N64Recomp/build --target N64Recomp RSPRecomp -j
tools/N64Recomp/build/N64Recomp troublemakers.us1.toml
tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml
```

### Build and run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mm_game -j8

./build/src/game/mm_game path/to/your.z64
```

The ROM is validated by hash (only US 1.1 boots), stored under
`~/.config/troublemakers-recomp/`, and the game auto-starts.

### Controls (keyboard; first SDL game controller also works)

| N64        | Key        | N64      | Key         |
|------------|------------|----------|-------------|
| Stick      | W A S D    | A        | X           |
| D-pad      | Arrows     | B        | C           |
| C-buttons  | I J K L    | Z        | Left Shift  |
| L / R      | Q / E      | Start    | Enter       |

### Developer modes & known issues

```sh
# full game loop with no window/GPU (CI-friendly):
cmake -B build_headless -DMM_BUILD_GRAPHICS=OFF && cmake --build build_headless --target mm_game -j8
MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build_headless/src/game/mm_game rom.z64

# runtime message-flow tracing:
MM_EVENT_TRACE=1 ./build/src/game/mm_game rom.z64 2> trace.log
```

- Keep the window **visible**: if the compositor fully occludes it (lock
  screen, minimize), the Vulkan present stalls and the game freezes with it
  (occlusion policy is a TODO).
- Gameplay beyond the press-start screen is untested frontier.
- stderr prints bring-up counters (`[gfx]`, `[mm_rsp]`) — temporary.

## Status / roadmap

- [x] Phase 0 — recompiler builds; whole-ROM translation succeeds (30,260
      symbols, 45MB of C, statically-linked overlays handled)
- [x] Phase 1 — runtime groundwork: N64ModernRuntime submodule; ALL translated game code compiles native (libmm_recompiled.a); see PHASE1_NOTES.md. Remaining Phase 1→2: link probe unresolved-symbol list = the work plan. Original scope:
      [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
      (librecomp = libultra reimplementation, ultramodern = platform layer);
      implement game entry + section/function lookup tables
- [x] Phase 2 — graphics/audio: RSP microcode via RSPRecomp (game uses
      gspFast3D for graphics — not F3DEX — and aspMain for audio; only aspMain
      is statically recompiled, RT64 interprets the display list); RT64 for
      RDP rendering. **The intro plays with correct picture and sound to the
      press-start screen.** Bug ledger: PHASE2..PHASE5 notes files.
- [ ] Phase 3 — game-specific glue: gameplay verification beyond press-start,
      save-in-anger testing (EEPROM), asset streaming across levels, window
      occlusion policy, stability pass, upstream the runtime patches
- [ ] Phase 4 — niceties: widescreen, high framerate, mod hooks (function
      names from the decomp make hooking pleasant)

Reference integration to model on: Zelda64Recomp (same author/toolchain).

## Licensing

All code authored in this repository is **MIT** (see LICENSE) — the most
permissive terms we control. Be aware of what the dependencies impose:

- [N64Recomp](tools/N64Recomp) (the recompiler) and RT64 are MIT.
- [N64ModernRuntime](lib/N64ModernRuntime) (librecomp + ultramodern, the
  runtime `mm_game` statically links) is **GPLv3**. Our MIT sources may be
  freely reused anywhere, but any *distributed binary* of `mm_game` is a
  combined work with the GPLv3 runtime and must be distributed under
  GPLv3-compatible terms.
- reference/Zelda64Recomp (GPLv3) is study-only, gitignored, and not part of
  the build; no code in this repo is derived from it.
- No game code, ROM contents, or recompiler output is committed or covered by
  the LICENSE — the translation runs locally from your own legally dumped ROM.
