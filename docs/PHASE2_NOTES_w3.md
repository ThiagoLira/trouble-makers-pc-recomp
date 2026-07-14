# Phase 2 Notes — w3 (graphics / RT64) — Mischief Makers static recomp

Worker w3. Owns `src/graphics/`, `lib/rt64/`, this file. Status: **gate PASSED.**

## What was done

1. **Studied the reference.** `reference/Zelda64Recomp` (depth-1, submodules
   NOT inited — its `lib/rt64` is empty) pins RT64 in `.gitmodules`:
   `https://github.com/rt64/rt64` @ `23cab603c4f9f4a8b369b38e036f1aa484603878`
   (`git ls-tree HEAD lib/rt64` in the reference repo). The reference's
   integration lives in `src/main/rt64_render_context.cpp` (the `zelda64::
   renderer::RT64Context` class, a subclass of `ultramodern::renderer::
   RendererContext`) and `src/main/main.cpp` (~L685) which plugs
   `zelda64::renderer::create_render_context` into
   `ultramodern::renderer::callbacks_t::create_render_context` for
   `recomp::start`. The abstract interface our side must implement is
   `lib/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp`
   (`RendererContext` pure-virtual: `valid`, `update_config`,
   `enable_instant_present`, `send_dl(const OSTask*)`, `update_screen`,
   `shutdown`, `get_display_framerate`, `get_resolution_scale`; plus
   `callbacks_t{create_render_context, get_graphics_api_name}` and
   `set_callbacks()`).

2. **Cloned the pinned RT64 fork** into `lib/rt64` (gitignored) at the exact
   reference commit, then `git submodule update --init --depth 1` for its 18
   registered contrib submodules (dxc-bin, Vulkan-Headers,
   VulkanMemoryAllocator, volk, imgui, im3d, implot, hlslpp, xxHash, zstd,
   stb, nativefiledialog-extended, spirv-cross, ddspp, re-spirv,
   D3D12MemoryAllocator, mupen64plus-core, mupen64plus-win32-deps). DLSS and
   xess are in RT64's .gitmodules but are not registered submodules on this
   commit and are Windows-only upscalers — not needed. The `re-spirv` contrib
   has a NESTED submodule (`external/SPIRV-Headers`) that must be inited
   separately or RT64 configure fails at `re-spirv/CMakeLists.txt:13`.

3. **Built RT64 as a static library on this machine.** Configured with the
   exact reference flags `RT64_STATIC=ON`, `RT64_SDL_WINDOW_VULKAN=ON` via
   `add_subdirectory(lib/rt64 build_w3/rt64)`. Result: `rt64.a` (14 MB) +
   `libmm_graphics.a` (17 KB). Zero errors.

4. **Wrote the glue** — `src/graphics/rt64_render_context.cpp` +
   `src/graphics/mm_graphics.h` + `src/graphics/CMakeLists.txt`. Adapted the
   reference's `rt64_render_context.cpp` for this game, trimmed of
   texture-pack / RmlUi / `recomp::mods` / `recompui` hooks (out of scope:
   those are Zelda-specific mod/UI features). Kept the full renderer surface:
   the `mm::graphics::RT64Context` class implementing `RendererContext`, the
   `to_rt64(...)` enum mappers, `set_application_user_config`,
   `map_setup_result`, `map_graphics_api`, `compute_max_supported_aa`, and
   the DMEM/IMEM/MI/DPC register backing storage.

## Build deviations from the reference

- **No system Vulkan SDK/headers — and none needed.** This box has the
  Vulkan *loader* (`libvulkan.so`, `pkg-config --modversion vulkan` →
  1.4.350, `vulkan.pc` with `Cflags: -I/usr/include`) but the Vulkan
  *headers* (`/usr/include/vulkan/vulkan.h`) are NOT installed
  (`vulkan-headers` package absent; `pacman -Q vulkan-headers` → not found;
  the .pc's `includedir` points at a dir with no `vulkan/` subdir). This does
  NOT block RT64: RT64 **bundles its own Vulkan headers** at
  `lib/rt64/src/contrib/Vulkan-Headers/include` and loads Vulkan at runtime
  via **volk** (`volkInitialize()` dlopens `libvulkan.so.1`), compiled with
  `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`. RT64's CMakeLists never calls
  `find_package(Vulkan)`. So: no Vulkan SDK, no problem.
- **DXC shaders build on Linux via the bundled binary.** RT64 compiles its
  HLSL→SPIR-V shaders at *build* time using the bundled prebuilt
  `lib/rt64/src/contrib/dxc/bin/x64/dxc-linux` (run with
  `LD_LIBRARY_PATH=.../src/contrib/dxc/lib/x64`). Verified it runs on this
  glibc (`libdxcompiler.so: 1.8`). No system `dxc`/`glslangValidator` needed
  (though `glslangValidator` IS present at `/usr/bin`).
- **`RT64_SDL_WINDOW_VULKAN` must be defined on the glue TU too, not just
  the rt64 target.** RT64's CMakeLists does `add_compile_definitions
  (RT64_SDL_WINDOW_VULKAN)` only within its own subdir scope, so it does not
  reach the `mm_graphics` target. Without it, `RT64::RenderWindow` resolves
  to the X11 `{Display*, Window}` struct (the `#elif defined(__linux__)`
  branch) instead of the `typedef SDL_Window*` (the
  `#elif defined(RT64_SDL_WINDOW_VULKAN)` branch), and
  `appCore.window = window_handle` fails to compile. Fix: added
  `RT64_SDL_WINDOW_VULKAN` (plus `HLSL_CPU`, `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`)
  to `mm_graphics`'s `target_compile_definitions`. The reference avoids this
  by setting `add_compile_definitions("RT64_SDL_WINDOW_VULKAN")` *globally*
  at the top of its root CMakeLists — we cannot edit the root CMakeLists, so
  the define is scoped to our target instead. Equivalent.
- **Consumer include dirs duplicated.** RT64's `include_directories(...)` in
  its CMakeLists are directory-scoped (not `target_include_directories ..
  PUBLIC`), so the rt64 target's contrib include set does NOT propagate to
  `mm_graphics`. RT64's public headers transitively need `vulkan/vulkan.h`,
  `vk_mem_alloc.h`, `hlslpp/...`, `imgui/...`, etc. Mirrored the full set in
  `mm_graphics`'s `target_include_directories(PRIVATE ...)` (same duplication
  the reference does at its ~L217). The reference also adds `dxc/inc` on
  Windows only — skipped (Linux).
- **gcc, not clang → no `-Werror`.** RT64's CMakeLists only enables `-Werror`
  under `CMAKE_CXX_COMPILER_ID STREQUAL "Clang"`. On gcc 16 the build emits
  benign warnings (notably hlslpp's `-Wcast-user-defined` in
  `dependent.h`) but no errors. Left as-is.
- **C++ standard:** RT64 sets `CMAKE_CXX_STANDARD 17` in its own scope (the
  `rt64` target is C++17); the root sets C++20, so `mm_graphics` is C++20.
  RT64's C++17 headers compile fine under C++20. No issue.

## The callback surface w1 (host/main) must call

Public header: `src/graphics/mm_graphics.h`. Link `mm_graphics` (which
PUBLIC-links `ultramodern`; PRIVATE-links `rt64` + `SDL2::SDL2`).

```cpp
#include "mm_graphics.h"

// Option A — one call, registers with ultramodern:
mm::graphics::register_callbacks();

// Option B — plug the function pointer directly into the callbacks struct
// (mirrors reference main.cpp ~L685):
ultramodern::renderer::callbacks_t renderer_callbacks{
    .create_render_context = mm::graphics::create_render_context,
};
ultramodern::renderer::set_callbacks(renderer_callbacks);
```

`create_render_context(uint8_t* rdram, SDL_Window* window, bool developer_mode)`
returns a `std::unique_ptr<ultramodern::renderer::RendererContext>` (our
`RT64Context`). Signature matches
`ultramodern::renderer::callbacks_t::create_render_context_t` exactly (on
Linux `WindowHandle` is `SDL_Window*`). The runtime then drives
`send_dl` (hands the F3DEX `OSTask` to RT64's interpreter via
`loadUCodeGBI` + `processDisplayLists`), `update_screen`, `update_config`,
`enable_instant_present`, and `shutdown` through the interface.

Exported symbols (verified via `nm libmm_graphics.a`):
- `mm::graphics::register_callbacks()`
- `mm::graphics::create_render_context(uint8_t*, SDL_Window*, bool)`

w1's main() should also provide the `gfx_callbacks` (`create_gfx`/
`create_window`/`update_gfx`) that owns the SDL_Window* and hands it to the
renderer — that's w1's job, not ours; we just consume the `SDL_Window*`.

## SUCCESS GATE — PASSED

```
cmake -B build_w3
cmake --build build_w3 --target mm_graphics -j4
```
Zero errors. Produces `build_w3/lib/libmm_graphics.a` (and `rt64.a` as a
build dependency). The root CMakeLists auto-detects `src/graphics/
CMakeLists.txt` (it's in `MM_PHASE2_COMPONENTS`), which triggers
`find_package(SDL2)` + `add_subdirectory(lib/N64ModernRuntime)` before our
component, so `ultramodern`/`librecomp`/`SDL2::SDL2` are available.

## Graceful degradation

`MM_BUILD_GRAPHICS` defaults ON but the CMakeLists returns early (no target,
just a `message(WARNING ...)` describing exactly what's missing) if:
- `lib/rt64/CMakeLists.txt` is absent (fork not cloned), or
- SDL2 is not found.
So `cmake -B build_w3` never breaks the tree for sibling workers. To opt out
entirely: `-DMM_BUILD_GRAPHICS=OFF`.

## Blockers / follow-ups

- None for this gate. RT64 builds; glue compiles; symbol surface is clean.
- Runtime validation (actually rendering a frame) is NOT in this gate — it
  requires w1's main() + a Vulkan-capable device + the game's RSP ucode
  (RSPRecomp, worker w2/rsp). The glue is written to the interface; runtime
  testing happens once the host wires `recomp::start`.
- RT64's `dataPath` / `appId` defaults are used (`useConfigurationFile=false`
  set explicitly, matching the reference). If shader-cache persistence is
  wanted later, set `appConfig.dataPath` to a writable dir.
- `rmonPrintf_recomp` / `__osGetCause_recomp` (PHASE1_NOTES item 7) are
  unrelated to graphics and remain stubs — not w3's scope.
