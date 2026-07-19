// Input backend for ultramodern's input::callbacks_t, backed by SDL2 and the
// persistent two-slot mappings in controls.cpp. The game is single-player, so
// keyboard and the first SDL game controller are combined into N64 port 0.
#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "mm_audio_input.hpp"

namespace mm_audio_input {
namespace {

// N64 controller button bits (libultra PR/os_cont.h layout).
constexpr uint16_t BTN_A      = 0x8000;
constexpr uint16_t BTN_B      = 0x4000;
constexpr uint16_t BTN_Z      = 0x2000; // CONT_G
constexpr uint16_t BTN_START = 0x1000;
constexpr uint16_t BTN_UP     = 0x0800;
constexpr uint16_t BTN_DOWN   = 0x0400;
constexpr uint16_t BTN_LEFT   = 0x0200;
constexpr uint16_t BTN_RIGHT  = 0x0100;
constexpr uint16_t BTN_L      = 0x0020;
constexpr uint16_t BTN_R      = 0x0010;
constexpr uint16_t BTN_CU     = 0x0008; // CONT_E
constexpr uint16_t BTN_CD     = 0x0004; // CONT_D
constexpr uint16_t BTN_CL     = 0x0002; // CONT_C
constexpr uint16_t BTN_CR     = 0x0001; // CONT_F

const Uint8* g_keys = nullptr;
int g_numkeys = 0;
SDL_GameController* g_controller = nullptr;
std::atomic_bool g_input_blocked{false};

constexpr float kDigitalAxisThreshold = 0.5f;

inline bool key(SDL_Scancode s) {
    return g_keys != nullptr && static_cast<int>(s) < g_numkeys && g_keys[s] != 0;
}

// Normalize an SDL axis to [-1.0, 1.0].
inline float axis_norm(Sint16 v) {
    return static_cast<float>(v) / (v < 0 ? 32768.0f : 32767.0f);
}

void open_first_controller() {
    if (g_controller != nullptr) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller != nullptr) {
                SDL_Joystick* joystick = SDL_GameControllerGetJoystick(g_controller);
                char guid[33] = {};
                if (joystick != nullptr) {
                    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid,
                                              static_cast<int>(sizeof(guid)));
                }
                char* mapping = SDL_GameControllerMapping(g_controller);
                const char* name = SDL_GameControllerName(g_controller);
                std::fprintf(stderr,
                    "[input] Controller connected: %s (GUID %s, SDL mapping: %s)\n",
                    name != nullptr ? name : "Unknown controller",
                    guid[0] != '\0' ? guid : "unknown",
                    mapping != nullptr ? "yes" : "no");
                SDL_free(mapping);
                return;
            }
        }
    }
}

void refresh_controller_internal() {
    if (g_controller != nullptr && !SDL_GameControllerGetAttached(g_controller)) {
        std::fprintf(stderr, "[input] Controller disconnected.\n");
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    open_first_controller();
}

float binding_strength(InputBinding binding) {
    switch (binding.type) {
        case BindingType::None:
            return 0.0f;
        case BindingType::Keyboard:
            return key(static_cast<SDL_Scancode>(binding.id)) ? 1.0f : 0.0f;
        case BindingType::ControllerButton:
            if (g_controller == nullptr) return 0.0f;
            return SDL_GameControllerGetButton(
                       g_controller, static_cast<SDL_GameControllerButton>(binding.id))
                ? 1.0f : 0.0f;
        case BindingType::ControllerAxis: {
            if (g_controller == nullptr || binding.id == 0) return 0.0f;
            const int encoded_axis = std::abs(binding.id);
            const auto axis = static_cast<SDL_GameControllerAxis>(encoded_axis - 1);
            const float direction = binding.id < 0 ? -1.0f : 1.0f;
            return std::max(0.0f,
                axis_norm(SDL_GameControllerGetAxis(g_controller, axis)) * direction);
        }
    }
    return 0.0f;
}

float input_strength(N64Input input) {
    float result = 0.0f;
    constexpr std::array devices{
        ControlDevice::Keyboard, ControlDevice::Controller};
    for (ControlDevice device : devices) {
        for (size_t slot = 0; slot < kBindingsPerInput; ++slot) {
            result = std::max(result, binding_strength(get_binding(device, input, slot)));
        }
    }
    return result;
}

void poll_input() {
    // Refresh the keyboard snapshot. SDL_PumpEvents is idempotent if the host
    // also pumps; calling it here keeps input self-sufficient even if the host
    // loop hasn't pumped yet this frame.
    SDL_PumpEvents();
    g_keys = SDL_GetKeyboardState(&g_numkeys);
    refresh_controller_internal();
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    if (g_input_blocked.load(std::memory_order_relaxed)) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    struct DigitalInput {
        N64Input input;
        uint16_t mask;
    };
    constexpr std::array digital_inputs{
        DigitalInput{N64Input::A, BTN_A},
        DigitalInput{N64Input::B, BTN_B},
        DigitalInput{N64Input::Z, BTN_Z},
        DigitalInput{N64Input::Start, BTN_START},
        DigitalInput{N64Input::DpadUp, BTN_UP},
        DigitalInput{N64Input::DpadDown, BTN_DOWN},
        DigitalInput{N64Input::DpadLeft, BTN_LEFT},
        DigitalInput{N64Input::DpadRight, BTN_RIGHT},
        DigitalInput{N64Input::L, BTN_L},
        DigitalInput{N64Input::R, BTN_R},
        DigitalInput{N64Input::CUp, BTN_CU},
        DigitalInput{N64Input::CDown, BTN_CD},
        DigitalInput{N64Input::CLeft, BTN_CL},
        DigitalInput{N64Input::CRight, BTN_CR},
    };

    uint16_t b = 0;
    for (const DigitalInput& input : digital_inputs) {
        if (input_strength(input.input) >= kDigitalAxisThreshold) b |= input.mask;
    }

    const float sx = input_strength(N64Input::AnalogRight) -
                     input_strength(N64Input::AnalogLeft);
    const float sy = input_strength(N64Input::AnalogUp) -
                     input_strength(N64Input::AnalogDown);

    *buttons = b;
    *x = std::clamp(sx, -1.0f, 1.0f);
    *y = std::clamp(sy, -1.0f, 1.0f);
    return true;
}

void set_rumble(int controller_num, bool on) {
    if (controller_num != 0 || g_controller == nullptr) return;
    if (!SDL_GameControllerHasRumble(g_controller)) return;
    // N64 rumble is binary; use full-strength low-freq motor. A 0-length pulse
    // stops it; a long pulse sustains it (the runtime re-issues each VI).
    SDL_GameControllerRumble(g_controller, on ? 0xFFFF : 0, on ? 0xFFFF : 0, 100);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }
    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

} // namespace

void refresh_controllers() {
    refresh_controller_internal();
}

void set_input_blocked(bool blocked) {
    g_input_blocked.store(blocked, std::memory_order_relaxed);
}

// Bring up the game-controller subsystem from init() (audio init lives in
// audio.cpp; this just makes sure input can open pads).
void init_input_subsystem() {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "mm_audio_input: SDL_InitSubSystem(GAMECONTROLLER): %s\n", SDL_GetError());
        }
    }
    refresh_controller_internal();
}

ultramodern::input::callbacks_t input_callbacks() {
    return ultramodern::input::callbacks_t{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
}

} // namespace mm_audio_input
