# PHASE6_NOTES_b — Lane B: clean widescreen wings at the renderer

**Mission.** With `--widescreen` (RT64 `AspectRatio::Expand`) the render field
widens, but the game only paints its 4:3 region: the side "wings" accumulate
**stale** framebuffer content (sprites/scenery that crossed the 4:3 edge freeze
there). Lane b's job: make the wings non-broken **today**, at the renderer
level — the extended regions must not accumulate stale content. Deliver an
env-gated implementation (`MM_CLEAR_WINGS=1`) + driver visual-verification
instructions.

**Gate: PASSED** (with an explicit caveat — see *Verification status*).

```
RESULT
  gate: PASSED
  summary:
    - Wing-clear implemented in RT64's FramebufferRenderer::recordFramebuffer + wired via
      MM_CLEAR_WINGS=1 in the mm_graphics glue; compiles, links into mm_game; default-off = zero behavior change.
    - Pixel verification is the driver's by design (sandbox has no Vulkan GPU); exact on-screen look-fors below.
```

---

## 1. Mechanism map (verified by reading RT64 @ 23cab603)

### 1a. What `AspectRatio::Expand` actually does

`WorkloadQueue::threadConfigurationUpdate` (`src/hle/rt64_workload_queue.cpp:131-151`)
sets `aspectRatioTarget = swapChainW / swapChainH` (clamped ≥ 4:3), then
(`:206-207`):

```
aspectRatioScale   = aspectRatioTarget / aspectRatioSource   // source = 4/3
resolutionScale    = { resolutionMultiplier * aspectRatioScale,   // X
                       resolutionMultiplier }                   // Y
```

The HD color render-target width is
`fbWidth * resolutionScale.x = fbWidth * resMult * aspectRatioScale`
(`FramebufferManager` → `RenderTarget::computeScaledSize`,
`src/hle/rt64_framebuffer_manager.cpp:158`). **So in Expand mode the HD color
target is genuinely wider than the game's 4:3 framebuffer.** The wings are real
HD pixels that exist beside the 4:3 content.

### 1b. Why the wings go stale (the crux)

Two different scale factors are applied to rects in
`FramebufferRenderer` (`src/render/rt64_framebuffer_renderer.cpp`):

* **3D projections** use `aspectRatioScale` (the *stretch*): the projection
  matrix's X axis is multiplied by `aspectRatioScale`
  (`rt64_projection_processor.cpp:11-16`), so 3D geometry widens and **fills
  the wings with fresh geometry each frame**.
* **2D rects** (fillrect/texrect/scissor/viewport) use `invRatioScale =
  1/aspectRatioScale` (`:1566, :1656, :1706, :1728`). With origin
  `G_EX_ORIGIN_NONE`, `convertFixedRect` (`:55-86`) maps a 0..320 FB rect to a
  **centered** HD region of width `320 * resMult` (i.e. the 4:3 size, centered
  in the wider target). The wings on either side are never addressed by 2D.

A texrect whose FB coords run **past x=320** (a sprite bleeding off the right
edge) still maps into the right wing HD space (the beyond-320 portion lands
beyond the centered 4:3 right edge). Those wing pixels are painted by the
sprite this frame, but **never cleared** (the game issues no full-target
clear that reaches the wings), so when the sprite moves back the old pixels
**freeze** — exactly the observed stale-wing symptom.

There is one exception (`:1571-1572`): a fillrect that *spans the whole scissor
width* gets `invRatioScale = 1.0` and is stretched to fill the **entire** target
(wings included). So a game-issued full-screen background fillrect *would* clear
the wings itself. The stale-wing symptom proves Mischief Makers does **not** rely
on such a clear every frame — confirming the wing-clear injection is the right
fix and is safe (it only touches pixels the game never rewrites that frame).

### 1c. Where RT64 already clears (and why it's not enough)

`recordFramebuffer` (`:1255`) sets up each color target's render pass:
barrier color → `COLOR_WRITE` (`:1269`), `setFramebuffer(colorDepthWrite)`
(`:1276`), then the scene draws (`:1277+`). The render pass uses a **LOAD**
(preserve) op — N64 games depend on framebuffer persistence — so wing pixels
are never implicitly cleared. Explicit `clearColor()` calls exist only for the
RT interleaved-scene scratch targets (`:1297`) and for the game's own `FillRect`
draw calls inside `submitRasterScene` (`:623-633`). **No path clears the wings
on a non-fullscissor-clear frame.** That is the gap lane b fills.

### 1d. Why a synthetic-DL glue approach was rejected

The brief offered an alternative: feed `processDisplayLists` a host-built
prologue DL with a fillrect covering the wings. This does **not** work for this
game:

* `processDisplayLists` (`src/hle/rt64_application.cpp:393-423`) interprets the
  DL **from RDRAM** (`memory[dlStartAddress]`) — a synthetic DL would have to
  live in RDRAM and would race the game's own DL submission ordering (clear
  must precede the game's draws for the *displayed* VI-origin buffer; the game's
  copy-to-VI ordering is unknown from the glue).
* A **standard F3D** `G_FILLRECT`/`G_SETSCISSOR` goes through the same
  `convertFixedRect` with `invRatioScale` and is **centered** — it physically
  **cannot address the wing HD region** (FB coords beyond 320 are not a defined
  wing region under the non-extended mapping). Reaching the wings requires the
  **extended GBI** (`G_EX_FILLRECT_V1` + `G_EX_SETSCISSOR_V1` with
  `G_EX_ORIGIN_LEFT/RIGHT`, `include/rt64_extended_gbi.h`). gspFast3D doesn't
  emit gEX; hand-encoding gEX byte-perfect + unverifiable = high risk.

The renderer-side clear is strictly cleaner: it operates in HD space directly,
uses RT64's own supported rect-`clearColor`, needs no DL semantics, and is
trivially gated.

---

## 2. The implementation (env-gated, default-off)

Privatized `lib/rt64` (`cp -rL` from the shared upstream into the worktree;
the symlink is replaced by a real tree so the patched sources compile into the
static lib that `mm_game` links). **Nothing is committed.**

### 2a. Plumbing the flag (follows the existing `fixRectLR` pattern exactly)

1. `src/common/rt64_enhancement_configuration.h` — `Rect::clearWings` (bool).
2. `src/common/rt64_enhancement_configuration.cpp` — ctor inits `rect.clearWings = false`.
3. `src/hle/rt64_workload_queue.h` — `WorkloadConfiguration::clearWings` (bool, default false).
4. `src/hle/rt64_workload_queue.cpp:250` — `workloadConfig.clearWings = ext.sharedResources->enhancementConfig.rect.clearWings;`
5. `src/hle/rt64_workload_queue.cpp:623` — `drawParams.clearWings = workloadConfig.clearWings;`
6. `src/hle/rt64_state.cpp:1250` — debugger path sets `drawParams.clearWings = false;` (that path forces aspectRatio 1:1 anyway, so it's a no-op there).
7. `src/render/rt64_framebuffer_renderer.h` — `DrawParams::clearWings` (bool); `Framebuffer::clearWings` (bool, default false) **and** `Framebuffer::aspectScale` (float, default 1.0).
8. `src/render/rt64_framebuffer_renderer.cpp` `addFramebuffer` — stores `framebuffer.clearWings = p.clearWings;` and `framebuffer.aspectScale = (p.aspectRatioSource>0)? p.aspectRatioTarget/p.aspectRatioSource : 1.0f;` (plain floats — avoids the hlslpp `float2` division that returns `float1`).

### 2b. The clear itself — `recordFramebuffer`, after `setFramebuffer`, before scene draws

```cpp
if (framebuffer.clearWings && colorTarget && colorTarget->width>0 && colorTarget->height>0) {
    const float aspectScale = framebuffer.aspectScale;
    if (aspectScale > 1.0f + 1e-4f) {
        const int32_t fullW = (int32_t)colorTarget->width;      // HD target width
        const int32_t fullH = (int32_t)colorTarget->height;
        const int32_t innerW = (int32_t)std::lround((float)fullW / aspectScale); // 4:3 HD width
        const int32_t leftEdge  = (fullW - innerW) / 2;          // centered
        const int32_t rightEdge = leftEdge + innerW;
        const RenderColor wingColor(0.0f, 0.0f, 0.0f, 1.0f);     // [MM] tweak here
        if (leftEdge  > 0)  clearColor(0, wingColor, &RenderRect(0,0,leftEdge,fullH), 1);
        if (rightEdge < fullW) clearColor(0, wingColor, &RenderRect(rightEdge,0,fullW,fullH), 1);
    }
}
```

**Why this placement is correct:**

* **Before the scene draws** (`:1277` loop) → the game's widened 3D geometry
  redraws fresh into the wings over the clear, and 2D stays 4:3-centered. Only
  pixels the game never rewrites that frame (the frozen stale) stay cleared.
  Net: wings show either fresh 3D geometry (where 3D covers them) or the clear
  color (where nothing draws) — **never stale**.
* `vkCmdClearAttachments` (the Vulkan impl of `clearColor`,
  `src/vulkan/rt64_vulkan.cpp:2852-2871`) clears the **explicit rects clamped
  to the render area** — it is **independent of any bound scissor/viewport**,
  so issuing it before `setScissors` is safe. `clearCommonRectVector` clamps to
  the framebuffer dims, so out-of-range rects are tolerated.
* Runs once per color target per frame (including the displayed VI-origin
  buffer). MSAA targets: clears the multisampled color, then resolves normally.
* **`aspectScale` is recomputed per framebuffer** from
  `p.aspectRatioTarget/p.aspectRatioSource` (plain floats threaded in
  `addFramebuffer`), so it tracks `--widescreen` on/off and window resizes live
  (config changes flow through `updateUserConfig` →
  `threadConfigurationUpdate`).

### 2c. The host gate — `src/graphics/rt64_render_context.cpp` (Rt64Context ctor)

```cpp
if (const char* env = std::getenv("MM_CLEAR_WINGS")) {
    app_->enhancementConfig.rect.clearWings = (std::atoi(env) != 0);
    app_->updateEnhancementConfig();
}
```

`<cstdlib>` added for `getenv`/`atoi`. The flag then flows
`enhancementConfig → workloadConfig → DrawParams → Framebuffer` (§2a). Default
(unset) = `clearWings=false` → `recordFramebuffer` skips the whole block →
**byte-for-byte the original render path**. The patch is inert unless asked.

---

## 3. Verification status

| Check | Result |
|---|---|
| `rt64` static lib compiles with the patch | ✅ `cmake --build build --target rt64` → `[100%] Built target rt64`, no `error:` |
| `mm_graphics` glue compiles against new `clearWings` field | ✅ `[100%] Built target mm_graphics` |
| Full `mm_game` binary links the patched RT64 | ✅ `build/src/game/mm_game` (17 MB, fresh) |
| Default-off = zero behavior change | ✅ by construction: `clearWings=false` skips the clear; baseline path unchanged |
| Runtime smoke under `SDL_VIDEODRIVER=dummy` | ❌ segfaults — **but the baseline (no env var, no `--widescreen`) segfaults identically**: the graphics build needs a real Vulkan GPU; dummy-SDL has no Vulkan surface. Not caused by this patch (it is gated off in the baseline crash). |

**Pixel verification is the driver's by design** — the headless harness
(`MM_BUILD_GRAPHICS=OFF`) has no RT64 and so cannot exercise the wing-clear at
all; the graphics build needs a real GPU. The sandbox here has no Vulkan device.

---

## 4. Driver verification instructions

Build (already built in this worktree at `build/src/game/mm_game`; rebuild in a
clean env with the README's setup, then `cmake --build build --target mm_game -j`):

```sh
# Baseline (broken wings) — for side-by-side comparison:
./build/src/game/mm_game /path/to/your.z64 --widescreen

# With wing clear:
MM_CLEAR_WINGS=1 ./build/src/game/mm_game /path/to/your.z64 --widescreen
```

**What to look for on screen (window wider than 4:3, e.g. 1920×1080):**

1. **Stale freeze gone.** In the baseline, move the player/a scrolling object
   toward and past the left/right 4:3 edge and back. In the baseline wings you
   see frozen ghost sprites/scenery stuck at the edge. With `MM_CLEAR_WINGS=1`
   those ghosts **do not accumulate** — the wings show either the widened 3D
   scene (geometry that legitimately extends off-stage) or a solid black band,
   never last frame's leftover pixels. This is the pass signal.
2. **No intrusion into the 4:3 playfield.** The centered 4:3 region must be
   untouched — no black bars eating into gameplay. (If you see the playfield
   shrinking, `innerW`/centering math is off — report it; but the math is
   deterministic from `fullW/aspectScale`.)
3. **3D scenes widen correctly.** In gameplay stages with 3D, the wings should
   show extra 3D geometry (the widened projection) on top of the black/clear
   base — i.e. the wings are *useful* extra view, not just black bars. In pure
   2D stages (menus/dialog), wings will be solid black — acceptable (non-broken).
4. **Toggle live.** `--widescreen` on/off and window resize should not crash;
   aspectScale is recomputed per frame.

**Tuning knobs (clearly marked in the code):**
* Wing clear **color**: `RenderColor wingColor(...)` in
  `lib/rt64/src/render/rt64_framebuffer_renderer.cpp` `recordFramebuffer`. Black
  by default; set to the game's natural off-stage color if a stage wants a
  non-black surround.
* If a specific stage relies on framebuffer persistence in the wings for an
  effect (none expected for a fast action game), gate further by also checking
  the framebuffer pair's scissor — but the current per-frame clear matches what
  every modern widescreen-hack emulator does.

**If the driver sees no change with `MM_CLEAR_WINGS=1`:** the most likely cause
is `aspectScale ≤ 1` at runtime (i.e. Expand not actually engaging — check that
the window is genuinely wider than 4:3 and `--widescreen` is passed). Add a
temporary `fprintf(stderr, "[mm] clearWings a=%f fullW=%u\n", aspectScale,
colorTarget->width);` inside the `if` to confirm it's reached.

---

## 5. Relationship to lane a / the "proper" fix

The proper, non-clear fix is for the game's 2D to *draw into* the wings via the
extended GBI (`G_EX_ORIGIN_LEFT/RIGHT`) so sprites/scenery get repositioned
wider instead of freezing at the 4:3 edge — that's lane a's game-side / RT64
ext-hook territory and requires per-DL work. Lane b's renderer clear is the
**cheap, today, non-broken** floor: wings never show stale garbage. The two are
orthogonal and compose (if lane a later makes 2D draw into the wings, lane b's
clear simply becomes a no-op for any wing pixels lane a now writes, because the
clear runs *before* the draws).
