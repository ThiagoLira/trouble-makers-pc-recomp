// Controller-first pre-game launcher. The full-screen composition follows the
// navigation model used by current N64 recompilation projects while retaining
// this project's game-specific display, input, and support options.

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2_custom.h"                // SDL2 platform backend compiled into rt64
#include "backends/imgui_impl_sdlrenderer2.h"      // renderer backend compiled into the troublemakers exe
#include "nfd.h"
#include "stb/stb_image.h"

#include "librecomp/game.hpp"

#include "launcher.h"
#include "mm_audio_input.hpp"
#include "session_log.h"
#include "steam_deck.h"

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

constexpr const char* kSplashAsset =
    "assets/launcher/splash-background.jpg";
constexpr const char* kTitleLogoAsset =
    "assets/launcher/title-logo.png";

enum class LauncherPage {
    Main,
    Graphics,
    Controls,
    Support,
    BrowseRom,
};

// Built-in ROM browser, used when the native file dialog is unavailable.
// On Linux nfd talks to xdg-desktop-portal over D-Bus; console-style distros
// (Batocera, bare kiosk sessions) run no portal, so NFD_OpenDialog fails
// outright and the launcher would otherwise have no way to pick a ROM.
struct BrowserEntry {
    std::filesystem::path path;
    std::string label;
    bool is_dir;
};

bool has_rom_extension(const std::filesystem::path& p) {
    std::string ext = path_to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Lists `dir` (directories first, then files, each case-insensitively
// sorted). Dotfiles are skipped; non-ROM files only when show_all is set.
// Returns false if the directory cannot be read.
bool list_browser_dir(const std::filesystem::path& dir, bool show_all,
                      std::vector<BrowserEntry>& out) {
    out.clear();
    std::error_code ec;
    std::filesystem::directory_iterator it(dir,
        std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return false;
    }
    std::vector<BrowserEntry> dirs;
    std::vector<BrowserEntry> files;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::filesystem::path& p = it->path();
        std::string name = path_to_utf8(p.filename());
        if (name.empty() || name[0] == '.') {
            continue;
        }
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            dirs.push_back({ p, name + "/", true });
        } else if (show_all || has_rom_extension(p)) {
            files.push_back({ p, std::move(name), false });
        }
    }
    auto by_name = [](const BrowserEntry& a, const BrowserEntry& b) {
        return lowercase(a.label) < lowercase(b.label);
    };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    out = std::move(dirs);
    out.insert(out.end(), std::make_move_iterator(files.begin()),
               std::make_move_iterator(files.end()));
    return true;
}

// Where the built-in browser opens: the last ROM's folder, else the folder
// the user launched from. AppRun cd's into the AppImage mount, so prefer the
// runtime's $OWD (original working dir) and the AppImage's own folder over
// the process cwd.
std::filesystem::path initial_browser_dir(const std::filesystem::path& rom_path) {
    std::error_code ec;
    auto usable = [&](const std::filesystem::path& p) {
        return !p.empty() && std::filesystem::is_directory(p, ec);
    };
    if (!rom_path.empty() && usable(rom_path.parent_path())) {
        return rom_path.parent_path();
    }
    if (const char* owd = std::getenv("OWD"); owd != nullptr && usable(owd)) {
        return owd;
    }
    if (const char* appimage = std::getenv("APPIMAGE"); appimage != nullptr) {
        const std::filesystem::path dir = std::filesystem::path(appimage).parent_path();
        if (usable(dir)) {
            return dir;
        }
    }
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec && usable(cwd)) {
        return cwd;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && usable(home)) {
        return home;
    }
    return std::filesystem::path("/");
}

struct SplashTexture {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

// RT64 intentionally compiles a custom ImGui SDL backend with controller
// support removed (it avoids conflicts with the controller path used by the
// renderer). The launcher is a separate, controller-first UI, so translate
// SDL controller events into ImGui's navigation keys here. Without this
// bridge NavEnableGamepad is set, but A/B/D-pad/stick input is never delivered
// to ImGui at all.
void update_gamepad_available(ImGuiIO& io) {
    bool available = false;
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            available = true;
            break;
        }
    }

    if (available) {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    } else {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
}

void process_gamepad_navigation_event(ImGuiIO& io, const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERBUTTONDOWN ||
        event.type == SDL_CONTROLLERBUTTONUP) {
        ImGuiKey key = ImGuiKey_None;
        switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A:
                key = ImGuiKey_GamepadFaceDown;
                break;
            case SDL_CONTROLLER_BUTTON_B:
                key = ImGuiKey_GamepadFaceRight;
                break;
            case SDL_CONTROLLER_BUTTON_X:
                key = ImGuiKey_GamepadFaceLeft;
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                key = ImGuiKey_GamepadFaceUp;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                key = ImGuiKey_GamepadDpadLeft;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                key = ImGuiKey_GamepadDpadRight;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                key = ImGuiKey_GamepadDpadUp;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                key = ImGuiKey_GamepadDpadDown;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                key = ImGuiKey_GamepadL1;
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                key = ImGuiKey_GamepadR1;
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                key = ImGuiKey_GamepadL3;
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
                key = ImGuiKey_GamepadR3;
                break;
            case SDL_CONTROLLER_BUTTON_START:
                key = ImGuiKey_GamepadStart;
                break;
            case SDL_CONTROLLER_BUTTON_BACK:
                key = ImGuiKey_GamepadBack;
                break;
            default:
                break;
        }

        if (key != ImGuiKey_None) {
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            io.AddKeyEvent(key, event.type == SDL_CONTROLLERBUTTONDOWN);
        }
        return;
    }

    if (event.type != SDL_CONTROLLERAXISMOTION) {
        return;
    }

    constexpr float dead_zone = 8000.0f;
    constexpr float positive_range = 32767.0f - dead_zone;
    constexpr float negative_range = 32768.0f - dead_zone;
    const float value = static_cast<float>(event.caxis.value);
    const auto positive = [value](float threshold, float range) {
        return std::clamp((value - threshold) / range, 0.0f, 1.0f);
    };
    const auto negative = [value](float threshold, float range) {
        return std::clamp((-value - threshold) / range, 0.0f, 1.0f);
    };
    const auto analog = [&io](ImGuiKey key, float strength) {
        io.AddKeyAnalogEvent(key, strength > 0.0f, strength);
    };

    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    switch (event.caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            analog(ImGuiKey_GamepadLStickLeft,
                   negative(dead_zone, negative_range));
            analog(ImGuiKey_GamepadLStickRight,
                   positive(dead_zone, positive_range));
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            analog(ImGuiKey_GamepadLStickUp,
                   negative(dead_zone, negative_range));
            analog(ImGuiKey_GamepadLStickDown,
                   positive(dead_zone, positive_range));
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX:
            analog(ImGuiKey_GamepadRStickLeft,
                   negative(dead_zone, negative_range));
            analog(ImGuiKey_GamepadRStickRight,
                   positive(dead_zone, positive_range));
            break;
        case SDL_CONTROLLER_AXIS_RIGHTY:
            analog(ImGuiKey_GamepadRStickUp,
                   negative(dead_zone, negative_range));
            analog(ImGuiKey_GamepadRStickDown,
                   positive(dead_zone, positive_range));
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            analog(ImGuiKey_GamepadL2,
                   positive(0.0f, 32767.0f));
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            analog(ImGuiKey_GamepadR2,
                   positive(0.0f, 32767.0f));
            break;
        default:
            break;
    }
}

std::filesystem::path launcher_asset_path(const char* relative_path) {
    if (char* base_path = SDL_GetBasePath()) {
        const std::filesystem::path beside_executable =
            std::filesystem::path(base_path) / relative_path;
        SDL_free(base_path);
        std::error_code ec;
        if (std::filesystem::exists(beside_executable, ec)) {
            return beside_executable;
        }
    }

    // Useful for development builds launched from the repository root.
    return std::filesystem::path(relative_path);
}

SplashTexture load_launcher_texture(SDL_Renderer* renderer,
                                    const char* relative_path,
                                    const char* description) {
    SplashTexture result{};
    const std::filesystem::path path = launcher_asset_path(relative_path);
    const std::string path_utf8 = path_to_utf8(path);
    int channels = 0;
    stbi_uc* pixels = stbi_load(
        path_utf8.c_str(), &result.width, &result.height, &channels, 4);
    if (pixels == nullptr) {
        std::fprintf(stderr, "[launcher] %s unavailable (%s): %s\n",
            description, path_utf8.c_str(), stbi_failure_reason());
        result.width = 0;
        result.height = 0;
        return result;
    }

    result.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, result.width, result.height);
    if (result.texture != nullptr) {
        SDL_UpdateTexture(result.texture, nullptr, pixels, result.width * 4);
        SDL_SetTextureBlendMode(result.texture, SDL_BLENDMODE_BLEND);
        std::fprintf(stderr, "[launcher] %s: %s (%dx%d)\n",
            description, path_utf8.c_str(), result.width, result.height);
    } else {
        std::fprintf(stderr, "[launcher] %s texture creation failed: %s\n",
            description, SDL_GetError());
        result.width = 0;
        result.height = 0;
    }
    stbi_image_free(pixels);
    return result;
}

ImVec2 draw_title_logo(const SplashTexture& logo,
                       float safe_x,
                       float screen_scale,
                       const ImVec2& display_size) {
    const float area_width = std::min(
        650.0f * screen_scale, display_size.x * 0.54f);
    const float top = 40.0f * screen_scale;

    if (logo.texture == nullptr || logo.width <= 0 || logo.height <= 0) {
        const char* fallback = "TROUBLE MAKERS";
        ImFont* font = ImGui::GetFont();
        const float font_size = 52.0f * screen_scale;
        const ImVec2 text_size = font->CalcTextSizeA(
            font_size, FLT_MAX, 0.0f, fallback);
        const ImVec2 local_pos(
            safe_x + (area_width - text_size.x) * 0.5f,
            top);
        const ImVec2 window_pos = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddText(
            font, font_size,
            ImVec2(window_pos.x + local_pos.x, window_pos.y + local_pos.y),
            IM_COL32(255, 197, 47, 255), fallback);
        return ImVec2(safe_x + area_width * 0.5f,
                      top + text_size.y);
    }

    const float max_height = 252.0f * screen_scale;
    const float image_scale = std::min(
        area_width / static_cast<float>(logo.width),
        max_height / static_cast<float>(logo.height));
    const ImVec2 size(
        static_cast<float>(logo.width) * image_scale,
        static_cast<float>(logo.height) * image_scale);
    const ImVec2 local_pos(
        safe_x + (area_width - size.x) * 0.5f,
        top);
    const ImVec2 window_pos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(logo.texture),
        ImVec2(window_pos.x + local_pos.x, window_pos.y + local_pos.y),
        ImVec2(window_pos.x + local_pos.x + size.x,
               window_pos.y + local_pos.y + size.y));
    return ImVec2(safe_x + area_width * 0.5f, top + size.y);
}

void draw_splash_background(const SplashTexture& splash,
                            const ImVec2& display_size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 edge(origin.x + display_size.x, origin.y + display_size.y);

    if (splash.texture != nullptr && splash.width > 0 && splash.height > 0) {
        const float image_aspect =
            static_cast<float>(splash.width) / static_cast<float>(splash.height);
        const float display_aspect = display_size.x / std::max(display_size.y, 1.0f);
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        if (image_aspect > display_aspect) {
            const float visible = display_aspect / image_aspect;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else {
            const float visible = image_aspect / display_aspect;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
        draw->AddImage(reinterpret_cast<ImTextureID>(splash.texture),
            origin, edge, uv0, uv1);
    } else {
        draw->AddRectFilledMultiColor(origin, edge,
            IM_COL32(37, 13, 43, 255), IM_COL32(8, 12, 25, 255),
            IM_COL32(3, 5, 12, 255), IM_COL32(18, 7, 27, 255));
    }

    // Keep the art readable on the left and guarantee high-contrast controls
    // on the right, regardless of the replacement image's color palette.
    draw->AddRectFilled(origin, edge, IM_COL32(5, 6, 12, 62));
    draw->AddRectFilledMultiColor(origin, edge,
        IM_COL32(3, 4, 10, 25), IM_COL32(3, 4, 10, 226),
        IM_COL32(3, 4, 10, 246), IM_COL32(3, 4, 10, 80));
    const ImVec2 lower(origin.x, origin.y + display_size.y * 0.62f);
    draw->AddRectFilledMultiColor(lower, edge,
        IM_COL32(3, 4, 10, 0), IM_COL32(3, 4, 10, 0),
        IM_COL32(3, 4, 10, 205), IM_COL32(3, 4, 10, 205));
}

// High-contrast burgundy and gold, derived from Marina's in-game palette.
void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 7.0f;
    style.GrabRounding = 7.0f;
    style.PopupRounding = 7.0f;
    style.FramePadding = ImVec2(14.0f, 9.0f);
    style.ItemSpacing = ImVec2(12.0f, 12.0f);
    style.WindowPadding = ImVec2(30.0f, 26.0f);
    style.ScrollbarSize = 22.0f;
    style.FrameBorderSize = 1.0f;
    style.ScaleAllSizes(scale);

    const ImVec4 frame(0.11f, 0.105f, 0.15f, 0.94f);
    const ImVec4 accent(0.60f, 0.12f, 0.26f, 1.00f);
    const ImVec4 accent_hi(0.86f, 0.20f, 0.38f, 1.00f);
    const ImVec4 gold(1.00f, 0.73f, 0.25f, 1.00f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.03f, 0.06f, 0.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.035f, 0.035f, 0.065f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.05f, 0.085f, 0.99f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.23f, 0.13f, 0.23f, 0.98f);
    c[ImGuiCol_FrameBgActive] = accent;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accent_hi;
    c[ImGuiCol_Header] = accent;
    c[ImGuiCol_HeaderHovered] = accent_hi;
    c[ImGuiCol_HeaderActive] = accent_hi;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_NavHighlight] = gold;
    c[ImGuiCol_Border] = ImVec4(0.55f, 0.36f, 0.48f, 0.55f);
    c[ImGuiCol_Separator] = ImVec4(0.55f, 0.36f, 0.48f, 0.55f);
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
    ImGui::TextDisabled("Select a binding, then press an input");

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

void draw_graphics_page(mm::launcher::DisplaySettings& settings,
                        bool have_desktop,
                        const SDL_DisplayMode& desktop,
                        int& preset_index,
                        const char* custom_label) {
    const float field_width = std::min(360.0f, ImGui::GetContentRegionAvail().x * 0.52f);

    ImGui::SeparatorText("DISPLAY");
    char fullscreen_label[64] = {};
    if (settings.fullscreen && have_desktop) {
        std::snprintf(fullscreen_label, sizeof fullscreen_label,
            "Desktop (%d x %d)", desktop.w, desktop.h);
    }
    const char* preview = settings.fullscreen && have_desktop
        ? fullscreen_label
        : (preset_index >= 0 ? kPresets[preset_index].label : custom_label);
    ImGui::SetNextItemWidth(field_width);
    ImGui::BeginDisabled(settings.fullscreen);
    if (ImGui::BeginCombo("Resolution", preview)) {
        if (preset_index < 0 && ImGui::Selectable(custom_label, true)) {
            // Preserve a custom command-line size until a preset is selected.
        }
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
            if (ImGui::Selectable(kPresets[i].label, i == preset_index)) {
                preset_index = i;
                settings.window_w = kPresets[i].w;
                settings.window_h = kPresets[i].h;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Checkbox("Fullscreen", &settings.fullscreen);
    if (settings.fullscreen && have_desktop) {
        ImGui::SameLine();
        ImGui::TextDisabled("Uses the desktop mode");
    }
    ImGui::Checkbox("Widescreen", &settings.widescreen);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Expands the rendered field to the window aspect.\n"
                          "Can reveal off-stage areas in this 2D game.");
    }

    ImGui::SeparatorText("IMAGE QUALITY");
    const char* msaa_preview = settings.msaa == 2 ? "2x"
                             : settings.msaa == 4 ? "4x" : "Off";
    ImGui::SetNextItemWidth(field_width);
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
        ImGui::SetTooltip("Smooths polygon edges. Selecting MSAA disables SSAA.");
    }

    char ssaa_preview[16] = "Off";
    if (settings.ssaa > 1) {
        std::snprintf(ssaa_preview, sizeof ssaa_preview, "%dx", settings.ssaa);
    }
    ImGui::SetNextItemWidth(field_width);
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
        ImGui::SetTooltip("Renders at 2x each dimension and downsamples.\n"
                          "This is much heavier than MSAA. Selecting SSAA disables MSAA.");
    }

    ImGui::TextUnformatted("Quick presets");
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float preset_width = std::max(
        120.0f, (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
    if (ImGui::Button("Recommended", ImVec2(preset_width, 0.0f))) {
        settings.fps = 0;
        settings.msaa = 0;
        settings.ssaa = 1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Native 60 fps with antialiasing disabled.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Higher FPS", ImVec2(preset_width, 0.0f))) {
        settings.fps = 120;
        settings.msaa = 0;
        settings.ssaa = 1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Targets 120 fps, clamped to the display refresh rate.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Try 4x MSAA", ImVec2(preset_width, 0.0f))) {
        settings.fps = 0;
        settings.msaa = 4;
        settings.ssaa = 1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Native 60 fps with 4x MSAA for comparison.");
    }

    ImGui::SeparatorText("ADVANCED");
    char fps_preview[24];
    if (settings.fps == 0) {
        std::snprintf(fps_preview, sizeof fps_preview, "Native (60)");
    } else if (settings.fps < 0) {
        std::snprintf(fps_preview, sizeof fps_preview, "Match display");
    } else {
        std::snprintf(fps_preview, sizeof fps_preview, "%d fps", settings.fps);
    }
    ImGui::SetNextItemWidth(field_width);
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
        ImGui::SetTooltip("RT64 interpolates the extra frames; game logic remains at 60 Hz.\n"
                          "The target is capped to the monitor refresh rate.");
    }

    ImGui::Checkbox("VSync", &settings.vsync);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Synchronizes presentation to the display. Disabling it can tear.");
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

    ImGui::TextDisabled("Estimated target: %d x %d · %d fps · ~%.1fx raster samples",
        estimated_internal_width, estimated_internal_height,
        effective_rate, estimated_sample_load);
    if (settings.ssaa > 1 && effective_rate > 60) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.48f, 0.32f, 1.0f));
        ImGui::TextWrapped("Very high cost: SSAA and interpolation multiply each other. "
                           "Reduce one if frame pacing degrades.");
        ImGui::PopStyleColor();
    } else if (estimated_sample_load >= 8.0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
        ImGui::TextWrapped("High renderer load. Reduce antialiasing or the frame-rate "
                           "target if pacing degrades.");
        ImGui::PopStyleColor();
    } else if (settings.msaa > 0 || settings.ssaa > 1) {
        ImGui::TextDisabled("AA is optional and often subtle at this internal resolution.");
    }

    ImGui::Checkbox("Enable debug menu", &settings.debug_menu);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("F1 or controller L+R+Start opens the in-game overlay.\n"
                          "Using a debug warp blocks save writes until exit.");
    }
}

} // namespace

namespace mm::launcher {

Outcome run(std::u8string game_id, const std::string& version_string,
            DisplaySettings& settings, std::filesystem::path& rom_path) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[launcher] SDL init failed: %s\n", SDL_GetError());
        return Outcome::Quit;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    std::fprintf(stderr, "[launcher] SDL video driver: %s\n",
                 SDL_GetCurrentVideoDriver() != nullptr
                    ? SDL_GetCurrentVideoDriver() : "unknown");

    SDL_DisplayMode desktop{};
    const bool have_desktop =
        SDL_GetCurrentDisplayMode(0, &desktop) == 0 &&
        desktop.w > 0 && desktop.h > 0;

    // 1280x800 is the launcher's reference canvas and the Steam Deck's native
    // display. Small displays get a borderless desktop window; larger desktop
    // displays get a centered, resizable 16:10 window.
    const bool compact_display =
        have_desktop && desktop.w <= 1280 && desktop.h <= 800;
    const bool deck_desktop_controls = mm::platform::is_steam_deck() &&
        std::getenv("SteamGameId") == nullptr &&
        std::getenv("SteamAppId") == nullptr;
    const int launcher_w = have_desktop ? std::min(desktop.w, 1280) : 1280;
    const int launcher_h = have_desktop ? std::min(desktop.h, 800) : 800;
    const Uint32 window_flags = SDL_WINDOW_ALLOW_HIGHDPI |
        SDL_WINDOW_RESIZABLE |
        (compact_display ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

    SDL_Window* window = SDL_CreateWindow(
        "Trouble Makers",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        launcher_w, launcher_h, window_flags);
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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini litter
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_NavEnableGamepad;
    update_gamepad_available(io);
    const float interface_scale = std::clamp(
        static_cast<float>(launcher_h) / 800.0f, 0.80f, 1.0f);
    apply_style(interface_scale);
    ImFontConfig font_cfg{};
    font_cfg.SizePixels = 20.0f * interface_scale;
    io.Fonts->AddFontDefault(&font_cfg);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SplashTexture splash = load_launcher_texture(
        renderer, kSplashAsset, "splash image");
    SplashTexture title_logo = load_launcher_texture(
        renderer, kTitleLogoAsset, "title logo");

    const bool nfd_ok = (NFD_Init() == NFD_OKAY);
    if (!nfd_ok) {
        std::fprintf(stderr,
            "[launcher] native file dialog unavailable (%s); using built-in browser\n",
            NFD_GetError());
    }

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
    LauncherPage page = LauncherPage::Main;
    bool request_focus = true;
    bool back_requested = false;

    // Built-in browser state. MM_BUILTIN_FILE_BROWSER forces it (testing, or
    // a portal that exists but misbehaves).
    bool use_builtin_browser =
        !nfd_ok || std::getenv("MM_BUILTIN_FILE_BROWSER") != nullptr;
    std::filesystem::path browser_dir;
    std::vector<BrowserEntry> browser_entries;
    std::string browser_error;
    bool browser_show_all = false;
    bool browser_focus_first = false;

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
        process_gamepad_navigation_event(io, ev);
        if (ev.type == SDL_QUIT) {
            running = false;
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED ||
            ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            mm_audio_input::refresh_controllers();
            update_gamepad_available(io);
        }
        if (!capture_active && page != LauncherPage::Main &&
            ((ev.type == SDL_KEYDOWN && ev.key.repeat == 0 &&
              ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) ||
             (ev.type == SDL_CONTROLLERBUTTONDOWN &&
              ev.cbutton.button == SDL_CONTROLLER_BUTTON_B))) {
            back_requested = true;
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
    // Validates and adopts a picked file. Returns true on success.
    auto accept_rom = [&](const std::filesystem::path& picked_path) {
        const recomp::RomValidationError error =
            recomp::select_rom(picked_path, game_id);
        if (error == recomp::RomValidationError::Good) {
            rom_valid = true;
            rom_error.clear();
            rom_path = picked_path;
            rom_display = path_to_utf8(picked_path.filename());
            return true;
        }
        rom_error = rom_error_message(error);
        return false;
    };
    auto browse_to = [&](const std::filesystem::path& dir) {
        if (list_browser_dir(dir, browser_show_all, browser_entries)) {
            browser_dir = dir;
            browser_error.clear();
        } else {
            browser_error = "Cannot open " + path_to_utf8(dir);
        }
        browser_focus_first = true;
    };
    auto open_builtin_browser = [&]() {
        rom_error.clear();
        browser_error.clear();
        browse_to(initial_browser_dir(rom_path));
        page = LauncherPage::BrowseRom;
    };
    auto select_rom = [&]() {
        if (use_builtin_browser) {
            open_builtin_browser();
            return;
        }

        // No filename filter: dumps commonly use .z64, .n64, or .v64 and are
        // often misnamed. Validation below is authoritative.
        nfdnchar_t* picked = nullptr;
        const nfdresult_t result = NFD_OpenDialogN(&picked, nullptr, 0, nullptr);
        if (result == NFD_OKAY) {
            const std::filesystem::path picked_path{picked};
            NFD_FreePathN(picked);
            accept_rom(picked_path);
        } else if (result == NFD_ERROR) {
            // Typically no xdg-desktop-portal (Batocera and other
            // console-style sessions). Fall back for the rest of the session.
            std::fprintf(stderr,
                "[launcher] file dialog error: %s; using built-in browser\n",
                NFD_GetError());
            use_builtin_browser = true;
            open_builtin_browser();
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

        if (back_requested) {
            page = LauncherPage::Main;
            back_requested = false;
            request_focus = true;
        }

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##launcher", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground);
        draw_splash_background(splash, io.DisplaySize);

        const float screen_scale = std::clamp(
            io.DisplaySize.y / 800.0f, 0.72f, 1.35f);
        const float safe_x = 54.0f * screen_scale;
        const float safe_y = 48.0f * screen_scale;

        ImGui::BeginDisabled(capture_active);
        if (page == LauncherPage::Main) {
            // Keep the identity lockup centered within the art half instead
            // of treating the game title like oversized menu copy.
            const ImVec2 title_anchor = draw_title_logo(
                title_logo, safe_x, screen_scale, io.DisplaySize);
            const char* subtitle = "N64 STATIC RECOMPILATION";
            const ImVec2 subtitle_size = ImGui::CalcTextSize(subtitle);
            ImGui::SetCursorPos(ImVec2(
                title_anchor.x - subtitle_size.x * 0.5f,
                title_anchor.y + 6.0f * screen_scale));
            ImGui::TextColored(ImVec4(1.00f, 0.73f, 0.25f, 1.0f),
                "%s", subtitle);
            const std::string version_label = "Version " + version_string;
            const ImVec2 version_size = ImGui::CalcTextSize(version_label.c_str());
            ImGui::SetCursorPosX(title_anchor.x - version_size.x * 0.5f);
            ImGui::TextDisabled("Version %s", version_string.c_str());

            const float menu_width = std::min(
                430.0f * screen_scale, io.DisplaySize.x * 0.43f);
            const float menu_height = io.DisplaySize.y - safe_y * 2.0f;
            ImGui::SetCursorPos(ImVec2(
                io.DisplaySize.x - menu_width - safe_x, safe_y));
            ImGui::BeginChild("##main_menu", ImVec2(menu_width, menu_height),
                true, ImGuiWindowFlags_NoScrollbar);

            ImGui::SetWindowFontScale(1.35f);
            ImGui::TextUnformatted("MAIN MENU");
            ImGui::SetWindowFontScale(1.0f);
            if (rom_valid) {
                ImGui::TextDisabled("ROM: %s", rom_display.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("Provide your own legally obtained ROM.");
                ImGui::PopStyleColor();
            }
            if (deck_desktop_controls) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.00f, 0.73f, 0.25f, 1.0f));
                ImGui::TextWrapped(
                    "STEAM DECK: Add this AppImage to Steam, then launch it "
                    "from Gaming Mode. Desktop Mode uses keyboard/mouse "
                    "button shortcuts.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f * screen_scale));

            const float button_height = 53.0f * screen_scale;
            const ImVec2 menu_button_size(-FLT_MIN, button_height);
            const bool focus_primary_action = request_focus;
            if (request_focus) {
                ImGui::SetKeyboardFocusHere();
                request_focus = false;
            }
            if (rom_valid) {
                const bool start_game =
                    ImGui::Button("START GAME", menu_button_size);
                if (focus_primary_action) {
                    ImGui::SetItemDefaultFocus();
                }
                if (start_game) {
                    outcome = Outcome::StartGame;
                    running = false;
                }
                if (ImGui::Button("CHANGE ROM", menu_button_size)) {
                    select_rom();
                }
            } else {
                const bool select_rom_pressed =
                    ImGui::Button("SELECT ROM", menu_button_size);
                if (focus_primary_action) {
                    ImGui::SetItemDefaultFocus();
                }
                if (select_rom_pressed) {
                    select_rom();
                }
            }

            if (ImGui::Button("GRAPHICS", menu_button_size)) {
                page = LauncherPage::Graphics;
                request_focus = true;
            }
            if (ImGui::Button("CONTROLS", menu_button_size)) {
                page = LauncherPage::Controls;
                request_focus = true;
            }
            if (ImGui::Button("SUPPORT", menu_button_size)) {
                page = LauncherPage::Support;
                request_focus = true;
            }
            if (ImGui::Button("EXIT", menu_button_size)) {
                running = false;
            }

            if (!rom_error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
                ImGui::TextWrapped("%s", rom_error.c_str());
                ImGui::PopStyleColor();
            }

            const float hint_y = ImGui::GetWindowHeight() -
                ImGui::GetTextLineHeightWithSpacing() -
                ImGui::GetStyle().WindowPadding.y;
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), hint_y));
            ImGui::TextDisabled(
                "A / Enter  Select    D-pad  Navigate    View  Fullscreen in game");
            ImGui::EndChild();
        } else {
            const float panel_width = io.DisplaySize.x - safe_x * 2.0f;
            const float panel_height = io.DisplaySize.y - safe_y * 2.0f;
            ImGui::SetCursorPos(ImVec2(safe_x, safe_y));
            ImGui::BeginChild("##settings_panel",
                ImVec2(panel_width, panel_height), true);

            const bool focus_back = request_focus;
            if (focus_back) {
                ImGui::SetKeyboardFocusHere();
                request_focus = false;
            }
            const bool back_pressed = ImGui::Button("<  BACK");
            if (focus_back) {
                ImGui::SetItemDefaultFocus();
            }
            if (back_pressed) {
                page = LauncherPage::Main;
                request_focus = true;
            }
            ImGui::SameLine();
            ImGui::SetWindowFontScale(1.55f);
            switch (page) {
                case LauncherPage::Graphics:
                    ImGui::TextUnformatted("GRAPHICS");
                    break;
                case LauncherPage::Controls:
                    ImGui::TextUnformatted("CONTROLS");
                    break;
                case LauncherPage::Support:
                    ImGui::TextUnformatted("SUPPORT");
                    break;
                case LauncherPage::BrowseRom:
                    ImGui::TextUnformatted("SELECT ROM");
                    break;
                default:
                    break;
            }
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();

            const float footer_height =
                ImGui::GetTextLineHeightWithSpacing() + 10.0f * screen_scale;
            const ImGuiWindowFlags content_flags =
                page == LauncherPage::Graphics
                    ? ImGuiWindowFlags_AlwaysVerticalScrollbar
                    : ImGuiWindowFlags_None;
            ImGui::BeginChild("##page_content",
                ImVec2(0.0f, -footer_height), false, content_flags);
            if (page == LauncherPage::Graphics) {
                draw_graphics_page(settings, have_desktop, desktop,
                    preset_index, custom_label);
            } else if (page == LauncherPage::Controls) {
                draw_controls_tab(controls_device, capture_active,
                    capture_device, capture_input, capture_slot,
                    axis_neutral, controls_status);
            } else if (page == LauncherPage::Support) {
                draw_support_tab(support_status);
            } else if (page == LauncherPage::BrowseRom) {
                ImGui::TextWrapped("%s", path_to_utf8(browser_dir).c_str());
                if (ImGui::Checkbox("Show all files", &browser_show_all)) {
                    browse_to(browser_dir);
                    browser_focus_first = false;
                }
                const auto show_error = [](const std::string& message) {
                    if (!message.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
                        ImGui::TextWrapped("%s", message.c_str());
                        ImGui::PopStyleColor();
                    }
                };
                show_error(browser_error);
                show_error(rom_error);
                ImGui::Separator();

                // Defer navigation until after the loop: browse_to()
                // replaces browser_entries.
                std::filesystem::path navigate_to;
                const std::filesystem::path parent = browser_dir.parent_path();
                const bool has_parent = !parent.empty() && parent != browser_dir;
                if (browser_focus_first) {
                    ImGui::SetKeyboardFocusHere();
                    browser_focus_first = false;
                }
                if (has_parent && ImGui::Selectable("../")) {
                    navigate_to = parent;
                }
                for (size_t i = 0; i < browser_entries.size(); ++i) {
                    const BrowserEntry& entry = browser_entries[i];
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(entry.label.c_str())) {
                        if (entry.is_dir) {
                            navigate_to = entry.path;
                        } else if (accept_rom(entry.path)) {
                            page = LauncherPage::Main;
                            request_focus = true;
                        }
                    }
                    ImGui::PopID();
                }
                if (browser_entries.empty()) {
                    ImGui::TextDisabled(browser_show_all
                        ? "(empty folder)"
                        : "(no .z64 / .n64 / .v64 files here)");
                }
                if (!navigate_to.empty()) {
                    rom_error.clear();
                    browse_to(navigate_to);
                }
            }
            ImGui::EndChild();

            ImGui::TextDisabled(
                "A / Enter  Select    B / Escape  Back    D-pad / Stick  Navigate");
            ImGui::EndChild();
        }
        ImGui::EndDisabled();

        if (capture_active) {
            ImGui::SetNextWindowPos(
                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.99f);
            ImGui::Begin("Bind input", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetWindowFontScale(1.25f);
            ImGui::Text("Binding %s (slot %zu)",
                mm_audio_input::input_name(capture_input), capture_slot + 1);
            ImGui::SetWindowFontScale(1.0f);
            if (capture_device == mm_audio_input::ControlDevice::Controller) {
                ImGui::TextWrapped(
                    "Press a controller button, or move an axis from neutral.\n"
                    "Return a held stick or trigger to neutral before binding it.");
            } else {
                ImGui::TextWrapped("Press a key.");
            }
            ImGui::TextDisabled("Escape cancels");
            if (ImGui::Button("CANCEL")) {
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
    if (splash.texture != nullptr) {
        SDL_DestroyTexture(splash.texture);
    }
    if (title_logo.texture != nullptr) {
        SDL_DestroyTexture(title_logo.texture);
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
