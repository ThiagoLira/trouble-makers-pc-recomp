# Phase 8 — The rotation-stage wall (scenes 13 & 69): UNSOLVED, but mapped

Status: `real-widescreen` is at `73dc088` (every-frame burgundy clear in
these scenes — stable but wrong), and branch **`rotation-wall-fix`** now
carries the SAME runtime behavior after human playtest rejected both
improvement variants (see "Playtest verdicts") — the branch's value is this
document plus the recorded one-line LOD fix (`766b13c` in its history). A
first fix attempt (`079ddff`, seed-pulse only) lives in the reflog.
§"Texgen experiment" records a second attempt that FAILED — do not re-run
it as-is.

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

## Texgen experiment (2026-07-12, branch `rotation-wall-fix`) — FAILED, learn from it

The wall's live geometry mode is **0x000A2205** = ZBUFFER|SHADE|
SHADING_SMOOTH|CULL_BACK|LIGHTING|**G_TEXTURE_GEN_LINEAR** — the LINEAR bit
WITHOUT G_TEXTURE_GEN. RSP LookAt is set (X=(0,1,0), Y=(1,0,0)), texture
scale 0x8000. RT64 only enables texgen when G_TEXTURE_GEN is present, so it
uses the raw vertex ST.

Hypothesis tested: Fast3D honors LINEAR alone as a texgen enable, so RT64
should too. Result of widening the enable mask (plus fixing an apparent
÷32 unit bug in TextureGen.hlsli — texgenUV is s10.5, the pipeline unit is
texels): **the whole scene degraded** — Marina, blocks, everything except
the HUD vanished under giant flat quads. The wall ALSO cannot become bricks
this way: its quads have (apparently) uniform normals, so generated coords
are constant per quad — flat by construction. Counter-evidence against
texgen entirely: the game CPU-writes SCROLLING S coordinates
(`(-0x40 - scroll>>16) << 5`) into the wall Vtx every frame — pointless if
the microcode ignored vertex ST. Both texgen changes were reverted. (The
÷32 unit suspicion in `computeTextureGen` may still be a real RT64 bug for
actual texgen content — it returns s10.5-scaled values where the rest of
the pipeline uses texels — but this game gave no case to validate it.)

## The real remaining contradiction — needs LLE ground truth

- Vertex ST spans −192..+128 texels (×0.5 scale) against a 32×32 tile with
  **mask=0, cm=WRAP**.
- RT64 (and, per its comment, the "hardware rule") forces CLAMP when
  mask==0 → everything outside 0..31 samples the border texel → flat wall
  with at most a narrow textured band. That IS what the port renders.
- Hardware shows repeating bricks across the full quad. If hardware truly
  clamped mask-0 tiles, it could not.
- Reconciliation candidate: on real RDP, an out-of-range coordinate on an
  unmasked tile may address TMEM linearly (taddr = tmem + t*line + s*size,
  wrapping TMEM) — i.e., ALIAS through the loaded texture with a row shift,
  which visually tiles it. RT64's `sampleTMEM()` (TextureDecoder.hlsli)
  already implements hardware-faithful TMEM addressing incl. the CI
  lower-half mask — routing mask-0 WRAP tiles with out-of-range texcoords
  to the rawTMEM path WITHOUT the forced clamp would reproduce that, IF the
  aliasing theory is right.
- **Decide it with an LLE reference**: run scene 69 in angrylion/ParaLLEl
  (or read angrylion's `tcclamp`/`tcmask` — does its clamp-enable really OR
  in `mask==0`, and does the texel fetch alias TMEM before or after
  clamping?). Do NOT keep guessing from HLE captures; that loop was run
  twice and both times produced a confident wrong theory.

## Playtest verdicts (2026-07-12) — why the branch reverted to GPT behavior

Both improvement variants were REJECTED by human playtest, and the reason
generalizes:

- LOD fix + per-frame burgundy clear: the now-solid wall panels race the
  mid-frame clear → wall pops in and out against burgundy. Player-visible
  flicker, worse than GPT's uniform burgundy.
- LOD fix + entry-pulse clear: whenever the wall geometry transiently
  drops out (see below), moving sprites smear into the uncleaned canvas —
  CLEARLY visible motion trails during actual play in BOTH stages.
- Conclusion: until BOTH the wall texture and the wall dropout are fixed,
  GPT's per-frame clear (uniform burgundy + sprites, wall never visible)
  is the least-bad state. The branch code was reverted to exactly that;
  the two fixes remain in this note (LOD one-liner: computeLOD else-branch
  lodFraction 0.0f) and in this branch's history (`766b13c`) for when the
  real fixes land.

**Harness blind spot (important):** test_render_burst.sh and the suite
capture near-static spawn moments; the test-only input pulse barely moves
Marina. Motion trails from the no-clear feedback are INVISIBLE to these
tools — captures showed "no trails" while a player saw them instantly.
Any future verification of clear-policy changes in scenes 13/69 needs
sustained scripted movement (or a human) before claiming victory.

## The transient wall dropout (pre-existing, unsolved)

The wall panels stop drawing entirely for seconds at a time (Vertigo burst:
solid frames alternating with black). Pre-existing — GPT built
test_render_burst.sh chasing "transient geometry loss". With per-frame
clear the absence reads burgundy; with a pulse it reads black/stale (and
enables the trails above). Root cause unknown — the panels are ordinary
actors (0x508, slots 0x91+, posX tracks the camera so hardware never culls
them); check the actor sort/cull path and the per-frame panel state machine
(func_801B3AF8_79B938) first.

## (Resolved 2026-07-12) Earlier "next step": the texgen-stub theory

The probe answered it: geometry mode is 0x000A2205 (LINEAR bit only, no
G_TEXTURE_GEN), so RT64's `usesTextureGen` is false and the raw vertex ST
path runs — the "constant texcoord stub" is NOT taken. (That path is also
not a stub: it stores the texture scale and the GPU computes real texgen in
RSPProcessCS.hlsl via `computeTextureGen`.) See "Texgen experiment" above
for why force-enabling texgen made things worse. The live question is the
mask-0 clamp-vs-TMEM-alias contradiction in the section above it.

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
