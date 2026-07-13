// src/game/app_dirs.h — per-platform config/save folder resolution.
//
// Modeled on the reference's zelda64::get_app_folder_path()
// (reference/Zelda64Recomp/src/game/config.cpp): a "portable.txt" marker or
// the APP_FOLDER_PATH environment variable (set by the AppImage AppRun in
// portable mode) redirect everything next to the executable; otherwise the
// OS-conventional per-user location is used.
#pragma once

#include <filesystem>

namespace mm {

// Root folder for config, saves, and the stored ROM:
//   - ./          if portable.txt exists in the working directory
//   - $APP_FOLDER_PATH  if set (Linux/macOS; AppImage portable mode)
//   - %LOCALAPPDATA%\troublemakers-recomp   on Windows
//   - $HOME/.config/troublemakers-recomp    on Linux/macOS
// Falls back to the current directory if everything else fails.
std::filesystem::path get_app_folder_path();

} // namespace mm
