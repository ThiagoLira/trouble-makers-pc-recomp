# Performance profiling and optimization map

This project profiles the shipped architecture as one native process: the
statically recompiled game functions, N64ModernRuntime, the recompiled audio
RSP microcode, RT64, Vulkan driver calls, SDL, and worker threads all appear in
the same call tree. Sampling is the default because instrumenting every one of
the 30,000+ generated functions would materially change the workload.

## Quick start (Linux)

GNU gprofng ships with current binutils and can profile optimized, unmodified
C/C++ executables. `MM_PROFILE_BUILD=ON` adds debug information and preserves
frame pointers while keeping the chosen optimization level intact. The helper
selects `RelWithDebInfo`, builds `troublemakers`, warms up the process, captures
a bounded interval, and creates flat and call-tree reports:

```sh
tools/profile_recomp.sh --duration 45 --warmup 15 -- \
    input/troublemakers.us1.z64 --window 960x720 --no-widescreen

# Instructions/cycles and cache/branch-miss attribution:
tools/profile_recomp.sh --mode counters --duration 20 -- \
    input/troublemakers.us1.z64 --window 960x720 --no-widescreen
tools/profile_recomp.sh --mode cache --duration 20 -- \
    input/troublemakers.us1.z64 --window 960x720 --no-widescreen

# Scheduler waits, allocation traffic, or I/O:
tools/profile_recomp.sh --mode sync --duration 20 -- ROM [game options]
tools/profile_recomp.sh --mode heap --duration 20 -- ROM [game options]
tools/profile_recomp.sh --mode io   --duration 20 -- ROM [game options]
```

Experiments and reports land under `build-profile/profiles/`, which is already
ignored as part of the build tree. The default run uses
`build-profile/profile-config/` so a profiling session cannot mutate the
player's normal config or save. Pass `--use-user-config` only when a specific
existing save is required for the scenario.

Each experiment also retains `run.log` and `environment.txt`. The latter
records the exact command, repository revision/dirty count, display session,
Vulkan ICD overrides, CPU, compiler, profiler version, and `vulkaninfo`
summary. Check both files for swapchain/import errors and confirm the intended
GPU before accepting a renderer result.

The reports distinguish exclusive time (work in that exact function) from
inclusive time (the function plus callees). Generate the navigable source and
disassembly view with:

```sh
tools/profile_recomp.sh --report build-profile/profiles/cpu-*.er --html
```

The profiler intentionally does not force a video or audio driver. Set the
same environment used for the release scenario being measured. For a
translated-game-only comparison without a GPU, make a separate headless build
with `MM_BUILD_GRAPHICS=OFF`; do not compare its absolute frame time with a
rendered build.

The current runtime emits a low-overhead `[perf]` snapshot every five seconds
without an environment flag. It reports frame cadence and tails, display-list
and screen-update CPU time, RT64 queue depths, compiled specialized shaders,
texture-cache occupancy/upload backlog, the actual render-target size and AA
mode, and process RSS/peak RSS/thread count on Linux and Windows. A companion
`[present]` record comes from RT64's real presentation thread. It reports
native/interpolated/skipped frames, measured present cadence and p95/p99 tail,
late and missed deadlines, queue skips/failures, and the CPU time blocked in
swap-chain acquisition, interpolation availability, the GPU fence, present
wait, pacing sleep/overshoot, and the platform `Present` call. The hot paths
only update relaxed atomics; formatting and file output happen outside the
presentation thread. GPU-fence wait time can expose a GPU bottleneck but is
not a full GPU workload duration; Vulkan/D3D timestamp queries or a vendor
profiler are still required for exact GPU time.

The snapshots are retained in `logs/latest.log` and `logs/previous.log` below
the app config folder. After reproducing a user-only slowdown for at least 15
seconds, relaunch, open **Support**, and use **Copy previous session**. That is
the preferred first diagnostic because it also includes the selected GPU and
driver, display backend/settings, audio health, watchdog progress, lifecycle,
and scene transitions.

The underlying tools and flags are documented by the
[GNU gprofng manual](https://sourceware.org/binutils/docs/gprofng.html).
Gprofng can sample a production executable without recompilation; debug
information adds source attribution, while its hardware-counter, sync, heap,
and I/O modes answer different questions.

## Findings: 2026-07-20--21

### Pre-fix CPU diagnostic

The first capture exposed a CPU scheduler problem, but it is deliberately not
the graphics baseline. It forced the Radeon ICD into an X11 run while the
desktop and display were attached to the NVIDIA GPU under Wayland. Later runs
reported repeated swapchain failures. Those attempts are discarded for
renderer and frame-time conclusions; only the runtime call tree below is
retained. The loop is independent of the Vulkan implementation and was
confirmed by its message-flow control path.

| Exclusive CPU | Function | Interpretation |
|---:|---|---|
| 10.534 s | `ConcurrentQueue::try_dequeue` | external-message queue drain |
| 7.016 s | `dequeue_external_messages` | drain-loop body |
| 0.941 s | `get_or_add_implicit_producer` | re-enqueuing blocked messages |
| 0.617 s | `do_send` | retrying delivery to N64 queues |
| 0.105 s | `aspMain` | recompiled audio RSP work |

Total sampled CPU time was 21.099 seconds. The queue functions above account
for 19.108 seconds (90.6%) when summed by exclusive symbol; `pause_self`'s call
tree attributes virtually all of that work to repeated external-message
drains. Recompiled game functions and RT64 CPU processing were masked by this
loop.

### Current-main post-fix capture

The replacement baseline was rerun after fast-forwarding to `origin/main` at
`5d2854c` (v0.3.5). It used the actual display stack reported by `vulkaninfo`:
Wayland and the NVIDIA GeForce RTX 5090 with proprietary driver 610.43.03. The
helper isolated both config path mechanisms used by the current launcher. The
run used 960x720, 4:3 presentation, MSAA 1x, SSAA 1x, a 3-second warm-up, and
30 seconds of clock sampling. It initialized the NVIDIA driver without
swapchain/import errors.

Total sampled CPU was 1.653 seconds across all threads. The old hotspot,
`dequeue_external_messages`, accounted for 0.007 second exclusive and 0.025
second inclusive. The highest resolved project functions were:

| Exclusive CPU | Inclusive CPU | Function |
|---:|---:|---|
| 0.265 s | 0.265 s | `RT64::RDP::loadBlockOperation` |
| 0.262 s | 0.375 s | `aspMain` |
| 0.233 s | 0.284 s | `RT64::TMEMHasher::hash` |
| 0.014 s | 0.014 s | `PresetDrawCallLibrary::findMaterialsInCache` |
| 0.010 s | 0.294 s | `TextureManager::uploadTexture` |
| 0.007 s | 0.025 s | `dequeue_external_messages` |

The dominant resolved project call path is now the graphics thread:
`send_dl` (0.723 s inclusive), `State::fullSync` (0.720 s), and
`fullSyncFramebufferPairTiles` (0.568 s). NVIDIA's driver contributed the
largest unresolved native symbol (0.127 s exclusive, 0.325 s inclusive). This
is a startup/attract sample, so the absolute values rank investigation targets;
they do not establish a general frame-time improvement.

After the startup window, all six telemetry windows held 60.0 VI/s and
60.0 display lists/s with 16.67 ms average frame intervals and no slow frames.
Display-list submission averaged 2.49--3.37 ms, renderer queue depths and
pending uploads remained zero, texture allocation remained bounded at 230
slots, threads remained at 65, and RSS moved from 442.1 to 446.0 MiB. That is
stable over this short run, but it is not a substitute for an affected user's
long-session report.

### Current-main traversal and display-rate soaks

The follow-up workload warped to progression Stage 3 and continuously crossed
the same streaming region in both directions. All accepted NVIDIA runs forced
`nvidia_icd.json` and the application itself identified an NVIDIA GeForce RTX
5090 on proprietary driver 610.43.03. This avoids treating a loader-visible
adapter or a failed swapchain as the device that actually rendered the test.

The longest run used a 1600x900 widescreen window and completed 15 minutes in
one process. Its unpositioned XWayland window resolved to SDL display 0 at 60
Hz, so it is a sustained 60 Hz result despite the physical output supporting a
higher mode. Excluding the initial incomplete and shutdown windows, the 178
five-second samples (890 seconds) measured:

| Signal | Observed behavior |
|---|---|
| Game cadence | 60.001 VI/s and 60.002 display lists/s on average |
| Frame interval | 16.67 ms average and 17.00 ms p95 in every window; zero >20 ms or >33 ms frames |
| Display-list CPU | 1.43--1.85 ms window averages; 2.89 ms largest individual sample |
| Workload/present queues | depth zero in every sample |
| Texture uploads/shaders | upload backlog zero; zero specialized shaders under the Blackwell workaround |
| Texture cache | 447 to 475 allocated slots while new scene content was encountered; resident entries continued to fall and rise instead of growing monotonically |
| Process resources | 361.4 to 366.0 MiB RSS, 64 threads throughout; the final five minutes were 364.8--366.0 MiB while only two new texture slots were allocated |

The small RSS steps track newly encountered texture slots rather than a rising
queue, thread leak, or worsening frame time. This run does not reproduce a
slowdown with elapsed time.

A separate positioned window selected SDL display 1 at 144 Hz and exercised
RT64's display-rate interpolation for 290 measured seconds. Game logic remained
exactly 60.000 VI/s, as designed, while RT64 reported the 144 Hz output. There
were no slow frames or queue/upload backlogs. Display-list CPU window averages
were 1.38--1.77 ms; the largest individual sample was 11.18 ms, still inside
the 16.67 ms native update budget. RSS rose 2.8 MiB while texture slots grew
from 447 to 471, then stabilized.

For a lower-end renderer check, a headless Gamescope compositor and the game
were both pinned to the integrated `RADV RAPHAEL_MENDOCINO` Vulkan device
(vendor `0x1002`, Mesa 26.1.4). The monitors on the test machine are attached
to NVIDIA, so a direct RADV/XWayland attempt could not create a swapchain and
was rejected. The accepted compositor path created a real 640x480 swapchain on
RADV; its PipeWire output was captured to confirm that it contained the live
Stage 3 scene rather than a black or stale buffer. Across 290 measured seconds
at native 60 Hz it averaged 60.000 VI/s and 60.003 display lists/s, recorded no
slow frames, kept both renderer queues and the upload queue empty, compiled 12
specialized shaders, and used 97.4--98.4 MiB RSS. Display-list CPU window
averages were 1.18--1.57 ms with a 2.50 ms maximum sample.

An intentionally extreme follow-up confined all 58 game and Vulkan-driver
threads to one logical CPU while leaving only the tiny Gamescope compositor
unrestricted. Across 170 measured seconds it still averaged 60.000 VI/s and
60.006 display lists/s. Six of roughly 10,200 native frames exceeded 20 ms,
none exceeded 33 ms, p95 was 19--20 ms, and the renderer queues briefly reached
depth 1 before draining. RSS stayed within 96.3--97.1 MiB. This is not a model
of a specific consumer CPU, but it confirms that the former one-core busy loop
is gone and shows the expected tail-latency limit under forced contention.

The integrated GPU was not limited to the low-resolution case. A 1280x720
widescreen run completed 110 measured seconds at 60.000 VI/s with no slow
frames or queueing; display-list CPU averages were 1.62--1.93 ms and its
largest sample was 2.93 ms. Separate 50-second passes exercised 2x SSAA and 4x
MSAA (the launcher intentionally makes those modes mutually exclusive). Both
held 60.000 VI/s with no slow frames or queueing. The 4x-MSAA process used
about 10 MiB more RSS than the 1x/2x-SSAA cases because of its multisampled
render targets.

These CPU numbers are host submission time, not GPU timestamps. The integrated
run is useful evidence for low-end scheduling, shader compilation, cache
boundedness, and actual rendering, but its headless final presentation is not
a physical-monitor latency measurement.

### Final post-fix traversal soak

After adding duplicate-event coalescing and bounding RT64's wing-clear
render-target bookkeeping to one submission, the final binary repeated the
Stage 3 bidirectional streaming workload for five measured minutes. It used a
1280x720 widescreen window on the NVIDIA GeForce RTX 5090/610.43.03 device,
attached to the 144 Hz display but explicitly targeted to the game's native
60 Hz. Across all 60 complete five-second running samples:

| Signal | Final result |
|---|---|
| Cadence | 60.0 VI, screen updates, display lists, and audio tasks per second in every sample |
| Frame tails | 16.67 ms average, 17.00 ms p95, zero >20 ms or >33 ms frames |
| Display-list CPU | 1.57--1.86 ms sample averages; 3.15 ms largest individual submission |
| Renderer state | workload/present depth zero and texture-upload backlog zero throughout |
| Texture cache | allocated slots 447 to 469 as content was encountered; resident entries continued to fall and rise |
| Process resources | RSS 384.1 to 386.4 MiB; 66 threads throughout |

The final samples remained in the same CPU/RSS band as the middle of the run;
there was no cadence loss or increasing queue/retry cost with elapsed time.
The bounded slot increase is accompanied by active resident-entry reuse, not a
monotonic live-texture backlog. The harness ended the process with its expected
timeout after 310 wall-clock seconds; the shutdown snapshot is excluded from
the table.

### 4:3 left-edge visual regression

A dense 180-frame native-60 capture reproduced the reported left-edge stretch
while a large tree crossed the viewport: frames 14--17 contained horizontal
texture bands extending to the far-left edge. Reverting the existing RT64
clipping patch removed those bands but restored the older solid-color slab, so
neither behavior was acceptable.

The faulty path manually clamped a negative texture-rectangle X coordinate to
zero and separately advanced its starting texture coordinate. That changed the
rectangle's interpolation and produced the bands. The fix instead decodes the
host-expanded tile hooks' two's-complement 12-bit X fields and submits the
original negative geometry and texture coordinates. RT64's existing rectangle
scissor then clips the geometry and interpolated texture together.

The exact Stage 2 failure window is clean in final 120-frame native-60 and
120-frame 144 Hz display-rate replays. A separate final 100-frame native-60
walk in Stage 3 moved trees and block structures across the left boundary;
every inspected frame was clean. These are ordinary walking inputs after a
short settle, not a fast jet through the scene. MSAA 4x and SSAA 2x Stage 2
passes also remained clean.

### Immediate slowdown reports

Reports that the recomp is slow from startup have four concrete explanations
that can vary by machine even when a high-end development system looks fine:

1. The pre-game launcher requested SDL VSync but had no independent frame
   limit. If accelerated renderer creation fell back to software, or a display
   backend ignored the VSync flag, its UI loop ran without a bound from the
   instant the window opened. It now logs the SDL renderer/flags and enforces
   a 16 ms minimum frame interval even when VSync is unavailable. Software
   fallback is capped at 2x UI scale, becomes event-driven after its first
   frame, and cannot redraw above 10 FPS during event storms. SDL documents
   both the [renderer capability flags](https://wiki.libsdl.org/SDL2/SDL_RendererFlags)
   and that [`SDL_WaitEvent`](https://wiki.libsdl.org/SDL2/SDL_WaitEvent)
   blocks until an event is available. Controller-axis
   events are still processed so the current main branch's binding UI works.
   A forced-software local test fell from approximately one saturated CPU core
   before pacing to 0.43 CPU-second over 10.17 seconds after these changes,
   while still responding immediately to input.
2. The blocked-message loop above consumes approximately one complete CPU
   core. That is easy to overlook on the 32-thread Ryzen 9 9950X used for the
   local trace, but it is a large fraction of a 4- or 8-thread machine and can
   interfere with graphics-driver and audio scheduling. Patch 0006 removes
   the wake loop, and patch 0008 bounds deferred duplicate signals.
3. The Linux NVIDIA workaround originally enabled RT64's slower ubershader-only
   path on every NVIDIA GPU with a 610-series-or-newer driver, although the
   corruption it addresses was observed on Blackwell. A 5090 can hide that
   cost; an older GeForce on the same driver cannot. The host now restricts
   the automatic workaround to GeForce RTX 50-series/Blackwell device names,
   while `MM_RT64_UBERSHADERS_ONLY=0` and `=1` remain available for A/B tests.
   [`0003-skip-specialized-shaders-in-ubershader-mode.patch`](../patches/rt64/0003-skip-specialized-shaders-in-ubershader-mode.patch)
   also stops compiling pipelines that the forced path can never use.
4. Vulkan can fall back to llvmpipe/lavapipe/SwiftShader when the real ICD is
   unavailable, or a hybrid system can expose an unexpected adapter. Startup
   now prints the selected API, device, vendor, raw driver version, and VRAM,
   and emits an explicit warning for known software Vulkan device names.

The obsolete per-message `MM_EVENT_TRACE` instrumentation is removed by patch
0007. Current main's session logger remains suitable for release because its
hot paths only update counters and the background reporter writes one bounded
snapshot every five seconds.

To collect a lightweight affected-machine log without a sampling profiler:

Run the packaged game normally, remain in the affected scene for 15 seconds or
longer, close it, relaunch, and copy the previous-session report from the
Support tab. Check the `[gfx] api=... device=... driver=...` line, MSAA/SSAA
line, and whether software Vulkan was reported. In each `[perf]` record,
`renderer-queues=workload/present` exposes backpressure, `dl=.../s` should
normally track the game cadence, and `shaders=0` is expected when Blackwell's
ubershader-only workaround is active. `textures=resident/slots` and `uploads=`
expose texture-cache growth or a stuck upload producer; `rss-mib`,
`peak-rss-mib`, and `threads` expose process-level growth on Linux and Windows.
Large `dl-cpu-ms` points to host display-list processing instead. In the
`[present]` line, compare `rate` with `target`, then inspect `late/missed`,
interpolated/skipped frames, and the per-stage wait maxima. A large `gpu` wait
implicates unfinished rendering; `acquire`, `swap`, or `present` implicates the
swap-chain/display path; interpolation wait and skipped counts implicate frame
generation; pacing overshoot implicates host scheduling.

The current-main local startup check selected the NVIDIA GeForce RTX 5090 on
Wayland, emitted no swapchain/import errors, sustained 60 display lists per
second, kept both queue depths at zero, and kept `shaders=0` under the
Blackwell workaround. After the first interval, display-list CPU averages were
2.49--3.37 ms with 5.86--6.93 ms maxima in that attract-mode segment.

### Windows v0.3.6 affected-machine report

The July 21 Windows report is valuable precisely because it does **not**
support several tempting explanations. With native 60 FPS and 4x MSAA, the
reported Stage 2 session held 60.0 VI, screen, display-list, and audio tasks per
second. Frame p95 stayed at 18 ms, display-list CPU generally stayed near
2.7--3.3 ms, both renderer queues stayed empty, and pending texture uploads
were always zero. The audio queue never emptied after warm-up and reported no
queue errors. PCM clipping indicates saturated samples, not an audio-buffer
underrun, so the clipping counts do not explain a video hitch. Likewise,
`textures=resident/slots` fluctuating under a bounded 478-slot allocation is
cache reuse, not proof that textures were synchronously loaded at each hitch.

There were sparse 20--20.5 ms frame intervals, including a small cluster late
in the run, but v0.3.6 measured the 60 Hz host call into `updateScreen`, not the
actual D3D12 presentation thread. It therefore could not distinguish a GPU
fence wait, frame-latency wait, `Present` delay, or missed interpolated frame.
It also printed zero RSS/thread data on Windows. The new `[present]` counters
and Windows process metrics close those blind spots; a future affected log can
attribute the hitch instead of inferring it from unrelated audio amplitudes or
texture counts.

The report also exposed two concrete configuration/diagnostic problems:

- A saved 3840x2160 fullscreen size was used to create RT64 on a 1920x1080
  desktop. RT64 initially reported 9x scale, then resized to 5x. With 2x SSAA,
  the old `scale=` field omitted the extra supersampling multiplier, hiding an
  even larger transient allocation. Fullscreen creation now uses the selected
  monitor's current mode before RT64 starts, while preserving the saved size
  for windowed mode. The log now reports RT64's actual render-target dimensions
  and downsample/MSAA state.
- SDL's `SDL_AudioCVT::len_ratio` produced an uninitialised, enormous value in
  one Windows log. Diagnostics now print the deterministic output/input sample
  rate ratio instead. This was a logging defect, not evidence of a resampler
  timing failure.

The user's A/B result is also actionable: 4x MSAA/native 60 felt substantially
better than 2x SSAA/high-rate interpolation, while AA itself made little visible
difference. The default remains native 60 with AA off. When hardware headroom
exists, spend it on frame interpolation before AA; the safe higher-FPS launcher
preset targets 120 FPS with AA off rather than automatically requesting 240.
SSAA plus interpolation is explicitly flagged as a very-high-cost combination.

### Historical pre-rebase long-session soak

Before updating to current main, a 93-second gprofng soak (90 seconds requested
plus collector shutdown) used the same pinned NVIDIA/Wayland device and
advanced through 5,335 display lists before collection stopped. Excluding the
collector-stop transition, the 89 once-per-second samples showed:

| Signal | Observed behavior |
|---|---|
| Game cadence | 60 display lists/second after the first 55-list interval |
| Workload/present queues | depth zero in every sample |
| Display-list CPU after gameplay scene load | 3.998--4.322 ms average per interval |
| Texture cache | reached 323 allocated slots, then reused/evicted them; upload backlog always zero |
| Specialized shaders | zero under the Blackwell ubershader workaround |
| Process RSS | 378.3 MiB at startup, 389.6 MiB at 89 seconds; flat at 389.5--389.6 MiB for the final 17 seconds |
| Threads | constant at 63 |

This does not reproduce a degradation with elapsed time: scene complexity
raised display-list work, then it stabilized; memory, threads, texture slots,
and queues also stabilized. It is not proof against an hours-long or
stage-specific leak. The final collection transition produced a 1.3-second
outlier and temporary queue depth immediately as gprofng stopped the target;
the following interval recovered, so it is excluded from application results.

The soak also found that the disabled temporary `MM_EVENT_TRACE` predicate
still consumed 0.185 sampled CPU-second. Patch 0007 removes that obsolete
send/receive/completion tracing and its per-message path strings entirely.
Affected-user logs should now use current main's automatic five-second session
snapshots, whose structured fields cover the relevant long-run failure modes.

A subsequent 15-second correct-NVIDIA profile validated that cleanup: the
trace symbol disappeared, `dequeue_external_messages` measured 0.003 second
exclusive/0.008 inclusive, cadence remained 60 display lists per second, and
both renderer queue depths remained zero.

### P0, implemented: remove the blocked-message busy loop

This is a measured problem, not a hypothesis. `wait_for_external_message()`
blocks for one new host event, then drains all producer sub-queues to avoid a
known DP-message starvation failure. When an event targets a full N64 message
queue and has `requeue_if_blocked`, it is placed back into the same blocking
concurrent queue. `pause_self()` immediately wakes on that same undeliverable
event and repeats the cycle, burning a core.

[`0006-Defer-blocked-external-messages-without-busy-waking.patch`](../patches/N64ModernRuntime/0006-Defer-blocked-external-messages-without-busy-waking.patch)
keeps failed deliveries in a game-thread-owned deferred list rather than in
the host wake-up queue:

1. A newly arrived host event still wakes `wait_for_external_message()`.
2. The full producer drain still visits VI, SP, DP, AI, SI, PI, and timer
   streams, preserving the starvation fix.
3. Failed deliveries move to the deferred list and are retried when a fresh
   host event arrives or whenever a game thread enters an OS message API.
4. The deferred list is never itself a wake source, so a full destination
   queue cannot spin `pause_self()`.
5. [`0008-coalesce-duplicate-deferred-host-events.patch`](../patches/N64ModernRuntime/0008-coalesce-duplicate-deferred-host-events.patch)
   retains at most one identical queue/message/jam signal, so an abandoned
   full destination cannot trade the busy loop for unbounded memory and retry
   work. N64 event sends are non-blocking, so retaining one occurrence already
   provides a stronger guarantee than dropping every signal received while
   full.

This also removes the implicit-producer lookup and repeated temporary-vector
allocation from the hot idle path. The optimized build succeeds, the clean
NVIDIA run continued to deliver matching display-list and audio completions,
and the drain dropped from 7.016 to 0.001 exclusive CPU-second across the two
diagnostic captures. The queue uses independent producer sub-queues and its
documentation cautions that `try_dequeue` can observe apparent emptiness; the
full drain is therefore retained rather than replaced with a single wait
result. See the
[ConcurrentQueue design notes](https://github.com/cameron314/concurrentqueue#reasons-not-to-use).

### P1: measure the RT64 TMEM path before changing it

`TextureManager::uploadTexture` hashes the relevant TMEM bytes before checking
whether the resulting hash is already in `hashSet`. Static textures can
therefore be hashed again even though their GPU upload is cached. A memoized
hash is plausible, but it must be keyed by every hash input and invalidated on
all block, tile, and TLUT writes. Palette-dependent CI textures make a coarse
or incomplete invalidation scheme unsafe.

First add low-overhead counters for hash calls, bytes hashed, unique hashes,
GPU uploads, and load-operation type. If repeated-hash rates justify it, test
one of these designs behind an experiment flag:

- Cache by a TMEM mutation generation plus the load-tile, size, TLUT, width,
  and height inputs. Increment the generation on every TMEM or palette write.
- Reuse a hash only inside a full-sync interval proven to contain no
  intervening overlapping load operation.
- Specialize the common non-RGBA32 `loadBlockOperation` copy/swizzle loop only
  after counters identify its sizes and alignment; compare generated assembly
  and cache misses before keeping a SIMD path.

The current upper bound is small in this scene: hashing plus block loading
used 0.078 sampled CPU-second over eight seconds. Correctness risk outweighs
blindly adding a complex cache without scene coverage and A/B measurements.

The final 20-second symbolized Stage 3 sample, taken after all fixes, reached
1.293 seconds of aggregate CPU across the process. Its largest resolved
project symbols were `TMEMHasher::hash` (0.176 s exclusive),
`loadBlockOperation` (0.127 s), and `aspMain` (0.116 s). In contrast,
`dequeue_external_messages` used 0.003 s exclusive/0.010 s inclusive and the
once-per-second renderer telemetry was below the resolved hot set. This
confirms that the former scheduler loop stayed removed and that any next CPU
optimization should measure duplicate TMEM work before changing rendering
semantics.

A follow-up five-second hardware-counter capture reinforces that direction.
The profiler attributed the largest project instruction counts to
`loadBlockOperation`, `aspMain`, and `TMEMHasher::hash`. The load/hash routines
showed high estimated IPC and essentially no attributed cache misses in the
four-counter sample, while most sampled cache and branch misses landed in the
NVIDIA driver, libc/SDL, and synchronization paths. Gprofng had to multiplex
generic events on an otherwise unidentified AMD PMU, so treat exact counts as
estimates; the useful conclusion is that the project hot loop looks
instruction-throughput-bound, not like a cache-latency emergency. Reduce
duplicate work and instructions before attempting prefetch or data-layout
changes.

### P2: broaden the workload and collect causal profiles

The next capture set should include:

- CPU clock sampling to expose the real translated-game and RT64 hot set.
- `counters` for per-function IPC and cycles. Low IPC in a large translated
  function suggests cache/memory pressure; high IPC plus high cycles suggests
  sheer instruction volume.
- `cache` to distinguish instruction-footprint pressure from data-cache and
  branch-prediction costs.
- `sync` to identify renderer fences or runtime scheduling waits. Wait time is
  not CPU time, but it explains frame latency and pipeline bubbles.
- `heap` over a stable gameplay interval, looking especially for per-frame
  RT64 and host glue allocations.

Capture at least three scenarios: a sprite-heavy 2D stage, a textured 3D/boss
scene, and a loading/transition interval. A single attract-mode profile cannot
represent all overlays and renderer paths.

### P3: compiler experiments, only after stable baselines

The generated game is split across many translation units and linked as a
whole archive. Interprocedural optimization may inline hot direct calls and
remove cross-TU overhead; profile-guided optimization can additionally improve
branch decisions and lay out the actually used function set. Both are
plausible for a large machine-generated binary, but neither is automatically a
win: link cost, binary size, instruction-cache locality, overlay coverage, and
untrained paths all matter.

Run these as controlled A/B experiments rather than release defaults:

- IPO/LTO via CMake's `INTERPROCEDURAL_OPTIMIZATION`, after
  `CheckIPOSupported` succeeds.
- GCC `-fprofile-generate` followed by representative training and
  `-fprofile-use`; include all major game/overlay and renderer scenarios.
- A target-specific RSP build (`x86-64-v3`/AVX2 on supported machines) versus
  the portable SSE4.1 baseline. The initial sample says `aspMain` is too small
  to justify reducing release CPU compatibility yet.
- Function-layout and size comparisons. The translated executable has tens of
  thousands of native functions, so instruction locality can matter more than
  an isolated instruction-count reduction.

GCC documents how [LTO merges translation units and how profile feedback
drives function reordering and other optimizations](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html).
CMake's portable enablement path is
[`CheckIPOSupported`](https://cmake.org/cmake/help/latest/module/CheckIPOSupported.html).

## Measurement discipline

- Compare the same scene, resolution, MSAA/SSAA, aspect mode, Vulkan device,
  audio backend, build type, warm-up, and capture duration.
- Keep symbols and frame pointers in both sides of an A/B test; do not compare
  a Debug build with Release.
- Use at least five runs for frame-time/counter claims and report median plus a
  tail measure (p95 or p99), not only an average.
- Separate CPU utilization, wall/frame latency, and GPU duration. Lower CPU
  use during a blocking wait is good for efficiency but does not prove a
  shorter frame.
- Treat profiler overhead as mode-specific. Heap and full synchronization
  tracing perturb the target more than clock sampling; use them to locate a
  cause, then confirm with the low-overhead CPU/counter modes.
