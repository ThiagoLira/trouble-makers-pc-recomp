# Phase 5 Notes (lane c) — release build + real AI_LEN pacing

## TL;DR

Both lane-c tasks are done and verified against the headless repro harness:

1. **Release build runs clean.** `cmake -B build_rel -DCMAKE_BUILD_TYPE=Release -DMM_BUILD_GRAPHICS=OFF` builds with no new warnings/errors (5 pre-existing runtime warnings only — see Flags) and runs headless to the known crash: `[gfx] send_dl #770` + `Failed to find function at 0x801B9900` in ~13.6s. No -O2 miscompile.
2. **AI_LEN mirror implemented.** `Sound_Update`'s raw `lw` of `AI_LEN_REG` (0xA4500004) now reads a *real* value — `frames_remaining*4` bytes, mirrored by a ~1ms host thread — instead of the always-zero page. The game still reaches the same crash point (send_dl #770 / 0x801B9900): **no regression**. Side effect (a good one): real pacing lifted the frame rate from the bug#10 baseline of ~17 fps to ~58 fps (near the 60 Hz VI cap).

GATE: PASSED.

## Task 1 — Release build

### Build
```
cmake -B build_rel -DCMAKE_BUILD_TYPE=Release -DMM_BUILD_GRAPHICS=OFF
cmake --build build_rel --target mm_game -j8
```
Exit 0. Warnings: 5, all in `lib/N64ModernRuntime` (the symlinked runtime), none in lane-c code, none promoted to errors:
- `wstring_convert` deprecated (euc-jp.cpp) — libc++ deprecation, pre-existing.
- `UNUSED` redefined (rabbitizer Utils.h) — pre-existing.
- "control reaches end of non-void function" (mod_manifest.cpp, recomp.cpp) — pre-existing runtime gaps behind `assert(false); exit()`.

No undefined symbols (`-Wl,--no-undefined` clean), no new link gaps.

### Run (headless repro harness)
```
MM_HEADLESS_GFX=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout -k 3 30 ./build_rel/src/game/mm_game ./rom.z64
```
Reaches the known crash:
```
[gfx] send_dl #770
Failed to find function at 0x801B9900
terminate called without an active exception   # death mechanism, see below
```
Wall time to crash: **13.58s** for 770 frames.

### fps measurement (release vs debug)

Both builds were timed to the crash and sampled over a clean 6s window
(send_dl counter deltas, stderr flushed — see Instrumentation):

| build | 6s steady-state | time-to-crash (770 frames) | fps |
|---|---|---|---|
| Release (-O2 -DNDEBUG) | send_dl #350 in 6.0s | 13.58s | **~58 fps** |
| Debug (no build type)  | send_dl #350 in 6.0s | 13.57s | **~58 fps** |

**The two are equal because the frame loop is VI-paced (~60 Hz), not
CPU-bound.** The game thread submits a gfx task → audio → blocks on the DP
queue, which `dp_complete` posts from the gfx thread once per VI; so the loop
runs at the VI cadence regardless of how fast the recompiled C executes. -O2
lowers per-frame CPU cost but not the paced rate, hence release ≈ debug.

(Sanity check that the equality is pacing, not coincidence: the 6s samples land
on the *same* send_dl #350 / aspMain 340 for both builds, and the crash lands on
send_dl #770 for both. If the loop were CPU-bound, release would clear 770 far
sooner than debug.)

### The "terminate called without an active exception" + core dump

This is the *death mechanism* of the known crash, not a new fault. In a Release
build `NDEBUG` is defined, so `get_function` (librecomp/src/overlays.cpp:367)
runs as `fprintf(stderr, "Failed to find function …"); std::exit(EXIT_FAILURE);`
— the `assert(false)` is compiled out. `std::exit` from the game thread tears
down static-duration objects while ultramodern's worker threads are still
joinable, so `std::thread::~thread()` on a joinable thread calls
`std::terminate` → `abort` → SIGABRT → "dumped core". Identical in the Debug
build (there `assert(false)` is active and itself aborts). It is synchronized
with the `Failed to find function` line, i.e. it is a direct consequence of the
0x801B9900 crash path — **not** caused by the AI mirror (the mirror thread is
detached; a detached thread cannot trigger the joinable-thread-destructor
terminate).

### Gotcha that initially looked like a release miscompile

When stderr is redirected to a file (not a tty), glibc makes it **block
buffered**; `abort()` does not flush stdio buffers, so the tail of the log
(aspMain 720/740/760 + the `Failed to find function` line) was *lost* and the
file run appeared to stop at `aspMain 700` with a bare "dumped core". Under gdb
(stderr to the tty, line-buffered) the full sequence `…760 Broke` → `Failed to
find function at 0x801B9900` is visible, proving release reaches the same crash
point as debug. The `fflush(stderr)` added to the send_dl counter (below) makes
the file-captured count reliable too.

### Flags verification (the mission's -O2-UB check)

`-fno-strict-aliasing` (needed because the `MEM_*` macros type-pun rdram bytes)
is set on the two type-pun-heavy *generated* TUs:
- `mm_recompiled` (root CMakeLists.txt:43) — the 91 generated game TUs.
- `mm_rsp` (src/rsp/CMakeLists.txt:39) — RSPRecomp-generated aspMain.

The runtime (`ultramodern`/`librecomp`) does **not** set it, but its `MEM_*`
usage dereferences `uint8_t* rdram` (a char type) cast to `int32_t*`/etc.; char
lvalues may alias any type per the standard, so strict-aliasing is not a hazard
there. Empirically confirmed: Release reaches the *identical* crash point
(send_dl #770 / 0x801B9900) as Debug — no -O2 miscompile. No runtime flag change
needed (and none made — the runtime is a shared symlink, READ-ONLY by the lane
rules unless a fix is required, which it isn't).

## Task 2 — AI_LEN mirror

### Root cause / hardware semantics

`Sound_Update` (decomp asm/nonmatchings/music/Sound_Update.s, vram 0x800023F0)
is the game's **only** raw RCP-register touch. It reads `AI_LEN_REG` directly
instead of through `osAiGetLength`:
```asm
lui  $t9, 0xA450          # %hi(D_A4500004)
lw   $t1, 4($t9)          # AI_LEN_REG  -> $t1 = bytes remaining in AI DMA
...
srl  $t2, $t1, 2          # bytes -> stereo frames (÷4)
sw   $t2, 0($s6)          # gAudioSamplesLeft = remaining frames
```
Every other `D_A4*` access lives in libultra (`osAiSetNextBuf` etc.) that
librecomp replaces wholesale, so this one `lw` is the sole raw MMIO hit.

librecomp maps the N64 address space flat (`addr - 0x80000000`), so that `lw`
hits **rdram offset 0x24500004** — beyond committed `mem_size` but inside the
4 GB PROT_NONE reservation. Phase 2 committed that one page (`0xA4500000`,
0x1000) as zero-fill: `AI_LEN` always read 0 = "DMA drained", so the game's
audio pacing logic free-ran. (Phase 2's TODO called for trap-based MMIO
forwarding; this lane implements the simpler, sufficient mirror instead.)

### The mirror

`MEM_W` (recomp.h:95) is a **plain aligned host word load** — no byte swizzle
for word accesses (only `MEM_B`/`MEM_H` carry the `^3`/`^2` XOR). So writing a
plain little-endian `u32` to `rdram+0x24500004` is read back verbatim by the
recompiled `lw`. (`0x24500004` is 4-aligned, so the store is valid — no swizzle
crossing.)

The value mirrored is ultramodern's own view of the output buffer:
`mm_audio_input::get_frames_remaining()` returns stereo *frames* at the game's
sample rate; `× 4` = bytes (2 channels × `sizeof(s16)`), which is exactly
`AI_LEN`'s unit and exactly what the game's `srl 2` turns back into frames — so
`gAudioSamplesLeft` tracks the real DAC queue depth instead of always 0.

Implementation (permanent code, commented):
- **`src/audio_input/mm_audio_input.hpp`**: exposed `size_t get_frames_remaining();`
  (was file-static in the anonymous namespace of audio.cpp; moved to the
  `mm_audio_input` namespace so the host can link it. The `audio_callbacks()`
  struct still uses it, unchanged.)
- **`src/audio_input/audio.cpp`**: moved `get_frames_remaining` out of the
  anonymous namespace into `mm_audio_input` (still references the TU-private
  globals `g_audio_device`/`g_sample_rate`/etc.).
- **`src/game/register_overlays.cpp`** (`on_game_init`): after the existing
  `mprotect` of the AI register page, spawn a detached host thread that, every
  ~1ms while `!exited`, writes
  `*reinterpret_cast<volatile uint32_t*>(rdram + 0x24500004) = get_frames_remaining()*4`.
  - `volatile`: this is an MMIO register mirror — the store must issue every
    tick even under -O2.
  - **Teardown-safe**: `exited` (`extern std::atomic_bool`, set by
    `ultramodern::quit()` in librecomp/src/recomp.cpp:536) is polled each tick.
    rdram is `munmap`'d only *after* `ultramodern::join_event_threads()`
    (recomp.cpp:825-829), i.e. only after `exited` has been observed — so the
    mirror stops writing before rdram is freed. Detached: the process either
    quits cleanly (thread sees `exited`, returns) or is killed/aborts (thread
    dies with the process).
  - The Phase-2 `TODO(phase3): trap-based MMIO` comment is replaced with the
    implemented-mirror description.

### Verification (no regression)

Headless run with the mirror active (Debug build, 50s cap):
```
[mm_rsp] aspMain 760/760 Broke
Failed to find function at 0x801B9900
```
— same crash point (frame ~770 / 0x801B9900) as the documented bug#10
baseline. Release build (above) reaches the identical point. The mirror does
not change *where* the game crashes.

### Side effect: pacing fixed (17 fps → ~58 fps)

With `AI_LEN=0` the game's audio pacing free-ran and bug#10 measured ~17 fps
(770 frames in ~45s). With the mirror feeding a real `AI_LEN`, the frame loop
paces correctly and runs at the VI cap (~58 fps; 770 frames in ~13.6s). The
crash point is unchanged — the game's RLE overlay streaming still lands on
0x801B9900 at the same boot-progress frame — it just arrives sooner because the
frames themselves arrive sooner. This is the intended effect of making `AI_LEN`
real; it is not a regression (the gate is "same crash point", met).

## Instrumentation added (needed for the fps measurement)

The headless `StubRendererContext::send_dl` was an empty no-op
(`void send_dl(const OSTask*) override {}`). The `[gfx] send_dl #N` counter the
mission references lived only in `src/graphics/rt64_render_context.cpp`, which
is **not built** with `MM_BUILD_GRAPHICS=OFF` — so headless had no frame
counter. Added the equivalent counter (every 10, plus the first 3) to the stub,
with `fflush(stderr)` so the count survives the block-buffered-stderr-on-abort
gotcha. Mirrors the existing "TEMP DIAGNOSTIC" pattern in rt64_render_context.
`src/rsp/rsp_callbacks.cpp`'s `aspMain N/N Broke` counter was already present
and used as the cross-check (aspMain 760 ≈ send_dl 770, the 2-audio-per-frame
double-buffer path).

## What remains

- **Bug #11 (not lane-c):** the 0x801B9900 jump — code above the static `.main`
  section loaded by the game's own RLE streaming, which the runtime doesn't know
  about (no DMA hook fired, section never marked loaded). This is the next
  frontier; lane c only confirms release + mirror don't regress it.
- **Real-renderer fps:** this lane is headless-only (`MM_BUILD_GRAPHICS=OFF`).
  The RT64 build's fps (and the PHASE3 present-backpressure park) is out of
  scope here.
- **AI_LEN accuracy:** the mirror reports the *host* output queue depth
  (SDL `SDL_GetQueuedAudioSize` rescaled to the game rate), which is a good
  proxy for "bytes remaining in the AI DMA". A trap-based MMIO forward to
  ultramodern's own AI state (the Phase-2 TODO) would be cycle-exact but is
  unnecessary for correct pacing, as the 17→58 fps result shows.
