// src/game/launcher.cpp — pre-game splash screen (ROM select + display options).
//
// See launcher.h. The flow and wording mirror the Zelda64Recomp launcher
// (reference/Zelda64Recomp/src/ui/ui_launcher.cpp): a "Select ROM" button
// that opens a native file dialog (nativefiledialog-extended, same library
// the reference uses), validation through recomp::select_rom with the same
// per-error messages, and a Start button that only enables once the ROM
// validated. Instead of the reference's RmlUi stack (a full HTML/CSS engine
// rendered through RT64), we draw with Dear ImGui on an SDL_Renderer: both
// are already vendored and compiled inside lib/rt64, and SDL_Renderer has a
// software fallback, so the splash runs anywhere the game itself can.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2_custom.h"                // SDL2 platform backend compiled into rt64
#include "backends/imgui_impl_sdlrenderer2.h"      // renderer backend compiled into the troublemakers exe
#include "nfd.h"

#include "librecomp/game.hpp"

#include "launcher.h"

namespace {

// Window-size presets offered by the Resolution combo. These are the values
// the --window CLI flag takes; RT64 renders the scene at window-integer-scale
// so a bigger window IS a higher internal resolution (see main.cpp).
struct Preset {
    int w, h;
    const char* label;
};
constexpr Preset kPresets[] = {
    { 960,  720, "960 x 720   (4:3)"  },
    { 1280,  960, "1280 x 960  (4:3)"  },
    { 1600, 1200, "1600 x 1200 (4:3)"  },
    { 1920, 1440, "1920 x 1440 (4:3)"  },
    { 1280,  720, "1280 x 720  (16:9)" },
    { 1920, 1080, "1920 x 1080 (16:9)" },
    { 2560, 1440, "2560 x 1440 (16:9)" },
    { 3840, 2160, "3840 x 2160 (16:9)" },
};

// Same messages as the reference launcher's select_rom switch.
// filesystem::path -> UTF-8 std::string (what ImGui expects). path::string()
// narrows through the ACP codepage on Windows and can throw; u8string() is
// UTF-8 on every platform.
std::string path_to_utf8(const std::filesystem::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

const char* rom_error_message(recomp::RomValidationError err) {
    switch (err) {
        case recomp::RomValidationError::Good:             return nullptr;
        case recomp::RomValidationError::FailedToOpen:     return "Failed to open ROM file.";
        case recomp::RomValidationError::NotARom:          return "This is not a valid ROM file.";
        case recomp::RomValidationError::IncorrectRom:     return "This ROM is not the correct game.";
        case recomp::RomValidationError::NotYet:           return "This game isn't supported yet.";
        case recomp::RomValidationError::IncorrectVersion: return "This ROM is the correct game, but the wrong version.\n"
                                                                  "This project requires the NTSC-U N64 version of the game.";
        default:                                           return "An unknown error has occurred.";
    }
}

// Muted burgundy accent (Marina's colors) over the stock dark style.
void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.WindowPadding = ImVec2(28.0f, 24.0f);

    const ImVec4 bg(0.075f, 0.070f, 0.085f, 1.00f);
    const ImVec4 frame(0.16f, 0.15f, 0.18f, 1.00f);
    const ImVec4 accent(0.55f, 0.16f, 0.22f, 1.00f);
    const ImVec4 accent_hi(0.70f, 0.22f, 0.30f, 1.00f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.095f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive] = accent;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accent_hi;
    c[ImGuiCol_Header] = accent;
    c[ImGuiCol_HeaderHovered] = accent_hi;
    c[ImGuiCol_HeaderActive] = accent_hi;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_NavHighlight] = accent_hi;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.28f, 0.32f, 1.00f);
}

} // namespace

namespace mm::launcher {

Outcome run(std::u8string game_id, const std::string& version_string,
            DisplaySettings& settings, std::filesystem::path& rom_path) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[launcher] SDL video init failed: %s\n", SDL_GetError());
        return Outcome::Quit;
    }

    // 640x480 is unreadably small on high-density displays: pick an integer
    // UI scale from the desktop height (1080p -> 1x, 1440p -> 2x, 4K -> 3x)
    // and scale window, style metrics, and font together below.
    int ui_scale = 1;
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(0, &desktop) == 0 && desktop.h > 0) {
        ui_scale = std::clamp(desktop.h / 720, 1, 4);
    }

    SDL_Window* window = SDL_CreateWindow(
        "Trouble Makers",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640 * ui_scale, 480 * ui_scale, SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        std::fprintf(stderr, "[launcher] window creation failed: %s\n", SDL_GetError());
        return Outcome::Quit;
    }

    // Accelerated when available, software otherwise: same portability story
    // as the game (which needs Vulkan anyway), but the splash never hard-fails.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return Outcome::Quit;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini litter
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_style();
    if (ui_scale > 1) {
        ImGui::GetStyle().ScaleAllSizes(static_cast<float>(ui_scale));
        // Rebuild the default font at the scaled size instead of
        // FontGlobalScale, which just stretches the 13px atlas blurry.
        ImFontConfig font_cfg{};
        font_cfg.SizePixels = 13.0f * ui_scale;
        io.Fonts->AddFontDefault(&font_cfg);
    }
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    const bool nfd_ok = (NFD_Init() == NFD_OKAY);

    // Revalidate a remembered ROM so a returning user just hits Start (the
    // reference does the same via recomp::is_rom_valid at launcher creation).
    bool rom_valid = false;
    std::string rom_error;
    std::string rom_display;
    if (!rom_path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(rom_path, ec)) {
            rom_valid = recomp::select_rom(rom_path, game_id) == recomp::RomValidationError::Good;
        }
        if (rom_valid) {
            rom_display = path_to_utf8(rom_path.filename());
        } else {
            rom_path.clear(); // stale entry; ask again, but don't show an error
        }
    }

    // Resolution combo state: index into kPresets, or -1 for a custom
    // --window value we don't want to clobber.
    int preset_index = -1;
    for (int i = 0; i < (int)std::size(kPresets); i++) {
        if (kPresets[i].w == settings.window_w && kPresets[i].h == settings.window_h) {
            preset_index = i;
            break;
        }
    }
    char custom_label[32] = {};
    if (preset_index < 0) {
        std::snprintf(custom_label, sizeof custom_label, "%d x %d (custom)",
                      settings.window_w, settings.window_h);
    }

    Outcome outcome = Outcome::Quit;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // --- Title block ---------------------------------------------------
        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextUnformatted("Trouble Makers");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("N64 recompilation  ·  v%s", version_string.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        // --- ROM -----------------------------------------------------------
        ImGui::SeparatorText("ROM");
        if (rom_valid) {
            ImGui::Text("Loaded: %s", rom_display.c_str());
        } else {
            ImGui::TextDisabled("No ROM selected. Provide your own legally obtained ROM.");
        }
        if (ImGui::Button("Select ROM...")) {
            if (nfd_ok) {
                // No filename filter, matching the reference dialog: dumps
                // show up as .z64/.n64/.v64 and plenty of misnamed variants.
                nfdnchar_t* picked = nullptr;
                nfdresult_t res = NFD_OpenDialogN(&picked, nullptr, 0, nullptr);
                if (res == NFD_OKAY) {
                    std::filesystem::path p{picked};
                    NFD_FreePathN(picked);
                    recomp::RomValidationError err = recomp::select_rom(p, game_id);
                    if (err == recomp::RomValidationError::Good) {
                        rom_valid = true;
                        rom_error.clear();
                        rom_path = p;
                        rom_display = path_to_utf8(p.filename());
                    } else {
                        rom_error = rom_error_message(err);
                    }
                } else if (res == NFD_ERROR) {
                    rom_error = std::string("File dialog error: ") + NFD_GetError();
                }
            } else {
                rom_error = "No native file dialog available. Pass the ROM path "
                            "on the command line instead (see --help).";
            }
        }
        if (!rom_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", rom_error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        // --- Display -------------------------------------------------------
        ImGui::SeparatorText("Display");
        const char* preview = preset_index >= 0 ? kPresets[preset_index].label : custom_label;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Resolution", preview)) {
            if (preset_index < 0 && ImGui::Selectable(custom_label, true)) {
                // keep the custom size
            }
            for (int i = 0; i < (int)std::size(kPresets); i++) {
                if (ImGui::Selectable(kPresets[i].label, i == preset_index)) {
                    preset_index = i;
                    settings.window_w = kPresets[i].w;
                    settings.window_h = kPresets[i].h;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Checkbox("Fullscreen", &settings.fullscreen);
        ImGui::Checkbox("Widescreen", &settings.widescreen);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Expands the rendered field to the window aspect.\n"
                              "Opt-in: can reveal off-stage areas in a 2D game.");
        }

        // --- Start / Exit ---------------------------------------------------
        // Pinned to the bottom of the window.
        float button_h = ImGui::GetFrameHeight();
        float pad_b = ImGui::GetStyle().WindowPadding.y;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - button_h - pad_b);
        ImGui::BeginDisabled(!rom_valid);
        if (ImGui::Button("Start Game", ImVec2(160.0f, 0.0f))) {
            outcome = Outcome::StartGame;
            running = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Exit", ImVec2(100.0f, 0.0f))) {
            running = false;
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 19, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    if (nfd_ok) {
        NFD_Quit();
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    // Leave SDL video initialized: the runtime re-inits it (refcounted) and
    // creates the real game window right after.

    return outcome;
}

} // namespace mm::launcher
