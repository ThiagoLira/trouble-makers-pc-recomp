// Save glue for librecomp. librecomp owns EEPROM persistence entirely
// (librecomp/src/eep.cpp wraps osEeprom*_recomp; librecomp/src/pi.cpp backs
// it with an in-memory buffer flushed to <config_path>/saves/<game_id>.bin by
// a dedicated saving thread). The host's only responsibilities are:
//   1. declare the save type on the GameEntry registered via recomp::register_game
//   2. call recomp::register_config_path with a writable folder
// This file provides the surface for both. See PHASE2_NOTES_w4.md.
#include <filesystem>

#include "librecomp/game.hpp"

#include "mm_audio_input.hpp"

namespace mm_audio_input {

// 4Kbit EEPROM (512 bytes / 64 8-byte blocks). All osEepromLongRead/Write
// calls in RecompiledFuncs use block addresses <= 0x2C (44), well within the
// 4K limit, so Eep4k is the correct (and most accurate) choice.
recomp::SaveType save_type() {
    return recomp::SaveType::Eep4k;
}

void register_save_config(const std::filesystem::path& config_path) {
    recomp::register_config_path(config_path);
}

} // namespace mm_audio_input
