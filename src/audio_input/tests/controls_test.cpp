#include "mm_audio_input.hpp"

#include <SDL2/SDL.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int check(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "controls test failed: %s\n", message);
    return 1;
}

} // namespace

int main() {
    using namespace mm_audio_input;
    namespace fs = std::filesystem;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("troublemakers-controls-test-" + std::to_string(unique));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (check(!ec, "create temporary directory")) return 1;

    int result = 0;
    result |= check(load_control_config(root), "create and load defaults");
    result |= check(fs::exists(root / "controls.json"), "controls.json exists");
    result |= check(get_binding(ControlDevice::Controller, N64Input::A, 0) ==
        InputBinding{BindingType::ControllerButton, SDL_CONTROLLER_BUTTON_A},
        "default A binding");
    result |= check(get_binding(ControlDevice::Controller, N64Input::B, 0) ==
        InputBinding{BindingType::ControllerButton, SDL_CONTROLLER_BUTTON_X},
        "default B uses the west face button");
    result |= check(get_binding(ControlDevice::Controller, N64Input::CDown, 0) ==
        InputBinding{BindingType::ControllerAxis, SDL_CONTROLLER_AXIS_RIGHTY + 1},
        "default C-down uses right-stick down");
    result |= check(get_binding(ControlDevice::Controller, N64Input::CDown, 1) ==
        InputBinding{BindingType::ControllerButton,
                     SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        "default C-down secondary binding");

    const InputBinding custom{BindingType::ControllerButton,
                              SDL_CONTROLLER_BUTTON_BACK};
    result |= check(set_binding(ControlDevice::Controller, N64Input::A, 1, custom),
                    "set second binding");
    result |= check(save_control_config(), "save custom binding");
    reset_all_bindings(ControlDevice::Controller);
    result |= check(load_control_config(root), "reload custom binding");
    result |= check(get_binding(ControlDevice::Controller, N64Input::A, 1) == custom,
                    "custom binding round-trip");

    SDL_Event event{};
    event.type = SDL_CONTROLLERAXISMOTION;
    event.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTX;
    event.caxis.value = -20000;
    InputBinding captured{};
    result |= check(binding_from_event(ControlDevice::Controller, event, captured),
                    "capture controller axis");
    result |= check(captured == InputBinding{BindingType::ControllerAxis,
        -(SDL_CONTROLLER_AXIS_RIGHTX + 1)}, "axis direction encoding");

    // A malformed primary file must not erase the prior valid backup.
    {
        std::ofstream corrupt(root / "controls.json", std::ios::trunc);
        corrupt << "{ not valid JSON";
    }
    reset_all_bindings(ControlDevice::Controller);
    result |= check(load_control_config(root), "recover backup after malformed JSON");
    result |= check(fs::exists(root / "controls.json.bak"), "backup is preserved");

    fs::remove_all(root, ec);
    return result == 0 ? 0 : 1;
}
