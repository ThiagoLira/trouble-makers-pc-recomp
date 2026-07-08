// mm_runtime_probe — Phase 1 stretch target.
//
// Purpose: link the whole translated game (mm_recompiled) against the runtime
// (librecomp + ultramodern) as far as the linker will take it, then surface the
// *unresolved* symbol set. That set is the Phase 2 work plan (graphics/audio/RSP
// via RT64 + RSPRecomp, game entry, overlay registration, controller/save glue).
//
// This main() intentionally does almost nothing: the goal is link-time
// discovery, not a running game. If this builds and links, the runtime is
// structurally complete enough to host the game; if it fails to link, the
// linker's undefined-reference list is the deliverable (see PHASE1_NOTES.md).
#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    std::printf("mm_runtime_probe: linked against librecomp/ultramodern.\n");
    return 0;
}
