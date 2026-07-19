#include "debug_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "librecomp/game.hpp"
#include "mm_audio_input.hpp"
#include "recomp.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

extern "C" int mm_debug_warp_prepare(
    unsigned char* rdram, int requested_scene, int requested_stage);

namespace mm::debug_menu {
namespace {

struct WarpEntry {
    int stage;
    int scene;
    const char* label;
    const char* name;
};

// Campaign rows from the ROM's gStageScenes progression table. Demo,
// opening, ending, credits, and unused extra rows are deliberately omitted;
// these are the same 52 playable rows covered by test_widescreen_playable.sh.
constexpr std::array<WarpEntry, 52> kWarps{{
    { 2,  0, "1-1",  "Meet Marina!" },
    { 3, 68, "1-2",  "Meet Calina!" },
    { 4, 53, "1-3",  "Clanball Land" },
    { 5, 55, "1-4",  "Spike Land" },
    { 6, 52, "1-5",  "3 Clancer Kids" },
    { 7, 67, "1-6",  "Blockman Rises" },
    { 8, 56, "1-7",  "Wormin' Up!" },
    { 9, 58, "1-8",  "Crisis: Nepton" },
    {10, 54, "1-9",  "Western World" },
    {11, 57, "1-10", "Volcano!" },

    {12,  1, "2-1",  "Sea of Lava" },
    {13, 69, "2-2",  "Vertigo!" },
    {14, 37, "2-3",  "Sink or Float" },
    {15, 60, "2-4",  "Hot Rush" },
    {16, 38, "2-5",  "Searin' Swing" },
    {17, 59, "2-6",  "Flamb!!" },
    {18, 61, "2-7",  "Tightrope Ride" },
    {19, 70, "2-8",  "Freefall!" },
    {20, 62, "2-9",  "Magma Rafts!" },
    {21, 13, "2-10", "Seasick Climb" },
    {22,  5, "2-11", "Migen Brawl!" },

    {24, 72, "3-1",  "Clanpot Shake" },
    {25, 12, "3-2",  "Clance War" },
    {26, 35, "3-3",  "Missile Surf!" },
    {27, 71, "3-4",  "Clanball Lift" },
    {28, 32, "3-5",  "Go Marzen 64" },
    {29, 31, "3-6",  "Chilly Dog!" },
    {30, 36, "3-7",  "Snowstorm Maze" },
    {31,  9, "3-8",  "LUNAR!" },
    {32, 33, "3-9",  "The Day Before" },
    {33, 18, "3-10", "The Day Of" },
    {34, 29, "3-11", "Cat-astrophe!" },
    {35, 19, "3-12", "CERBERUS" },

    {37, 42, "4-1",  "Rolling Rock!" },
    {38, 39, "4-2",  "Toadly Raw!" },
    {39, 40, "4-3",  "7 Clancer Kids" },
    {40, 23, "4-4",  "Rescue! Act 1" },
    {41, 41, "4-5",  "Rescue! Act 2" },
    {42, 10, "4-6",  "Taurus!" },
    {43, 48, "4-7",  "Ghost Catcher" },
    {44, 24, "4-8",  "Aster's Tryke" },
    {45, 46, "4-9",  "Moley Cow!" },
    {46, 47, "4-10", "Aster's Maze" },
    {47, 25, "4-11", "SASQUATCH beta" },

    {49, 77, "5-1", "Clance War II" },
    {50, 20, "5-2", "Counterattack" },
    {51, 78, "5-3", "Bee's the One" },
    {52, 85, "5-4", "MERCO!" },
    {53, 22, "5-5", "Trapped?" },
    {54, 26, "5-6", "PHOENIX" },
    {56, 79, "5-7", "Inner Struggle" },
    {57, 27, "5-8", "Final Battle" },
}};

constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kGameStateSubState = 0x800BE4F4u;
constexpr uint32_t kCannotPause = 0x800BE4ECu;
constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kLoadFlag = 0x800D2908u;
constexpr uint32_t kDisplayListA = 0x8027CEE8u;
constexpr uint32_t kDisplayListB = 0x8027EEE8u;

std::atomic_bool g_enabled{false};
std::atomic_bool g_open{false};
std::atomic_bool g_game_ready{false};
std::atomic_bool g_save_protected{false};
std::atomic_bool g_waiting_for_release{false};
std::atomic<int> g_page{0};
std::atomic<int> g_selected_warp{0};
std::atomic<int> g_pending_warp{-1};
std::atomic<int> g_current_stage{-1};
std::atomic<int> g_current_scene{-1};
std::atomic<int> g_current_game_state{-1};

std::array<bool, SDL_CONTROLLER_BUTTON_MAX> g_pad_buttons{};

gpr rdram_gpr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

bool activation_chord_held() {
    return g_pad_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] &&
           g_pad_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] &&
           g_pad_buttons[SDL_CONTROLLER_BUTTON_START];
}

bool navigation_input_released() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const bool keyboard_held =
        keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_ESCAPE] ||
        keys[SDL_SCANCODE_F1] || keys[SDL_SCANCODE_UP] ||
        keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_LEFT] ||
        keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_PAGEUP] ||
        keys[SDL_SCANCODE_PAGEDOWN];
    const bool controller_held =
        g_pad_buttons[SDL_CONTROLLER_BUTTON_A] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_B] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] ||
        g_pad_buttons[SDL_CONTROLLER_BUTTON_START];
    return !keyboard_held && !controller_held;
}

void close_overlay() {
    g_open.store(false, std::memory_order_release);
    g_waiting_for_release.store(true, std::memory_order_release);
    std::fprintf(stderr, "[debug-menu] closed\n");
}

void toggle_overlay() {
    if (g_open.load(std::memory_order_acquire)) {
        close_overlay();
        return;
    }
    if (!g_game_ready.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "[debug-menu] toggle ignored: game is not ready\n");
        return;
    }
    g_open.store(true, std::memory_order_release);
    g_waiting_for_release.store(false, std::memory_order_release);
    mm_audio_input::set_input_blocked(true);
    std::fprintf(stderr, "[debug-menu] opened\n");
}

void change_page(int delta) {
    int page = g_page.load(std::memory_order_relaxed);
    page = (page + delta + 2) % 2;
    g_page.store(page, std::memory_order_relaxed);
}

void move_selection(int delta) {
    if (g_page.load(std::memory_order_relaxed) != 0) {
        return;
    }
    const int count = static_cast<int>(kWarps.size());
    int selected = g_selected_warp.load(std::memory_order_relaxed);
    selected = (selected + delta + count) % count;
    g_selected_warp.store(selected, std::memory_order_relaxed);
}

void activate_selection() {
    if (g_page.load(std::memory_order_relaxed) != 0) {
        return;
    }
    g_pending_warp.store(
        g_selected_warp.load(std::memory_order_relaxed),
        std::memory_order_release);
    close_overlay();
}

bool handle_navigation_key(SDL_Scancode key) {
    switch (key) {
        case SDL_SCANCODE_ESCAPE:   close_overlay(); return true;
        case SDL_SCANCODE_UP:       move_selection(-1); return true;
        case SDL_SCANCODE_DOWN:     move_selection(1); return true;
        case SDL_SCANCODE_PAGEUP:
        case SDL_SCANCODE_LEFT:     change_page(-1); return true;
        case SDL_SCANCODE_PAGEDOWN:
        case SDL_SCANCODE_RIGHT:    change_page(1); return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: activate_selection(); return true;
        default: return false;
    }
}

bool handle_navigation_button(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_B: close_overlay(); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move_selection(-1); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_selection(1); return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: change_page(-1); return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: change_page(1); return true;
        case SDL_CONTROLLER_BUTTON_A: activate_selection(); return true;
        default: return false;
    }
}

} // namespace

void configure(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    std::fprintf(stderr, "[debug-menu] %s\n", enabled ? "enabled" : "disabled");
    if (!enabled) {
        g_open.store(false, std::memory_order_release);
        g_waiting_for_release.store(false, std::memory_order_release);
        mm_audio_input::set_input_blocked(false);
    }
}

bool handle_event(const SDL_Event& event) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return false;
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN ||
        event.type == SDL_CONTROLLERBUTTONUP) {
        const Uint8 button = event.cbutton.button;
        if (button < g_pad_buttons.size()) {
            g_pad_buttons[button] = event.type == SDL_CONTROLLERBUTTONDOWN;
        }
    }

    if (g_waiting_for_release.load(std::memory_order_acquire)) {
        if (navigation_input_released()) {
            g_waiting_for_release.store(false, std::memory_order_release);
            mm_audio_input::set_input_blocked(false);
        }
        return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
               event.type == SDL_CONTROLLERBUTTONDOWN ||
               event.type == SDL_CONTROLLERBUTTONUP;
    }

    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
        if (event.key.keysym.scancode == SDL_SCANCODE_F1) {
            toggle_overlay();
            return true;
        }
        if (g_open.load(std::memory_order_acquire)) {
            handle_navigation_key(event.key.keysym.scancode);
            return true;
        }
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        if (!g_open.load(std::memory_order_acquire) && activation_chord_held()) {
            toggle_overlay();
            return true;
        }
        if (g_open.load(std::memory_order_acquire)) {
            handle_navigation_button(event.cbutton.button);
            return true;
        }
    }

    // Keep every keyboard/controller event away from the normal hotkey path
    // while the menu owns input, including key-up events.
    if (g_open.load(std::memory_order_acquire)) {
        return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
               event.type == SDL_CONTROLLERBUTTONDOWN ||
               event.type == SDL_CONTROLLERBUTTONUP ||
               event.type == SDL_CONTROLLERAXISMOTION;
    }
    return false;
}

extern "C" void mm_debug_menu_tick(unsigned char* rdram) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const int game_state = MEM_HU(0, rdram_gpr(kGameState));
    g_current_game_state.store(game_state, std::memory_order_relaxed);
    g_current_stage.store(MEM_HU(0, rdram_gpr(kCurrentStage)),
                          std::memory_order_relaxed);
    g_current_scene.store(
        static_cast<int16_t>(MEM_HU(0, rdram_gpr(kCurrentScene))),
        std::memory_order_relaxed);
    g_game_ready.store(game_state != 0, std::memory_order_release);

    const int selected = g_pending_warp.exchange(-1, std::memory_order_acq_rel);
    if (selected >= 0 && selected < static_cast<int>(kWarps.size())) {
        const WarpEntry& warp = kWarps[selected];

        // From this point through process exit, EEPROM writes become successful
        // no-ops. Reads still use the original on-disk save buffer, so a debug
        // run cannot leak unlocked stages, gems, times, or completion flags.
        recomp::set_save_writes_enabled(false);
        g_save_protected.store(true, std::memory_order_release);

        const int scene = mm_debug_warp_prepare(rdram, warp.scene, warp.stage);
        MEM_H(0, rdram_gpr(kCurrentScene)) = static_cast<int16_t>(scene);
        MEM_H(0, rdram_gpr(kCannotPause)) = 0;
        MEM_H(0, rdram_gpr(kLoadFlag)) = 1;
        MEM_H(0, rdram_gpr(kGameState)) = 5; // GAMESTATE_LOADING
        MEM_H(0, rdram_gpr(kGameStateSubState)) = 0;

        // Match the established MM_WARP harness: discard stale commands from
        // both display-list arenas before the new stage's loading pass.
        for (int i = 0; i < 0x1000; ++i) {
            MEM_W(i * 4, rdram_gpr(kDisplayListA)) = 0;
        }
        for (int i = 0; i < 0xC00; ++i) {
            MEM_W(i * 4, rdram_gpr(kDisplayListB)) = 0;
        }
        std::fprintf(stderr,
            "[debug-menu] warp %s %s; save writes disabled for this session\n",
            warp.label, warp.name);
        return;
    }
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !g_open.load(std::memory_order_acquire)) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(
        std::min(720.0f, std::max(420.0f, io.DisplaySize.x - 48.0f)),
        std::min(560.0f, std::max(320.0f, io.DisplaySize.y - 48.0f)));
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Trouble Makers Debug Menu", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const int page = g_page.load(std::memory_order_relaxed);
    ImGui::TextUnformatted(page == 0 ? "[ Level Warp ]   Session"
                                    : "  Level Warp   [ Session ]");
    ImGui::SameLine();
    ImGui::TextDisabled("  PageUp/PageDown or L/R");
    ImGui::Separator();

    if (page == 0) {
        ImGui::TextWrapped(
            "Choose a campaign stage. Enter/A warps; Esc/B closes. "
            "The first warp disables save writes until the game exits.");
        ImGui::Spacing();
        const int selected = g_selected_warp.load(std::memory_order_relaxed);
        ImGui::BeginChild("##warps", ImVec2(0.0f, -34.0f), true);
        for (int i = 0; i < static_cast<int>(kWarps.size()); ++i) {
            char row[128];
            std::snprintf(row, sizeof row, "%5s   %-24s  (stage %02d / scene %02d)",
                          kWarps[i].label, kWarps[i].name,
                          kWarps[i].stage, kWarps[i].scene);
            ImGui::Selectable(row, i == selected);
            if (i == selected) {
                ImGui::SetScrollHereY(0.5f);
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down: select   Enter/A: warp   Esc/B: close");
    }
    else {
        ImGui::TextUnformatted("Debug session status");
        ImGui::Spacing();
        ImGui::BulletText("Current progression row: %d",
            g_current_stage.load(std::memory_order_relaxed));
        ImGui::BulletText("Current scene: %d",
            g_current_scene.load(std::memory_order_relaxed));
        ImGui::BulletText("Game state: %d",
            g_current_game_state.load(std::memory_order_relaxed));
        if (g_save_protected.load(std::memory_order_acquire)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
            ImGui::BulletText("Save protection: ACTIVE (save writes are blocked)");
            ImGui::PopStyleColor();
        }
        else {
            ImGui::BulletText("Save protection: armed; activates on first warp");
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Debug warps change the game's in-memory progression data so the "
            "destination loads with a complete stage-table row. Those changes "
            "are never copied to the save file after protection activates.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "F1 or L+R+Start toggles this overlay. Gameplay input is held "
            "while it is open.");
    }

    ImGui::End();
}
#endif

} // namespace mm::debug_menu
