# Mischief Makers (US 1.1): ROM and engine technical reference

This document consolidates the reverse-engineering knowledge used to build
Trouble Makers. It describes the original *Mischief Makers* US 1.1 program,
the hardware contracts on which it depends, and the narrow compatibility work
needed to execute that program as a native PC recompilation.

It is intentionally organized by subsystem rather than by discovery date.
Historical phase reports, task briefs, workstation paths, transient logs,
and abandoned experiments have been removed. A statement under **ROM
behavior** describes the cartridge program or its data. A statement under
**Recomp consequence** describes host behavior added by this project and must
not be mistaken for original game logic.

The checked-in symbol map and patches are the executable specification. When
this document and the source disagree, use this order of authority:

1. `symbols/troublemakers.us1.toml` and byte reads from a verified US 1.1 ROM;
2. `troublemakers.us1.toml` and `aspMain.us1.rsp.toml`;
3. translated-code hooks in `src/game/`;
4. runtime and RT64 patches in `patches/`;
5. this document.

No ROM, game asset, decompilation checkout, or generated recompiler output is
part of this repository. All ROM offsets below refer to a legally dumped,
big-endian US 1.1 image.

## 1. Cartridge identity and immutable facts

| Property | Value | Evidence or consequence |
|---|---:|---|
| Region/revision | US 1.1 | The only supported program revision |
| ROM size | 8 MiB | Byte-level ucode and section searches use this image |
| Byte order | big-endian `.z64` | No normalization changes are needed before hashing |
| Internal ROM name | `MISCHIEF MAKERS` | Header identity checked at launch |
| Entry point | `0x80000400` | Recompiler entry and initial code load address |
| Whole-ROM SHA-256 | `e00ab74c3dee79efaafe8e10f2a6b67784d7327ab588d8ef90ad8945427da627` | Builder-facing dump verification |
| Whole-ROM XXH3-64 | `0xECB515AE2C898E4F` | Runtime acceptance fingerprint |
| Native game cadence | 60 Hz | Game logic and VI cadence; display-rate interpolation is host-only |
| Main graphics ucode | `gspFast3D` | Original Fast3D, not F3DEX/F3DEX2 |
| Audio ucode | Nintendo SDK-2.0-family `aspMain` | Flat image linked at RSP IMEM `0x1080` |
| Save medium | 4 Kbit EEPROM | 64 blocks of eight bytes |
| Original RDRAM footprint | 4 MiB | The recomp uses otherwise-unused expansion memory for widened data |
| Native canvas | 320 × 240 | Most 2D systems author only the central 4:3 field |

The runtime hashes the complete byte-normalized ROM, not only the header or
boot code. A different regional build is therefore rejected even if it has
the same internal title. Function addresses, overlay tables, and instruction
patches in this project are revision-specific.

`has_compressed_code = false` in the host game entry does **not** mean the
cartridge contains no compression. It means the initial recompiled program
image does not require a single host-side decompression callback. The game
still uses its own Trouble RLE asset paths, and it streams raw overlay code at
runtime.

## 2. Addressing, byte order, and data representation

N64 virtual addresses in this document use the program's KSEG0/KSEG1 view.
The recomp runtime presents a flat memory arena, so the common conversion is:

```text
rdram_offset = n64_virtual_address - 0x80000000
```

Examples:

- `0x8012ABF0` is at RDRAM offset `0x0012ABF0`;
- the uncached AI register `0xA4500004` maps to host reservation offset
  `0x24500004` when the translated `lw` is allowed to execute directly.

The generated accessors preserve N64 big-endian sub-word addressing:

- byte accesses XOR the low address bits with `3`;
- halfword accesses XOR them with `2`;
- aligned word accesses are direct host word loads/stores.

That distinction matters when inspecting memory in a debugger. An aligned
`MEM_W` value can be read as a host word directly; manually applying the byte
XOR to it corrupts the interpretation.

Many camera and actor coordinates use `FixedCoord`, a Q16.16 union:

```text
bits 31..16  signed integer pixel coordinate (`whole`)
bits 15..0   fractional coordinate (`frac`)
```

Several visibility tests load only the signed upper halfword. A change to a
view bound may therefore preserve the low fractional bits while changing its
integer extent.

## 3. Program image, function discovery, and overlays

### 3.1 Static image

The symbol map describes 177 code sections and overlay entries. The initial
main section begins at ROM `0x00001000`, lands at `0x80000400`, and contains
the boot/runtime-facing portion of the game. N64Recomp translates more than
30,000 functions into native C and emits a section table whose entries bind:

```text
(ROM interval, N64 RAM destination, translated function array)
```

The translated archive must be linked as a whole archive. Function lookup is
data-driven through these arrays, so a normal static-library call-graph link
would discard functions that are reachable only through an N64 function
pointer.

Forty-five local libultra/libaudio functions were absent from the source ELF's
global symbol table even though the game stores pointers to them. They are
listed explicitly in the recompiler's `manual_funcs` map. This is a useful ROM
analysis rule: a stripped or local function is not dead merely because it has
no exported symbol or direct `jal` reference.

### 3.2 Runtime overlay slots

The game has five object-code overlay loaders. Their destinations are fixed
slots reused by different worlds or actor banks:

| Slot | Loader | Object-code RAM base |
|---:|---|---:|
| 0 | `func_80025EC4` | `0x80192100` |
| 1 | `func_80025F70` | `0x8019B100` |
| 2 | `func_8002601C` | `0x801A6900` |
| 3 | `func_800260C8` | `0x801B0900` |
| 4 | `func_80026174` | `0x801B9900` |

Each loader indexes an `OverlayLoadEntry` table and performs two synchronous
PI transfers: a small dispatch table followed by the raw object code. For
slot 4, the dispatch table lands at `0x801B9800` and the object begins at
`0x801B9900`.

A concrete slot-4 section is:

```text
ROM  0x007C6AE0 .. 0x007CDD60
RAM  0x801B9900
size 0x00007580
first translated function at RAM offset 0
```

The loading chain is:

```text
slot loader
  -> DMA_ReadSync
  -> osPiStartDma
  -> runtime ROM-to-RDRAM copy
  -> indirect call through the freshly loaded dispatch/object slot
```

**Recomp consequence.** Copying the bytes is insufficient for static
recompilation: the host function map must also be told which translated
section now occupies that RAM interval. The runtime therefore calls
`load_overlays(rom_offset, ram_address, size)` after every ROM read. Asset
DMAs do not overlap code sections and are a cheap no-op; code DMAs register
the correct function array before the first indirect jump. Reusing a slot
overwrites its current address-to-function mappings, matching the cartridge's
overlay semantics.

### 3.3 Address-selected thread entries

The retail boot code passes `rmonMain` at `0x8009A2B8` to `osCreateThread`.
It is an ignored debug-monitor routine rather than translated game code, but
thread creation resolves its entry by N64 address. On retail hardware the
rmon thread waits for debugger traffic that never arrives.

**Recomp consequence.** A host no-op is registered at the same address after
the overlay map is initialized. Registering it earlier is ineffective because
runtime initialization clears the loaded-function map.

The complete translated archive exposes only two libultra wrapper references
that the generic runtime does not provide: `rmonPrintf_recomp` and
`__osGetCause_recomp`. The production host emits the rmon format string to its
diagnostic stream without interpreting MIPS varargs, and reports CP0 Cause as
zero. Both belong to the retail debug-monitor path; neither is part of normal
gameplay scheduling.

## 4. Boot, threads, tasks, and message queues

### 4.1 Start order

The game must not enter `0x80000400` before the first VI tick has established a
valid video mode. The PC host selects the ROM first, lets the VI callback seed
the runtime's `ViState`, and starts the game at the end of that callback.
Starting from renderer initialization alone creates a null-mode race that does
not exist on the console boot path.

### 4.2 Per-frame ordering

The central `Thread_MainProc` frame sequence is:

```text
build and submit graphics OSTask
  -> schedule/update audio
  -> block on gDisplayProcessorMessageQueue
  -> next frame after OS_EVENT_DP
```

`gDisplayProcessorMessageQueue` is at `0x8012ABF0`. Boot registers it for
`OS_EVENT_DP`. The PC renderer posts the matching completion only after the
display list has been accepted and the render path reaches DP completion.
Consequently, a blocked or occluded presentation path can back-pressure the
original game at exactly the queue on which the N64 program expects the RDP.

The audio path uses double-buffered task records; observed task structures
alternate between `0x8013EE10` and `0x8013EE70`. When a slow frame causes the
double-audio path to run, one graphics task plus two audio tasks can produce
three SP-completion events while the size-one audio message queue consumes
only the two audio completions.

On N64, `osSendMesg(..., OS_MESG_NOBLOCK)` and hardware event delivery are
lossy when the destination queue is full. Preserving every surplus SP signal
instead changes the program: an undeliverable token can remain pending and
starve the one-shot DP signal. The runtime configuration therefore keeps SP
event delivery lossy. Blocked host events are deferred without using the
deferred list as a wake source, and identical deferred signals are coalesced.

Self-branches in translated code become a cooperative `pause_self` call. A
quiet, zero-CPU parked thread can therefore represent a legitimate N64 wait;
it is not by itself evidence that the translated function stopped executing.

## 5. RSP programs and task routing

The ROM contains three standard Nintendo RSP programs:

| Program | RAM text symbol | Text bytes | ROM text | ROM data | Data bytes | Host treatment |
|---|---:|---:|---:|---:|---:|---|
| `rspboot` | `0x800BA9E0` | `0x0D0` | `0x000BB5E0` | — | — | console task bootstrap behavior supplied by runtime |
| `gspFast3D` | `0x800BAAB0` | `0x1400` in asset | `0x000BB6B0` | `0x000EF610` | `0x0800` | graphics task is interpreted by RT64 |
| `aspMain` | `0x800BBEB0` | `0x0E20` | `0x000BCAB0` | `0x000EFE10` | `0x02C0` | statically recompiled with RSPRecomp |

The Fast3D asset is `0x1400` bytes, but boot submits only `0x1000` text bytes,
the size of IMEM. Graphics tasks bypass the audio RSP dispatcher; RT64
interprets the Fast3D display list found at `OSTask.data_ptr`. Recompiling the
graphics microcode is neither required nor desirable because it drives DPC
state that the non-graphics RSPRecomp path deliberately does not model.

### 5.1 The `aspMain` link address

The audio text is linked for IMEM `0x04001080`, not the physical IMEM start
`0x04001000`. Its `0x0E20` bytes therefore span logical IMEM
`0x1080..0x1EA0`.

The decisive proof is internal, not heuristic:

- command-dispatch entry zero is `0x1118`;
- `cmd_SPNOOP` is at text offset `0x98`;
- `0x1118 - 0x98 = 0x1080`;
- the entry `jal 0x1150` reaches the prefetch helper at text offset `0xD0`;
- `0x1150 - 0xD0 = 0x1080`;
- helpers at `0x1184` and `0x11B0` independently imply the same base;
- the highest dispatch entry, `0x1E24`, is inside a `0x1080..0x1EA0` image but
  falls past a wrongly based `0x1000..0x1E20` image.

A base of `0x1000` shifts every absolute `j`, `jal`, and dispatch destination
by `0x80`. The entry prefetch then lands in the DMA-write helper tail,
`cmd_SEGMENT` returns into a DMA-read helper, stale `$ra` returns to
initialization, `$29` resets to `0x380`, and the same first command repeats
forever. This is why an apparently plausible IMEM base can compile cleanly but
fail only at runtime.

### 5.2 Audio command engine

The data blob contains a sixteen-entry big-endian halfword dispatch table at
DMEM `+0x10`:

```text
1118 1470 11DC 1B38 1214 187C 1254 12D0
12EC 1328 140C 1294 1E24 138C 170C 144C
```

The command loop at IMEM `0x1108` reads two words from `$29`, calculates:

```text
dispatch_byte_offset = (word0 >> 23) & 0xFE
handler = signed_halfword(DMEM[0x10 + dispatch_byte_offset])
jr handler
```

`cmd_SPNOOP` at `0x1118` advances the stream, fetches another chunk when
required, and exits with `break` when the command list is exhausted.
`cmd_SEGMENT` is dispatch entry 7 at `0x12D0`; it stores a segment base and
jumps back to `0x1118`.

The runtime loads `OSTask` at DMEM `0xFC0`. `aspMain` reads `data_ptr` from
DMEM `0xFF0`, fetches at most `0x140` bytes into DMEM `0x380`, and maintains
`$29/$30` as the current command pointer/count. The ucode owns this prefetch;
a host-side manual first-chunk copy is both redundant and capable of hiding a
bad link address.

All sixteen dispatch destinations plus the two `jr $5` return points are
declared as indirect targets in `aspMain.us1.rsp.toml`. Generated vector code
uses SSSE3/SSE4.1-class operations on x86; this is a host compilation property,
not an N64 requirement.

## 6. Graphics, framebuffers, and authored 4:3 coverage

### 6.1 Fast3D/RDP path

The CPU builds Fast3D display lists in RDRAM. A graphics OSTask names the list,
and RT64 identifies/interprets the original Fast3D command dialect. The RSP
audio callback must never receive `M_GFXTASK`.

Observed VI origins alternate between physical `0x001DAA80` and `0x003DAA80`,
corresponding to the game's two framebuffers at KSEG0 `0x801DAA80` and
`0x803DAA80`. The native logical canvas is 320 × 240.

The game also uses framebuffer rows around the VI origin as RDP-copied palette
scratch. At least one row lies above the address the VI actually scans, and
scratch can reach the first one or two scanned rows that a CRT would normally
hide in overscan. A modern presenter that reconstructs a framebuffer base
above `VI_ORIGIN` and samples from row zero exposes this as a garbage strip
during the Nintendo/Enix/Treasure logos.

**Recomp consequence.** RT64 calculates how many inferred framebuffer rows
precede `VI_ORIGIN`, skips them unconditionally, and crops two additional top
rows by default to reproduce the effective overscan. This is a VI presentation
correction; clearing or editing the framebuffer would destroy legitimate
scratch/data ownership.

The game does not guarantee a complete color clear of every possible expanded
target pixel on every frame. That is legal on the console: authored geometry,
rectangles, and effects cover the native framebuffer, and render passes may
depend on framebuffer persistence. It becomes visible only when a host render
target is wider than the coordinate system the game paints.

### 6.2 Rectangles versus projected geometry

This distinction explains most widescreen artifacts:

- perspective and orthographic triangle projections can be expanded by the
  renderer and naturally reveal more world geometry;
- fill rectangles, texture rectangles, scissor rectangles, and viewports use
  320-wide framebuffer coordinates and normally remain centered at 4:3;
- a sprite that straddles `x=320` can write a few pixels into an expanded
  render target, then stop being redrawn after the game culls it, leaving a
  stale vertical trail;
- a native full-width scissor still clips negative-X wing tiles unless the
  renderer expands that scissor specifically for the widened rectangle path.

Two narrow rectangle rules are required:

1. widened tile hooks encode negative X in the RDP command's 12-bit
   quarter-pixel field. The renderer sign-extends that field and lets its
   normal scissor clip geometry and texture interpolation together. Clamping X
   to zero while separately advancing texture S produces horizontal bands at
   the 4:3 left edge;
2. RT64's `fixRectLR` compatibility rule must only grow an edge that falls up
   to one pixel short of its scissor. Using an absolute-distance test also
   shrinks a valid rectangle that extends slightly past the original right
   scissor. Once the scissor is widened, that shortened tile exposes the black
   wing clear for one frame as a vertical line near the old `x=320` boundary.

The second condition was the intermittent right-edge seam visible only at
certain horizontal scroll phases. Keeping the per-frame wing clear and fixing
the edge rule is essential: disabling the clear hides the seam by preserving
old pixels but restores stale sprite/scenery trails.

The local renderer checkout can silently retain the old rule after a project
update because `lib/rt64` is ignored by the parent repository. This reproduced
the seam in the first level at 144 Hz despite patch `0006` already being
checked in. Configuration and every RT64 build now run a reverse dry-run check
of the required patches through `cmake/VerifyRT64Patches.cmake`, rejecting
missing or partially applied fixes without modifying the dependency source.

The game's border helpers make the authored safe area still narrower:
`Gfx_DrawLetterbox` (`0x800218FC`) and `Gfx_DrawBorderRect` (`0x80021690`)
normally leave a visible region around `x=14..306`, `y=20..212`. Suppressing
only the vertical border strips during expanded gameplay removes seams while
retaining top/bottom letterbox animation. The renderer's pre-draw wing clear
therefore reaches inward through the exact suppressed `x=0..14` and
`x=302..320` gutters as well as the true expanded wings. Otherwise a moving
actor can leave persistent pixels inside a former border when a stage does not
repaint that gutter. The inset applies only to the known 320-wide game target;
other framebuffer widths keep the ordinary outer-wing clear.

### 6.3 Display-list capacity

The original six per-layer display-list arenas are double-buffered by layer:

| Layer | Buffer A | Buffer B |
|---|---:|---:|
| midground | `0x80178470` | `0x80179A90` |
| environment | `0x8017B0B0` | `0x8017C6D0` |
| backdrop/static | `0x8017DCF0` | `0x8017F310` |

Each arena is `0x1620` bytes. A tile consumes 80 bytes, so the original
70-tile list needs `70 * 80 + 8 = 0x15E8` bytes including `G_ENDDL`, leaving
`0x38` bytes. There is no bounds check.

This exact sizing is an important original-engine invariant. Doubling a layer
from 70 to 140 tiles requires `0x2BC8` bytes and silently overwrites the next
arena or layer grid if the arena is not relocated. A top-level frame list has
the same class of risk: widened Stage 3/scene 68 content exceeded the original
`0x6000` payload and reached controller data.

**Recomp consequence.** Widescreen uses enlarged per-layer arenas in unused
expansion RDRAM and two `0x10000`-stride frame arenas at `0x80460000` and
`0x80470000`. This is capacity restoration, not a visual heuristic.

## 7. Audio and the one direct MMIO dependency

`Sound_Update` at `0x800023F0` is the only game function that reads an RCP
register directly. It performs:

```mips
lui  t9, 0xA450
lw   t1, 4(t9)       # AI_LEN_REG: bytes remaining
srl  t2, t1, 2       # bytes / 4 -> stereo frames
sw   t2, ...         # gAudioSamplesLeft
```

All other AI register access is inside libultra functions replaced by the
runtime. Returning zero permanently tells the game that the DAC is always
drained and changes its pacing.

**Recomp consequence.** The host commits the otherwise-protected page
containing `0xA4500004` and mirrors its actual queued stereo-frame count times
four at roughly 1 ms cadence. An aligned host word store is correct for this
translated `lw`. The mirror terminates before RDRAM is released.

The host audio contract is easy to misread:

- `queue_samples` receives a count of signed 16-bit samples, not bytes;
- `get_frames_remaining` returns stereo frames;
- AI length is bytes, hence `frames * 2 channels * sizeof(s16)`;
- N64 AI stereo pairs require L/R correction before little-endian host output;
- the current output device path resamples to a stable host rate while
  reporting remaining data back in the game's rate domain.

### 7.1 Stalled host playback and synthesis overflow

A Steam Deck startup failure reported `Failed to find function at 0x08000000`.
A hardware watchpoint and a local reproduction with SDL playback held paused
identified an audio command-list overflow: commands replaced the synthesizer
filter callback at `0x80152F78` (originally `0x800B2154`).

`Sound_Update` at `0x800022D0..0x80002310` calculates
`(target - remaining + 96) & 0xFFF0`, reads the result as signed 16-bit, then
applies the minimum sample count. An unbounded host queue eventually wraps
that subtraction into a large positive request. The captured transition was
33,414 remaining frames producing a 32,576-frame request instead of the normal
352–464 frames. The two `0x3800` command buffers cannot hold that synthesis.

The SDL backend discards queued playback older than 250 ms before adding a
new buffer. Its backlog report is independently bounded and capped to the
signed 16-bit range, so the register-mirroring thread cannot expose an unsafe
value between queue updates. Normal rate conversion and the one-VI backoff
are preserved. The sample rate shared with that thread is atomic.

`mm_audio_queue` exercises the observed wraparound, normal backlog reporting,
and oversized queue values. Build `mm_audio_queue_tests` and run
`ctest --test-dir build -R mm_audio_queue --output-on-failure`. A local
15-second run with playback permanently paused continued producing game and
audio tasks at 60 Hz without heap corruption; a separate delayed-resume run
checks recovery when playback starts draining again.

## 8. EEPROM save layout

The program calls `osEepromProbe`, `osEepromLongRead`, and
`osEepromLongWrite`; it has no SRAM or FlashRAM access path. The observed
starting block addresses are:

```text
0x02, 0x0C, 0x14, 0x24, 0x2C
```

The greatest referenced block is 44, below the 64-block limit of 4 Kbit
EEPROM. `Eep4k` is therefore the accurate cartridge type; advertising 16 Kbit
would work but would misdescribe the original medium.

Real libultra long reads/writes loop over complete eight-byte blocks while the
requested length remains positive. A non-multiple length therefore rounds up
and transfers one final full block, including bytes adjacent to the nominal
caller buffer. The game's first save operation relies on that permissive
behavior. A host wrapper that asserts `nbytes % 8 == 0` is stricter than the
console and fails valid game code; the runtime rounds up before copying.

The runtime persists the emulated EEPROM as a save file. That storage format
and atomic-write policy are host concerns and are not part of the ROM layout.

## 9. Stage, scene, pause, and dialogue state

The progression table contains 64 rows, of which 52 are player-controlled
campaign entries. A scene number alone is not a complete level identity:
multiple progression rows can resolve to the same scene while selecting
different stage IDs, maps, overlays, or progression state.

| Address | Width | Meaning |
|---:|---:|---|
| `0x800BE4E8` | `u16` | `gGamePaused` |
| `0x800BE4EC` | `u16` | `gCannotPause` |
| `0x800BE4F0` | `u16` | `gGameState` (`5` loading, `6` gameplay) |
| `0x800BE4F4` | `u16` | game-state substate |
| `0x800BE5D0` | `s16` | current runtime scene |
| `0x800C8378` | `u16[]` | progression row -> scene table |
| `0x800C83F8` | `u16[]` | progression row -> stage-ID table |
| `0x80178162` | `u16` | current progression row |
| `0x801781E0` | `u16` | stage timer |
| `0x800D28E4` | `u16` | current stage ID |
| `0x800D28E8` | `u16` | stage cinema state |
| `0x801783F0` | `u16` | generic dialogue state |
| `0x80178418` | `u16` | queued/current dialogue message |
| `0x80178438` | `u16` | dialogue textbox actor/state |

Stage cinematics usually freeze the stage timer and assert `gCannotPause`.
Short gameplay effects can do either transiently, so neither signal alone is
a perfect cinematic classifier. Generic NPC dialogue has its own state trio
and can remain part of an otherwise active gameplay scene.

A correct debug warp must select the complete progression row: current stage,
stage ID, unlock index, scene, loading state, and the canvas state that normal
boot would initialize. Writing only `gCurrentScene` reuses stale map and
overlay assets and creates convincing but invalid rendering failures.

### 9.1 Pause is a fixed-canvas iris

The pause transition is a four-sided iris authored for the 320 × 240 canvas.
`gGamePaused` is asserted before the closing iris and is cleared by
`func_800208D4` only after the opening iris has restored gameplay.

**Recomp consequence.** In widescreen, the renderer enters original 4:3 on
the same tick that `gGamePaused` becomes nonzero. The side regions are black
before the first closing-iris frame. It remains 4:3 through the opening iris,
then expands on the first tick after the flag clears, before the first visible
unpaused frame. This preserves both the original pause animation and a fully
black paused background.

## 10. Camera rectangle and actor visibility systems

### 10.1 Camera data

| Address | Symbol/role | Original behavior |
|---:|---|---|
| `0x800BE558` | `gScreenPosCurrentX` | Q16.16 camera center X |
| `0x800BE55C` | `gScreenPosCurrentY` | Q16.16 camera center Y |
| `0x800BE568` | left view/cull bound | camera X minus `0x90` pixels |
| `0x800BE56C` | right view/cull bound | camera X plus `0x90` pixels |
| `0x800BE570` | first vertical bound | camera Y minus `0x70` pixels |
| `0x800BE574` | second vertical bound | camera Y plus `0x70` pixels |
| `0x800D2920` | authored stage left value | consulted by camera/world logic |
| `0x800D2924` | authored stage right value | consulted by camera/world logic |

`Camera_UpdateViewBounds` begins at `0x800462F0`; `func_800463C0` calls it and
then explicitly restores the left bound. Actor draw functions compare their
world/screen X against the upper halfwords of `0x800BE568/56C` and skip the
draw outside that interval.

These globals are not purely visual. Marina's collision/world-wall routine
and scripted exits also consume them. Unconditionally widening the stored
rectangle can therefore let the player move past authored floor or prevent a
stage ending from firing. The Lunar ending in scene 9, cinema states 21..25,
is a concrete case.

**Recomp consequence.** Expanded gameplay applies a wider `±0x180` rectangle
at the translated return boundary, after the original routine computes its
authored values. The player-wall consumer receives reconstructed vanilla
`±0x90` operands. Original 4:3, cinematics, non-stage IDs, and the Lunar ending
retain the cartridge boundaries. Vertical bounds are never widened.

### 10.2 Visibility is a pipeline, not one cull

The camera rectangle is only the first of several original 4:3-sized windows:

| Original path | Purpose | Original horizontal extent |
|---|---|---:|
| `Camera_UpdateViewBounds` | common draw rectangle | `±0x90` |
| `func_80016D94` | destroy onscreen-only actors | `±0xD0` |
| `func_8001DC60` / `func_8001DE30` | map-authored actor prefetch rings | inner `±0xB0`, outer `±0xD0` |
| `func_800451E4` | gem/prop world-record admission | normal `±0xA8`, alternate `±0x100` |
| `Actor_IsOutsideRegion` | SFX/behavior distance | `0x90 + object length` |
| `func_8000FBF4` | final depth-sorted actor-list admission | approximately `±0xC0` |
| `func_801A78DC_7670EC` | state-1 terrain draw toggle | `-0xBF .. +0xBF` |

An actor may therefore remain alive and have `ACTOR_FLAG_DRAW` set while never
reaching the list consumed by `func_80009BE8`. This explains pop-in that
survives a camera-bound patch. The project widens these roles separately and
keeps cleanup hysteresis outside the visible rectangle instead of assigning
one global magic extent.

The primary actor pool begins at `0x800EF510`, contains 208 records, and uses
`0x198` bytes per record. Frequently useful fields are:

| Record offset | Meaning |
|---:|---|
| `0x80` | flags, including active/draw state |
| `0x88`, `0x8C` | transformed X/Y position |
| `0xD0` | actor state |
| `0xD2` | actor type |
| `0x17C` | actor-private display-list pointer |

The separate clan/prop world-record path originally has 64 records of
`0x90` bytes at `0x801069E0`. A wide admission window can exceed that fixed
capacity; one measured dense scene needs 97. The recomp relocates the array
and raises all producers and consumers together to 128. Enlarging only a
loader bound would instead turn visual pop-in into memory corruption.

Approximately 69 actor-type-specific assembly sites still contain private X
thresholds between roughly `±0x70` and `±0x100`. They are the remaining long
tail if a particular actor family still exhibits edge behavior.

## 11. Tile layers and parallax maps

### 11.1 Shared grid architecture

Three primary tile layers use the same static draw function,
`func_80082380`; the scrolling backdrop variant uses `func_80082820`.

| Layer | Grid | Wrapper | Principal inputs |
|---|---:|---|---|
| midground | `0x80180930` | `func_80082CFC` | direct map `0x80108DE8`, mode/masks/shift |
| environment | `0x80180B60` | `func_80082E04` | map pointer `0x8013746C`, layout mode |
| backdrop/static | `0x80180D90` | `func_80082F10` | map pointer `0x80137470`, scrolling selector |

The original draw is seven rows by ten columns of 32 × 32 CI8 tiles: 320 ×
224 pixels. Each destination array has 140 `s32` slots, but original code
fills and walks 70 of them at stride ten.

Both draw loops consume the buffer through a walking pointer. Their column
counter controls screen X; it does not index the source array. If a widened
scratch buffer is passed, the argument must point at the scratch **base**.
Adding an imagined left-wing offset shifts every center tile and eventually
reads junk. This single distinction accounted for earlier rainbow tiles and
boot-logo-like garbage.

Tile IDs become texture pointers through:

```text
texture_pointer = *0x80180FC0 + ((tile_id + layer_addend) << 10)
```

Every tile occupies `0x400` bytes. Tile zero is valid and must not be treated
as transparent by convention.

### 11.2 Backdrop/static map

The backdrop map is a wrapping 16 × 16 byte grid. For a row/column derived
from the current scroll origin:

```text
map_col  = ((x0 + column * 32 + 512) >> 5) & 0x0F
row_block = ((y0 + row * 32) >> 1) & 0xF0
tile_id   = map[map_col + row_block]
```

The scrolling variant can use one X origin per row from `0x8011D3B0`, giving
the horizontal parallax band. The static variant uses one backdrop scroll
origin. Both wrap in map space, so the columns outside the original ten are
real neighboring map tiles rather than mirrored pixels.

### 11.3 Environment map

The environment layer has two layouts selected by `0x800BE58C`:

- the normal 16 × 16 layout uses column mask `0x0F` and row block
  `((y >> 1) & 0xF0)`;
- variant 1 uses a 128 × 8 layout, column mask `0x7F`, and row block
  `((y << 2) & 0x380)`.

Its origins derive from scroll registers at `0x800BE578/580`, with the same
32-pixel column step used by the center fill.

### 11.4 Midground map

The midground mode at `0x800BE588` selects camera/parallax formulas. Mode zero
uses `camera_x - 0x92`; modes one through three include the game's `1.55`
parallax division, with a small mode-one X bias. Mode three uses a distinct Y
formula. Column mask, row mask, and shift live at
`0x800BE64C/650/654`.

The midground grid is stored in reverse row order. For memory row `r`, the
corresponding map Y is based on `y0 - (6-r)*32`, not `y0 + r*32`. Forgetting
that reversal produces a plausible but vertically wrong wing.

### 11.5 Fill/draw timing and texture residency

`func_8001107C` fills the center grids early in the tick. Camera and parallax
controllers can update scroll registers before the later draw hooks execute.
Combining the old center with wing columns calculated from new registers
causes a one-tile discontinuity. The recomp snapshots every map pointer,
origin, addend, mask, shift, and per-row band scroll at fill time; all added
columns use that same snapshot.

`func_80026220` records the decompressed map-bank bounds at
`0x80137724/728`. Some maps contain tile IDs whose `0x400`-byte texture slot is
past the loaded bank end. The center never asks for those off-frame IDs, but
new columns can. A per-tile test:

```text
pool_base + ((tile + addend) << 10) + 0x400 <= effective_bank_end
```

prevents unloaded data from becoming texture noise. `func_80026428` reloads a
prefix of the bank in several scenes without updating the recorded extent;
the effective host bound is the union of that prefix and the still-resident
authoritative tail. The ROM's two bound words retain their original meaning.

## 12. Repeating scenery and fixed-viewport effects

### 12.1 Native 512-pixel actor wrap

Two shared actor signatures implement repeating landscape by teleporting a
single copy across a 512-pixel period:

- type `0x181C`, state `2`: foreground trees/grass, sometimes horizontally
  flipped;
- type `0x000D`, state `0x50`: four 128-pixel landscape/floor panels.

The controller is approximately:

```text
screen_x = ((world_x - camera_x) & 0x1FF) - 0x100
```

That covers 320 pixels but not an expanded field at the `-256/+255` rollover.
The recomp inserts a renderer-only adjacent copy shifted by `±512`; simulation
and collision continue to use the original actor.

Actor bytes `0x00..0x7F` contain two renderer-owned 64-byte matrices, one for
each framebuffer. Copying the full `0x198`-byte record can overwrite a matrix
still referenced by the other in-flight display list and cause intermittent
flicker. Only controller-owned bytes `0x80..0x197` are cloned; the current
matrix is rebuilt normally.

### 12.2 Portrait transition grid

`func_8000EA88` draws 66 `PortraitStruct` records:

- entries `0..63` are an 8 × 8 death/room-transition wipe for 320 × 240;
- entries `64..65` are the actual HUD portraits.

In expanded gameplay, drawing the fixed grid produces checker-shaped holes in
only the center. The recomp lets its controller advance but starts visual
iteration at entry 64. Original-mode scenes still draw all 66.

### 12.3 Top-layer color grading

Several scenes express a fixed-viewport color-grade rectangle as a top actor
of type `0x2700` with flag bit 3. It is omitted only in expanded gameplay;
cinematics and original mode retain it.

### 12.4 Missile Surf presentation

Missile Surf (scene 35) rapidly retires and recreates exhaust and explosion
sprites. RT64's temporal interpolation can preserve earlier draws beside their
replacements, producing separated flame copies that flicker in and out. Direct
180 Hz presentation captures reproduce the fault, while native 60 Hz captures
contain exactly one exhaust sprite per game frame.

While scene 35 is actively running, the renderer therefore presents its native
game frames without synthesis. The user's saved frame-rate choice is not
modified and is restored immediately on leaving the scene. Vertigo retains
the user's interpolation setting.

### 12.5 Stage-selection presentation

The same native-frame override covers game state 12 (`GameState_Transition`)
and state 14 (`GameState_Records`). State 12 owns the stage-clear return,
map opening/unlock animation, selection, and departure. Suppressing the whole
controller covers the first opening frames, before the menu becomes interactive.
The renderer restores the user's selected rate on leaving these states.

### 12.6 World 3 panoramas

Clanpot Shake (scene 72), Clance War (12), Go Marzen 64 (32), Chilly Dog!
(31), Snowstorm Maze (36), and Lunar (9) use panoramas whose offscreen map
cells contain repeating filler. The textures are resident, so the normal
bank-bound check cannot reject them. Extending that map reveals square sky
patches alongside the correctly authored center.

During expanded gameplay, only their static backdrop (`D_80180D90`) uses
the original ten tile columns. Each emitted Fast3D texture rectangle is
clipped to the authored horizontal safe area, x=14..302, then stretched to
the window width. Texture S and dS/dX change with the rectangle so tile
contents stretch continuously. The renderer publishes the window aspect
through an atomic value, allowing resizing and different widescreen ratios.
Terrain, actors, and Snowstorm Maze's snow layer retain normal proportions
and the wider field of view. Original 4:3 and cinematic rendering use the
existing path.

`mm_presentation` tests native-frame state selection, panorama coverage at
16:10/16:9/21:9, tile joins, clipping, UV steps, and unchanged vertical values.

The Day Before (scene 33) and Missile Surf (35) instead use screen-space
actors of type `0x1C0D`, state 1, for their sky, clouds, and distant snow.
Their horizontal positions and scales are transformed together from the same
authored safe area to the expanded window. Only renderer-owned copies are
changed; the original controller records and both in-flight matrices are
preserved. Vertical scale and material flags are copied unchanged. Midground
trees, terrain, gameplay actors, and collision retain
their normal proportions and expanded view.

Missile Surf's translucent sky otherwise accumulates over old pixels in the
center while RT64 clears the wings each frame, exposing a brighter 4:3 box.
A separate renderer-owned opaque copy of its `0x1820` sky panel is inserted
before the background lights. The original translucent panel then composites
normally over those lights. This seeds the entire background consistently
without hiding the lights or disabling the wing clear. The extra actor's two
matrices are also preserved across frames, and the local draw list reserves
space for all remaining original actors before adding any repeat/base.

The rectangle correction requires the generated call to
`mm_ws_stretch_backdrop_rect`. Stale `RecompiledFuncs` can otherwise link
successfully and leave the original snow-stage fixes painting only the center.
Configuration and incremental builds verify this and the other critical
widescreen hooks; regenerate from `troublemakers.us1.toml` when that check
fails. Generated C is never hand-edited.

## 13. Rotating-room material contract

Vertigo (progression row 13, scene 69) and Seasick Climb (row 21, scene 13)
share a rotating room in the Migen's Shrine overlay. The room is a rotated
320 × 240 canvas containing normal 3D actors, not a tile background.

Two actor families own the affected private display lists:

| Actor type | Family | Source lists |
|---:|---|---|
| `0x0508` | thirteen wall-animation lists | `0x801B6F28..0x801B7C58` |
| `0x0509` | seven moving-platform lists | `0x801B8808..0x801B8FB8` |

The exact wall groups are seven `0x0F8`-byte lists beginning at
`0x801B6F28`, and six `0x148`-byte lists beginning at `0x801B75F0`. Each
platform list is `0x148` bytes. They are appended by `func_80009BE8` through
the actor's `dlist_17C` field.

The shared setup selects a two-cycle TRILERP combiner. The private lists load
32 × 32 CI8 textures into render tile 0, but:

1. mip tile 1 is not configured although cycle one can blend TEXEL0 and
   TEXEL1 through `LOD_FRAC`;
2. tile 0 declares `maskS=maskT=0` while live UVs visibly require a 32-texel
   repeat.

On the original presentation this legacy state has a known visual intent. A
modern renderer can resolve it as transparent/flat wall panels or a stretched
platform edge, exposing persistent framebuffer pixels as trails.

**Recomp consequence.** Only actor types `0x0508/0509` and twenty allowlisted
source addresses are corrected. Private copies prepend combiner words
`FCFFFE04 FF10F3FF` so cycle one passes TEXEL0, and change matching tile words
from `F5480800 00000000` to `F5480800 00014050`, declaring 32 × 32 wrap masks.
Expected command counts and `G_ENDDL` are verified before redirection; a source
layout mismatch fails closed to the original list. There is no scene-wide
clear, global LOD rule, texture-address heuristic, or renderer-wide
mask-zero override.

## 14. Widescreen compatibility model

Widescreen is not a single projection flag for this engine. A complete frame
is a composite of projected geometry, screen-space tile grids, culled actors,
fixed-canvas effects, and persistent framebuffer content. The current model is:

1. keep opening cinematics, stage cinematics, pause, and known fixed-canvas
   gameplay in original 4:3, while ordinary NPC dialogue remains in the active
   expanded gameplay presentation;
2. enter expansion only after controlled gameplay is stable;
3. widen the X visibility pipeline while preserving Y and player-world walls;
4. rebuild tile wings from the original wrapping maps using the same tick's
   fill state;
5. provide enough actor/list memory for the larger visible set;
6. clear only genuinely unused render-target wings before drawing, never the
   authored center;
7. remove or reinterpret only fixed-viewport effects whose semantic identity
   is known.

The renderer returns to 4:3 after 60 stable inactive frames and enters
expanded gameplay after 30 stable active frames. Pause entry and exit bypass
that generic hysteresis at their exact original state boundaries.

The following gameplay scenes remain intentionally 4:3 because their authored
composition is not a scrollable world:

| Scene | Reason |
|---:|---|
| 25 | fixed vertical boss canvas |
| 27 | fixed final-battle backdrop |
| 57 | fixed authored volcano canvas |
| 71 | fixed Clanball Lift canvas |
| 79 | 4:3 post-process canvas |
| 85 | fixed MERCO boss backdrop |

The rotating scenes 13 and 69 are no longer on this list: their material and
coverage are handled narrowly enough to support expansion. Scene 9 temporarily
uses original boundaries during cinema states 21..25 so Lunar's scripted exit
can complete.

### 14.1 Host-only memory map for expansion data

These regions are outside the original 4 MiB program footprint and contain no
cartridge-authored addresses:

| Region | Host use |
|---:|---|
| `0x80400000..0x80417FFF` | six enlarged layer display-list arenas |
| `0x804269E0...` | relocated 128-entry clan/prop array |
| `0x80440168..0x80454E80` | renderer-only repeated actors and sky base |
| `0x80455000..0x80456DFF` | corrected rotation-material display lists |
| `0x80460000..0x8047FFFF` | double-buffered top-level frame arenas |
| `0x9FFFA000..0x9FFFFFFF` | runtime scratch lists and 7 × 20 grids |

Keeping this map explicit prevents one compatibility fix from overwriting
another and makes it clear which addresses do not describe the original ROM.

## 15. Compatibility catalogue

| Original-ROM behavior | Why a generic host fails | Project treatment |
|---|---|---|
| boot schedules game after VI is already valid | early PC start can observe null VI mode | start on first completed VI callback |
| `rmonMain` is passed as an address | ignored function has no translated entry | register retail-equivalent host no-op at `0x8009A2B8` |
| local functions appear only in pointer tables | stripped ELF misses them | explicit `manual_funcs` metadata |
| flat global symbol namespace contains a function named `send` | can interpose libc/shared-library `send(2)` | hide translated symbols from dynamic export |
| `Sound_Update` reads `AI_LEN` directly | normal libultra wrapper cannot intercept it | mirror the hardware register word |
| `aspMain` is linked at IMEM `0x1080` | assuming `0x1000` misroutes every absolute jump | compile at the proven link base |
| SP event sends may be dropped | reliable requeue changes size-one queue behavior | retain lossy SP semantics |
| overlay code arrives via ordinary PI DMA | bytes land without host function registration | register overlapping translated sections after ROM reads |
| layer arenas are sized for exactly 70 tiles | 140-tile widescreen draw overruns adjacent state | relocate arenas before widening loops |
| scene/map identity is a progression row | changing scene number alone keeps stale assets | warp/select the full row |
| fixed 320-wide effects cover the native canvas | center-only overlay becomes visible in expansion | aspect-state gating or semantic filtering |
| some private Fast3D lists depend on legacy texture state | modern sampler resolves undefined inputs visibly | exact actor/list allowlist correction |

### 15.1 Compiler boundary

GCC 11 generated a translated-game archive that corrupted dialogue display
lists in an otherwise-correct Linux release. Binary/library substitution
isolated the defect to `mm_recompiled`; RT64, window backends, scaling, and
packaging did not move the failure. GCC 12 and newer render the complete
conversation correctly at Release optimization, so CMake rejects older GNU C
compilers. This is a toolchain compatibility boundary, not ROM behavior.

## 16. Performance findings that remain actionable

The native process contains the translated CPU program, recompiled audio RSP,
runtime scheduler, RT64, graphics driver, SDL, and worker threads in one call
tree. Sampling an optimized build is preferred; instrumenting 30,000-plus
generated functions changes the workload.

The principal resolved performance bug was a blocked-message wake loop. A
host event that could not enter a full N64 queue was immediately reinserted
into the same wake queue; `pause_self` woke on it and retried indefinitely.
Before correction, external-message dequeue/requeue work accounted for about
90% of sampled CPU in the diagnostic run. Deferring without self-waking and
coalescing duplicates removed the saturated-core behavior while retaining a
full producer drain, which is needed to avoid DP-message starvation.

Sustained traversal tests after the scheduler and renderer bookkeeping fixes
held the native 60 Hz cadence with empty work/present queues, bounded texture
allocation, stable thread counts, and stable memory after content warm-up on
both discrete and integrated Vulkan paths. The leading resolved project CPU
symbols are now RT64 TMEM block loading/hashing and `aspMain`, rather than a
scheduler loop.

The next graphics optimization should measure duplicate TMEM hash work before
adding a cache. CI textures make the hash palette-dependent; any memoization
must be invalidated by all overlapping TMEM and TLUT writes. LTO/PGO and
target-specific RSP builds are experiments, not justified release defaults
without representative overlay and scene coverage.

Profile an optimized build with:

```sh
tools/profile_recomp.sh --duration 45 --warmup 15 -- \
    input/troublemakers.us1.z64 --window 960x720 --no-widescreen

tools/profile_recomp.sh --mode counters --duration 20 -- \
    input/troublemakers.us1.z64 --window 960x720 --no-widescreen
```

Compare identical scene, aspect, resolution, AA, display rate, device,
backend, warm-up, and duration. CPU utilization, frame latency, and GPU time
are different quantities; a GPU-fence wait is not itself a GPU timestamp.

## 17. Validation and regression strategy

### 17.1 Headless execution

The headless renderer runs the game loop, overlay streaming, audio ucode, and
message flow without Vulkan:

```sh
cmake -B build-headless -DMM_BUILD_GRAPHICS=OFF
cmake --build build-headless --target troublemakers -j

MM_HEADLESS_GFX=1 \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
./build-headless/src/game/troublemakers input/troublemakers.us1.z64
```

It can prove task completion, pacing, overlay lookup, and absence of crashes.
It cannot prove pixel correctness.

### 17.2 Visual suites

Use isolated configuration directories for every automated capture. Display
settings persist, so an earlier widescreen run can otherwise contaminate a
nominal 4:3 baseline. Compare matching gameplay/camera states rather than
matching wall-clock timestamps; attract-mode timing is not deterministic
enough for image A/B by timestamp.

Broad campaign coverage:

```sh
MM_WIDESCREEN=1 tools/test_widescreen_playable.sh \
    ./build/src/game/troublemakers input/troublemakers.us1.z64 build/visual-wide

MM_WIDESCREEN=0 tools/test_widescreen_playable.sh \
    ./build/src/game/troublemakers input/troublemakers.us1.z64 build/visual-4x3
```

Focused rotating-room coverage:

```sh
tools/test_rotation_regressions.sh \
    ./build/src/game/troublemakers \
    input/troublemakers.us1.z64 \
    build/visual-rotation
```

Focused terrain rollover coverage:

```sh
tools/test_terrain_pop.sh \
    ./build/src/game/troublemakers \
    input/troublemakers.us1.z64 \
    build/visual-terrain
```

The regression gates should verify more than non-black pixels:

- the exact progression row and scene reached gameplay-ready state;
- requested 4:3/expanded presentation actually became active;
- audio and display-list counters continued at the expected cadence;
- moving actors stayed in renderer lists through both edges;
- native 512-pixel scenery rollovers remained covered on consecutive frames;
- pause closed over black wings and reopened directly into an expanded frame;
- fixed-canvas transitions did not punch holes in the center;
- dense frame sequences, not only spawn frames, contained no transient trails,
  gray slabs, texture noise, or left-edge stretch;
- no fatal/assert/unknown-function signature appeared in the session log.

The established broad gate covers all 52 player-controlled progression rows
in both widescreen preference and explicit 4:3. Focused suites sample transient
motion densely because a sparse stage screenshot cannot catch framebuffer,
matrix-lifetime, or material failures.

### 17.3 Debugging discipline

- Regenerate translated output after changing TOML hooks, then rerun CMake
  configure because the generated source glob is configure-time.
- Never hand-edit generated `RecompiledFuncs/` or generated `aspMain.cpp` as a
  final fix.
- Launch under `gdb` when attach is restricted. For aligned RDRAM words use
  `rdram + (vaddr - 0x80000000)` directly.
- Inspect individual captured frames when a contact-sheet viewer shows a
  suspicious repeated strip; sheet rendering itself can create false trails.
- Treat a headless success as a simulation gate, not a visual gate.
- Prefer a semantic allowlist—actor type plus exact display-list address and
  validated layout—over a renderer-wide rule inferred from one scene.

## 18. Known long tail

The principal engine-wide 4:3 assumptions are covered, but the ROM was not
authored as a widescreen game. Remaining investigations should start here:

- actor-type-private horizontal thresholds still exist at dozens of overlay
  sites; patch only a reproduced family;
- a scene can intentionally stage actors or art outside 4:3 for a cinematic,
  so “more visible content” is not automatically correct;
- TMEM hashing is the current measurable native hot area, but a cache must
  respect tile, load, and palette mutation;
- any new layer width must be paired with destination, display-list, actor,
  and top-level frame capacity analysis;
- direct hardware loads should be searched explicitly—wrapper coverage does
  not prove that all MMIO was abstracted;
- new compiler versions require dialogue and dense-motion visual validation,
  not only a successful build.

The core lesson from the ROM is that apparently separate rendering glitches
often share a capacity, timing, or ownership invariant. Follow the data from
the original producer through every cull, buffer, task, and consumer before
changing the renderer globally.
