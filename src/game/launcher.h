// src/game/launcher.h — pre-game splash screen (ROM select + display options).
//
// Modeled on the launcher flow of the other N64 recomp projects
// (Zelda64Recomp ui_launcher.cpp): Select ROM -> recomp::select_rom -> Start.
// Ours runs in its own small SDL2 window *before* the runtime spawns the game
// window, drawn with the Dear ImGui + nativefiledialog-extended libraries
// already vendored inside lib/rt64 — no new dependencies.
//
// Only compiled when the RT64 graphics component is built (MM_HAS_GRAPHICS);
// headless builds keep the CLI-only flow.
#pragma once

#include <filesystem>
#include <string>

namespace mm::launcher {

// The display options the splash exposes (see the CLI flags in main.cpp).
struct DisplaySettings {
    int window_w;
    int window_h;
    bool fullscreen;
    bool widescreen;
    int msaa;
    int ssaa;
    // Frame-rate target for RT64 frame interpolation. 0 = native 60Hz (no
    // interpolation); -1 = match the display's refresh; a positive value is
    // an interpolated FPS target (game logic still runs at 60Hz).
    int fps;
};

enum class Outcome {
    StartGame, // a validated ROM is loaded in librecomp; boot the runtime
    Quit,      // user closed the launcher; exit cleanly without starting
};

// Blocks running the splash screen. `rom_path` carries the remembered ROM in
// (revalidated silently if it still exists) and the user's selection out.
// `game_id` must already be registered via recomp::register_game.
Outcome run(std::u8string game_id, const std::string& version_string,
            DisplaySettings& settings, std::filesystem::path& rom_path);

} // namespace mm::launcher
