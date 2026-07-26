// Public API of the mm_rsp static library (RSP microcode recompilation).
//
// Public API for the game's statically recompiled aspMain audio ucode and the
// recomp::rsp::callbacks_t that dispatches audio RSP tasks to it. The host
// executable places these callbacks in the recomp::Configuration passed to
// recomp::start().
//
// Host contract:
//   #include "mm_rsp.hpp"   // this dir is on mm_rsp's PUBLIC include path
//   ...
//   recomp::Configuration cfg{};
//   cfg.rsp_callbacks = mm_rsp::make_callbacks();
//   ... (renderer/audio/input callbacks)
//   recomp::start(cfg);
//
// Graphics RSP tasks (gspFast3D) are deliberately NOT recompiled here — RT64
// interprets the display list via renderer_callbacks. See docs/README.md.
#pragma once

#include "librecomp/rsp.hpp"

namespace mm_rsp {
// Build the RSP callback set. Installs the recompiled aspMain audio ucode for
// M_AUDTASK; all other task types return nullptr (gfx tasks never reach RSP
// dispatch — ultramodern routes them to the renderer).
recomp::rsp::callbacks_t make_callbacks();

// Convenience: install the callbacks via recomp::rsp::set_callbacks() and prime
// the RSP lookup-table constants. Use this only if the host calls set_callbacks
// directly instead of going through Configuration; the Configuration path is
// preferred.
void register_callbacks();
} // namespace mm_rsp
