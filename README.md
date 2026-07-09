# trouble-makers-pc-recomp

Static recompilation of **Mischief Makers** (N64, US 1.1) to portable C via
[N64Recomp](https://github.com/N64Recomp/N64Recomp) — the path to a native PC
build. Sibling project of
[trouble-makers-ai-recomp](../trouble-makers-ai-recomp), whose byte-perfect
decompilation build supplies the symbol-rich ELF that drives this recompiler
(every function the decomp names shows up named here).

## Pipeline

```
decomp repo:  ./trouble build            -> build/troublemakers.elf  (byte-perfect, symbol-rich)
this repo:    cp that ELF into input/
              tools/N64Recomp/build/N64Recomp troublemakers.us1.toml
              -> RecompiledFuncs/*.c     (whole game as portable C)
```

Build the recompiler once: `cmake -B tools/N64Recomp/build tools/N64Recomp && cmake --build tools/N64Recomp/build -j`

No game code, ROM contents, or recompiler output is committed — the
translation runs locally from your legally dumped ROM's decomp build.

## Status / roadmap

- [x] Phase 0 — recompiler builds; whole-ROM translation succeeds (30,260
      symbols, 45MB of C, statically-linked overlays handled)
- [x] Phase 1 — runtime groundwork: N64ModernRuntime submodule; ALL translated game code compiles native (libmm_recompiled.a); see PHASE1_NOTES.md. Remaining Phase 1→2: link probe unresolved-symbol list = the work plan. Original scope:
      [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
      (librecomp = libultra reimplementation, ultramodern = platform layer);
      implement game entry + section/function lookup tables
- [ ] Phase 2 — graphics/audio: RSP microcode via RSPRecomp (game uses
      gspFast3D for graphics — not F3DEX — and aspMain for audio; only aspMain
      is statically recompiled, RT64 interprets the display list); RT64 for
      RDP rendering
- [ ] Phase 3 — game-specific glue: controller/save (EEPROM), Trouble RLE
      asset streaming quirks, TLB if any, stability pass
- [ ] Phase 4 — niceties: widescreen, high framerate, mod hooks (function
      names from the decomp make hooking pleasant)

Reference integration to model on: Zelda64Recomp (same author/toolchain).

## Licensing

All code authored in this repository is **MIT** (see LICENSE) — the most
permissive terms we control. Be aware of what the dependencies impose:

- [N64Recomp](tools/N64Recomp) (the recompiler) and RT64 are MIT.
- [N64ModernRuntime](lib/N64ModernRuntime) (librecomp + ultramodern, the
  runtime `mm_game` statically links) is **GPLv3**. Our MIT sources may be
  freely reused anywhere, but any *distributed binary* of `mm_game` is a
  combined work with the GPLv3 runtime and must be distributed under
  GPLv3-compatible terms.
- reference/Zelda64Recomp (GPLv3) is study-only, gitignored, and not part of
  the build; no code in this repo is derived from it.
- No game code, ROM contents, or recompiler output is committed or covered by
  the LICENSE — the translation runs locally from your own legally dumped ROM.
