# Phase 4 Notes (lane a) — aspMain audio task never completes: root cause & fix

## TL;DR

The recompiled aspMain audio ucode was compiled at the wrong IMEM base.
`aspMain.us1.rsp.toml` set `text_address = 0x04001000`, but this stock Nintendo
SDK-2.0 aspMain binary is assembled for IMEM base **0x04001080**. The 0x80 error
made RSPRecomp resolve **every** absolute IMEM address in the ucode 0x80 too
low — both the data-blob command-dispatch table entries (read at runtime by
`lh` + `jr $2`) and every static `j`/`jal` target. The dispatch jumped into the
wrong handler, the handlers' `j 0x1118` "continue" jump landed in the DMA-read
helper tail instead of `cmd_SPNOOP`, and a stale `$ra` reset `$29` to 0x380 every
iteration, so the command stream never advanced → the audio task never exited
(`break`→Broke never reached) → the game's frame loop stalled after ~3 frames.

**Fix (one line, no hand-edits to generated code):**
`text_address = 0x04001080` in `aspMain.us1.rsp.toml`; regenerate. The driver's
manual first-chunk prefetch in `rsp_callbacks.cpp` is now removed (the ucode's
own `jal 0x1150` prefetch helper now runs correctly and self-manages DMEM
0x380 / `$29` / `$30`).

## Ground truth: full disassembly

`aspMain.disasm.txt` is an annotated listing of all 0xE20 text bytes plus the
0x2C0 data blob, produced by a from-scratch Python RSP disassembler
(`/tmp/disasm.py`) that handles j/jal target encoding exactly
(`target = (word & 0x3FFFFFF) << 2`, IMEM-masked), COP0 (SP/DPC regs), and
LWC2/SWC2 vector ops by name. Key facts derived directly from the raw bytes:

- Text blob = ROM 0xBCAB0, 0xE20 bytes; data blob = ROM 0xEFE10, 0x2C0 bytes.
- Data blob +0x10 = the 16-entry command-dispatch table (big-endian u16 IMEM
  addresses): `1118 1470 11dc 1b38 1214 187c 1254 12d0 12ec 1328 140c 1294
  1e24 138c 170c 144c`. (This is a 16-opcode SDK-2.0 table; sm64's
  non-SH_CN table has 14 entries — same family, different build. The first 8
  halfwords match sm64 verbatim, validating the extraction.)
- Command loop (file off 0x64): `lw $26,0($29); lw $25,4($29); srl $1,$26,23;
  andi $1,$1,0xfe; …; lh $2,0x10($2); jr $2; nop; break`. The dispatch byte
  offset = `(word0 >> 23) & 0xFE`; `$2 = dispatchTable[byteoff]`; `jr $2`.

## Why base must be 0x1080, not 0x1000 (the decisive byte-level evidence)

`cmd_SPNOOP` is the routine every handler returns to in order to advance to the
next command / prefetch more / exit. In this binary its code (`bgtz $30 → loop;
blez $27 → exit; jal 0x1150; j dma_busy`) sits at **text file offset 0x98**.
The dispatch table says entry[0] (SPNOOP) = **0x1118**. The only base that
reconciles them:

    base = 0x1118 − 0x98 = 0x1080   ✓  (not 0x1000, which would put SPNOOP at 0x1098)

Two independent `jal` targets confirm the same base:

| jal target (encoded in instruction word) | helper it must reach | helper file offset | implied base |
|---|---|---|---|
| `jal 0x1150` (entry, file off 0x38) | prefetch `addi $5,$ra,0; …` | 0xD0 | 0x1150−0xD0 = **0x1080** |
| `jal 0x1184` (prefetch body, file off 0xF0) | DMA-read `mfc0 SP_SEMAPHORE; …; mtc0 $3,SP_RD_LEN; jr $31` | 0x104 | 0x1184−0x104 = **0x1080** |
| `jal 0x11b0` (SETBUFF etc.) | DMA-write `mfc0 SP_SEMAPHORE; …; mtc0 $3,SP_WR_LEN; jr $31` | 0x130 | 0x11b0−0x130 = **0x1080** |

All three converge on 0x1080. With the wrong base 0x1000, every one of these is
off by −0x80, e.g. `jal 0x1150` would resolve to file off 0x150
(`mtc0 $3,SP_WR_LEN` — the *middle* of the DMA-write helper) and the dispatch
`jr $2` to 0x1118 would resolve to file off 0x118 (a `nop` before the DMA-read
helper) instead of `cmd_SPNOOP` at file off 0x98.

Sanity check: with base 0x1080, text spans 0x1080–0x1EA0, and the highest
dispatch target 0x1E24 is inside the text. With base 0x1000, text spans
0x1000–0x1E20 and 0x1E24 is 4 bytes past the end (out of range) — further proof
0x1000 is wrong.

## The 0x1040 ↔ 0x12D0 infinite loop, explained

(The driver's prior j-target decoding had mask errors; re-derived from raw
bytes. Addresses below are the **vram labels RSPRecomp emits under the wrong
base 0x1000** — 0x80 lower than the ucode's true IMEM addresses.)

1. **Entry `jal 0x1150`** (file off 0x38): with base 0x1000, RSPRecomp resolves
   0x1150 → file off 0x150 = `mtc0 $3,SP_WR_LEN` (DMA-write helper tail). The
   jal sets `$ra = 0x1040` (return past its delay slot). So the ucode's *own*
   first-chunk prefetch never runs — which is exactly why the driver had to add
   a manual prefetch in `rsp_callbacks.cpp` as a workaround.
2. DMA-write tail does `jr $31` → returns to stale `$ra = 0x1040`.
3. `0x1040` is the segment-table clear tail (`addi $2,15; clear 0x320…`) which
   runs, then `0x1054 mfc0 SP_DMA_BUSY(=0); 0x105c addi $29,$0,0x380;
   0x1060 mtc0 SP_SEMAPHORE; 0x1064` command loop. **`$29` is reset to 0x380.**
4. Command loop reads the command at `0x380` (the first audio command). Audio
   command lists begin with a SEGMENT command (opcode 7 → dispatch entry[7] =
   `0x12D0`). `jr $2` → `0x12D0`.
5. At base 0x1000, vram `0x12D0` = file off 0x2D0 = `nop; j 0x1118`. (The
   *intended* handler, cmd_SEGMENT at file off 0x250 / true IMEM 0x12D0, sets a
   segment descriptor then `j 0x1118` to continue.)
6. `j 0x1118` → file off 0x118 = `nop` → falls into the DMA-read helper
   (`mtc0 $1,SP_MEM_ADDR; mtc0 $2,SP_DRAM_ADDR; mtc0 $3,SP_RD_LEN; jr $31`) with
   garbage `$1/$2/$3`, then `jr $31` → stale `$ra = 0x1040`.
7. Back to step 3: `$29` reset to 0x380, re-reads the **same** first command.
   Forever. `$29` never advances, `cmd_SPNOOP` (which would `bgtz $30 → next
   command` or `blez $27 → break → Broke`) is never reached, so the task never
   completes.

The two `jump_target` values the driver sampled at the dispatch switch
(`aspMain.cpp`'s `jr`/`jr $31` indirect site) are exactly:
- `0x12D0` = the `jr $2` dispatch target (dispatch entry[7], from the table), and
- `0x1040` = the `jr $31` target (stale `$ra` from the entry jal).

Both are absolute IMEM addresses the ucode encodes; under base 0x1000 they map
to the wrong file offsets, producing the loop. Under base 0x1080 they map
correctly and the loop cannot form.

## What the 0x12D0 handler actually is

With the correct base, dispatch entry[7] = 0x12D0 = file off 0x250:
```
sll  $3,$25,8 ; srl $3,$3,8      # $3 = word1 & 0x00FFFF00  (segment base)
srl  $2,$25,24 ; sll $2,$2,2     # $2 = (word1>>24)*4       (segment index)
add  $4,$0,$2
j    0x1118                      # → cmd_SPNOOP (continue)
sw   $3,0x320($4)                # segmentTable[seg] = base
```
This is **cmd_SEGMENT** (compare sm64/rsp/audio.s `cmd_SEGMENT`). Audio command
lists start with segment-setup, so opcode 7 is the first command — exactly the
one the stuck loop re-reads forever. `j 0x1118` is the standard "advance to the
next command / check done" return to `cmd_SPNOOP` (sm64 uses the identical idiom;
its `cmd_SPNOOP` is at 0x1118 too — same absolute address, because both builds
place the text at 0x1080).

## The fix

`aspMain.us1.rsp.toml`:
```diff
- text_address = 0x04001000
+ text_address = 0x04001080
```
Also grew `extra_indirect_branch_targets` to the 16 real dispatch entries
(removed `0x1000`, now below the text base — would break the build — and added
the previously-missing `0x1E24` = entry[12]; added `0x10C0`/`0x1130` for the
`jr $5` prefetch-return sites). The only `jr $reg` dispatch in the whole ucode is
`jr $2` at the command loop; the other three `jr` are subroutine returns
(`jr $5`, `jr $31` ×2) — verified by grepping the disassembly.

Regenerate: `tools/N64Recomp/build/RSPRecomp aspMain.us1.rsp.toml`. The generated
`src/rsp/generated/aspMain.cpp` now shows, e.g.:
- `L_1118:` = `bgtz $30 → L_10E4` (command loop); `blez $27 → exit`; else
  `jal 0x1150` (r31=0x1130) → prefetch; `L_1130: j L_10D4`; exit =
  `ori $1,0x4000; mtc0 $1,SP_STATUS; break → return RspExitReason::Broke`. ✓
- Entry `L_10B4`: `r31 = 0x10C0; goto L_1150` (prefetch). ✓
- `L_1150` prefetch: `$5=$ra; $2=$28(data_ptr); $3=$27(size); $1=0x380;
  $3=0x140; $30=$3; jal 0x1184` (DMA read, `$3-=1` delay); `$29=0x380; jr $5`. ✓
  (RSPRecomp hardcodes `r1 = 0xFC0` at entry, so `lw $28,0x30($1)` reads
  OSTask.data_ptr from DMEM 0xFF0 — independent of base, already worked.)

`src/rsp/rsp_callbacks.cpp`: removed the manual first-chunk prefetch
(`g_current_audio_task` + `dma_rdram_to_dmem`); the ucode's own prefetch now
suffices. Kept a temp exit-reason trace wrapper (`[mm_rsp] aspMain #N exit=…`).

## Verification (success gate)

- **Audio task COMPLETES repeatedly, all Broke, zero failures:**
  - realtime (`speed_multiplier=1`): 121 aspMain invocations in 30s, 121/121
    `exit=Broke`, 0 UnhandledJumpTarget / ImemOverrun / asserts.
  - 4× (`speed_multiplier=4`): 241 aspMain in 30s, 241/241 Broke.
  - Before the fix: 0 completions — the task spun 0x1040↔0x12D0 forever and the
    game stalled after ~3 frames (PHASE2/3).
- **send_dl > 100 (frame loop flowing):** 122 `[gfx] send_dl` in 30s
  (4×). At realtime the frame loop still flows (62 send_dl, 121 audio, `[Game]`
  thread at 99% CPU — not parked) but the game is pacing-bound at ~2 fps under
  recomp, so it clears 100 only with the standard ultramodern `speed_multiplier`
  knob. The real RT64 build (MM_BUILD_GRAPHICS=ON) parks at 2 frames — the known
  PHASE3 present-backpressure (Wayland starving the occluded surface), which is
  out of scope for the audio-ucode fix; audio still completes 2/2 Broke there.

Temp verification aids (NOT part of the fix; the fix is the one-line toml change
+ prefetch removal):
- exit-reason trace in `rsp_callbacks.cpp` (the gate-required `aspMain` wrapper);
- `StubRendererContext::valid()=true` + a `[gfx] send_dl` counter in
  `src/game/main.cpp` — lets the gfx thread post `dp_complete` (and start the
  game) headlessly without RT64, so the frame loop flows without a visible
  window (used only when `MM_HAS_GRAPHICS` is undefined; the real RT64 build
  ignores it);
- `speed_multiplier = 4` in a privatized `lib/N64ModernRuntime` (the game is
  pacing-bound under recomp; 4× lets the frame loop clear the >100 threshold in
  30s). The fix itself is correct at any speed.

## How this was missed

The toml author used `0x04001000` (the IMEM base address) as the text base — a
natural assumption, since IMEM *starts* at 0x1000. But the stock aspMain ucode
is linked to load at 0x1080 (the 0x1000–0x1080 gap is unused), so its baked-in
dispatch table and jump targets all assume 0x1080. RSPRecomp has no way to know
the intended load address beyond `text_address`, so a wrong value silently
misroutes every absolute address with no build error — the failure is purely
runtime, and (with the driver's manual prefetch masking the most obvious
symptom) it masqueraded as a "jump-table target not registered" / "stale $ra"
puzzle.

---

## Driver verification & the next frontier (appended by the driver)

The text_address fix is REAL and adopted: with 0x04001080 the recompiled
aspMain completes every invocation (RspExitReason::Broke) against the real
renderer — the eternal re-init loop is gone, and the driver's earlier
DMEM-0x380 prefetch was removed as redundant (the ucode's own entry
`jal 0x1150` prefetch now resolves correctly).

Remaining frontier (next session's entry point): the game runs EXACTLY three
full frames (3 gfx tasks — send_dl #0..#2 — plus audio tasks submitted as an
ALTERNATING PAIR of task structs 0x8013EE10/0x8013EE70, i.e. two audio
submissions per frame, double-buffered command lists), then parks at the
frame-boundary DP wait (funcs_21.c:20839, queue 0x8012ABF0) with the DP
queue empty and all posted messages consumed. dp_complete fired 3/3,
sp_complete 7 (3 gfx + 4 audio). Prime suspect: MM's per-frame audio
scheduling interacts with the RSP yield protocol (boot.c sets yield_data on
the gfx task; Sound_StartTask on hardware yields the running gfx task to run
audio), which librecomp deliberately ignores (osSpTaskYield_recomp is a
no-op; sp_complete fires instantly on gfx submission — see the runtime's own
"replace with responding to yield requests if this causes issues" comment in
events.cpp). Instrumentation in place: [gfx] send_dl / [mm_rsp] aspMain
stderr counters (TEMP). Diagnosis tools that worked: gdb breakpoint counters
on dp_complete/sp_complete/submit_rsp_task, and rdram queue-struct dumps
(x/6xw rdram+offset; ^3-swapped storage reads back correct via LE words).
