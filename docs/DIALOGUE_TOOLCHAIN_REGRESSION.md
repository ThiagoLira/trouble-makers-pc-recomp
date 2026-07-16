# Dialogue/toolchain release regression (July 2026)

## Symptom

The Linux release built on Ubuntu 22.04 rendered ordinary gameplay, but NPC
dialogue was corrupted: the translucent box disappeared, the original 4:3
center darkened, and letters became noise or isolated tiles. The same scene
was correct in an earlier locally built AppImage.

The deterministic repro is progression stage 3 / scene 68 with Marina moving
right into the NPC beside the block house:

```bash
MM_WARP_STAGE=3 MM_WARP_AT=1 MM_WARP_DELAY=1 \
MM_TEST_AUTO_ADVANCE=1 MM_TEST_MOVE=jet-right \
./troublemakers rom.z64 --window 1600x900 --widescreen
```

## Isolation evidence

- Native Wayland, XWayland/X11, and Wayland with HiDPI disabled all failed in
  the shipped build. This ruled out the window backend and fractional scaling.
- Swapping AppImage libraries did not move the failure. A bad executable with
  known-good libraries remained bad; a known-good executable with the release
  libraries remained good.
- RT64 reported and received `ubershadersOnly=1` on both its normal and
  mid-frame framebuffer paths. The NVIDIA specialized-SPIR-V workaround was
  active and was not the dialogue failure.
- Linking only the Ubuntu GCC 11 `mm_recompiled` archive into the known-good
  GCC 16 renderer reproduced the corruption. Linking the GCC 12 archive into
  that renderer fixed it.
- GCC 11 at `-O2` still failed. `-O0` was substantially slower and is not a
  viable release configuration. GCC 12 and GCC 16 render the complete
  multi-page conversation correctly at the normal Release `-O3` setting.

The regression therefore came from GCC 11 code generation in the translated
game library, not RT64, SDL, AppImage bundling, or the widescreen mode switch.

## Fix

Linux CI keeps its Ubuntu 22.04/glibc 2.35 compatibility baseline but installs
and selects Jammy's GCC/G++ 12 packages. CMake rejects GNU C compilers older
than 12 so a visually corrupt release cannot be produced silently again.

## Validation gate

The GCC 12 candidate was exercised on an RTX 5090 under native KDE Wayland:

- the entire stage-3 conversation, including both pages and controller glyphs;
- eight moving frames in Vertigo and eight in Seasick Climb;
- all 52 progression-table playable stages, three moving frames per stage;
- the long stage-57 fixed-4:3 boss transition with its normal timeout;
- the focused forest terrain test: 24 dense moving frames, native actor-wrap
  proof, four landscape clones, and stable scene-0/scene-68 rollover samples.

The playable sweep passed 52/52. The native-Wayland test harness now uses
Spectacle and compensates for KWin's centered drop-shadow canvas when sampling
exact regression pixels.
