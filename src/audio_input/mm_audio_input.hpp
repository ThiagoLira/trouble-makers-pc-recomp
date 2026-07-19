// mm_audio_input — Phase 2 audio + input + save glue for Trouble Makers:
// static recomp. Backs ultramodern's abstract `audio_callbacks_t` and
// `input::callbacks_t` with SDL2, and exposes the game's EEPROM save type for
// the GameEntry that src/game/ registers with librecomp.
//
// This header is the contract worker w1 (src/game/, main()) adopts: call
// `mm_audio_input::init()` once after SDL2 is brought up, then plug the
// returned callback structs into `recomp::Configuration` before
// `recomp::start()`. See PHASE2_NOTES_w4.md for the exact call sequence.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <SDL2/SDL_events.h>

#include "ultramodern/ultramodern.hpp"   // ultramodern::audio_callbacks_t
#include "ultramodern/input.hpp"         // ultramodern::input::callbacks_t
#include "librecomp/game.hpp"            // recomp::SaveType

namespace mm_audio_input {

// The same typed, two-slot binding model used by the modern N64 recomp
// frontends, kept game-local so the launcher can stay on Dear ImGui.
enum class ControlDevice : uint8_t {
    Keyboard,
    Controller,
};

enum class N64Input : uint8_t {
    AnalogUp,
    AnalogDown,
    AnalogLeft,
    AnalogRight,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    A,
    B,
    Z,
    Start,
    L,
    R,
    CUp,
    CDown,
    CLeft,
    CRight,
    Count,
};

enum class BindingType : uint8_t {
    None,
    Keyboard,
    ControllerButton,
    ControllerAxis,
};

struct InputBinding {
    BindingType type = BindingType::None;
    // Keyboard: SDL_Scancode. Controller button: SDL_GameControllerButton.
    // Controller axis: signed (SDL_GameControllerAxis + 1), where the sign is
    // the direction. Zero is reserved for an unbound input.
    int id = 0;

    bool operator==(const InputBinding&) const = default;
};

constexpr size_t kBindingsPerInput = 2;
constexpr size_t kN64InputCount = static_cast<size_t>(N64Input::Count);

const char* input_name(N64Input input);
InputBinding get_binding(ControlDevice device, N64Input input, size_t slot);
bool set_binding(ControlDevice device, N64Input input, size_t slot, InputBinding binding);
void clear_bindings(ControlDevice device, N64Input input);
void reset_bindings(ControlDevice device, N64Input input);
void reset_all_bindings(ControlDevice device);
std::string binding_name(InputBinding binding);

// Load/save <config_path>/controls.json. Loading starts from defaults and only
// overlays valid entries, so old, partial, or hand-edited files remain safe.
bool load_control_config(const std::filesystem::path& config_path);
bool save_control_config();
std::filesystem::path control_config_path();

// Translate one SDL event while the launcher is in binding-capture mode.
// Axis motion must already have crossed the caller's neutral/activation gate.
bool binding_from_event(ControlDevice device, const SDL_Event& event,
                        InputBinding& binding);

// Keep the single active SDL controller in sync with hotplug events. The
// launcher calls this before the runtime begins polling gameplay input.
void refresh_controllers();

// One-time setup. Idempotent. Brings up the SDL2 audio + game-controller
// subsystems, opens the audio device, and opens the first attached
// SDL game controller if any. Independent of SDL video — safe to call before
// the window exists (SDL_InitSubSystem initializes the SDL core on demand).
void init();

// Filled audio callback struct (queue_samples / get_frames_remaining /
// set_frequency). Hand this to recomp::Configuration::audio_callbacks.
ultramodern::audio_callbacks_t audio_callbacks();

// Remaining audio buffered, in stereo *frames* at the game's sample rate (the
// same value ultramodern's audio callback uses). Exposed so the host can mirror
// it into the N64 AI_LEN register for games that read AI_LEN directly instead
// of through osAiGetLength (see src/game/register_overlays.cpp).
size_t get_frames_remaining();

// Filled input callback struct (poll_input / get_input / set_rumble /
// get_connected_device_info). Hand this to recomp::Configuration::input_callbacks.
ultramodern::input::callbacks_t input_callbacks();

// Suppress all gameplay input while a host UI owns the keyboard/controller.
// The debug overlay uses this so its navigation keys never leak through to
// Marina or the game's pause menu. Thread-safe: the SDL event and game-input
// callbacks run on different runtime threads.
void set_input_blocked(bool blocked);

// The game's save type. The US 1.1 ROM uses 4Kbit EEPROM — see
// PHASE2_NOTES_w4.md for the evidence (osEepromProbe/LongRead/LongWrite calls
// in RecompiledFuncs, max block address 0x2C < 64-block 4K limit). Set this
// on the GameEntry::save_type field when registering the game.
recomp::SaveType save_type();

// Thin wrapper over recomp::register_config_path: tells librecomp where to
// persist saves (it writes <config_path>/saves/<game_id>.bin internally, so the
// host only needs to declare the type + path — no per-game EEPROM code here).
void register_save_config(const std::filesystem::path& config_path);

} // namespace mm_audio_input
