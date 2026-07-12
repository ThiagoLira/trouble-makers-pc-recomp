// Host helpers for translated-code widescreen hooks.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "ultramodern/config.hpp"

namespace {

constexpr uint32_t kStaticScratch = 0x9FFFC000u;
constexpr uint32_t kBackdropMapPtr = 0x80137470u;
constexpr uint32_t kBackdropAddend = 0x80137478u;
constexpr uint32_t kTexturePoolPtr = 0x80180FC0u;
constexpr uint32_t kBackdropScrollX = 0x800BE57Cu;
constexpr uint32_t kBackdropScrollY = 0x800BE584u;
constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kCannotPause = 0x800BE4ECu;
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kStageTime = 0x801781E0u;
constexpr uint32_t kStageScenes = 0x800C8378u;
constexpr uint32_t kStageIds = 0x800C83F8u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kHighestUnlockedStage = 0x80171B18u;
constexpr uint32_t kCurrentStageId = 0x800D28E4u;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

bool valid_rdram_ptr(uint32_t ptr) {
    return ptr >= 0x80000000u && ptr < 0x80800000u;
}

int g_gameplay_wide_active = 0;

int raw_gameplay_active(uint8_t* rdram) {
    static int previous_stage_time = -1;
    static int frames_since_stage_time_changed = 1000;
    if (!env_enabled("MM_WIDESCREEN_ACTIVE")) {
        return 0;
    }

    const int game_state = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kGameState)));
    const int scene = static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
    const int stage_time = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kStageTime)));
    const int cannot_pause = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCannotPause)));
    if (game_state != 6 || scene == 36 || scene == 57 || scene == 71) {
        previous_stage_time = stage_time;
        frames_since_stage_time_changed = 1000;
        return 0;
    }

    if (stage_time != previous_stage_time) {
        previous_stage_time = stage_time;
        frames_since_stage_time_changed = 0;
    }
    else if (frames_since_stage_time_changed < 1000) {
        ++frames_since_stage_time_changed;
    }

    // gStageTime is incremented by func_80020024 only during active stage
    // control; it freezes for cinema/dialogue. The timer saturates at 36000,
    // where the ordinary pause permission becomes the remaining signal.
    return cannot_pause == 0 &&
        (frames_since_stage_time_changed < 3 || stage_time >= 36000);
}

} // namespace

// Widescreen is intentionally a gameplay-only feature. Cinematics frequently
// stage actors and fixed-size sets immediately outside the 4:3 viewport; the
// original game hides those areas by construction. gCannotPause is asserted
// by the stage cinema state machines and cleared when control returns to the
// player, while game state 6 is the actual gameplay dispatcher.
extern "C" int mm_widescreen_gameplay_active(uint8_t* rdram) {
    (void)rdram;
    return g_gameplay_wide_active;
}

// Flip RT64 between its native 4:3 and expanded projection only when the
// gameplay/cinema boundary changes. The user's --widescreen preference remains
// untouched; this is a transient presentation gate, not a config toggle.
extern "C" void mm_widescreen_sync_mode(uint8_t* rdram) {
    static int previous_active = -1;
    static int candidate_active = -1;
    static int candidate_frames = 0;
    const int raw_active = raw_gameplay_active(rdram);

    if (previous_active < 0) {
        previous_active = raw_active;
        candidate_active = raw_active;
    }
    else if (raw_active == previous_active) {
        candidate_active = raw_active;
        candidate_frames = 0;
        return;
    }
    else {
        if (raw_active != candidate_active) {
            candidate_active = raw_active;
            candidate_frames = 1;
        }
        else {
            ++candidate_frames;
        }

        // Hide a cinematic almost immediately, but do not reopen the wings
        // until control has remained stable for half a second. Several stage
        // scripts pulse gCannotPause for one or two frames during gameplay.
        const int required_frames = raw_active ? 30 : 3;
        if (candidate_frames < required_frames) {
            return;
        }
        previous_active = raw_active;
        candidate_frames = 0;
    }
    g_gameplay_wide_active = previous_active;

    auto config = ultramodern::renderer::get_graphics_config();
    const auto desired = previous_active
        ? ultramodern::renderer::AspectRatio::Expand
        : ultramodern::renderer::AspectRatio::Original;
    if (config.ar_option != desired) {
        config.ar_option = desired;
        ultramodern::renderer::set_graphics_config(config);
    }
    std::fprintf(stderr, "[widescreen] mode=%s\n",
        previous_active ? "gameplay-expand" : "cinematic-4:3");
}

// The debug warp must select a complete stage-table row, not merely change
// gCurrentScene. Otherwise loading keeps the previous row's map/overlay ID and
// produces convincing but meaningless stale-asset corruption. Select the first
// matching progression entry, just like the game's debug stage selector.
extern "C" int mm_debug_warp_prepare(
    uint8_t* rdram, int requested_scene, int requested_stage) {
    const gpr scenes = static_cast<gpr>(static_cast<int32_t>(kStageScenes));
    const gpr stage_ids = static_cast<gpr>(static_cast<int32_t>(kStageIds));

    for (int stage = 0; stage < 64; ++stage) {
        if (requested_stage >= 0 ? stage != requested_stage
                                 : MEM_HU(stage * 2, scenes) != requested_scene) {
            continue;
        }

        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kCurrentStage))) = stage;
        MEM_B(0, static_cast<gpr>(static_cast<int32_t>(kHighestUnlockedStage))) = stage;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kCurrentStageId))) =
            MEM_HU(stage * 2, stage_ids);
        return MEM_HU(stage * 2, scenes);
    }
    return static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
}

// The non-scrolling background shares D_80180D90 with the scrolling band,
// but reaches func_80082380 instead of func_80082820. The main entry hook has
// already copied its ten original columns into kStaticScratch when this runs.
// Populate the five columns on either side from the same wrapping 16x16 map
// formula used by the game's original func_8001107C fill.
extern "C" void mm_widescreen_finish_static_bg(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t buffer = static_cast<uint32_t>(ctx->r7);
    if (buffer != kStaticScratch ||
        !env_enabled("MM_STATIC_WINGS") ||
        !mm_widescreen_gameplay_active(rdram)) {
        return;
    }

    // These scenes route different kinds of data through this buffer;
    // interpreting them as the generic wrapping backdrop produces invalid
    // tile references. Their verified environment/midground layers still
    // provide safe extension.
    const int scene = static_cast<int16_t>(
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
    if (scene == 0 || scene == 10 || scene == 20 || scene == 23 || scene == 24 ||
        scene == 36 || scene == 40 || scene == 42 || scene == 46 ||
        scene == 47 || scene == 48 || scene == 52 || scene == 53 ||
        scene == 54 || scene == 55 || scene == 57 || scene == 65 ||
        scene == 68 || scene == 71 || scene == 77 || scene == 78) {
        return;
    }

    const gpr scratch = static_cast<gpr>(static_cast<int32_t>(kStaticScratch));
    const gpr map = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kBackdropMapPtr)));
    if (!valid_rdram_ptr(static_cast<uint32_t>(map))) {
        return;
    }

    const int addend = MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kBackdropAddend)));
    const uint32_t texture_pool = static_cast<uint32_t>(
        MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kTexturePoolPtr))));
    const int scroll_x = static_cast<int16_t>(
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kBackdropScrollX))));
    const int scroll_y = static_cast<int16_t>(
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kBackdropScrollY))));
    const int source_x = (scroll_x - 2) & 0x1FF;
    const int source_y = (-12 - scroll_y) & 0x1FF;

    for (int row = 0; row < 7; ++row) {
        const int row_block = ((source_y + row * 32) >> 1) & 0xF0;
        for (int wide_col = 0; wide_col < 20; ++wide_col) {
            const int col = wide_col - 5;
            if (col >= 0 && col < 10) {
                continue;
            }

            const int map_col = ((source_x >> 5) + col + 16) & 0xF;
            const int tile = MEM_BU(map_col + row_block, map);
            const int32_t texture = tile == 0 ? 0 : static_cast<int32_t>(
                (static_cast<uint32_t>((tile + addend) & 0xFFFF) << 10) + texture_pool);
            MEM_W((row * 20 + wide_col) * 4, scratch) = texture;
        }
    }
}
