# Phase 8 — The rotation-stage wall (scenes 13 & 69): UNSOLVED, but mapped

Status: the port is intentionally rolled back to `73dc088` (every-frame
burgundy clear in these scenes — stable but wrong). A one-commit fix attempt
(`079ddff`, seed-pulse clear) was reverted at the user's request; it lives in
the reflog. This note preserves everything learned so the next attempt starts
at the finish line, not the starting line.

## Ground truth (real hardware — do not re-derive from theory)

Longplay recorded on hardware: https://www.youtube.com/watch?v=3eT2Kf_d9mg
(Vertigo, stage 2-2 = scene 69). Frames extracted at 1:00–1:20 show:

- The playfield is FULLY COVERED by a textured stone-brick wall (greenish
  gray), tilting in perspective as the camera rotates. Other stage sections
  use a wall of character-portrait tiles.
- No sprite trails, no smear, no flat color field. The wall is opaque and
  redrawn every frame; it overwrites all residue.
- Seasick Climb (scene 13) uses the same system (same overlay).

Every port build to date renders this wall UNTEXTURED: flat lit panels
(correct positions, correct perspective, correct shading gradient — single
texel color). Whatever "background bug" reports mention for these stages,
this is the bug. GPT's per-frame burgundy clear and Claude's seed-pulse were
both symptom patches over this.

## How the wall is actually drawn (decomp-verified, community repo)

These are "case 7" scenes in `func_800255B4` (`D_800CD034[scene]`): ALL tile
layers off (`gDrawMidground/gDrawEnvLayer/gDrawBackground` = 0). There is no
tile-layer background and no framebuffer-as-texture trick. The rotation is a
`guLookAt` UP-VECTOR spin (scene 13: `func_80024428`; 69: `func_80024528`),
so the whole projected scene rotates.

The wall = actorType **0x508** panel actors from the Migen's Shrine overlay
(ROM 0x7986D0), 13 panels recycled to cover the screen
(`func_801B37B0_79B5F0`), drawn via the **ACTOR_GFLAG_3DOBJ (0x2000)**
branch of `func_80009BE8` at vaddr 0x8000D208:

1. `gSPMatrix(0x01000040, PHYS(D_801720F0 + idx*64))` — physical address.
2. Setup DL by `jtbl_800EAFC0[D_800BE70C]`; these scenes use mode 3:
   `D_800E3CC8` (fullbright) for 0x508 with `unk_D8 < 7`, else `D_800E3C60`
   (lit; scene 13 rotates the light direction per frame via
   `D_800E3C48[0x10..0x11]`). Shared body (decompiled at `src/E44A0.c`
   `D_800E3AC8`): 2-CYCLE, `G_SHADE|G_CULL_BACK|G_LIGHTING|G_SHADING_SMOOTH`
   (NO Z-buffer), `G_RM_OPA_SURF`, combiner **G_CC_TRILERP, G_CC_MODULATEIA2**
   (`FC26A004 1F1093FF`): cyc1 = (TEXEL1−TEXEL0)·LOD_FRAC + TEXEL0,
   cyc2 = COMBINED·SHADE.
3. `gSPDisplayList(actor->unk_17C)` — the wall model (overlay .data,
   Fast3D, tri indices ×10). Per `unk_D8`: `D_801B8808` (bricks, textures
   0x80265218/618/0x80266218 + TLUT 0x80266618), `D_801B8A98` (portrait
   wall, textures 0x803D2050/2450 + TLUT 0x803D3050), variants
   D_801B8BE0/8D28/8E70/8FB8, default D_801B8950.
4. Restore DL `D_800E3A50`.

Model DL structure: LOADTLUT 256 entries (tile7, tmem 0x100) then per
texture: SETTIMG(CI-as-16b) → LOADBLOCK 512 texels (= CI8 32×32) → render
tile0: CI8, line 4, tmem 0, pal 0, **maskS/T = 0, cm WRAP**, (0,0)-(31,31).
5 quads per panel. Vertices carry normals (lit) and are **CPU-rewritten
every frame** (`func_801B3A14_79B854` writes Y and S coords into the Vtx
arrays at 0x801B8088/0x801B8148 etc.).

Key inherited state: the model DL itself never sets `gsSPTexture` (scale
0x8000 = 0.5, G_ON) nor `G_TT_RGBA16` — both come from `D_800E39C0`, emitted
once at the top of the actor pass. No Z-buffer: the wall wins by being drawn
first (actor Z sort, wall at posZ = −0x100) and overdrawn by everything.
That's also why hardware has no trails.

## Port-side instrumentation results (all checks PASSED — eliminate these)

Debug fprintf probes (recipes below) proved at the wall drawcall in RT64:

- Drawcall state: `texOn=1 tile=0 levels=1 tileCount=2`, otherMode H
  0x0098ACFF (TT=RGBA16 ✓, TL=TILE ✓), L 0x0F0A4001, combiner
  1F1093FF/FC26A004 ✓, tile0 = CI8 line 4 tmem 0 masks/t=0 cms/t=0
  uls=0 lrs=124 ✓. State inheritance across DLs WORKS.
- Vertices as read by the RSP (addr 0x1B8088/0x1B8148): proper quads
  (±48, ±16, z 128/−192) with VARYING texcoords (s ±1536, ±6144, 4096;
  t ±512, ±1536). CPU mutation arrives ✓.
- TMEM loads each frame: LOADTLUT from 0x266618 carries real RGBA16 palette
  (D33ACD19...), LOADBLOCKs from 0x265618/0x265A18 carry real CI8 indices
  (4746454447474748...). Data path WORKS.
- RT64's `modulo(x, 0)` returns x, and rt64_state.cpp already ORs
  G_TX_CLAMP into cms/cmt when mask==0 (hardware rule) — the mask-0 WRAP
  tile is handled.

## CONFIRMED RT64 divergence #1 (fix verified, currently reverted)

`lib/rt64/src/shaders/TextureSampler.hlsli`, `computeLOD()`: when texture
LOD is DISABLED (`G_TL_TILE`) it sets `lodFraction = 1.0f`. That makes the
wall's TRILERP collapse to TEXEL1 = the never-configured stale tile 1 →
per-frame random flat colors (red/gray/pink/black flicker). Hardware
resolves this combiner to TEXEL0 (bricks visible on hardware ⇒ LOD_FRAC=0).
Changing it to `0.0f` verifiably stopped the flicker (panels became stable).
One-line change, worth re-applying — but NOT sufficient on its own.

## The remaining unknown (exact next step)

After the LOD fix the wall is still FLAT (one texel stretched, correct
lighting). Everything upstream is proven good, so the loss is in how RT64
turns per-vertex ST into GPU texcoords. Prime suspect, not yet confirmed:

`lib/rt64/src/hle/rt64_rsp.cpp` `setVertexCommon()`:
```
const uint32_t textureGenMask = G_LIGHTING | G_TEXTURE_GEN;
const bool usesTextureGen = (geometryMode & textureGenMask) == textureGenMask;
...
if (usesTextureGen) {           // <- STUB: constant texcoords for all verts
    tcFloats.emplace_back(TextureSc);
    tcFloats.emplace_back(TextureTc);
}
```
If the wall's geometry mode has `G_TEXTURE_GEN` set alongside `G_LIGHTING`
(the setup DLs D_800E3C60/D_800E3CC8 must be dumped to check — the decomp
transcription listed only SHADE|CULL_BACK|LIGHTING|SHADING_SMOOTH, but that
was read from the mode-0 variant D_800E3AC8), then RT64 replaces every
texcoord with a CONSTANT → exactly one texel → exactly the flat lit wall.
Verification is one line away: log
`geometryModeStack[geometryModeStackSize - 1]` (NOT `geometryMode`, which
isn't in scope in `setVertex`) next to the existing vertex probe, or just
dump the 8 words of D_800E3C60/D_800E3CC8 from RDRAM.

If TEXGEN is confirmed: hardware texgen maps the LIT NORMAL through the
lookAt vectors to ST (spherical env mapping). Fast3D texgen on a wall with
per-vertex normals would produce varying coords — the fix is implementing
real texgen in setVertexCommon (the lookAt data is already tracked;
RasterPS may even support it via RSP_LOOKAT_INDEX_ENABLED — check whether
the constant fill is a leftover CPU fallback that should defer to the GPU
path). CAUTION: if TEXGEN is set, the CPU-written S coords are ignored by
hardware too, so the "scrolling" comes from somewhere else — re-read
func_801B3A14 in that light.

If TEXGEN is NOT set: instrument `tcFloats` right after the else-branch fill
for the wall vertices and follow the values into RasterPS.

## The clear policy (once the wall renders)

- Hardware never clears the framebuffer in these scenes; the opaque wall is
  the implicit clear. Trails/smear CANNOT appear once the wall draws.
- The port-only bug the clears were fighting: RT64's two double-buffered HD
  color targets get seeded differently at stage entry and their feedback
  histories never reconverge (beige/burgundy 30 Hz flicker, visible in
  pre-73dc088 builds).
- Correct end state: with the wall fixed, REMOVE the per-frame burgundy
  clear (`g_force_frame_clear` in src/game/widescreen.cpp + the
  forceFrameClear block in the rt64 patch). A brief seed clear at stage
  entry (reverted commit `079ddff` implements exactly that: 6-frame pulse on
  entering game state 6, rearmed outside it) is a reasonable belt-and-
  suspenders against entry garbage, but it must not run per-frame.
- These scenes should probably stay 4:3 regardless: the wall panels are
  spawned to cover a 320×240 window (13 panels around world X=400) — 16:9
  would need more panels, a separate widescreen problem.

## Instrumentation recipes (what worked)

- `tools/test_render_burst.sh BIN ROM OUT STAGE` with
  `MM_CAPTURE_FRAMES/MM_CAPTURE_INTERVAL`; stage 13 = scene 69 (Vertigo),
  stage 21 = scene 13 (Seasick Climb). Stages 13/21 are in the fixed-4:3
  list, captured after 12 s.
- Drawcall state probe: rt64_state.cpp, inside `flush()` before the
  `if (drawCall.tileCount > 0)` block — filter
  `otherMode.cycleType() == G_CYC_2CYCLE && tileCount != 1`.
- Vertex probe: rt64_rsp.cpp `setVertex()` — filter
  `rdramAddress in [0x1B7000, 0x1B9000)`.
- Load probe: rt64_state.cpp `fullSyncFramebufferPairTiles()` →
  `loadOperation` lambda — filter `loadOp.texture.address in
  [0x260000, 0x270000)`; print `fromRDRAM(...)` bytes.
- Gotcha: `~/.config/troublemakers-recomp/display.cfg` keeps `widescreen=1`
  from any widescreen run; flagless launches inherit it.
- Hardware reference frames: `yt-dlp -f "bv*[height<=480]" 
  --download-sections "*01:00-01:20"` on the longplay + ffmpeg fps=1.
