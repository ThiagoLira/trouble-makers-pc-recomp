# CLAUDE.md

Guidance for AI agents (and humans) working in this repository. Read this
first; the deep engineering history lives in [`docs/`](docs/).

## What this project is

**Trouble Makers** is a native PC port of the Nintendo 64 game *Mischief
Makers* (Treasure, 1997), produced by **static recompilation**: the game's
MIPS machine code is translated to C ahead of time by
[N64Recomp](https://github.com/N64Recomp/N64Recomp), compiled natively, and
linked against [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
(a libultra re-implementation) with [RT64](https://github.com/rt64/rt64)
rendering on Vulkan. It is **not** an emulator — there is no CPU simulated at
runtime; the game *is* the native binary. Same toolchain as Zelda64Recomp.

The checked-in `symbols/troublemakers.us1.toml` map is the authoritative input
for game functions and overlays. N64Recomp combines it with the builder's own
US 1.1 ROM; no decompilation checkout or ELF is part of the build. Do not
substitute symbols or code from an unrelated third-party decompilation.

**No game code, ROM, or recompiler output is committed to this repo.** Users
supply their own legally dumped Mischief Makers (US 1.1) ROM.

## Naming / branding

- Project/product name: **Trouble Makers** (no "Recompiled" suffix).
- The shipped executable / CMake target is **`troublemakers`**.
- The game's real name, *Mischief Makers*, appears **only in the README** and
  in `GameEntry.internal_name = "MISCHIEF MAKERS"` (a required ROM-header hash
  match — do not change it).
- Internal libraries (`mm_rsp`, `mm_audio_input`, `mm_graphics`,
  `mm_recompiled`), the `mm::` namespaces, and `MM_*` env/macro names are
  internal and intentionally left as-is.

## Repository layout

- `src/game/` — host entry (`main.cpp`), launcher splash (`launcher.cpp`),
  overlay registration, OS shims, per-platform config dir (`app_dirs.cpp`)
- `src/rsp/` — recompiled `aspMain` audio microcode (generated) + dispatch
- `src/graphics/` — RT64 renderer glue
- `src/audio_input/` — SDL2 audio, input, EEPROM save config
- `patches/` — runtime patches pending upstream (`N64ModernRuntime/`, `rt64/`)
- `tools/N64Recomp/` — the recompiler (submodule)
- `symbols/troublemakers.us1.toml` — checked-in function/overlay metadata
- `docs/` — full engineering history: phase notes, mission prompts, debugging
  recipes. Start with `docs/README.md`.
- `input/troublemakers.us1.z64` — builder-supplied ROM (gitignored)
- `RecompiledFuncs/`, `src/rsp/generated/` — generated C (gitignored)
- `lib/rt64`, `lib/N64ModernRuntime` — see gotchas below

## Build & regenerate

```sh
# 1. Put your own US 1.1 ROM at input/troublemakers.us1.z64, then generate:
tools/N64Recomp/build/N64Recomp troublemakers.us1.toml  # -> RecompiledFuncs/
tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml    # -> src/rsp/generated/

# 2. Build the app:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target troublemakers -j

# no-arg launch opens the launcher; pass a ROM to boot straight in:
./build/src/game/troublemakers [rom.z64] [--widescreen|--fullscreen|--window WxH|--msaa N|--ssaa N]
```

- The N64Recomp **CMake target** is `N64RecompCLI`, which produces the
  executable `tools/N64Recomp/build/N64Recomp`. The bare CMake target
  `N64Recomp` is a static library.
- Regenerating `RecompiledFuncs/` can add new `funcs_NN.c` files, so **re-run
  cmake configure** afterward (the source glob is configure-time).
- Never hand-edit `RecompiledFuncs/*` — fixes belong in the `.toml` config or
  the headers under `include/`.

## Critical gotchas

- **`lib/N64ModernRuntime` is a submodule pinned to UPSTREAM.** The working
  tree runs branch `tm-fixes` = pin + `patches/N64ModernRuntime/*.patch`
  applied. **Never commit the submodule gitlink** (`.gitmodules` has
  `ignore=all`). Apply with
  `git -C lib/N64ModernRuntime am "$(pwd)"/patches/N64ModernRuntime/*.patch`.
- **`lib/rt64` is a NESTED git repo, not a submodule**, and is gitignored.
  Clone the pinned fork (commit `23cab603`), init its submodules, then apply
  `patches/rt64/*.patch`. On a fresh worktree also check it out with
  `core.autocrlf=false` (LF patches).
- **Windows builds require clang-cl** (MSVC `cl` is unsupported for the
  generated C). SDL2 is fetched automatically; DXC + SDL2 DLLs are copied
  next to `troublemakers.exe`.
- **Config/saves** live in `troublemakers-recomp` under the user config dir
  (`%LOCALAPPDATA%` on Windows, `~/.config` on Linux), or next to the binary
  if a `portable.txt` is present. Do not rename this dir — it would orphan
  existing saves.
- **Headless dev harness** (no GPU): `cmake -B build_headless
  -DMM_BUILD_GRAPHICS=OFF`, then `MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy
  SDL_AUDIODRIVER=dummy ./build_headless/src/game/troublemakers rom.z64`.
- **gdb**: ptrace attach is blocked (yama) — launch under gdb instead
  (`gdb --args ...`). rdram aligned-word reads: `x/xw rdram + (vaddr -
  0x80000000)`.
- **Never `pkill -f` a pattern that matches your own command line.** Use
  `timeout -k`; the game sometimes ignores plain SIGTERM.

## Engine facts worth knowing

Natively **60 fps**; uses **gspFast3D** (not F3DEX) + **aspMain** audio ucode
(RSPRecomp'd, IMEM base **0x04001080**, not 0x1000); saves to **4Kbit
EEPROM**; pokes exactly one hardware register raw (**AI_LEN**, host-mirrored —
see `docs/PHASE5_NOTES_c.md`). Widescreen is opt-in and still experimental;
read `docs/PHASE7_NOTES_widescreen.md` §2 and §4 before touching tile draws.

## CI & releases

`.github/workflows/build.yml` builds the Linux AppImage and the Windows
package on every push (needs a `TM_ASSETS_REPO` secret providing
`troublemakers.us1.z64`; CI generates both the game and RSP code from it).
Release binaries are those artifacts. Package an AppImage
locally with `.github/linux/appimage.sh` (`NO_STRIP=1` on bleeding-edge
distros; the host needs the SVG gdk-pixbuf loader, e.g. `librsvg2-common`).
