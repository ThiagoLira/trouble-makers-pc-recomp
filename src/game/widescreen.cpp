// Host helpers for translated-code widescreen hooks.

#include <atomic>
#include <cstdint>
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

// Test-only controller driver. The hook runs after the game has copied its
// controller state into Marina's per-frame input words, so it can exercise
// camera scrolling and actor streaming without depending on X11 focus. Move
// right, pause, then left; the neutral gaps also expose stale-frame trails.
extern "C" void mm_test_drive_marina(uint8_t* rdram) {
    static int motion_frame = 0;
    static uint16_t previous_direction = 0;
    if (!env_enabled("MM_TEST_MOVE") ||
        !mm_widescreen_gameplay_active(rdram) ||
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kGameState))) != 6 ||
        MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(kCannotPause))) != 0) {
        motion_frame = 0;
        previous_direction = 0;
        return;
    }

    const int phase = motion_frame++ % 120;
    uint16_t direction = 0;
    if (phase < 15) {
        direction = MEM_HU(0,
            static_cast<gpr>(static_cast<int32_t>(kButtonDRight)));
    }
    else if (phase >= 60 && phase < 75) {
        direction = MEM_HU(0,
            static_cast<gpr>(static_cast<int32_t>(kButtonDLeft)));
    }
    const uint16_t pressed = direction != previous_direction ? direction : 0;
    previous_direction = direction;

    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaHold))) = direction;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kMarinaPress))) = pressed;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonHold))) = direction;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kGlobalButtonPress))) = pressed;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonHoldHistory))) = direction;
    MEM_H(0, static_cast<gpr>(static_cast<int32_t>(kButtonPressHistory))) = pressed;
    if ((motion_frame % 60) == 0) {
        std::fprintf(stderr,
            "[test-move] phase=%d direction=%04x actor-x=%d camera-x=%d\n",
            phase, direction,
            static_cast<int32_t>(MEM_W(0, static_cast<gpr>(
                static_cast<int32_t>(kMarinaActorPosX)))),
            static_cast<int32_t>(MEM_W(0, static_cast<gpr>(
                static_cast<int32_t>(kCamX)))));
    }
}

namespace {

gpr rdram_gpr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

int16_t read_s16(uint8_t* rdram, uint32_t vaddr) {
    return static_cast<int16_t>(MEM_HU(0, rdram_gpr(vaddr)));
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
    const gpr map = MEM_W(0, rdram_gpr(kBackdropMapPtr));
    const bool wings = wings_active(rdram, "MM_BAND_WINGS") &&
        valid_rdram_ptr(static_cast<uint32_t>(map));
    const int addend = MEM_HU(0, rdram_gpr(kBackdropAddend));
    const int y0 = (-12 - read_s16(rdram, kBackdropScrollY)) & 0x1FF;

    const auto band_slot = [&](int row, int col) -> int32_t {
        const int row_block = ((y0 + row * 32) >> 1) & 0xF0;
        const int x0 = (read_s16(rdram, kBandRowScroll + row * 4) - 2) & 0x1FF;
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
        const int scene = static_cast<int16_t>(
            MEM_HU(0, rdram_gpr(kCurrentScene)));
        if (scene != audited_scene) {
            audited_scene = scene;
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

    if (buffer == kEnvBuffer) {
        wc.dst = rdram_gpr(kEnvScratch);
        const gpr map = MEM_W(0, rdram_gpr(kEnvMapPtr));
        const bool wings = wings_active(rdram, "MM_ENV_WINGS") &&
            valid_rdram_ptr(static_cast<uint32_t>(map));
        const int addend = MEM_HU(0, rdram_gpr(kEnvAddend));
        const bool wide = MEM_HU(0, rdram_gpr(kEnvMode)) == 1; // 128x8 map
        const int x0 = (read_s16(rdram, kEnvScrollX) - 2) & (wide ? 0xFFF : 0x1FF);
        const int y0 = (-12 - read_s16(rdram, kEnvScrollY)) & (wide ? 0xFF : 0x1FF);

        const auto env_slot = [&](int row, int col) -> int32_t {
            const int y = y0 + row * 32;
            const int row_block = wide ? ((y << 2) & 0x380) : ((y >> 1) & 0xF0);
            const int map_col = ((x0 + col * 32 + 0x1000) >> 5) & (wide ? 0x7F : 0xF);
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        };
        repack_grid(wc, env_slot, wings);
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
            const int scene = static_cast<int16_t>(
                MEM_HU(0, rdram_gpr(kCurrentScene)));
            if (scene != audited_scene) {
                audited_scene = scene;
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
        const int addend = MEM_HU(0, rdram_gpr(kMidAddend));
        const int mode = MEM_HU(0, rdram_gpr(kMidMode));
        const int col_mask = MEM_HU(0, rdram_gpr(kMidColMask));
        const int row_mask = MEM_HU(0, rdram_gpr(kMidRowMask));
        const int shift = MEM_HU(0, rdram_gpr(kMidShift)) & 0x1F;
        const int map_h = MEM_HU(0, rdram_gpr(kMidMapH));
        const int cam_x = read_s16(rdram, kCamX);
        const int cam_y = read_s16(rdram, kCamY);
        const int shake = read_s16(rdram, kCamShake);

        // X/Y bases per parallax mode, from func_8001107C's switch.
        int x0;
        if (mode == 0) {
            x0 = cam_x - 0x92;
        }
        else if (mode == 1) {
            x0 = static_cast<int>(static_cast<double>(cam_x) / 1.55 - 2.0);
        }
        else {
            x0 = static_cast<int>(static_cast<double>(cam_x) / 1.55);
        }
        const int y0 = (mode == 3)
            ? static_cast<int>(static_cast<double>(map_h) -
                (static_cast<double>(cam_y) / 1.55 + static_cast<double>(shake) - 92.0))
            : (map_h - cam_y - shake + 0x5C);

        repack_grid(wc, [&](int row, int col) -> int32_t {
            // The game's fill writes rows in DESCENDING memory order (fill
            // row rf lands at memory row 6-rf), stepping y down 32 per fill
            // row; so memory/draw row r sits at y0 - (6-r)*32.
            const uint32_t y = static_cast<uint32_t>((y0 - (6 - row) * 32) & 0xFFFF);
            const int row_block = static_cast<int>((y << shift) &
                static_cast<uint32_t>(row_mask));
            const int map_col = (((x0 & 0xFFFF) + col * 32 + 0x10000) >> 5) & col_mask;
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        }, wings);
    }
    else if (buffer == kBandBuffer) {
        // Non-scrolling background variant (D_800BE6FC == 0): same 16x16
        // wrapping map as the band, single scroll register.
        const gpr map = MEM_W(0, rdram_gpr(kBackdropMapPtr));
        const bool wings = wings_active(rdram, "MM_STATIC_WINGS") &&
            valid_rdram_ptr(static_cast<uint32_t>(map));
        const int addend = MEM_HU(0, rdram_gpr(kBackdropAddend));
        const int x0 = (read_s16(rdram, kBackdropScrollX) - 2) & 0x1FF;
        const int y0 = (-12 - read_s16(rdram, kBackdropScrollY)) & 0x1FF;

        repack_grid(wc, [&](int row, int col) -> int32_t {
            const int row_block = ((y0 + row * 32) >> 1) & 0xF0;
            const int map_col = ((x0 + col * 32 + 512) >> 5) & 0xF;
            const int tile = MEM_BU(map_col + row_block, map);
            return wing_slot(rdram, tile, addend, wc.texture_pool);
        }, wings);
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
