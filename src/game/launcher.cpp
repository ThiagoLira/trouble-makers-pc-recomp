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
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "mm_audio_input.hpp"
#include "session_log.h"

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

void save_controls_with_status(std::string& status) {
    if (mm_audio_input::save_control_config()) {
        status = "Bindings saved to controls.json.";
    } else {
        status = "Could not save controls.json; see the log for details.";
    }
}

void draw_controls_tab(mm_audio_input::ControlDevice& selected_device,
                       bool& capture_active,
                       mm_audio_input::ControlDevice& capture_device,
                       mm_audio_input::N64Input& capture_input,
                       size_t& capture_slot,
                       std::array<bool, SDL_CONTROLLER_AXIS_MAX>& axis_neutral,
                       std::string& status) {
    using namespace mm_audio_input;

    if (ImGui::RadioButton("Controller",
            selected_device == ControlDevice::Controller)) {
        selected_device = ControlDevice::Controller;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Keyboard",
            selected_device == ControlDevice::Keyboard)) {
        selected_device = ControlDevice::Keyboard;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Click a binding, then press an input");

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInner |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
    const float footer_height = ImGui::GetFrameHeightWithSpacing() * 3.2f;
    if (ImGui::BeginTable("##control_bindings", 5, flags,
                          ImVec2(0.0f, -footer_height))) {
        const float font_size = ImGui::GetFontSize();
        ImGui::TableSetupColumn("N64 input", ImGuiTableColumnFlags_WidthFixed,
                                font_size * 7.5f);
        ImGui::TableSetupColumn("Binding 1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Binding 2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, font_size * 4.2f);
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed,
                                font_size * 4.2f);
        ImGui::TableHeadersRow();

        for (size_t index = 0; index < kN64InputCount; ++index) {
            const auto input = static_cast<N64Input>(index);
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input_name(input));

            for (size_t slot = 0; slot < kBindingsPerInput; ++slot) {
                ImGui::TableSetColumnIndex(static_cast<int>(slot + 1));
                ImGui::PushID(static_cast<int>(slot));
                const std::string label = binding_name(
                    get_binding(selected_device, input, slot));
                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                    capture_active = true;
                    capture_device = selected_device;
                    capture_input = input;
                    capture_slot = slot;
                    axis_neutral.fill(false);
                    status.clear();
                }
                ImGui::PopID();
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Clear")) {
                clear_bindings(selected_device, input);
                save_controls_with_status(status);
            }
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Reset")) {
                reset_bindings(selected_device, input);
                save_controls_with_status(status);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Reset all")) {
        reset_all_bindings(selected_device);
        save_controls_with_status(status);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Two bindings can be active at the same time.");
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}

void draw_support_tab(std::string& status) {
    using mm::session_log::Session;
    ImGui::TextWrapped(
        "When reporting a problem, reproduce it once, relaunch, then copy the "
        "previous session report. The pasted report is capped for GitHub; the "
        "full logs remain on disk.");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextUnformatted("Log folder:");
    ImGui::TextWrapped("%s",
        path_to_utf8(mm::session_log::log_directory()).c_str());

    const bool has_previous = mm::session_log::has_report(Session::Previous);
    ImGui::BeginDisabled(!has_previous);
    if (ImGui::Button("Copy previous session")) {
        const std::string report = mm::session_log::read_report(Session::Previous);
        if (!report.empty() && SDL_SetClipboardText(report.c_str()) == 0) {
            status = "Previous session report copied to the clipboard.";
        } else {
            status = std::string("Could not copy the report: ") + SDL_GetError();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copy current session")) {
        const std::string report = mm::session_log::read_report(Session::Current);
        if (!report.empty() && SDL_SetClipboardText(report.c_str()) == 0) {
            status = "Current session report copied to the clipboard.";
        } else {
            status = std::string("Could not copy the report: ") + SDL_GetError();
        }
    }
    if (!has_previous) {
        ImGui::TextDisabled("No previous session log exists yet.");
    }
    if (!status.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextWrapped("%s", status.c_str());
    }
}

} // namespace

namespace mm::launcher {

Outcome run(std::u8string game_id, const std::string& version_string,
            DisplaySettings& settings, std::filesystem::path& rom_path) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[launcher] SDL video init failed: %s\n", SDL_GetError());
        return Outcome::Quit;
    }
    std::fprintf(stderr, "[launcher] SDL video driver: %s\n",
                 SDL_GetCurrentVideoDriver() != nullptr
                    ? SDL_GetCurrentVideoDriver() : "unknown");

    // Scale the launcher on platforms where SDL reports physical desktop
    // pixels without the window system scaling the window for us. Windows
    // already applies the user's display scale to an SDL window; deriving a
    // second scale from the physical desktop height makes a 1440p/150% setup
    // effectively 3x, which overflows the launcher and breaks hit testing.
    SDL_DisplayMode desktop{};
    const bool have_desktop =
        SDL_GetCurrentDisplayMode(0, &desktop) == 0 &&
        desktop.w > 0 && desktop.h > 0;
    int ui_scale = 1;
#if !defined(_WIN32)
    if (have_desktop) {
        ui_scale = std::clamp(desktop.h / 720, 1, 4);
    }
#endif

    SDL_Window* window = SDL_CreateWindow(
        "Trouble Makers",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640 * ui_scale, 560 * ui_scale, SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        std::fprintf(stderr, "[launcher] window creation failed: %s\n", SDL_GetError());
        return Outcome::Quit;
    }

    // Accelerated when available, software otherwise: same portability story
    // as the game (which needs Vulkan anyway), but the splash never hard-fails.
    bool software_renderer = false;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        software_renderer = renderer != nullptr;
    }
    if (renderer == nullptr) {
        std::fprintf(stderr, "[launcher] renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return Outcome::Quit;
    }
    SDL_RendererInfo renderer_info{};
    if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
        software_renderer =
            (renderer_info.flags & SDL_RENDERER_SOFTWARE) != 0;
        std::fprintf(stderr,
            "[launcher] renderer=%s accelerated=%s software=%s vsync=%s\n",
            renderer_info.name != nullptr ? renderer_info.name : "unknown",
            (renderer_info.flags & SDL_RENDERER_ACCELERATED) ? "yes" : "no",
            (renderer_info.flags & SDL_RENDERER_SOFTWARE) ? "yes" : "no",
            (renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no");
    }
    if (software_renderer && ui_scale > 2) {
        // Avoid software-blitting an oversized HiDPI launcher surface.
        ui_scale = 2;
        SDL_SetWindowSize(window, 640 * ui_scale, 560 * ui_scale);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED);
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

    mm_audio_input::ControlDevice controls_device =
        mm_audio_input::ControlDevice::Controller;
    mm_audio_input::ControlDevice capture_device = controls_device;
    mm_audio_input::N64Input capture_input = mm_audio_input::N64Input::A;
    size_t capture_slot = 0;
    bool capture_active = false;
    std::array<bool, SDL_CONTROLLER_AXIS_MAX> axis_neutral{};
    std::string controls_status;
    std::string support_status;

    Outcome outcome = Outcome::Quit;
    bool running = true;
    bool first_frame = true;
    const bool trace_events = std::getenv("MM_LAUNCHER_TRACE") != nullptr;
    std::uint64_t event_count = 0;
    auto process_event = [&](SDL_Event& ev) {
        ++event_count;
        if (trace_events && ((event_count <= 20) || ((event_count % 100) == 0))) {
            std::fprintf(stderr, "[launcher] event #%llu type=0x%x\n",
                static_cast<unsigned long long>(event_count), ev.type);
        }
        ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) {
            running = false;
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED ||
            ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            mm_audio_input::refresh_controllers();
        }

        if (capture_active) {
            bool accepted = false;
            mm_audio_input::InputBinding binding{};
            if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 &&
                ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                capture_active = false;
                controls_status = "Binding cancelled.";
            } else if (capture_device == mm_audio_input::ControlDevice::Controller &&
                       ev.type == SDL_CONTROLLERAXISMOTION &&
                       ev.caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
                const int value = static_cast<int>(ev.caxis.value);
                const int magnitude = value < 0 ? -value : value;
                if (magnitude < 8192) {
                    axis_neutral[ev.caxis.axis] = true;
                } else if (magnitude >= 16384 && axis_neutral[ev.caxis.axis]) {
                    accepted = mm_audio_input::binding_from_event(
                        capture_device, ev, binding);
                }
            } else {
                accepted = mm_audio_input::binding_from_event(
                    capture_device, ev, binding);
            }

            if (accepted) {
                if (mm_audio_input::set_binding(
                        capture_device, capture_input, capture_slot, binding)) {
                    save_controls_with_status(controls_status);
                } else {
                    controls_status = "That input cannot be used for this binding.";
                }
                capture_active = false;
            }
        }
    };
    while (running) {
        SDL_Event ev;
        if (software_renderer && !first_frame) {
            // There is no continuous animation. Sleep until input/window
            // activity instead of repainting the same CPU surface indefinitely.
            if (SDL_WaitEvent(&ev) == 0) {
                continue;
            }
            process_event(ev);
        }
        while (SDL_PollEvent(&ev)) {
            process_event(ev);
        }
        first_frame = false;
        const Uint64 frame_started = SDL_GetTicks64();

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

        ImGui::BeginDisabled(capture_active);
        if (ImGui::BeginTabBar("##launcher_tabs")) {
        if (ImGui::BeginTabItem("Play")) {
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
        char fullscreen_label[64] = {};
        if (settings.fullscreen && have_desktop) {
            std::snprintf(fullscreen_label, sizeof fullscreen_label,
                          "Desktop (%d x %d)", desktop.w, desktop.h);
        }
        const char* preview = settings.fullscreen && have_desktop
            ? fullscreen_label
            : (preset_index >= 0 ? kPresets[preset_index].label : custom_label);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::BeginDisabled(settings.fullscreen);
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
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Checkbox("Fullscreen", &settings.fullscreen);
        if (settings.fullscreen && have_desktop) {
            ImGui::TextDisabled(
                "Fullscreen uses the desktop mode; the saved size is restored in windowed mode.");
        }
        ImGui::Checkbox("Widescreen", &settings.widescreen);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Expands the rendered field to the window aspect.\n"
                              "Opt-in: can reveal off-stage areas in a 2D game.");
        }

        const char* msaa_preview = settings.msaa == 2 ? "2x"
                                 : settings.msaa == 4 ? "4x"
                                                      : "Off";
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("MSAA", msaa_preview)) {
            constexpr int values[] = {0, 2, 4};
            constexpr const char* labels[] = {"Off", "2x", "4x"};
            for (int i = 0; i < static_cast<int>(std::size(values)); ++i) {
                if (ImGui::Selectable(labels[i], settings.msaa == values[i])) {
                    settings.msaa = values[i];
                    if (settings.msaa > 0) {
                        settings.ssaa = 1;
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Optionally smooths polygon edges, but the difference can be subtle\n"
                              "at the game's already-high internal resolution. Selecting MSAA disables SSAA.");
        }

        char ssaa_preview[16] = "Off";
        if (settings.ssaa > 1) {
            std::snprintf(ssaa_preview, sizeof ssaa_preview, "%dx", settings.ssaa);
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("SSAA", ssaa_preview)) {
            constexpr int values[] = {1, 2};
            constexpr const char* labels[] = {"Off", "2x"};
            for (int i = 0; i < static_cast<int>(std::size(values)); ++i) {
                if (ImGui::Selectable(labels[i], settings.ssaa == values[i])) {
                    settings.ssaa = values[i];
                    if (settings.ssaa > 1) {
                        settings.msaa = 0;
                    }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Renders at 2x each dimension (4x as many pixels) and downsamples.\n"
                              "This is substantially heavier than MSAA, especially above 60 fps.\n"
                              "Selecting SSAA disables MSAA.");
        }

        ImGui::TextUnformatted("Quick presets");
        if (ImGui::Button("Recommended (AA off)")) {
            settings.fps = 0;
            settings.msaa = 0;
            settings.ssaa = 1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Native 60 fps with antialiasing disabled.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Higher FPS (AA off)")) {
            settings.fps = 120;
            settings.msaa = 0;
            settings.ssaa = 1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Targets 120 fps with interpolation (clamped to the display). Prefer\n"
                              "this visible motion upgrade over AA when there is spare performance.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Try 4x MSAA")) {
            settings.fps = 0;
            settings.msaa = 4;
            settings.ssaa = 1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Native 60 fps with 4x MSAA for an A/B image-quality check.");
        }

        // --- Advanced ------------------------------------------------------
        ImGui::SeparatorText("Advanced");

        // Frame interpolation (RT64). 0 = native 60; -1 = match display; a
        // positive value is an interpolated target (game logic stays 60Hz).
        char fps_preview[24];
        if (settings.fps == 0)       std::snprintf(fps_preview, sizeof fps_preview, "Native (60)");
        else if (settings.fps < 0)   std::snprintf(fps_preview, sizeof fps_preview, "Match display");
        else                         std::snprintf(fps_preview, sizeof fps_preview, "%d fps", settings.fps);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Frame rate", fps_preview)) {
            constexpr int values[] = {0, 120, 144, 240, -1};
            constexpr const char* labels[] = {
                "Native (60)", "120 fps", "144 fps", "240 fps", "Match display"};
            for (int i = 0; i < static_cast<int>(std::size(values)); ++i) {
                if (ImGui::Selectable(labels[i], settings.fps == values[i])) {
                    settings.fps = values[i];
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Interpolates smoother motion above the game's 60Hz using RT64.\n"
                              "The game's logic still runs at 60Hz; the extra frames are\n"
                              "synthesized. Capped to your monitor's refresh rate.");
        }

        ImGui::Checkbox("VSync", &settings.vsync);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Waits for the display's vertical sync before presenting (default).\n"
                "Turn off to skip that wait without touching driver settings.\n"
                "Presentation can tear; frame pacing above 60 fps may be less even.");
        }

        const int output_width = settings.fullscreen && have_desktop
            ? desktop.w : settings.window_w;
        const int output_height = settings.fullscreen && have_desktop
            ? desktop.h : settings.window_h;
        int effective_rate = 60;
        if (settings.fps < 0) {
            effective_rate = have_desktop && desktop.refresh_rate > 0
                ? desktop.refresh_rate : 60;
        } else if (settings.fps > 0) {
            effective_rate = settings.fps;
            if (have_desktop && desktop.refresh_rate > 0) {
                effective_rate = std::min(effective_rate, desktop.refresh_rate);
            }
        }
        const int output_scale = std::max((output_height + 239) / 240, 1);
        const int internal_scale = output_scale * std::max(settings.ssaa, 1);
        const double aspect_expansion = settings.widescreen && output_height > 0
            ? std::max((static_cast<double>(output_width) / output_height) /
                       (4.0 / 3.0), 1.0)
            : 1.0;
        const int estimated_internal_width = static_cast<int>(std::lround(
            320.0 * internal_scale * aspect_expansion));
        const int estimated_internal_height = 240 * internal_scale;
        const double frame_multiplier = std::max(effective_rate / 60.0, 1.0);
        const double aa_multiplier = settings.ssaa > 1
            ? static_cast<double>(settings.ssaa * settings.ssaa)
            : static_cast<double>(std::max(settings.msaa, 1));
        const double estimated_sample_load = frame_multiplier * aa_multiplier;

        ImGui::TextDisabled(
            "Estimated internal target: %d x %d, %d fps target, ~%.1fx raster samples",
            estimated_internal_width, estimated_internal_height,
            effective_rate, estimated_sample_load);
        ImGui::TextDisabled(
            "For a noticeable upgrade, increase frame rate before enabling AA.");
        if (settings.ssaa > 1 && effective_rate > 60) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(1.00f, 0.48f, 0.32f, 1.0f));
            ImGui::TextWrapped(
                "Very high cost: SSAA and frame interpolation multiply each other. "
                "Use Recommended (AA off) or reduce the frame-rate target if pacing degrades.");
            ImGui::PopStyleColor();
        } else if (estimated_sample_load >= 8.0) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
            ImGui::TextWrapped(
                "High renderer load. Test native 60 or reduce antialiasing if frame pacing degrades.");
            ImGui::PopStyleColor();
        } else if (settings.msaa > 0 || settings.ssaa > 1) {
            ImGui::TextDisabled(
                "AA is optional and often subtle here. Compare it with AA off before keeping the cost.");
        }

        ImGui::Checkbox("Enable debug menu", &settings.debug_menu);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "F1 or controller L+R+Start opens the in-game overlay.\n"
                "Using a debug warp blocks all save writes until exit.");
        }

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls")) {
            draw_controls_tab(controls_device, capture_active, capture_device,
                              capture_input, capture_slot, axis_neutral,
                              controls_status);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Support")) {
            draw_support_tab(support_status);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }

        // --- Start / Exit ---------------------------------------------------
        // Pinned to the bottom of the window. Derive the size from the scaled
        // font and padding: fixed pixel widths clip both labels when the Linux
        // launcher scales itself for a high-DPI desktop.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float extra_edge = 2.0f * static_cast<float>(ui_scale);
        const float button_h = ImGui::GetFontSize() + style.FramePadding.y * 2.0f +
                               extra_edge;
        const auto button_width = [&](const char* label, float base_width) {
            return std::max(base_width * static_cast<float>(ui_scale),
                ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f +
                extra_edge);
        };
        const float start_button_w = button_width("Start Game", 160.0f);
        const float exit_button_w = button_width("Exit", 100.0f);
        const float pad_b = style.WindowPadding.y;
        const float footer_y = ImGui::GetWindowHeight() - button_h - pad_b;
        // Never move the cursor backwards over the Advanced controls. Apart
        // from drawing the rows on top of each other, overlapping ImGui items
        // leave the earlier checkbox's hit rectangle in charge, so clicking
        // Start toggles the hidden checkbox instead. If an unusually small
        // window still cannot fit everything, keeping the natural cursor
        // position lets ImGui expose the footer through normal scrolling.
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), footer_y));
        ImGui::BeginDisabled(!rom_valid);
        if (ImGui::Button("Start Game", ImVec2(start_button_w, button_h))) {
            outcome = Outcome::StartGame;
            running = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Exit", ImVec2(exit_button_w, button_h))) {
            running = false;
        }
        ImGui::EndDisabled();

        if (capture_active) {
            ImGui::SetNextWindowPos(
                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.98f);
            ImGui::Begin("Bind input", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("Binding %s (slot %zu)",
                        mm_audio_input::input_name(capture_input), capture_slot + 1);
            if (capture_device == mm_audio_input::ControlDevice::Controller) {
                ImGui::TextWrapped(
                    "Press a controller button, or move an axis from neutral.\n"
                    "Return a held stick or trigger to neutral before binding it.");
            } else {
                ImGui::TextWrapped("Press a key.");
            }
            ImGui::TextDisabled("Escape cancels");
            if (ImGui::Button("Cancel")) {
                capture_active = false;
                controls_status = "Binding cancelled.";
            }
            ImGui::End();
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 19, 18, 22, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);

        // PRESENTVSYNC is a request and may be ignored. Bound accelerated
        // backends independently and cap software redraw storms at 10 FPS.
        const Uint64 minimum_frame_milliseconds =
            software_renderer ? 100 : 16;
        const Uint64 elapsed = SDL_GetTicks64() - frame_started;
        if (elapsed < minimum_frame_milliseconds) {
            SDL_Delay(static_cast<Uint32>(
                minimum_frame_milliseconds - elapsed));
        }
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
