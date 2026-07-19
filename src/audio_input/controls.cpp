// Persistent keyboard/controller bindings shared by the SDL input backend and
// the Dear ImGui launcher. The data model follows the established N64 recomp
// convention: two typed bindings per logical N64 input, with signed SDL axes.

#include "mm_audio_input.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "json/json.hpp"

namespace mm_audio_input {
namespace {

using BindingSlots = std::array<InputBinding, kBindingsPerInput>;
using ControlMap = std::array<BindingSlots, kN64InputCount>;

constexpr int kConfigVersion = 1;

constexpr size_t index_of(N64Input input) {
    return static_cast<size_t>(input);
}

constexpr int axis_id(SDL_GameControllerAxis axis, int direction) {
    return (static_cast<int>(axis) + 1) * direction;
}

ControlMap make_keyboard_defaults() {
    ControlMap map{};
    auto bind = [&](N64Input input, SDL_Scancode scancode, size_t slot = 0) {
        map[index_of(input)][slot] = {
            BindingType::Keyboard, static_cast<int>(scancode)};
    };

    bind(N64Input::AnalogUp, SDL_SCANCODE_W);
    bind(N64Input::AnalogDown, SDL_SCANCODE_S);
    bind(N64Input::AnalogLeft, SDL_SCANCODE_A);
    bind(N64Input::AnalogRight, SDL_SCANCODE_D);
    bind(N64Input::DpadUp, SDL_SCANCODE_UP);
    bind(N64Input::DpadDown, SDL_SCANCODE_DOWN);
    bind(N64Input::DpadLeft, SDL_SCANCODE_LEFT);
    bind(N64Input::DpadRight, SDL_SCANCODE_RIGHT);
    bind(N64Input::A, SDL_SCANCODE_X);
    bind(N64Input::B, SDL_SCANCODE_C);
    bind(N64Input::Z, SDL_SCANCODE_LSHIFT);
    bind(N64Input::Start, SDL_SCANCODE_RETURN);
    bind(N64Input::L, SDL_SCANCODE_Q);
    bind(N64Input::R, SDL_SCANCODE_E);
    bind(N64Input::CUp, SDL_SCANCODE_I);
    bind(N64Input::CDown, SDL_SCANCODE_K);
    bind(N64Input::CLeft, SDL_SCANCODE_J);
    bind(N64Input::CRight, SDL_SCANCODE_L);
    return map;
}

ControlMap make_controller_defaults() {
    ControlMap map{};
    auto button = [&](N64Input input, SDL_GameControllerButton value,
                      size_t slot = 0) {
        map[index_of(input)][slot] = {
            BindingType::ControllerButton, static_cast<int>(value)};
    };
    auto axis = [&](N64Input input, SDL_GameControllerAxis value, int direction,
                    size_t slot = 0) {
        map[index_of(input)][slot] = {
            BindingType::ControllerAxis, axis_id(value, direction)};
    };

    axis(N64Input::AnalogUp, SDL_CONTROLLER_AXIS_LEFTY, -1);
    axis(N64Input::AnalogDown, SDL_CONTROLLER_AXIS_LEFTY, 1);
    axis(N64Input::AnalogLeft, SDL_CONTROLLER_AXIS_LEFTX, -1);
    axis(N64Input::AnalogRight, SDL_CONTROLLER_AXIS_LEFTX, 1);
    button(N64Input::DpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
    button(N64Input::DpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    button(N64Input::DpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    button(N64Input::DpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    // These match the current shared recomp frontend defaults. The right stick
    // is the primary C-button cluster; secondary bindings keep it convenient on
    // controllers without a comfortable right stick.
    button(N64Input::A, SDL_CONTROLLER_BUTTON_A);                 // south
    button(N64Input::B, SDL_CONTROLLER_BUTTON_X);                 // west
    axis(N64Input::Z, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1);
    button(N64Input::Start, SDL_CONTROLLER_BUTTON_START);
    button(N64Input::L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    axis(N64Input::R, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1);
    axis(N64Input::CUp, SDL_CONTROLLER_AXIS_RIGHTY, -1);
    button(N64Input::CUp, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 1);
    axis(N64Input::CDown, SDL_CONTROLLER_AXIS_RIGHTY, 1);
    button(N64Input::CDown, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    axis(N64Input::CLeft, SDL_CONTROLLER_AXIS_RIGHTX, -1);
    button(N64Input::CLeft, SDL_CONTROLLER_BUTTON_Y, 1);           // north
    axis(N64Input::CRight, SDL_CONTROLLER_AXIS_RIGHTX, 1);
    button(N64Input::CRight, SDL_CONTROLLER_BUTTON_B, 1);         // east
    return map;
}

const ControlMap kKeyboardDefaults = make_keyboard_defaults();
const ControlMap kControllerDefaults = make_controller_defaults();
ControlMap g_keyboard = kKeyboardDefaults;
ControlMap g_controller = kControllerDefaults;
std::filesystem::path g_config_path;

constexpr std::array<const char*, kN64InputCount> kInputNames = {
    "Analog Up", "Analog Down", "Analog Left", "Analog Right",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "A", "B", "Z", "Start", "L", "R",
    "C Up", "C Down", "C Left", "C Right",
};

constexpr std::array<const char*, kN64InputCount> kConfigNames = {
    "analog_up", "analog_down", "analog_left", "analog_right",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "a", "b", "z", "start", "l", "r",
    "c_up", "c_down", "c_left", "c_right",
};

ControlMap& map_for(ControlDevice device) {
    return device == ControlDevice::Keyboard ? g_keyboard : g_controller;
}

const ControlMap& defaults_for(ControlDevice device) {
    return device == ControlDevice::Keyboard ? kKeyboardDefaults : kControllerDefaults;
}

const char* binding_type_name(BindingType type) {
    switch (type) {
        case BindingType::None: return "none";
        case BindingType::Keyboard: return "keyboard";
        case BindingType::ControllerButton: return "controller_button";
        case BindingType::ControllerAxis: return "controller_axis";
    }
    return "none";
}

bool binding_type_from_name(std::string_view name, BindingType& type) {
    if (name == "none") type = BindingType::None;
    else if (name == "keyboard") type = BindingType::Keyboard;
    else if (name == "controller_button") type = BindingType::ControllerButton;
    else if (name == "controller_axis") type = BindingType::ControllerAxis;
    else return false;
    return true;
}

bool valid_binding(ControlDevice device, InputBinding binding) {
    if (binding.type == BindingType::None) return binding.id == 0;
    if (device == ControlDevice::Keyboard) {
        return binding.type == BindingType::Keyboard &&
               binding.id > SDL_SCANCODE_UNKNOWN && binding.id < SDL_NUM_SCANCODES;
    }
    if (binding.type == BindingType::ControllerButton) {
        return binding.id >= 0 && binding.id < SDL_CONTROLLER_BUTTON_MAX;
    }
    if (binding.type == BindingType::ControllerAxis) {
        const int64_t encoded_axis = binding.id < 0
            ? -static_cast<int64_t>(binding.id) : binding.id;
        return encoded_axis >= 1 && encoded_axis <= SDL_CONTROLLER_AXIS_MAX;
    }
    return false;
}

bool parse_binding(const nlohmann::json& value, ControlDevice device,
                   InputBinding& binding) {
    if (!value.is_object()) return false;
    const auto type_it = value.find("type");
    const auto id_it = value.find("id");
    if (type_it == value.end() || !type_it->is_string() ||
        id_it == value.end() || !id_it->is_number_integer()) {
        return false;
    }

    BindingType type{};
    if (!binding_type_from_name(type_it->get<std::string>(), type)) return false;
    int64_t id = 0;
    try {
        id = id_it->get<int64_t>();
    } catch (const std::exception&) {
        return false;
    }
    if (id < std::numeric_limits<int>::min() ||
        id > std::numeric_limits<int>::max()) {
        return false;
    }
    InputBinding candidate{type, static_cast<int>(id)};
    if (!valid_binding(device, candidate)) return false;
    binding = candidate;
    return true;
}

bool load_section(const nlohmann::json& root, const char* section_name,
                  ControlDevice device) {
    const auto section_it = root.find(section_name);
    if (section_it == root.end()) return true; // missing section keeps defaults
    if (!section_it->is_object()) {
        std::fprintf(stderr, "[controls] '%s' must be an object; using defaults.\n",
                     section_name);
        return false;
    }

    ControlMap& map = map_for(device);
    for (size_t input_index = 0; input_index < kN64InputCount; ++input_index) {
        const auto input_it = section_it->find(kConfigNames[input_index]);
        if (input_it == section_it->end()) continue;
        if (!input_it->is_array()) {
            std::fprintf(stderr, "[controls] %s.%s must be an array; keeping defaults.\n",
                         section_name, kConfigNames[input_index]);
            continue;
        }
        const size_t count = std::min(input_it->size(), kBindingsPerInput);
        for (size_t slot = 0; slot < count; ++slot) {
            InputBinding binding{};
            if (parse_binding((*input_it)[slot], device, binding)) {
                map[input_index][slot] = binding;
            } else {
                std::fprintf(stderr,
                    "[controls] Ignoring invalid binding at %s.%s[%zu].\n",
                    section_name, kConfigNames[input_index], slot);
            }
        }
    }
    return true;
}

bool load_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    try {
        nlohmann::json root;
        file >> root;
        if (!root.is_object()) throw std::runtime_error("root is not an object");
        const auto version_it = root.find("version");
        if (version_it == root.end() || !version_it->is_number_integer() ||
            version_it->get<int>() != kConfigVersion) {
            throw std::runtime_error("unsupported or missing version");
        }
        load_section(root, "keyboard", ControlDevice::Keyboard);
        load_section(root, "controller", ControlDevice::Controller);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[controls] Could not load %s: %s\n",
                     path.filename().string().c_str(), e.what());
        return false;
    }
}

nlohmann::json binding_json(InputBinding binding) {
    return nlohmann::json{{"type", binding_type_name(binding.type)}, {"id", binding.id}};
}

nlohmann::json section_json(const ControlMap& map) {
    nlohmann::json section = nlohmann::json::object();
    for (size_t input_index = 0; input_index < kN64InputCount; ++input_index) {
        nlohmann::json slots = nlohmann::json::array();
        for (InputBinding binding : map[input_index]) {
            slots.push_back(binding_json(binding));
        }
        section[kConfigNames[input_index]] = std::move(slots);
    }
    return section;
}

const char* controller_button_name(int id) {
    switch (static_cast<SDL_GameControllerButton>(id)) {
        case SDL_CONTROLLER_BUTTON_A: return "A / South";
        case SDL_CONTROLLER_BUTTON_B: return "B / East";
        case SDL_CONTROLLER_BUTTON_X: return "X / West";
        case SDL_CONTROLLER_BUTTON_Y: return "Y / North";
        case SDL_CONTROLLER_BUTTON_BACK: return "Back";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
        case SDL_CONTROLLER_BUTTON_START: return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left Stick Click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right Stick Click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Left Shoulder";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Right Shoulder";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad Right";
        default: return nullptr;
    }
}

const char* controller_axis_name(int axis) {
    switch (static_cast<SDL_GameControllerAxis>(axis)) {
        case SDL_CONTROLLER_AXIS_LEFTX: return "Left Stick X";
        case SDL_CONTROLLER_AXIS_LEFTY: return "Left Stick Y";
        case SDL_CONTROLLER_AXIS_RIGHTX: return "Right Stick X";
        case SDL_CONTROLLER_AXIS_RIGHTY: return "Right Stick Y";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Left Trigger";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Right Trigger";
        default: return nullptr;
    }
}

} // namespace

const char* input_name(N64Input input) {
    const size_t index = index_of(input);
    return index < kN64InputCount ? kInputNames[index] : "Unknown";
}

InputBinding get_binding(ControlDevice device, N64Input input, size_t slot) {
    const size_t index = index_of(input);
    if (index >= kN64InputCount || slot >= kBindingsPerInput) return {};
    return map_for(device)[index][slot];
}

bool set_binding(ControlDevice device, N64Input input, size_t slot,
                 InputBinding binding) {
    const size_t index = index_of(input);
    if (index >= kN64InputCount || slot >= kBindingsPerInput ||
        !valid_binding(device, binding)) {
        return false;
    }
    map_for(device)[index][slot] = binding;
    return true;
}

void clear_bindings(ControlDevice device, N64Input input) {
    const size_t index = index_of(input);
    if (index < kN64InputCount) map_for(device)[index] = {};
}

void reset_bindings(ControlDevice device, N64Input input) {
    const size_t index = index_of(input);
    if (index < kN64InputCount) {
        map_for(device)[index] = defaults_for(device)[index];
    }
}

void reset_all_bindings(ControlDevice device) {
    map_for(device) = defaults_for(device);
}

std::string binding_name(InputBinding binding) {
    switch (binding.type) {
        case BindingType::None:
            return "Unbound";
        case BindingType::Keyboard: {
            const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.id));
            return name != nullptr && name[0] != '\0'
                ? std::string(name) : "Key " + std::to_string(binding.id);
        }
        case BindingType::ControllerButton: {
            const char* name = controller_button_name(binding.id);
            return name != nullptr ? std::string(name)
                                   : "Button " + std::to_string(binding.id);
        }
        case BindingType::ControllerAxis: {
            const int encoded = binding.id < 0 ? -binding.id : binding.id;
            const int axis = encoded - 1;
            const char* name = controller_axis_name(axis);
            std::string result = name != nullptr ? name : "Axis " + std::to_string(axis);
            result += binding.id < 0 ? " -" : " +";
            return result;
        }
    }
    return "Unbound";
}

bool load_control_config(const std::filesystem::path& config_path) {
    g_config_path = config_path / "controls.json";
    g_keyboard = kKeyboardDefaults;
    g_controller = kControllerDefaults;

    std::error_code ec;
    const bool primary_exists = std::filesystem::exists(g_config_path, ec);
    if (primary_exists && load_file(g_config_path)) {
        std::fprintf(stderr, "[controls] Loaded controls.json.\n");
        return true;
    }

    std::filesystem::path backup = g_config_path;
    backup += ".bak";
    if (std::filesystem::exists(backup, ec) && load_file(backup)) {
        std::fprintf(stderr, "[controls] Recovered bindings from controls.json.bak.\n");
        return true;
    }

    if (!primary_exists) {
        const bool saved = save_control_config();
        std::fprintf(stderr, "[controls] Using default bindings%s.\n",
                     saved ? " (created controls.json)" : "");
        return saved;
    }

    std::fprintf(stderr, "[controls] Using defaults; the invalid file was preserved.\n");
    return false;
}

bool save_control_config() {
    if (g_config_path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(g_config_path.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr, "[controls] Could not create the config directory: %s\n",
                     ec.message().c_str());
        return false;
    }

    nlohmann::json root{
        {"version", kConfigVersion},
        {"keyboard", section_json(g_keyboard)},
        {"controller", section_json(g_controller)},
    };

    std::filesystem::path temporary = g_config_path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file.is_open()) {
            std::fprintf(stderr, "[controls] Could not write controls.json.tmp.\n");
            return false;
        }
        file << root.dump(2) << '\n';
        if (!file.good()) {
            std::fprintf(stderr, "[controls] Failed while writing controls.json.tmp.\n");
            file.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::path backup = g_config_path;
    backup += ".bak";
    const bool had_primary = std::filesystem::exists(g_config_path, ec);
    if (had_primary) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(g_config_path, backup, ec);
        if (ec) {
            std::fprintf(stderr, "[controls] Could not rotate controls.json: %s\n",
                         ec.message().c_str());
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    ec.clear();
    std::filesystem::rename(temporary, g_config_path, ec);
    if (!ec) return true;

    std::fprintf(stderr, "[controls] Could not install controls.json: %s\n",
                 ec.message().c_str());
    if (had_primary) {
        std::error_code restore_ec;
        std::filesystem::rename(backup, g_config_path, restore_ec);
    }
    std::filesystem::remove(temporary, ec);
    return false;
}

std::filesystem::path control_config_path() {
    return g_config_path;
}

bool binding_from_event(ControlDevice device, const SDL_Event& event,
                        InputBinding& binding) {
    if (device == ControlDevice::Keyboard && event.type == SDL_KEYDOWN &&
        event.key.repeat == 0 && event.key.keysym.scancode != SDL_SCANCODE_UNKNOWN) {
        binding = {BindingType::Keyboard,
                   static_cast<int>(event.key.keysym.scancode)};
        return true;
    }
    if (device != ControlDevice::Controller) return false;

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        binding = {BindingType::ControllerButton,
                   static_cast<int>(event.cbutton.button)};
        return valid_binding(device, binding);
    }
    if (event.type == SDL_CONTROLLERAXISMOTION) {
        const int direction = event.caxis.value < 0 ? -1 : 1;
        binding = {BindingType::ControllerAxis,
                   axis_id(static_cast<SDL_GameControllerAxis>(event.caxis.axis), direction)};
        return valid_binding(device, binding);
    }
    return false;
}

} // namespace mm_audio_input
