// Input backend for ultramodern's input::callbacks_t, backed by SDL2.
//
// The game is 1-player, so only controller 0 is serviced. Two
// sources feed port 0: the keyboard (always available, see the map below) and,
// if one is attached, the first SDL game controller. Rumble is forwarded to
// the game controller when supported.
//
// Default keyboard map (documented in PHASE2_NOTES_w4.md):
//   Analog stick .... W A S D
//   D-pad ........... Arrow Up/Down/Left/Right
//   A ............... X
//   B ............... C
//   Z (trigger) ..... Left Shift
//   L ............... Q
//   R ............... E
//   C-up ............ I      C-down .. K
//   C-left .......... J      C-right . L
//   Start ........... Return
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>

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
            if (g_controller != nullptr) return;
        }
    }
}

void poll_input() {
    // Refresh the keyboard snapshot. SDL_PumpEvents is idempotent if the host
    // also pumps; calling it here keeps input self-sufficient even if the host
    // loop hasn't pumped yet this frame.
    SDL_PumpEvents();
    g_keys = SDL_GetKeyboardState(&g_numkeys);
    open_first_controller();
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    uint16_t b = 0;
    float sx = 0.0f;
    float sy = 0.0f;

    // ---- Keyboard ----
    if (key(SDL_SCANCODE_X))            b |= BTN_A;
    if (key(SDL_SCANCODE_C))            b |= BTN_B;
    if (key(SDL_SCANCODE_LSHIFT))       b |= BTN_Z;
    if (key(SDL_SCANCODE_RETURN))       b |= BTN_START;
    if (key(SDL_SCANCODE_Q))            b |= BTN_L;
    if (key(SDL_SCANCODE_E))            b |= BTN_R;
    if (key(SDL_SCANCODE_I))            b |= BTN_CU;
    if (key(SDL_SCANCODE_K))            b |= BTN_CD;
    if (key(SDL_SCANCODE_J))            b |= BTN_CL;
    if (key(SDL_SCANCODE_L))            b |= BTN_CR;

    if (key(SDL_SCANCODE_UP))           b |= BTN_UP;
    if (key(SDL_SCANCODE_DOWN))         b |= BTN_DOWN;
    if (key(SDL_SCANCODE_LEFT))         b |= BTN_LEFT;
    if (key(SDL_SCANCODE_RIGHT))        b |= BTN_RIGHT;

    // Analog stick via WASD (up = +y, matching N64 osContData convention).
    if (key(SDL_SCANCODE_W)) sy += 1.0f;
    if (key(SDL_SCANCODE_S)) sy -= 1.0f;
    if (key(SDL_SCANCODE_A)) sx -= 1.0f;
    if (key(SDL_SCANCODE_D)) sx += 1.0f;

    // ---- Game controller (if attached) ----
    if (g_controller != nullptr) {
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A))            b |= BTN_A;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B))            b |= BTN_B;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= BTN_L;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))b |= BTN_R;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START))        b |= BTN_START;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP))     b |= BTN_UP;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))   b |= BTN_DOWN;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))   b |= BTN_LEFT;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))  b |= BTN_RIGHT;
        // Map face X/Y to the C-buttons (north=C-up, west=C-left, east=C-right
        // already taken by B; south taken by A) so the C-group is reachable.
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y))           b |= BTN_CU;
        if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X))           b |= BTN_CL;
        // Left trigger stands in for Z (the N64's main trigger).
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8191) b |= BTN_Z;

        // Analog stick overrides keyboard so a real stick wins when used.
        Sint16 ax = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax != 0 || ay != 0) {
            sx = axis_norm(ax);
            sy = -axis_norm(ay); // SDL Y is down-positive; N64 Y is up-positive.
        }
    }

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

// Bring up the game-controller subsystem from init() (audio init lives in
// audio.cpp; this just makes sure input can open pads).
void init_input_subsystem() {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "mm_audio_input: SDL_InitSubSystem(GAMECONTROLLER): %s\n", SDL_GetError());
        }
    }
    open_first_controller();
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
