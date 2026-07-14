# Phase 2 Notes — worker w4 (audio + input + save)

Engineer: w4. Status: **gate PASSED.** Nothing committed. Files live in
`src/audio_input/` (+ this notes file). Built with its own `build_w4/`.

## SUCCESS GATE — PASSED

```
cmake -B build_w4 && cmake --build build_w4 --target mm_audio_input -j4
```
Zero errors, zero warnings in `src/audio_input/*`. Produces
`build_w4/lib/libmm_audio_input.a` (20 KB). Depends transitively on
`librecomp`/`ultramodern` (built into the same `build_w4/`); those emit a few
of their own pre-existing warnings (e.g. `recomp.cpp:707` return-type), not
mine.

## What w1 should call (the contract)

`#include "mm_audio_input.hpp"` (header is on the `mm_audio_input` target's
PUBLIC include path). Then, in main() after SDL2 video is up but before
`recomp::start()`:

```cpp
#include "mm_audio_input.hpp"
// after SDL_Init(SDL_INIT_VIDEO) and a window exists:
mm_audio_input::init();                                   // opens audio device + game-controller subsystem
mm_audio_input::register_save_config(<writable config dir>); // recomp::register_config_path wrapper
```

and when filling `recomp::Configuration`:

```cpp
cfg.audio_callbacks = mm_audio_input::audio_callbacks();
cfg.input_callbacks = mm_audio_input::input_callbacks();
// and on the GameEntry passed to recomp::register_game:
entry.save_type = mm_audio_input::save_type();            // recomp::SaveType::Eep4k
```

Link w1's final executable against the `mm_audio_input` target (it transitively
pulls `librecomp`, `ultramodern`, SDL2). The five exported symbols (all
`mm_audio_input::`): `init`, `audio_callbacks`, `input_callbacks`,
`save_type`, `register_save_config`. w1 still owns gfx/rsp/events/error/threads
callbacks and the `recomp::start()` call.

## Interface findings

### Audio (`ultramodern::audio_callbacks_t`, ultramodern.hpp:125)
Three function pointers; ultramodern's `src/audio.cpp` drives them:
- `queue_samples(int16_t*, size_t)` — `size_t` is the **int16 sample count**
  (2 per stereo frame), not bytes. ultramodern computes it as `byte_count/2`.
- `get_frames_remaining() -> size_t` — must return **stereo frames**; ultramodern
  multiplies by `2*sizeof(int16_t)` to get bytes.
- `set_frequency(uint32_t)` — the game's AI DAC rate; called via
  `ultramodern::set_audio_frequency` when the game calls osAiSetFrequency.

### Input (`ultramodern::input::callbacks_t`, input.hpp:27)
- `poll_input()` — pump per frame.
- `get_input(int controller_num, uint16_t* buttons, float* x, float* y) -> bool`
  — `controller_num` 0-indexed; `buttons` is the N64 button bitmask; `x`,`y` are
  floats clamped to [-1,1] (up = +y, verified against reference controls.cpp).
- `set_rumble(int, bool)` and `get_connected_device_info(int)` — return
  `Device::Controller` + `Pak::RumblePak` for port 0 so the rumble path is
  active (matches Zelda64Recomp).

These are passed through `recomp::Configuration` to `recomp::start()`, which
calls `ultramodern::set_audio_callbacks` / `ultramodern::input::set_callbacks`
internally. Workers do **not** call the `set_*` functions directly.

### Save (librecomp) — EEPROM is handled internally
`librecomp/src/eep.cpp` implements `osEepromProbe/Read/Write/LongRead/LongWrite_recomp`
on top of `save_read`/`save_write` (in `librecomp/src/pi.cpp`). `pi.cpp` backs
those with an in-memory buffer flushed to `<config_path>/saves/<game_id>.bin`
by a dedicated coalescing saving thread, with atomic file-swap backup. **The
host only (a) sets `GameEntry::save_type` and (b) calls
`recomp::register_config_path`.** No per-game EEPROM code is required from w4;
`save.cpp` is just the config surface for those two steps.

## Save-type evidence: Eep4k (4Kbit EEPROM)

- `RecompiledFuncs/funcs_76.c`, `funcs_84.c`, `funcs_87.c` call
  `osEepromProbe_recomp`, `osEepromLongRead_recomp`, `osEepromLongWrite_recomp`
  (declared in `RecompiledFuncs/funcs.h`). No `osFlashrom*`/`sram` references
  anywhere in `RecompiledFuncs/` → the game uses EEPROM only.
- The recompiled calls pass the EEPROM block address in `ctx->r5` (eep.cpp
  multiplies it by 8). Scanning every `r5 = ADD32(...)` immediately before an
  `osEepromLong*_recomp` call across the three TUs yields block addresses
  `{0x02, 0x0C, 0x14, 0x24, 0x2C}` — max **0x2C (44)**. The 4K EEPROM has 64
  blocks (0x00–0x3F); 16K has 256. All accesses fit comfortably in 4K, so
  `SaveType::Eep4k` is the accurate choice. (`AllowAll` would also work but
  over-reports as 16K; Eep4k matches the real cart.)
- Cross-checked against the decomp repo (`~/repos/trouble-makers-ai-recomp`,
  read-only): its `ultralib/include/PR/os_cont.h` gives the button bit layout;
  game-level EEPROM calls appear only via libultra wrappers in
  `src/ultralib/io/contep*.c`. The probe (`conteepprobe.c`) returns a generic
  `CONT_EEPROM` flag, not 4K-vs-16K — so the size call must come from observed
  addresses (above), which is conclusive for 4K.

## Implementation notes

### audio.cpp
SDL2 queue device (`SDL_OpenAudioDevice`, no callback) at fixed 48 kHz,
AUDIO_S16LSB, stereo. `set_frequency` rebuilds an `SDL_AudioCVT`
(S16, in-channels, game-rate → S16, 2, 48000). `queue_samples` swaps each
stereo pair (the N64 AI DMA delivers L/R swapped vs little-endian interleaved,
per the reference's "address xor" comment) then resamples and queues.
`get_frames_remaining` reports queued output frames scaled back to the game's
sample rate, minus ~1 VI backoff (prevents underrun-pop on games that size
generation off the remaining count). This is the Zelda64Recomp approach minus
the float volume scaling — simpler, still correct.

### input.cpp
Keyboard (always) + first SDL game controller (if attached) both feed port 0.
Keyboard map (documented at top of input.cpp and in the banner above):

| N64 | Key | | N64 | Key |
|---|---|---|---|---|
| Stick ↑↓←→ | W A S D | | A | X |
| D-pad | Arrows | | B | C |
| C-up/down/left/right | I K J L | | Z | Left Shift |
| L / R | Q / E | | Start | Return |

Controller mapping: face A→A, B→B, X→C-left, Y→C-up, L/R shoulders→L/R,
Start→Start, D-pad→D-pad, left trigger→Z, left stick→analog (overrides WASD).
Rumble forwarded via `SDL_GameControllerRumble` when supported. SDL Y axis is
down-positive, so it's negated to match N64 up-positive.

`poll_input` calls `SDL_PumpEvents()` + `SDL_GetKeyboardState()` and lazily
opens the first pad. Pumping here is idempotent if w1's loop also pumps.

### CMakeLists.txt
`mm_audio_input` STATIC, links `librecomp` + `ultramodern` + `${SDL2_LIBRARIES}`
PUBLIC. Note: had to add `${RUNTIME_DIR}/N64Recomp/include` explicitly —
`librecomp/game.hpp` `#include "recomp.h"`, and librecomp links the `N64Recomp`
target *PRIVATE*, so that path isn't propagated to consumers. (The root's
`RUNTIME_INCLUDES` has it but only feeds `mm_recompiled`/the probe, not this
target.) SDL2 is found + globally include-dir'd by the root before this dir is
added.

## Blockers / follow-ups for later phases

- None for the gate. Two Phase-1 stubs (`rmonPrintf_recomp`, `__osGetCause_recomp`)
  are out of w4's scope (librecomp-internal, PHASE1_NOTES.md item 7).
- Audio output rate is fixed at 48 kHz with SDL resampling; if a later phase
  wants bit-exact output, reopen the device at the game's rate on
  `set_frequency` instead. Current path is robust and pop-free.
- Keyboard map is hard-coded; Phase 4 (mod hooks / config) could externalize it
  via librecomp's input-binding system like the reference does.
