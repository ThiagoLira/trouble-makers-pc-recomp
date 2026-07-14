// Host helpers for translated-code widescreen hooks.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "ultramodern/config.hpp"

namespace {

// Scratch grids (7x20 s32) the repack hooks hand to the widened draw loops.
constexpr uint32_t kGenericScratch = 0x9FFFC000u;
constexpr uint32_t kEnvScratch = 0x9FFFD000u;
constexpr uint32_t kMidScratch = 0x9FFFE000u;
constexpr uint32_t kBandScratch = 0x9FFFF000u;
constexpr uint32_t kFilteredActorList = 0x9FFFA000u;
constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kActorsTop = 0x80171F10u;
// Stable synthetic indices used only by the actor renderer. The corresponding
// 208 actor-sized records occupy 0x80440168..0x80454CE8, clear of the widened
// tile arenas, relocated clan records, and runtime allocations.
constexpr int kWrappedRepeatActorBase = 8521;
constexpr uint32_t kClanBlocks = 0x804269E0u;
constexpr uint32_t kClanBlockCount = 0x801782C0u;

// The three 7x10 s32 layer grids the game passes to the draw funcs.
constexpr uint32_t kMidBuffer = 0x80180930u;
constexpr uint32_t kEnvBuffer = 0x80180B60u;
constexpr uint32_t kBandBuffer = 0x80180D90u;

// Layer fill inputs (all from the community decomp's func_8001107C).
constexpr uint32_t kMidMap = 0x80108DE8u;
constexpr uint32_t kMidAddend = 0x80137474u;
constexpr uint32_t kEnvMapPtr = 0x8013746Cu;
constexpr uint32_t kEnvAddend = 0x80137476u;
constexpr uint32_t kBackdropMapPtr = 0x80137470u;
constexpr uint32_t kBackdropAddend = 0x80137478u;
constexpr uint32_t kTexturePoolPtr = 0x80180FC0u;
constexpr uint32_t kMidMode = 0x800BE588u;      // 0..3, X/Y parallax variants
constexpr uint32_t kEnvMode = 0x800BE58Cu;      // 1 = 128x8 map, else 16x16
constexpr uint32_t kBandPerRow = 0x800BE6FCu;   // != 0: per-row parallax band
constexpr uint32_t kEnvScrollX = 0x800BE578u;
constexpr uint32_t kEnvScrollY = 0x800BE580u;
constexpr uint32_t kBackdropScrollX = 0x800BE57Cu;
constexpr uint32_t kBackdropScrollY = 0x800BE584u;
constexpr uint32_t kCamX = 0x800BE558u;
constexpr uint32_t kCamY = 0x800BE55Cu;
constexpr uint32_t kViewLeft = 0x800BE568u;
constexpr uint32_t kViewRight = 0x800BE56Cu;
constexpr uint32_t kViewBottom = 0x800BE570u;
constexpr uint32_t kViewTop = 0x800BE574u;
constexpr uint32_t kCamShake = 0x800BE594u;
constexpr uint32_t kMidColMask = 0x800BE64Cu;
constexpr uint32_t kMidRowMask = 0x800BE650u;
constexpr uint32_t kMidShift = 0x800BE654u;
constexpr uint32_t kMidMapH = 0x800BE648u;       // 0x4000 >> shift
constexpr uint32_t kBandRowScroll = 0x8011D3B0u; // s16[7], stride 4

// Map-bank residency: func_80026220 records the RLE-decompressed extent of
// the tile-texture bank here. Tile textures live at kPoolBase + slot*0x400;
// map cells whose slot lies past the decompressed end have no texture loaded
// (the source of the forest-sky wing noise and the old per-scene blacklists).
constexpr uint32_t kBankStart = 0x80137724u;
constexpr uint32_t kBankEnd = 0x80137728u;
constexpr uint32_t kMapBankDest = 0x80380000u;
constexpr uint32_t kPoolBase = 0x80380600u;

constexpr uint32_t kCurrentScene = 0x800BE5D0u;
constexpr uint32_t kCannotPause = 0x800BE4ECu;
constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kStageTime = 0x801781E0u;
constexpr uint32_t kStageScenes = 0x800C8378u;
constexpr uint32_t kStageIds = 0x800C83F8u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kHighestUnlockedStage = 0x80171B18u;
constexpr uint32_t kCurrentStageId = 0x800D28E4u;
constexpr uint32_t kMarinaHold = 0x801370CCu;
constexpr uint32_t kMarinaPress = 0x801370CEu;
constexpr uint32_t kButtonHoldHistory = 0x801225F0u;
constexpr uint32_t kButtonPressHistory = 0x8011DD70u;
constexpr uint32_t kButtonDLeft = 0x800BE50Cu;
constexpr uint32_t kButtonDRight = 0x800BE510u;
constexpr uint32_t kMarinaActorPosX = 0x800EF598u;
constexpr uint32_t kGlobalButtonHold = 0x800BE4F8u;
constexpr uint32_t kGlobalButtonPress = 0x800BE4FCu;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

bool valid_rdram_ptr(uint32_t ptr) {
    return ptr >= 0x80000000u && ptr < 0x80800000u;
}

int g_gameplay_wide_active = 0;
std::atomic<int> g_force_frame_clear{0};
std::atomic<int> g_rotation_wall_wrap{0};

// These gameplay scenes transform or composite an authored 320x240 canvas
// rather than drawing a scrollable world. Expanding the projection reveals
// outside the canvas (rotation voids, disconnected boss backdrops, or a
// 4:3-only post-process rectangle), so present them faithfully in 4:3 just
// like cinematics. Ordinary 2D/2.5D stages—including the road and Taurus—
// remain genuinely expanded.
bool scene_requires_original(int scene) {
    switch (scene) {
        case 25: // SASQUATCH beta: fixed vertical boss canvas
        case 27: // Final Battle: fixed boss backdrop
        case 57: // Volcano: fixed authored canvas
        case 71: // Clanball Lift: fixed authored canvas
        case 79: // Inner Struggle: 4:3 post-process canvas
        case 85: // MERCO: fixed boss backdrop
            return true;
        default:
            return false;
    }
}

// Emit a renderer-independent test marker after the stage timer has advanced
// for 30 controlled ticks. Fixed-canvas and vanilla 4:3 runs never enter
// expanded mode, so the screenshot harness cannot use "gameplay-expand" as
// their readiness signal. This also avoids wall-clock delays, which become
// wildly inaccurate while the harness holds the fast-forward key.
void report_gameplay_ready(uint8_t* rdram, int scene) {
    (void)rdram;
    static int tracked_scene = -1;
    static int previous_stage_time = -1;
    static int advancing_ticks = 0;
    static bool reported = false;

    const int stage_time = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kStageTime)));
    if (scene != tracked_scene) {
        tracked_scene = scene;
        previous_stage_time = stage_time;
        advancing_ticks = 0;
        reported = false;
    }

    const int game_state = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kGameState)));
    const int cannot_pause = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCannotPause)));
    if (game_state != 6 || cannot_pause != 0) {
        previous_stage_time = stage_time;
        advancing_ticks = 0;
        return;
    }

    if (stage_time != previous_stage_time) {
        previous_stage_time = stage_time;
        if (advancing_ticks < 30) {
            ++advancing_ticks;
        }
    }

    if (!reported && advancing_ticks >= 30) {
        reported = true;
        std::fprintf(stderr,
            "[widescreen] gameplay-ready scene=%d\n", scene);
    }
}

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
    if (game_state != 6 || scene_requires_original(scene)) {
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

    // gStageTime normally advances during control and freezes for cinema,
    // while gCannotPause prevents an early expand during the tail of stage
    // intros whose timer has already started. Some gameplay effects briefly
    // assert gCannotPause too; mm_widescreen_sync_mode's one-second exit
    // hysteresis absorbs those pulses without snapping back to 4:3.
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

// RT64 normally preserves the N64 framebuffer until the game's own draws
// replace it. The two rotating-camera stages use a persistent burgundy canvas;
// RT64's HD target otherwise feeds old sprite pixels back into that canvas,
// producing a Solitaire-style trail.
namespace RT64 {
int mm_widescreen_force_frame_clear() {
    return g_force_frame_clear.load(std::memory_order_relaxed);
}

int mm_widescreen_rotation_wall_wrap() {
    return g_rotation_wall_wrap.load(std::memory_order_relaxed);
}
} // namespace RT64

// Flip RT64 between its native 4:3 and expanded projection only when the
// gameplay/cinema boundary changes. The user's --widescreen preference remains
// untouched; this is a transient presentation gate, not a config toggle.
extern "C" void mm_widescreen_sync_mode(uint8_t* rdram) {
    static int previous_active = -1;
    static int candidate_active = -1;
    static int candidate_frames = 0;
    const int scene = static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
    report_gameplay_ready(rdram, scene);
    g_rotation_wall_wrap.store(scene == 13 || scene == 69,
        std::memory_order_relaxed);
    // With the wall panels rendering (RT64 LOD_FRAC fix), the rotation
    // stages' own opaque wall covers the playfield every frame, exactly as
    // on hardware — no clear needed, and a per-frame clear races the wall
    // draw mid-frame (geometry flickering in and out). Keep a brief seed
    // pulse at gameplay entry only, so RT64's two double-buffered HD
    // targets start from a common base instead of diverging (the 30 Hz
    // beige/burgundy flicker seen in older builds).
    static int rotation_seed_frames = 0;
    const int rotation_game_state = MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kGameState)));
    int force_clear = 0;
    if (scene != 13 && scene != 69) {
        rotation_seed_frames = 0;
    }
    else if (rotation_game_state != 6) {
        rotation_seed_frames = 6;
    }
    else if (rotation_seed_frames > 0) {
        --rotation_seed_frames;
        force_clear = 1;
    }
    g_force_frame_clear.store(force_clear, std::memory_order_relaxed);
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

        // Enter either presentation only after it has remained stable. Stage
        // effects can freeze gStageTime and assert gCannotPause for dozens of
        // frames (Rolling Rock does this repeatedly); switching aspect ratio
        // during those effects exposes the centered 4:3 composite and can
        // split RT64's layer targets into missing squares. Opening cinematics
        // still start in Original mode because their initial raw state is 0;
        // an in-stage cinema gets one second of hysteresis before the wings
        // close, which is preferable to destructive gameplay flicker.
        const int required_frames = raw_active ? 30 : 60;
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
        std::fprintf(stderr, "[warp] stage=%d scene=%d\n", stage,
            static_cast<int16_t>(MEM_HU(stage * 2, scenes)));
        return MEM_HU(stage * 2, scenes);
    }
    return static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCurrentScene))));
}

// Test-only controller driver. The hook writes both the live and copied input
// words so it can exercise camera scrolling and actor streaming without X11
// focus. MM_TEST_MOVE=1 performs the original short right/left taps;
// MM_TEST_MOVE=jet-right/jet-left settles, double-taps that direction, then
// holds it long enough to engage Marina's jet and traverse a meaningful part
// of the stage. jet-bounce alternates long right/left jets so a render-cull
// boundary is crossed in both directions during a single frame burst.
extern "C" void mm_test_drive_marina(uint8_t* rdram) {
    static int motion_frame = 0;
    static uint16_t previous_input = 0;
    const char* move_mode = std::getenv("MM_TEST_MOVE");
    if (move_mode == nullptr || move_mode[0] == '\0' || move_mode[0] == '0') {
        motion_frame = 0;
        previous_input = 0;
        return;
    }

    const bool jet_right = std::strcmp(move_mode, "jet-right") == 0;
    const bool jet_hop_right =
        std::strcmp(move_mode, "jet-hop-right") == 0;
    const bool jet_left = std::strcmp(move_mode, "jet-left") == 0;
    const bool jet_bounce = std::strcmp(move_mode, "jet-bounce") == 0;
    const bool jet = jet_right || jet_hop_right || jet_left || jet_bounce;
    const uint16_t button_right = MEM_HU(0,
        static_cast<gpr>(static_cast<int32_t>(kButtonDRight)));
    const uint16_t button_left = MEM_HU(0,
        static_cast<gpr>(static_cast<int32_t>(kButtonDLeft)));
    const uint16_t jet_direction = jet_left ? button_left : button_right;
    const bool controls_blocked =
        !mm_widescreen_gameplay_active(rdram) ||
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kGameState))) != 6 ||
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCannotPause))) != 0;
    if (controls_blocked) {
        // A stage-3 tutorial interrupts control as Marina crosses the first
        // house. Restarting the jet sequence after the text closes makes the
        // harness boost over the trigger again forever. Clear injected input
        // while blocked, but preserve a completed jet as a continuous hold so
        // the moving test naturally walks out of the tutorial region.
        if (!jet || MEM_HU(0, static_cast<gpr>(
                static_cast<int32_t>(kGameState))) != 6) {
            motion_frame = 0;
        }
        // Bounce mode deliberately creates a fresh edge after an interruption
        // so its scheduled reverse leg cannot inherit a stale held direction.
        previous_input = jet && !jet_bounce && motion_frame >= 34
            ? jet_direction : 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaHold))) = 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaPress))) = 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonHold))) = 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonPress))) = 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonHoldHistory))) = 0;
        MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonPressHistory))) = 0;
        return;
    }

    // Jet modes double-tap once and then keep holding the direction. Repeating
    // the whole 300-frame cycle can retrigger local tutorial volumes and never
    // exercises the rest of a stage.
    const int phase = jet_bounce ? motion_frame++ % 600
        : (jet ? motion_frame++ : motion_frame++ % 120);
    uint16_t input = 0;
    if (jet_bounce) {
        // R, neutral, long R; settle; L, neutral, long L. The quiet tail
        // creates a clean rising edge when the 600-frame cycle repeats.
        if ((phase >= 30 && phase < 32) ||
            (phase >= 34 && phase < 270)) {
            input = button_right;
        }
        else if ((phase >= 300 && phase < 302) ||
                 (phase >= 304 && phase < 540)) {
            input = button_left;
        }
    }
    else if (jet) {
        // Two distinct rising edges, then a long hold: R, neutral, R+hold.
        if ((phase >= 30 && phase < 32) ||
            phase >= 34) {
            input |= jet_direction;
        }
        // Clear the tall clan-block structures used in the early forest
        // stages so the long-distance streaming capture does not spend the
        // rest of the run pushing into the first wall. N64 A is jump.
        if (jet_hop_right && phase >= 50 && (phase % 90) < 12) {
            input |= 0x8000u;
        }
    }
    else if (phase < 15) {
        input = MEM_HU(0,
            static_cast<gpr>(static_cast<int32_t>(kButtonDRight)));
    }
    else if (phase >= 60 && phase < 75) {
        input = MEM_HU(0,
            static_cast<gpr>(static_cast<int32_t>(kButtonDLeft)));
    }
    const uint16_t pressed = input & ~previous_input;
    previous_input = input;

    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaHold))) = input;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaPress))) = pressed;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonHold))) = input;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonPress))) = pressed;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonHoldHistory))) = input;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonPressHistory))) = pressed;
    if ((motion_frame % (jet ? 15 : 60)) == 0) {
        const gpr marina = static_cast<gpr>(static_cast<int32_t>(kActors));
        const int clan_slots = MEM_HU(0, static_cast<gpr>(
            static_cast<int32_t>(kClanBlockCount)));
        int clan_count = 0;
        int clan_min_x = 32767;
        int clan_max_x = -32768;
        const gpr clan_blocks = static_cast<gpr>(
            static_cast<int32_t>(kClanBlocks));
        for (int i = 0; i < clan_slots && i < 128; ++i) {
            if (MEM_HU(i * 0x90 + 0x80, clan_blocks) == 0) {
                continue;
            }
            const int x = static_cast<int16_t>(MEM_HU(i * 0x90 + 0x84,
                clan_blocks));
            ++clan_count;
            clan_min_x = x < clan_min_x ? x : clan_min_x;
            clan_max_x = x > clan_max_x ? x : clan_max_x;
        }
        std::fprintf(stderr,
            "[test-move] mode=%s frame=%d phase=%d hold=%04x press=%04x "
            "actor=%d,%d camera=%d,%d velocity=%d,%d state=%04x "
            "clan=%d range=%d..%d\n",
            jet_bounce ? "jet-bounce"
                : (jet_hop_right ? "jet-hop-right"
                    : (jet_right ? "jet-right"
                        : (jet_left ? "jet-left" : "patrol"))),
            motion_frame, phase,
            input, pressed,
            static_cast<int32_t>(MEM_W(0, static_cast<gpr>(
                static_cast<int32_t>(kMarinaActorPosX)))) >> 16,
            static_cast<int32_t>(MEM_W(0x8C, marina)) >> 16,
            static_cast<int32_t>(MEM_W(0, static_cast<gpr>(
                static_cast<int32_t>(kCamX)))) >> 16,
            static_cast<int32_t>(MEM_W(0, static_cast<gpr>(
                static_cast<int32_t>(kCamY)))) >> 16,
            static_cast<int32_t>(MEM_W(0xEC, marina)) >> 16,
            static_cast<int32_t>(MEM_W(0xF0, marina)) >> 16,
            MEM_HU(0xD0, marina), clan_count,
            clan_count == 0 ? 0 : clan_min_x,
            clan_count == 0 ? 0 : clan_max_x);
    }
}

// Test-only actor census for diagnosing the long tail of per-type 4:3 culls.
// Log active actors near the camera and every draw-bit transition. In normal
// builds this is a single getenv check and does not touch emulated memory.
extern "C" void mm_test_trace_actors(uint8_t* rdram) {
    if (!env_enabled("MM_TEST_ACTOR_TRACE") ||
        !mm_widescreen_gameplay_active(rdram)) {
        return;
    }

    constexpr int kActorCount = 208;
    static uint32_t previous_flags[kActorCount]{};
    static uint16_t previous_types[kActorCount]{};
    static int frame = 0;
    const int cam_x = static_cast<int32_t>(MEM_W(
        0, static_cast<gpr>(static_cast<int32_t>(kCamX)))) >> 16;
    const int cam_y = static_cast<int32_t>(MEM_W(
        0, static_cast<gpr>(static_cast<int32_t>(kCamY)))) >> 16;
    const gpr actors = static_cast<gpr>(static_cast<int32_t>(kActors));

    if ((frame % 15) == 0) {
        const int left = static_cast<int16_t>(MEM_HU(
            0, static_cast<gpr>(static_cast<int32_t>(kViewLeft))));
        const int right = static_cast<int16_t>(MEM_HU(
            0, static_cast<gpr>(static_cast<int32_t>(kViewRight))));
        const int bottom = static_cast<int16_t>(MEM_HU(
            0, static_cast<gpr>(static_cast<int32_t>(kViewBottom))));
        const int top = static_cast<int16_t>(MEM_HU(
            0, static_cast<gpr>(static_cast<int32_t>(kViewTop))));
        std::fprintf(stderr,
            "[test-view] frame=%d cam=%d,%d bounds=%d..%d,%d..%d "
            "margin=%d,%d\n",
            frame, cam_x, cam_y, left, right, top, bottom,
            cam_x - left, right - cam_x);
    }

    const auto dispatch_for_type = [rdram](uint16_t type) -> uint32_t {
        const uint8_t bank = type >> 8;
        const uint8_t index = type;
        uint32_t table = 0;
        if (type < 0x100) {
            table = 0x800C7FE0u;
        }
        else if (bank == 1 || bank == 4 || bank == 5 || bank == 7 ||
                 bank == 14 || bank == 27 ||
                 (bank >= 39 && bank <= 42)) {
            table = 0x801B0800u;
        }
        else if (bank == 2 || bank == 3 || (bank >= 9 && bank <= 13) ||
                 (bank >= 17 && bank <= 19)) {
            table = 0x8019B000u;
        }
        else if (bank == 23 || bank == 24 || bank == 26 ||
                 (bank >= 28 && bank <= 32) || bank == 37) {
            table = 0x801A6800u;
        }
        else if (bank == 6 || bank == 15 || bank == 16 ||
                 (bank >= 20 && bank <= 22) || bank == 25 ||
                 (bank >= 33 && bank <= 36) || bank == 38 ||
                 bank == 43 || bank == 44) {
            table = 0x80192000u;
        }
        return table == 0 ? 0 : MEM_W(index * 4,
            static_cast<gpr>(static_cast<int32_t>(table)));
    };

    for (int slot = 0; slot < kActorCount; ++slot) {
        const gpr actor = actors + slot * 0x198;
        const uint32_t flags = MEM_W(0x80, actor);
        const uint16_t type = MEM_HU(0xD2, actor);
        const bool active = (flags & 2u) != 0;
        const bool was_active = (previous_flags[slot] & 2u) != 0;
        const bool draw_changed = ((flags ^ previous_flags[slot]) & 1u) != 0;
        const bool identity_changed = type != previous_types[slot];
        const int x = static_cast<int32_t>(MEM_W(0x88, actor)) >> 16;
        const int y = static_cast<int32_t>(MEM_W(0x8C, actor)) >> 16;
        // Actor positions have already been transformed into screen space by
        // the time this end-of-frame hook runs. Adding the camera position is
        // therefore a useful approximate world coordinate; subtracting it a
        // second time hid exactly the actors entering a widescreen wing.
        const int world_x = x + cam_x;
        const int world_y = y + cam_y;
        const bool periodic_nearby = active && (frame % 15) == 0 &&
            x >= -512 && x <= 512 && y >= -512 && y <= 512;

        if (periodic_nearby || draw_changed || active != was_active ||
            (active && identity_changed)) {
            std::fprintf(stderr,
                "[test-actor] frame=%d slot=%d type=%04x flags=%08x "
                "draw=%d active=%d screen=%d,%d world~=%d,%d cam=%d,%d "
                "extent=%d z=%d gfx=%04x "
                "gflags=%04x state=%04x dispatch=%08x event=%s%s%s\n",
                frame, slot, type, flags, flags & 1u ? 1 : 0,
                active ? 1 : 0, x, y, world_x, world_y, cam_x, cam_y,
                static_cast<int32_t>(MEM_W(0x188, actor)),
                static_cast<int32_t>(MEM_W(0x90, actor)) >> 16,
                MEM_HU(0x84, actor), MEM_HU(0x94, actor),
                MEM_HU(0xD0, actor), dispatch_for_type(type),
                draw_changed ? "draw " : "",
                active != was_active ? "active " : "",
                identity_changed ? "type" : "");
        }
        previous_flags[slot] = flags;
        previous_types[slot] = type;
    }
    ++frame;
}

// Test-only census for the world-record loader. This catches streaming limits
// that a static screenshot cannot: the widened loader may admit more
// candidates than its original 64-entry destination can hold.
extern "C" void mm_test_trace_streaming(
    uint8_t* rdram, recomp_context* ctx) {
    if (!env_enabled("MM_TEST_STREAM_TRACE") ||
        !mm_widescreen_gameplay_active(rdram)) {
        return;
    }

    static int frame = 0;

    if (const char* forced_camera = std::getenv("MM_TEST_STREAM_CAMERA")) {
        int x = 0;
        int y = 0;
        if (std::sscanf(forced_camera, "%d,%d", &x, &y) >= 1) {
            MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kCamX))) = x << 16;
            if (std::strchr(forced_camera, ',') != nullptr) {
                MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kCamY))) = y << 16;
            }
        }
    }
    const int cam_x = static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCamX))));
    const int cam_y = static_cast<int16_t>(MEM_HU(
        0, static_cast<gpr>(static_cast<int32_t>(kCamY))));
    const bool alternate = MEM_BU(
        0, static_cast<gpr>(static_cast<int32_t>(0x800BE710u))) != 0;
    constexpr int kStreamRadius = 0x1C0;
    constexpr int kStreamWidth = kStreamRadius * 2;
    const int left = cam_x - kStreamRadius;
    const int right = cam_x + kStreamRadius;
    const int top = cam_y - (alternate ? 0xD0 : 0x84);
    const int bottom = cam_y + (alternate ? 0xD0 : 0x84);
    const gpr records = static_cast<gpr>(ctx->r4);
    int candidates = 0;
    int candidate_min_x = 32767;
    int candidate_max_x = -32768;
    for (int i = 0; i < 4096; ++i) {
        const uint16_t flags = MEM_HU(i * 6, records);
        if (flags == 0) {
            break;
        }
        const int x = MEM_HU(i * 6 + 2, records);
        const int y = MEM_HU(i * 6 + 4, records);
        if (x > left && x < right && y > top && y < bottom &&
            (flags & 0xC000u) == 0) {
            const int relative_x = x - cam_x;
            ++candidates;
            candidate_min_x = relative_x < candidate_min_x
                ? relative_x : candidate_min_x;
            candidate_max_x = relative_x > candidate_max_x
                ? relative_x : candidate_max_x;
        }
    }

    static int scanned_stage = -1;
    const int stage = MEM_HU(0, static_cast<gpr>(
        static_cast<int32_t>(kCurrentStage)));
    if (stage != scanned_stage) {
        scanned_stage = stage;
        int world_min_x = 65535;
        int world_max_x = 0;
        int world_min_y = 65535;
        int world_max_y = 0;
        int record_count = 0;
        for (int i = 0; i < 4096; ++i) {
            const uint16_t flags = MEM_HU(i * 6, records);
            if (flags == 0) {
                break;
            }
            const int x = MEM_HU(i * 6 + 2, records);
            const int y = MEM_HU(i * 6 + 4, records);
            if (y > top && y < bottom && (flags & 0xC000u) == 0) {
                ++record_count;
                world_min_x = x < world_min_x ? x : world_min_x;
                world_max_x = x > world_max_x ? x : world_max_x;
            }
            if ((flags & 0xC000u) == 0) {
                world_min_y = y < world_min_y ? y : world_min_y;
                world_max_y = y > world_max_y ? y : world_max_y;
            }
        }
        int peak = 0;
        int peak_camera = 0;
        int saturated_first = -1;
        int saturated_last = -1;
        for (int test_camera = world_min_x - kStreamRadius;
             test_camera <= world_max_x + kStreamRadius; ++test_camera) {
            int count = 0;
            for (int i = 0; i < 4096; ++i) {
                const uint16_t flags = MEM_HU(i * 6, records);
                if (flags == 0) {
                    break;
                }
                const int x = MEM_HU(i * 6 + 2, records);
                const int y = MEM_HU(i * 6 + 4, records);
                if (x > test_camera - kStreamRadius &&
                    x < test_camera + kStreamRadius &&
                    y > top && y < bottom && (flags & 0xC000u) == 0) {
                    ++count;
                }
            }
            if (count > peak) {
                peak = count;
                peak_camera = test_camera;
            }
            if (count > 64) {
                saturated_first = saturated_first < 0
                    ? test_camera : saturated_first;
                saturated_last = test_camera;
            }
        }

        // Repeat the census across every vertical slice in the record table.
        // Sorting the eligible X coordinates lets a sliding window find the
        // densest 0x300-wide region without an expensive X/Y brute force.
        int slice_x[4096];
        int peak_2d = 0;
        int peak_2d_x = 0;
        int peak_2d_y = 0;
        const int y_radius = alternate ? 0xD0 : 0x84;
        for (int test_y = world_min_y - y_radius;
             test_y <= world_max_y + y_radius; ++test_y) {
            int slice_count = 0;
            for (int i = 0; i < 4096; ++i) {
                const uint16_t flags = MEM_HU(i * 6, records);
                if (flags == 0) {
                    break;
                }
                const int y = MEM_HU(i * 6 + 4, records);
                if (y > test_y - y_radius && y < test_y + y_radius &&
                    (flags & 0xC000u) == 0) {
                    slice_x[slice_count++] = MEM_HU(i * 6 + 2, records);
                }
            }
            std::sort(slice_x, slice_x + slice_count);
            int first = 0;
            for (int last = 0; last < slice_count; ++last) {
                while (slice_x[last] - slice_x[first] >= kStreamWidth) {
                    ++first;
                }
                const int count = last - first + 1;
                if (count > peak_2d) {
                    peak_2d = count;
                    peak_2d_x = (slice_x[first] + slice_x[last]) / 2;
                    peak_2d_y = test_y;
                }
            }
        }
        std::fprintf(stderr,
            "[test-stream-scan] stage=%d records=%d world=%d..%d peak=%d "
            "camera=%d saturated=%d..%d peak2d=%d camera2d=%d,%d\n",
            stage, record_count, world_min_x, world_max_x, peak, peak_camera,
            saturated_first, saturated_last, peak_2d, peak_2d_x, peak_2d_y);
    }

    int loaded = 0;
    const gpr clan_blocks = static_cast<gpr>(
        static_cast<int32_t>(kClanBlocks));
    for (int i = 0; i < 128; ++i) {
        loaded += MEM_HU(i * 0x90 + 0x80, clan_blocks) != 0;
    }
    static int previous_candidates = -1;
    static int previous_loaded = -1;
    if (candidates != previous_candidates || loaded != previous_loaded ||
        (frame % 120) == 0) {
        std::fprintf(stderr,
            "[test-stream] frame=%d stage=%d camera=%d,%d candidates=%d "
            "loaded=%d range=%d..%d alternate=%d\n",
            frame, stage, cam_x, cam_y, candidates, loaded,
            candidates == 0 ? 0 : candidate_min_x,
            candidates == 0 ? 0 : candidate_max_x, alternate ? 1 : 0);
        previous_candidates = candidates;
        previous_loaded = loaded;
    }
    ++frame;
}

namespace {

gpr rdram_gpr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

int16_t read_s16(uint8_t* rdram, uint32_t vaddr) {
    return static_cast<int16_t>(MEM_HU(0, rdram_gpr(vaddr)));
}

struct LayerFillState {
    int scene = -32768;
    uint16_t mid_x0 = 0;
    uint16_t mid_y0 = 0;
    uint16_t mid_col_mask = 0;
    uint16_t mid_row_mask = 0;
    uint16_t mid_shift = 0;
    uint16_t mid_addend = 0;
    uint32_t env_map = 0;
    uint16_t env_x0 = 0;
    uint16_t env_y0 = 0;
    uint16_t env_addend = 0;
    bool env_wide = false;
    uint32_t backdrop_map = 0;
    uint16_t backdrop_x0 = 0;
    uint16_t backdrop_y0 = 0;
    uint16_t backdrop_addend = 0;
    uint16_t band_x0[7]{};
    bool band_per_row = false;
    bool valid = false;
};

LayerFillState g_layer_fill_state;

void compute_mid_fill_bases(uint8_t* rdram, uint16_t& x0, uint16_t& y0) {
    const int mode = MEM_HU(0, rdram_gpr(kMidMode));
    const int map_h = MEM_HU(0, rdram_gpr(kMidMapH));
    const int cam_x = read_s16(rdram, kCamX);
    const int cam_y = read_s16(rdram, kCamY);
    const int shake = read_s16(rdram, kCamShake);

    int x;
    if (mode == 0) {
        x = cam_x - 0x92;
    }
    else if (mode == 1) {
        x = static_cast<int>(static_cast<double>(cam_x) / 1.55 - 2.0);
    }
    else {
        x = static_cast<int>(static_cast<double>(cam_x) / 1.55);
    }
    const int y = (mode == 3)
        ? static_cast<int>(static_cast<double>(map_h) -
            (static_cast<double>(cam_y) / 1.55 +
                static_cast<double>(shake) - 92.0))
        : (map_h - cam_y - shake + 0x5C);
    x0 = static_cast<uint16_t>(x);
    y0 = static_cast<uint16_t>(y);
}

// Wing tile -> s32 texture-pointer slot, exactly like the game's own
// s16-grid fill + draw-side conversion: value = ((tile + addend) << 10) +
// pool base, INCLUDING tile 0 (the game draws pool slot addend<<10 for it;
// skipping it leaves holes where authored maps use tile 0 -- safe now that
// the display-list arenas are relocated and can hold all 140 tiles). The
// single wing-only deviation: a slot whose texture lies past the
// decompressed map-bank end has no data loaded -- draw nothing instead of
// noise. This per-tile residency check replaces the old per-scene
// blacklists.
// Host-side effective bank extent: func_80026428 reloads the beginning of a
// bank without updating *0x80137728 (scenes 10, 24, 46..51). Importantly it
// does NOT erase the still-resident tail loaded by func_80026220, so the
// effective end is max(authoritative end, replacement end). Treating the
// replacement's shorter end as the whole bank removed complete environment
// and midground layers from Taurus's wings. Keep this host-side — the game's
// own bss values must retain their original semantics.
uint32_t g_host_bank_end = 0;

int32_t wing_slot(uint8_t* rdram, int tile, int addend, uint32_t texture_pool) {
    const uint32_t value = static_cast<uint32_t>(tile + addend) & 0xFFFFu;
    uint32_t bank_end = static_cast<uint32_t>(MEM_W(0, rdram_gpr(kBankEnd)));
    if (g_host_bank_end != 0) {
        bank_end = g_host_bank_end;
    }
    if (bank_end > kPoolBase && bank_end < 0x80800000u) {
        const uint32_t slot_end = kPoolBase + (value << 10) + 0x400u;
        if (slot_end > bank_end) {
            return 0;
        }
    }
    return static_cast<int32_t>((value << 10) + texture_pool);
}

struct WingContext {
    uint8_t* rdram;
    gpr src;            // game's 7x10 s32 grid (center columns, verbatim)
    gpr dst;            // 7x20 scratch grid
    uint32_t texture_pool;
};

// Repack the 7x10 grid to stride 20. wing(row, col) supplies the s32 slot
// for camera-relative columns -5..-1 and 10..14; pass nullptr for zeroed
// wings (unknown caller or wings disabled).
template <typename WingFn>
void repack_grid(const WingContext& wc, WingFn wing, bool wings_on) {
    uint8_t* rdram = wc.rdram;
    for (int row = 0; row < 7; ++row) {
        for (int wide_col = 0; wide_col < 20; ++wide_col) {
            const int col = wide_col - 5;
            int32_t value;
            if (col >= 0 && col < 10) {
                value = MEM_W((row * 10 + col) * 4, wc.src);
            }
            else if (wings_on) {
                value = wing(row, col);
            }
            else {
                value = 0;
            }
            MEM_W((row * 20 + wide_col) * 4, wc.dst) = value;
        }
    }
}

bool wings_active(uint8_t* rdram, const char* layer_env) {
    return env_enabled(layer_env) && mm_widescreen_gameplay_active(rdram);
}

void trace_layer_once(uint8_t* rdram, const char* layer, gpr grid) {
    if (!env_enabled("MM_TEST_AUTO_ADVANCE") ||
        !mm_widescreen_gameplay_active(rdram)) {
        return;
    }

    struct TraceKey {
        int scene;
        const char* layer;
    };
    static TraceKey seen[256]{};
    static int seen_count = 0;
    const int scene = static_cast<int16_t>(MEM_HU(0, rdram_gpr(kCurrentScene)));
    for (int i = 0; i < seen_count; ++i) {
        if (seen[i].scene == scene && seen[i].layer == layer) {
            return;
        }
    }
    if (seen_count < static_cast<int>(sizeof(seen) / sizeof(seen[0]))) {
        seen[seen_count++] = {scene, layer};
    }

    int center = 0;
    int wings = 0;
    int center_past_bank = 0;
    uint32_t min_value = 0xFFFFFFFFu;
    uint32_t max_value = 0;
    const uint32_t bank_end = g_host_bank_end != 0
        ? g_host_bank_end
        : static_cast<uint32_t>(MEM_W(0, rdram_gpr(kBankEnd)));
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 20; ++col) {
            const uint32_t value = static_cast<uint32_t>(
                MEM_W((row * 20 + col) * 4, grid));
            if (value == 0) {
                continue;
            }
            min_value = value < min_value ? value : min_value;
            max_value = value > max_value ? value : max_value;
            if (col >= 5 && col < 15) {
                ++center;
                if (bank_end > kPoolBase && value + 0x400u > bank_end) {
                    ++center_past_bank;
                }
            }
            else {
                ++wings;
            }
        }
    }
    std::fprintf(stderr,
        "[widescreen-layer] scene=%d layer=%s center=%d/70 wings=%d/70 "
        "center_past_bank=%d range=%08x..%08x bank_end=%08x\n",
        scene, layer, center, wings, center_past_bank,
        min_value == 0xFFFFFFFFu ? 0 : min_value, max_value, bank_end);
}

// The game's six per-layer display-list arenas (double-buffered mid/env/band)
// are 0x1620 bytes each: exactly 70 tiles x 80 bytes + an 8-byte G_ENDDL,
// with 0x38 bytes of slack. The widened 7x20 loops can emit up to 140 tiles
// (0x2BC8 bytes), and the draw loop has NO bounds check — the overrun from
// the last arena (D_8017F310) stomps the layer texture-pointer grids at
// 0x80180930+ and can reach the texture-pool base at 0x80180FC0, which is
// the intermittent full-frame tile noise. The arena base is parameter-borne
// ($a0): the draw func keeps its cursor in a register, and emits both the
// G_ENDDL and the gSPDisplayList submission from that same base, so
// remapping $a0 at function entry relocates everything, including what the
// RSP reads. 0x80400000..0x80418000 is free: the game uses 4MB and the
// runtime's extended allocations start at 0x80800000.
uint32_t remap_arena(uint32_t base) {
    switch (base) {
        case 0x80178470u: return 0x80400000u; // midground, buffer A
        case 0x80179A90u: return 0x80404000u; // midground, buffer B
        case 0x8017B0B0u: return 0x80408000u; // env, buffer A
        case 0x8017C6D0u: return 0x8040C000u; // env, buffer B
        case 0x8017DCF0u: return 0x80410000u; // band/static, buffer A
        case 0x8017F310u: return 0x80414000u; // band/static, buffer B
        default: return base;                 // unknown caller: leave alone
    }
}

} // namespace

// func_8001107C fills all three center tile grids near the start of the game
// tick. The camera and parallax actors can update their scroll registers again
// before func_80082CFC converts and draws those grids. Re-reading the registers
// while adding the widescreen columns then mixes the newly filled 4:3 center
// with wings from a different map origin, producing whole-tile pops. Snapshot
// every input used by the fill and keep all widened layers on that snapshot.
extern "C" void mm_ws_capture_mid_fill_state(
    uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    LayerFillState& state = g_layer_fill_state;
    state.scene = static_cast<int16_t>(
        MEM_HU(0, rdram_gpr(kCurrentScene)));
    compute_mid_fill_bases(rdram, state.mid_x0, state.mid_y0);
    state.mid_col_mask = MEM_HU(0, rdram_gpr(kMidColMask));
    state.mid_row_mask = MEM_HU(0, rdram_gpr(kMidRowMask));
    state.mid_shift = MEM_HU(0, rdram_gpr(kMidShift)) & 0x1F;
    state.mid_addend = MEM_HU(0, rdram_gpr(kMidAddend));

    state.env_map = static_cast<uint32_t>(
        MEM_W(0, rdram_gpr(kEnvMapPtr)));
    state.env_addend = MEM_HU(0, rdram_gpr(kEnvAddend));
    state.env_wide = MEM_HU(0, rdram_gpr(kEnvMode)) == 1;
    state.env_x0 = static_cast<uint16_t>(
        (read_s16(rdram, kEnvScrollX) - 2) &
        (state.env_wide ? 0xFFF : 0x1FF));
    state.env_y0 = static_cast<uint16_t>(
        (-12 - read_s16(rdram, kEnvScrollY)) &
        (state.env_wide ? 0xFF : 0x1FF));

    state.backdrop_map = static_cast<uint32_t>(
        MEM_W(0, rdram_gpr(kBackdropMapPtr)));
    state.backdrop_addend = MEM_HU(0, rdram_gpr(kBackdropAddend));
    state.backdrop_x0 = static_cast<uint16_t>(
        (read_s16(rdram, kBackdropScrollX) - 2) & 0x1FF);
    state.backdrop_y0 = static_cast<uint16_t>(
        (-12 - read_s16(rdram, kBackdropScrollY)) & 0x1FF);
    for (int row = 0; row < 7; ++row) {
        state.band_x0[row] = static_cast<uint16_t>(
            (read_s16(rdram, kBandRowScroll + row * 4) - 2) & 0x1FF);
    }
    state.band_per_row = MEM_HU(0, rdram_gpr(kBandPerRow)) != 0;
    state.valid = true;
}

// Two scenery controllers represent 512px repeating strips by teleporting
// actors between screen X -256/+255:
//
// - type 0x181c state 2: foreground trees/grass decoration, including a flip;
// - type 0x000d state 0x50: four 128px environment-landscape panels.
//
// That was sufficient for the original 320px viewport, but in 16:9 part of a
// wide actor is still visible when its sole copy teleports to the opposite
// wing. After func_8000FBF4 has built the renderer's local actor list, add a
// render-only adjacent repeat. Controllers and collision continue to use the
// untouched source actors.
extern "C" void mm_ws_repeat_wrapped_terrain(
    uint8_t* rdram, recomp_context* ctx) {
    if (!g_gameplay_wide_active ||
        env_enabled("MM_TEST_DISABLE_TERRAIN_REPEAT")) {
        return;
    }

    constexpr int kActorCount = 208;
    constexpr int kActorSize = 0x198;
    const int count = static_cast<int32_t>(ctx->r2);
    if (count <= 0 || count > kActorCount) {
        return;
    }

    const gpr list = ctx->r29 + 0x430;
    const gpr actors = rdram_gpr(kActors);
    uint16_t output[kActorCount];
    int out = 0;
    int clones = 0;
    int removed = 0;
    int hidden_state = -1;
    if (const char* value = std::getenv("MM_TEST_HIDE_TERRAIN_STATE")) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 0 && parsed <= 4) {
            hidden_state = static_cast<int>(parsed);
        }
    }

    for (int i = 0; i < count; ++i) {
        const uint16_t index = MEM_HU(i * 2, list);
        const gpr source = actors + index * kActorSize;
        const uint16_t type = MEM_HU(0xD2, source);
        const int terrain_state = MEM_HU(0xD0, source);
        const bool terrain = type == 0x181C;
        if (terrain && terrain_state == hidden_state) {
            ++removed;
            continue;
        }
        const bool flipping_terrain =
            terrain && terrain_state == 2 &&
            (MEM_HU(0x94, source) & 0x0100u) != 0;
        const bool landscape = type == 0x000D && terrain_state == 0x50;
        if (landscape && env_enabled("MM_TEST_HIDE_LANDSCAPE_PANELS")) {
            ++removed;
            continue;
        }
        const bool landscape_panel = landscape &&
            !env_enabled("MM_TEST_DISABLE_LANDSCAPE_REPEAT");
        const bool repeating_scenery = flipping_terrain || landscape_panel;

        if (repeating_scenery && out + 1 < kActorCount) {
            const int source_slot = static_cast<int>(index);
            if (source_slot >= 0 && source_slot < kActorCount) {
                const uint16_t clone_index = static_cast<uint16_t>(
                    kWrappedRepeatActorBase + source_slot);
                const gpr clone = actors + clone_index * kActorSize;
                // The first 0x80 bytes contain two renderer-owned 64-byte
                // matrices, one per framebuffer. Copying the whole source
                // record here can overwrite the matrix still referenced by
                // the in-flight framebuffer and make the repeated panel
                // flicker. Refresh only the controller-owned actor fields;
                // func_80009BE8 rebuilds the current clone matrix below.
                for (int word = 0x80; word < kActorSize; word += 4) {
                    MEM_W(word, clone) = MEM_W(word, source);
                }

                const int screen_x = static_cast<int16_t>(
                    MEM_HU(0x88, source));
                const int32_t raw_x = static_cast<int32_t>(
                    MEM_W(0x88, source));
                MEM_W(0x88, clone) = static_cast<uint32_t>(
                    raw_x + (screen_x <= 0 ? 0x02000000 : -0x02000000));
                if (flipping_terrain) {
                    MEM_W(0x80, clone) = MEM_W(0x80, clone) ^ 0x20u;
                }

                if (env_enabled("MM_TEST_TERRAIN_REPEAT_TRACE")) {
                    static bool reported_source[kActorCount]{};
                    if (!reported_source[source_slot]) {
                        reported_source[source_slot] = true;
                        std::fprintf(stderr,
                            "[widescreen-terrain-clone] source=%d clone=%u "
                            "type=%04x state=%04x screen=%d "
                            "clone_screen=%d flags=%08x\n",
                            source_slot, clone_index, type, terrain_state, screen_x,
                            static_cast<int16_t>(MEM_HU(0x88, clone)),
                            MEM_W(0x80, source));
                    }
                }

                // Draw the adjacent copy first so the controller-owned actor
                // remains authoritative in the unlikely overlap region.
                output[out++] = clone_index;
                ++clones;
            }
        }
        output[out++] = index;
    }

    if (clones == 0 && removed == 0) {
        return;
    }
    for (int i = 0; i < out; ++i) {
        MEM_H(i * 2, list) = output[i];
    }
    ctx->r2 = out;

    if (env_enabled("MM_TEST_ACTOR_TRACE")) {
        static int reported_scene = -32768;
        const int scene = static_cast<int16_t>(
            MEM_HU(0, rdram_gpr(kCurrentScene)));
        if (scene != reported_scene) {
            reported_scene = scene;
            std::fprintf(stderr,
                "[widescreen-terrain-repeat] scene=%d source=%d "
                "clones=%d hidden_state=%d removed=%d output=%d\n",
                scene, count, clones, hidden_state, removed, out);
        }
    }
}

// Some stages add their color grading as a frozen, top-layer actor whose
// graphic is exactly the original viewport. In expanded mode that becomes a
// dark 4:3 box over an otherwise-correct widescreen background. Preserve the
// effect in original-mode cinematics, but omit only this actor signature from
// the gameplay draw list; scene art and ordinary actors stay untouched.
extern "C" void mm_ws_filter_fixed_viewport_effects(
    uint8_t* rdram, recomp_context* ctx) {
    const uint32_t list = static_cast<uint32_t>(ctx->r4);
    if (!g_gameplay_wide_active || list != kActorsTop) {
        return;
    }

    const gpr filtered = rdram_gpr(kFilteredActorList);
    const gpr actors = rdram_gpr(kActors);
    int out = 0;
    int removed = 0;
    for (int slot = 0; slot < 240; ++slot) {
        const int index = static_cast<int16_t>(MEM_HU(
            slot * 2, rdram_gpr(list)));
        if (index < 0) {
            break;
        }

        const gpr actor = actors + index * 0x198;
        const uint32_t flags = MEM_W(0x80, actor);
        const bool fixed_viewport_effect =
            MEM_HU(0xD2, actor) == 0x2700 && (flags & (1u << 3)) != 0;
        if (fixed_viewport_effect) {
            ++removed;
        }
        else {
            MEM_H(out * 2, filtered) = index;
            ++out;
        }
    }

    if (removed != 0) {
        MEM_H(out * 2, filtered) = -1;
        ctx->r4 = filtered;
    }
}

// func_8000EA88 draws 66 PortraitStruct records. Entries 0..63 form an 8x8
// fixed-viewport death/room-transition wipe; entries 64/65 are the actual HUD
// portraits. Let the transition controller run, but begin the render loop at
// the HUD entries in expanded gameplay so its 320x240 checker cannot punch
// black holes through only the center of a widescreen frame.
extern "C" int mm_ws_portrait_draw_start(uint8_t* rdram) {
    (void)rdram;
    if (g_gameplay_wide_active &&
        !env_enabled("MM_TEST_KEEP_FIXED_VIEWPORT_EFFECTS")) {
        return 0x40;
    }
    return 0;
}

// Scrolling band (func_80082820 entry; sole live caller passes D_80180D90,
// only when D_800BE6FC != 0). Per-row parallax X from D_8011D3B0[row]; rows
// whose scroll is the -1 sentinel are skipped by the draw loop itself.
extern "C" void mm_ws_band_repack(uint8_t* rdram, recomp_context* ctx) {
    ctx->r4 = static_cast<gpr>(static_cast<int32_t>(
        remap_arena(static_cast<uint32_t>(ctx->r4))));
    if (static_cast<uint32_t>(ctx->r5) != kBandBuffer) {
        return;
    }

    WingContext wc{rdram, static_cast<gpr>(ctx->r5), rdram_gpr(kBandScratch),
        static_cast<uint32_t>(MEM_W(0, rdram_gpr(kTexturePoolPtr)))};
    const int scene = static_cast<int16_t>(
        MEM_HU(0, rdram_gpr(kCurrentScene)));
    const bool captured = g_layer_fill_state.valid &&
        g_layer_fill_state.scene == scene;
    const gpr map = captured
        ? rdram_gpr(g_layer_fill_state.backdrop_map)
        : MEM_W(0, rdram_gpr(kBackdropMapPtr));
    const bool wings = wings_active(rdram, "MM_BAND_WINGS") &&
        valid_rdram_ptr(static_cast<uint32_t>(map));
    const int addend = captured
        ? g_layer_fill_state.backdrop_addend
        : MEM_HU(0, rdram_gpr(kBackdropAddend));
    const int y0 = captured
        ? g_layer_fill_state.backdrop_y0
        : ((-12 - read_s16(rdram, kBackdropScrollY)) & 0x1FF);

    const auto band_slot = [&](int row, int col) -> int32_t {
        const int row_block = ((y0 + row * 32) >> 1) & 0xF0;
        const int x0 = captured
            ? g_layer_fill_state.band_x0[row]
            : ((read_s16(rdram, kBandRowScroll + row * 4) - 2) & 0x1FF);
        const int map_col = ((x0 + col * 32 + 512) >> 5) & 0xF;
        const int tile = MEM_BU(map_col + row_block, map);
        return wing_slot(rdram, tile, addend, wc.texture_pool);
    };
    repack_grid(wc, band_slot, wings);
    if (env_enabled("MM_TEST_AUTO_ADVANCE")) {
        int mismatches = 0;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 10; ++col) {
                if (MEM_W((row * 10 + col) * 4, wc.src) !=
                    band_slot(row, col)) {
                    ++mismatches;
                }
            }
        }
        static int audited_scene = -1;
        static int previous_mismatches = -1;
        if (scene != audited_scene || mismatches != previous_mismatches) {
            audited_scene = scene;
            previous_mismatches = mismatches;
            std::fprintf(stderr,
                "[widescreen-formula] scene=%d layer=band mismatches=%d/70\n",
                scene, mismatches);
        }
    }
    trace_layer_once(rdram, "band", wc.dst);
    ctx->r5 = wc.dst;
}

// Shared static tile-grid draw (func_80082380 entry). Serves the midground,
// environment, and non-scrolling background grids; every caller passes a
// 7x10 stride-10 grid, so ALWAYS repack to stride 20 (the draw loop is
// statically widened). Known layers get real map-derived wing columns using
// the exact fill formulas from the game's func_8001107C; anything else gets
// zeroed wings and a byte-identical center.
extern "C" void mm_ws_static_repack(uint8_t* rdram, recomp_context* ctx) {
    ctx->r4 = static_cast<gpr>(static_cast<int32_t>(
        remap_arena(static_cast<uint32_t>(ctx->r4))));
    const uint32_t buffer = static_cast<uint32_t>(ctx->r7);
    WingContext wc{rdram, static_cast<gpr>(ctx->r7), rdram_gpr(kGenericScratch),
        static_cast<uint32_t>(MEM_W(0, rdram_gpr(kTexturePoolPtr)))};
    const int scene = static_cast<int16_t>(
        MEM_HU(0, rdram_gpr(kCurrentScene)));
    const bool captured = g_layer_fill_state.valid &&
        g_layer_fill_state.scene == scene;

    if (buffer == kEnvBuffer) {
        wc.dst = rdram_gpr(kEnvScratch);
        const gpr map = captured
            ? rdram_gpr(g_layer_fill_state.env_map)
            : MEM_W(0, rdram_gpr(kEnvMapPtr));
        const bool wings = wings_active(rdram, "MM_ENV_WINGS") &&
            valid_rdram_ptr(static_cast<uint32_t>(map));
        const int addend = captured
            ? g_layer_fill_state.env_addend
            : MEM_HU(0, rdram_gpr(kEnvAddend));
        const bool wide = captured
            ? g_layer_fill_state.env_wide
            : MEM_HU(0, rdram_gpr(kEnvMode)) == 1; // 128x8 map
        const int x0 = captured
            ? g_layer_fill_state.env_x0
            : ((read_s16(rdram, kEnvScrollX) - 2) &
                (wide ? 0xFFF : 0x1FF));
        const int y0 = captured
            ? g_layer_fill_state.env_y0
            : ((-12 - read_s16(rdram, kEnvScrollY)) &
                (wide ? 0xFF : 0x1FF));

        const auto env_slot = [&](int row, int col) -> int32_t {
            const int y = y0 + row * 32;
            const int row_block = wide ? ((y << 2) & 0x380) : ((y >> 1) & 0xF0);
            const int map_col = ((x0 + col * 32 + 0x1000) >> 5) & (wide ? 0x7F : 0xF);
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        };
        repack_grid(wc, env_slot, wings);
        if (env_enabled("MM_TEST_LAYER_TRACE")) {
            const int draw_x = static_cast<int32_t>(ctx->r5);
            const int draw_y = static_cast<int32_t>(ctx->r6);
            static int previous_scene = -32768;
            static int previous_x0 = -1;
            static int previous_y0 = -1;
            static int previous_draw_x = 0x7FFFFFFF;
            static int previous_draw_y = 0x7FFFFFFF;
            if (scene != previous_scene || x0 != previous_x0 ||
                y0 != previous_y0 || draw_x != previous_draw_x ||
                draw_y != previous_draw_y) {
                const int cam_x = static_cast<int32_t>(
                    MEM_W(0, rdram_gpr(kCamX))) >> 16;
                const int cam_y = static_cast<int32_t>(
                    MEM_W(0, rdram_gpr(kCamY))) >> 16;
                std::fprintf(stderr,
                    "[widescreen-env-grid] scene=%d cam=%d,%d "
                    "origin=%d,%d draw=%d,%d wide=%d",
                    scene, cam_x, cam_y, x0, y0, draw_x, draw_y,
                    wide ? 1 : 0);
                for (int row = 0; row < 7; ++row) {
                    std::fprintf(stderr, " r%d=", row);
                    const int y = y0 + row * 32;
                    const int row_block = wide
                        ? ((y << 2) & 0x380)
                        : ((y >> 1) & 0xF0);
                    for (int col = 8; col <= 14; ++col) {
                        const int map_col =
                            ((x0 + col * 32 + 0x1000) >> 5) &
                            (wide ? 0x7F : 0xF);
                        std::fprintf(stderr, "%s%02x",
                            col == 8 ? "" : ",",
                            MEM_BU(map_col + row_block, map));
                    }
                }
                std::fprintf(stderr, "\n");
                previous_scene = scene;
                previous_x0 = x0;
                previous_y0 = y0;
                previous_draw_x = draw_x;
                previous_draw_y = draw_y;
            }
        }
        if (env_enabled("MM_TEST_AUTO_ADVANCE")) {
            int mismatches = 0;
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 10; ++col) {
                    if (MEM_W((row * 10 + col) * 4, wc.src) !=
                        env_slot(row, col)) {
                        ++mismatches;
                    }
                }
            }
            static int audited_scene = -1;
            static int previous_mismatches = -1;
            if (scene != audited_scene || mismatches != previous_mismatches) {
                audited_scene = scene;
                previous_mismatches = mismatches;
                std::fprintf(stderr,
                    "[widescreen-formula] scene=%d layer=env mismatches=%d/70\n",
                    scene, mismatches);
            }
        }
    }
    else if (buffer == kMidBuffer) {
        wc.dst = rdram_gpr(kMidScratch);
        const bool wings = wings_active(rdram, "MM_MID_WINGS");
        const gpr map = rdram_gpr(kMidMap);
        const int addend = captured
            ? g_layer_fill_state.mid_addend
            : MEM_HU(0, rdram_gpr(kMidAddend));
        const int col_mask = captured
            ? g_layer_fill_state.mid_col_mask
            : MEM_HU(0, rdram_gpr(kMidColMask));
        const int row_mask = captured
            ? g_layer_fill_state.mid_row_mask
            : MEM_HU(0, rdram_gpr(kMidRowMask));
        const int shift = captured
            ? g_layer_fill_state.mid_shift
            : (MEM_HU(0, rdram_gpr(kMidShift)) & 0x1F);
        uint16_t x0;
        uint16_t y0;
        if (captured) {
            x0 = g_layer_fill_state.mid_x0;
            y0 = g_layer_fill_state.mid_y0;
        }
        else {
            compute_mid_fill_bases(rdram, x0, y0);
        }

        const auto mid_slot = [&](int row, int col) -> int32_t {
            // The game's fill writes rows in DESCENDING memory order (fill
            // row rf lands at memory row 6-rf), stepping y down 32 per fill
            // row; so memory/draw row r sits at y0 - (6-r)*32.
            const uint32_t y = static_cast<uint16_t>(
                y0 - (6 - row) * 32);
            const int row_block = static_cast<int>((y << shift) &
                static_cast<uint32_t>(row_mask));
            const int map_col = ((static_cast<uint16_t>(
                x0 + col * 32)) >> 5) & col_mask;
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        };
        repack_grid(wc, mid_slot, wings);
        if (env_enabled("MM_TEST_AUTO_ADVANCE")) {
            int mismatches = 0;
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 10; ++col) {
                    if (MEM_W((row * 10 + col) * 4, wc.src) !=
                        mid_slot(row, col)) {
                        ++mismatches;
                    }
                }
            }
            static int audited_scene = -1;
            static int previous_mismatches = -1;
            if (scene != audited_scene || mismatches != previous_mismatches) {
                audited_scene = scene;
                previous_mismatches = mismatches;
                std::fprintf(stderr,
                    "[widescreen-formula] scene=%d layer=mid mismatches=%d/70\n",
                    scene, mismatches);
            }
        }
    }
    else if (buffer == kBandBuffer) {
        // Non-scrolling background variant (D_800BE6FC == 0): same 16x16
        // wrapping map as the band, single scroll register.
        const gpr map = captured
            ? rdram_gpr(g_layer_fill_state.backdrop_map)
            : MEM_W(0, rdram_gpr(kBackdropMapPtr));
        const bool wings = wings_active(rdram, "MM_STATIC_WINGS") &&
            valid_rdram_ptr(static_cast<uint32_t>(map));
        const int addend = captured
            ? g_layer_fill_state.backdrop_addend
            : MEM_HU(0, rdram_gpr(kBackdropAddend));
        const int x0 = captured
            ? g_layer_fill_state.backdrop_x0
            : ((read_s16(rdram, kBackdropScrollX) - 2) & 0x1FF);
        const int y0 = captured
            ? g_layer_fill_state.backdrop_y0
            : ((-12 - read_s16(rdram, kBackdropScrollY)) & 0x1FF);

        const auto static_slot = [&](int row, int col) -> int32_t {
            const int row_block = ((y0 + row * 32) >> 1) & 0xF0;
            const int map_col = ((x0 + col * 32 + 512) >> 5) & 0xF;
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        };
        repack_grid(wc, static_slot, wings);
        if (env_enabled("MM_TEST_AUTO_ADVANCE")) {
            int mismatches = 0;
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 10; ++col) {
                    if (MEM_W((row * 10 + col) * 4, wc.src) !=
                        static_slot(row, col)) {
                        ++mismatches;
                    }
                }
            }
            static int audited_scene = -1;
            static int previous_mismatches = -1;
            if (scene != audited_scene || mismatches != previous_mismatches) {
                audited_scene = scene;
                previous_mismatches = mismatches;
                std::fprintf(stderr,
                    "[widescreen-formula] scene=%d layer=static mismatches=%d/70\n",
                    scene, mismatches);
            }
        }
    }
    else {
        // Unknown caller: center copy only, zero wings.
        repack_grid(wc, [](int, int) -> int32_t { return 0; }, false);
    }
    trace_layer_once(rdram,
        buffer == kEnvBuffer ? "env" :
        buffer == kMidBuffer ? "mid" :
        buffer == kBandBuffer ? "static" : "unknown",
        wc.dst);
    ctx->r7 = wc.dst;
}

// func_80026428 decompresses a replacement map bank over 0x80380000 for
// scenes 10, 24 and 46..51 but never updates the recorded bank extent, so
// the residency check above needs the union of the replacement prefix and
// the still-resident authoritative bank tail. Hooked right after its
// Trouble_RLE_Type1 call (before_vram 0x80026484), where $v0 still holds the
// decompressed size. Host-side only; game memory is never written.
extern "C" void mm_ws_fix_bank_bound(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = static_cast<uint32_t>(ctx->r2);
    if (size == 0 || size > 0x40000u) {
        return;
    }
    const uint32_t authoritative_end = static_cast<uint32_t>(
        MEM_W(0, rdram_gpr(kBankEnd)));
    const uint32_t replacement_end = kMapBankDest + size;
    g_host_bank_end = authoritative_end > replacement_end
        ? authoritative_end : replacement_end;
}

// func_80026220 is the authoritative bank loader (records the extent at
// 0x80137724/28 itself); a fresh load supersedes any effective union from an
// earlier scene.
extern "C" void mm_ws_bank_reloaded(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    g_host_bank_end = 0;
}
