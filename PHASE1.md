You are the Phase 1 engineer on trouble-makers-pc-recomp (Mischief Makers N64 -> PC static recompilation). Work ONLY in /home/thiago/repos/trouble-makers-pc-recomp. Read README.md first — it has the pipeline and the phase roadmap. Phase 0 is done: tools/N64Recomp is built, and RecompiledFuncs/ contains the whole game translated to C (regenerate anytime with `tools/N64Recomp/build/N64Recomp troublemakers.us1.toml`; input ELF at input/troublemakers.elf).

YOUR MISSION — Phase 1 groundwork, in order, committing NOTHING (the driver commits):

1. Add the runtime as a submodule: `git submodule add https://github.com/N64Recomp/N64Recomp.git` is already done for the recompiler; now do `git submodule add https://github.com/N64Recomp/N64ModernRuntime.git lib/N64ModernRuntime` and init its submodules. Study its layout: librecomp (libultra reimplementation + recomp support) and ultramodern (platform/threading layer), and what headers the recompiled C expects (recomp.h etc.).

2. Clone the reference integration READ-ONLY for study: `git clone --depth 1 https://github.com/Zelda64Recomp/Zelda64Recomp.git reference/Zelda64Recomp` (reference/ is for study only — never copy files wholesale; it's a different game and licensed work; learn the INTEGRATION PATTERNS: their CMakeLists, how RecompiledFuncs are compiled, how overlay/section lookup tables are generated, what glue librecomp needs).

3. Create OUR CMakeLists.txt with a static library target `mm_recompiled` that compiles every file in RecompiledFuncs/ with the include paths those files need (they include "recomp.h" / runtime headers — find them in N64ModernRuntime; you may need a small include/ shim dir with game-specific config like recomp_config.h — model on the reference).

4. SUCCESS GATE (must reach): `cmake -B build && cmake --build build --target mm_recompiled -j8` compiles ALL RecompiledFuncs objects with zero errors. This is the Phase 1 milestone — the whole translated game compiling as host-native code. Warnings are fine. If specific generated files are structurally uncompilable, document each in PHASE1_NOTES.md with the error rather than hacking the generated code (RecompiledFuncs is regenerated output — fixes belong in the recompiler config or the shim headers, never hand-edits to RecompiledFuncs).

5. STRETCH (only if gate passed): a `mm_runtime_probe` executable target that links mm_recompiled against librecomp/ultramodern as far as it can; document unresolved symbols in PHASE1_NOTES.md — that unresolved list IS the Phase 2 work plan.

Write PHASE1_NOTES.md as you go: decisions, blockers, what Phase 2 needs. End your final message with:
RESULT
gate: <PASSED or FAILED>
stretch: <attempted/passed/skipped>
notes: <three lines max>
