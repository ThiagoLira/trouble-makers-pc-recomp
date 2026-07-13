# Phase 2 Notes — worker w1 (game core)

Engineer: w1. Owns `src/game/` (`main.cpp`, `register_overlays.cpp`,
`os_stubs.cpp`, `CMakeLists.txt`) + this file. Status: **gate PASSED,
stretch PASSED.** Nothing committed — the driver commits.

## What was built

1. **`register_overlays.cpp`** — `#include`s `RecompiledFuncs/recomp_overlays.inl`
   (which itself pulls `recomp.h`, `funcs.h`, `librecomp/sections.h`) exactly
   once and feeds `section_table[]` / `num_sections` (177) /
   `overlay_sections_by_index[]` to `recomp::overlays::register_overlays(...)`.
   Modeled 1:1 on `reference/Zelda64Recomp/src/main/register_overlays.cpp`.
2. **`os_stubs.cpp`** — real host impls of the two OS wrappers librecomp lacks
   (see PHASE1_NOTES "Genuine link-time gaps"):
   - `rmonPrintf_recomp` — reads the N64 format-string pointer from `$a0`
     (`ctx->r4`), pulls the C string out of rdram (bounds-checked, endianness-
     xored like `MEM_BU`), and forwards it **verbatim** to host stderr. Varargs
     are NOT decoded (see Decisions).
   - `__osGetCause_recomp` — `ctx->r2 = 0` (no exception, no IP bits).
3. **`main.cpp`** — host entry. `recomp::Version` 0.2.0, registers one
   `GameEntry`, calls `troublemakers::register_overlays()`, optionally
   `recomp::select_rom(argv[1], game_id)` when a ROM is passed, builds a
   `recomp::Configuration` from stub callback seams, and calls
   `recomp::start(cfg)`.
4. **`CMakeLists.txt`** — target `mm_game` (executable). Whole-archive links
   `mm_recompiled librecomp ultramodern` (the game is data-driven: the
   `FuncEntry` tables hold function pointers no call graph reaches, so
   `--whole-archive` is mandatory), `--no-undefined`, plus `${SDL2_LIBRARIES}`.

## librecomp / ultramodern API surface used

- `recomp::register_config_path(path)` — must be called before `start` (mods
  init writes under it). Set to `~/.config/troublemakers-recomp`.
- `recomp::register_game(const GameEntry&)` — keyed by `game_id` (a
  `std::u8string`); `select_rom` looks the entry up BY that game_id.
- `recomp::overlays::register_overlays(overlay_section_table_data_t,
  overlays_by_index_t)` — `SectionTableEntry*` + `int*` tables.
- `recomp::select_rom(path, game_id&)` — caller must pass the registered
  game_id; it byteswap-normalizes the ROM, then `check_hash` =
  `XXH3_64bits(rom_data)` vs `GameEntry::rom_hash`. On match it writes the ROM
  to `config_path/<game_id>.z64` (stored for later boots).
- `recomp::start(const Configuration&)` — asserts `rsp_callbacks` and
  `renderer_callbacks` non-empty; calls `ultramodern::set_callbacks(...)`; spawns
  the game thread (`preinit` → `init_events` → gfx thread → `wait_for_game_started`
  loop) and runs the host event loop (`update_gfx` per tick) until
  `ultramodern::quit()`.

Callback struct shapes (all in `ultramodern/include` + `librecomp/include`):
- `recomp::rsp::callbacks_t{ RspUcodeFunc*(*get_rsp_microcode)(const OSTask*) }`
- `ultramodern::renderer::callbacks_t{ create_render_context_t*; get_graphics_api_name_t* = nullptr }`
  — `create_render_context` returns `std::unique_ptr<RendererContext>` (abstract;
  must subclass). Mandatory.
- `ultramodern::gfx_callbacks_t{ create_gfx; create_window; update_gfx }` —
  `create_window` asserted non-null when `window_handle` left default. On Linux
  `WindowHandle = SDL_Window*`.
- `ultramodern::audio_callbacks_t{ queue_samples; get_frames_remaining; set_frequency }`
- `ultramodern::input::callbacks_t{ poll_input; get_input; set_rumble; get_connected_device_info }`
- `ultramodern::events::callbacks_t{ vi_callback; gfx_init_callback }` (nullable)
- `ultramodern::error_handling::callbacks_t{ message_box }`
- `ultramodern::threads::callbacks_t{ get_game_thread_name }` (nullable)
- `ultramodern::MessageQueueControl` — all fields default.

## Stub seams the sibling workers must fill (each its own namespace in main.cpp)

| seam (namespace) | what it stubs | sibling worker / real impl |
|---|---|---|
| `mm::stub::renderer` + `StubRendererContext` | `create_render_context` | **graphics** — RT64 `RendererContext` (RDP/F3DEX) |
| `mm::stub::gfx` | `create_gfx`/`create_window`/`update_gfx` (SDL window) | **graphics** owns the real window/renderer |
| rsp callbacks (inline lambda in `main`) | `get_rsp_microcode` → nullptr | **rsp** — RSPRecomp of F3DEX ucode |
| `mm::stub::audio` | `queue_samples`/`get_frames_remaining`/`set_frequency` | **audio_input** — SDL audio driver |
| `mm::stub::input` | `poll_input`/`get_input`/`set_rumble`/`get_connected_device_info` | **audio_input** — SDL game controller |
| `mm::stub::events/error/threads` | no-op vi/error/thread-name | optional, can stay stubbed |

Replacement is local: drop a real function/struct into the matching namespace
(or swap the `cfg` field initializer in `main`). No other group needs to
change. `StubRendererContext` is an implementation detail of the renderer seam
— the graphics worker replaces it wholesale with an RT64 context.

## SUCCESS GATE — PASSED

```
cmake -B build_w1 && cmake --build build_w1 --target mm_game -j4
```
Zero errors, **zero warnings** for `src/game/*` (whole `mm_recompiled` +
`librecomp` + `ultramodern` + transitive `N64Recomp`/`LiveRecomp`/`miniz`
rebuild clean). Binary: `build_w1/src/game/mm_game`. `nm` confirms
`rmonPrintf_recomp`, `__osGetCause_recomp`, `recomp_entrypoint`,
`get_entrypoint_address` all defined (T).

## STRETCH — PASSED (clean early abort, as expected)

Copied `baserom.us1.z64` → `/tmp` (original untouched), ran headless:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout 12 ./build_w1/src/game/mm_game /tmp/mm_stretch.z64
```

Log (then idle until 12s timeout → exit 124, no crash):

```
[recomp] ROM validated and loaded.
[stub] create_render_context: no real renderer (RT64) yet — gfx thread will abort cleanly.
```

Interpretation:
- ROM wiring is correct end-to-end: the XXH3-64 hash
  `0xECB515AE2C898E4F` matches `baserom.us1.z64` after librecomp's byteswap
  normalization; `internal_name "MISCHIEF MAKERS"` matches; `select_rom`
  stored the ROM. Entry addr `0x80000400` + `recomp_entrypoint` wired in the
  GameEntry. Save type left as `Eep4k` (EEPROM 4Kbit — confirm in Phase 3).
- Startup reaches: SDL init → window creation → game thread `preinit` →
  gfx thread `create_render_context` → `StubRendererContext::valid()==false`
  → gfx thread returns cleanly (no crash) → host loop idles.
- It does NOT proceed to the game entrypoint because (a) no real renderer
  (graphics worker / RT64) and (b) `recomp::start_game` is never called — in
  the reference that trigger comes from the launcher UI, which no worker has
  built yet. Calling `start_game` now would run `recomp_entrypoint`, which
  immediately submits an RSP task → `get_rsp_microcode` returns nullptr →
  runtime exits. Both are sibling-worker deliverables (graphics + rsp).

## Decisions

- **rmonPrintf varargs not decoded.** Decoding MIPS varargs through host
  printf is fragile (`%s` args are rdram pointers; `%d/%x` in `$a1..$a3` then
  N64 stack at `$sp+`; layout depends on the specifier sequence) and rmon is
  debug-only. We print the format string verbatim to stderr (no host
  `printf` with game-controlled args → no format-string hazard). Revisit if a
  boot path depends on formatted output.
- **ROM hash** computed with system `xxh3sum` (= librecomp's `XXH3_64bits`).
  `.z64` is big-endian / `NotByteswapped`, so the hash is taken on the
  original bytes — no normalization mismatch.
- **`mm_game` links `--whole-archive` over all three runtime libs**, matching
  the proven `mm_runtime_probe`. `mm_recompiled` whole-archive is *required*
  (data-driven function tables); `librecomp`/`ultramodern` whole-archive
  matches the probe's clean `--no-undefined` link.
- **`is_enabled=true`** for the GameEntry (so `NotYet` vs `IncorrectRom`
  paths behave), `has_compressed_code=false` (the recomp input is the
  decompressed/decomp ELF's ROM image — no runtime decompression routine
  needed).
- Config dir `~/.config/troublemakers-recomp` (relocatable by siblings).

## Blockers

None for this worker. Upstream dependencies to actually run the game:
- **graphics** worker: real `RendererContext` (RT64) + window.
- **rsp** worker: `get_rsp_microcode` returning the F3DEX RSP ucode.
- **audio_input** worker: real audio + controller drivers.
- A game-start trigger (launcher UI or a `start_game` call site) — not
  assigned to w1; the reference does it from recompui.

Note: clangd reports false `lightweightsemaphore.h not found` /
`no matching vector constructor` errors for `main.cpp` because it can't see
the runtime's thirdparty include dirs. The CMake build (the gate) is clean.
