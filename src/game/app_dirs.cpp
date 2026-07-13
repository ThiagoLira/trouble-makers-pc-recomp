// src/game/app_dirs.cpp — per-platform config/save folder resolution.
// See app_dirs.h. Mirrors reference/Zelda64Recomp/src/game/config.cpp
// (zelda64::get_app_folder_path) minus the macOS bundle handling, which can
// come with a macOS port.

#include <cstdlib>

#include "app_dirs.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace {
constexpr const char* kProgramId = "troublemakers-recomp";
}

namespace mm {

std::filesystem::path get_app_folder_path() {
    // Portable mode: a portable.txt next to the executable (the launcher
    // scripts cd there first) keeps everything self-contained.
    std::error_code ec;
    if (std::filesystem::exists("portable.txt", ec)) {
        return std::filesystem::current_path(ec);
    }

#if defined(_WIN32)
    std::filesystem::path recomp_dir{};
    PWSTR known_path = NULL;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known_path);
    if (result == S_OK) {
        recomp_dir = std::filesystem::path{known_path} / kProgramId;
    }
    CoTaskMemFree(known_path);
    if (!recomp_dir.empty()) {
        return recomp_dir;
    }
#else
    // AppImage portable mode: AppRun sets APP_FOLDER_PATH to the directory
    // holding the AppImage when a portable.txt sits next to it.
    if (const char* app_folder = getenv("APP_FOLDER_PATH")) {
        return std::filesystem::path{app_folder};
    }

    const char* homedir = getenv("HOME");
    if (homedir == nullptr) {
        if (passwd* pw = getpwuid(getuid())) {
            homedir = pw->pw_dir;
        }
    }
    if (homedir != nullptr) {
        return std::filesystem::path{homedir} / ".config" / kProgramId;
    }
#endif

    // Last resort: run out of the current directory.
    return std::filesystem::current_path(ec);
}

} // namespace mm
