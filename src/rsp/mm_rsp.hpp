// Public API of the mm_rsp static library (RSP microcode recompilation).
//
// Worker w2 owns the game's statically-recompiled RSP ucode (the aspMain audio
// microcode) and the recomp::rsp::callbacks_t that dispatches RSP tasks to it.
// Worker w1 (src/game/) builds the host main(); it fills the
// recomp::Configuration it hands to recomp::start() with the callbacks produced
// here.
//
// Contract for w1:
//   #include "mm_rsp.hpp"   // this dir is on mm_rsp's PUBLIC include path
//   ...
//   recomp::Configuration cfg{};
//   cfg.rsp_callbacks = mm_rsp::make_callbacks();
//   ... (renderer/audio/input/... callbacks from the other workers)
//   recomp::start(cfg);
//
// Graphics RSP tasks (gspFast3D) are deliberately NOT recompiled here — RT64
// interprets the display list via renderer_callbacks. See PHASE2_NOTES_w2.md.
#pragma once

#include "librecomp/rsp.hpp"

namespace mm_rsp {
// Build the RSP callback set. Installs the recompiled aspMain audio ucode for
// M_AUDTASK; all other task types return nullptr (gfx tasks never reach RSP
// dispatch — ultramodern routes them to the renderer).
recomp::rsp::callbacks_t make_callbacks();

// Convenience: install the callbacks via recomp::rsp::set_callbacks() and prime
// the RSP lookup-table constants. Use this only if w1 calls set_callbacks
// directly instead of going through Configuration; the Configuration path is
// preferred.
void register_callbacks();
} // namespace mm_rsp
