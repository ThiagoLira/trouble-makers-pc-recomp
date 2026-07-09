// src/game/register_overlays.cpp — Phase 2 worker w1 (game core).
//
// Registers the recompiled game's overlay/section table with librecomp so the
// runtime can resolve N64 overlay loads (load_overlays/unload_overlays) to the
// correct host-side recompiled functions.
//
// The section/overlay data is emitted by N64Recomp into
// RecompiledFuncs/recomp_overlays.inl (section_table[], num_sections=177,
// overlay_sections_by_index[]). That .inl is NOT compiled directly by the
// mm_recompiled library — it is #included here, exactly once, by the TU that
// feeds it to recomp::overlays::register_overlays(). Modeled on
// reference/Zelda64Recomp/src/main/register_overlays.cpp.

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "../../RecompiledFuncs/recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "mm_audio_input.hpp"

// Set by ultramodern::quit() (librecomp/src/recomp.cpp) and polled by the
// runtime's own event threads. We poll it from the AI_LEN mirror thread for the
// same reason: rdram is munmap'd only after the event threads join — i.e. only
// after `exited` has been observed — so stopping the mirror on `exited`
// guarantees it never writes to freed rdram during teardown.
extern std::atomic_bool exited;

namespace {

// Host-side replacement for rmonMain, the N64 debug-monitor thread entry.
// rmonMain is in N64Recomp's ignored_funcs (rmon is never recompiled), but
// this game's boot code (decomp src/boot.c) passes it to osCreateThread, so
// the runtime resolves it BY ADDRESS when the rmon thread first runs — with
// no entry registered, get_function() aborts. On retail hardware the rmon
// thread parks forever waiting for debugger serial traffic that never
// arrives; returning immediately is equivalent (the host thread exits
// cleanly). Address from the decomp: symbol_addrs_libultra.txt,
// rmonMain = 0x8009A2B8. This is the only ignored function the game's own
// code uses as a thread entry (pimgr/vimgr spawn theirs inside natively
// wrapped osCreatePiManager/osCreateViManager, which never execute).
//
// Must be registered from GameEntry::on_init_callback, NOT from main():
// librecomp's init() calls init_overlays() when the game starts, which
// func_map.clear()s anything registered earlier.
constexpr int32_t kRmonMainVram = 0x8009A2B8;

void rmonMain_host(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    std::fprintf(stderr, "[rmon] rmon thread started; idling (host no-op).\n");
}

} // namespace

namespace troublemakers {

void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}

void on_game_init(uint8_t* rdram, recomp_context* /*ctx*/) {
    recomp::overlays::add_loaded_function(kRmonMainVram, rmonMain_host);

    // Sound_Update (decomp music.c, asm at 0x800023F0) reads the AI hardware
    // registers directly (`lui $t9,0xA450; lw $t1,4($t9)` = AI_LEN_REG at
    // 0xA4500004) instead of calling osAiGetLength, then `srl $t2,$t1,2` ->
    // gAudioSamplesLeft: it divides the byte count by 4 to get remaining stereo
    // frames. This is the game's ONLY raw MMIO touch — every other D_A4*
    // access in the decomp lives in libultra functions librecomp replaces
    // wholesale.
    //
    // librecomp maps N64 addresses flat (addr - 0x80000000), so that `lw` hits
    // rdram offset 0x24500004 — beyond the committed mem_size but inside
    // librecomp's PROT_NONE reservation. Commit the AI register page and mirror
    // a *real* AI_LEN into it. On hardware AI_LEN = bytes remaining in the
    // in-flight AI DMA; the game uses it to pace audio (it backs off until the
    // DAC drains). With the page merely zero-filled, AI_LEN always reads 0
    // ("drained") and the game's pacing logic free-runs.
    //
    // MEM_W (recomp.h) is a plain aligned host word load — no byte swizzle for
    // word accesses (only MEM_B/MEM_H carry the ^3/^2 XOR), so writing a plain
    // little-endian u32 to rdram+0x24500004 is read back verbatim by the
    // recompiled `lw`. We mirror ultramodern's view of the output buffer:
    // get_frames_remaining() is in stereo frames at the game's sample rate, and
    // *4 = bytes (2 channels * sizeof(s16)), which is exactly AI_LEN's unit and
    // what the game's `srl 2` turns back into frames — so gAudioSamplesLeft
    // tracks the real DAC queue depth instead of always reading zero.
    constexpr uint32_t kAiRegsVaddr = 0xA4500000u;
    void* ai_page = rdram + (kAiRegsVaddr - 0x80000000u);
    if (mprotect(ai_page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        std::perror("[recomp] mprotect(AI register page)");
        return;
    }

    // 0xA4500004 - 0x80000000 = 0x24500004, which is 4-byte aligned, so a plain
    // aligned word store is valid (no swizzle crossing). volatile: this is a
    // memory-mapped register mirror — the store must reach rdram every tick
    // even under -O2 (the value read from get_frames_remaining is opaque to the
    // compiler, but volatile documents and guarantees the MMIO store semantic).
    constexpr uintptr_t kAiLenOffset = 0xA4500004u - 0x80000000u;
    volatile uint32_t* ai_len = reinterpret_cast<volatile uint32_t*>(rdram + kAiLenOffset);

    // ~1ms cadence: the DAC drains continuously and the game samples AI_LEN
    // once per Sound_Update, so 1ms keeps the value fresh without busy-burning
    // a core. Self-terminates on `exited` (see the extern above) so it never
    // outlives rdram. Detached: the process either runs until ultramodern::quit
    // (thread observes `exited` and returns) or is killed/aborts (thread dies
    // with the process).
    std::thread([ai_len]() {
        while (!exited.load(std::memory_order_relaxed)) {
            *ai_len = static_cast<uint32_t>(mm_audio_input::get_frames_remaining()) * 4u;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    // ========================================================================
    // PHASE 6 (lane a) — MM_WIDE widescreen-cull experiment.
    //
    // Problem: with --widescreen (RT64 AspectRatio::Expand) the render field
    // widens, but the game only paints/culls its 4:3 region. Sprites/scenery
    // that cross the 4:3 edge get drawn once (while straddling the edge) and
    // then freeze in the side "wings" — they stop being redrawn once fully
    // past the edge, so stale framebuffer accumulates. Evidence the game does
    // NOT hard-scissor: it draws past x=320 (the frozen pixels prove that),
    // it just stops *updating* there.
    //
    // The lever (found by decomp spelunking, see PHASE6_NOTES_a.md): the game
    // computes a per-frame view cull rectangle in FixedCoord (Q16.16) and every
    // actor/sprite draw function culls against it:
    //   D_800BE568 = LEFT  bound  = gScreenPosCurrentX.whole - 0x90  (0x90 = 144)
    //   D_800BE56C = RIGHT bound  = gScreenPosCurrentX.whole + 0x90
    //   D_800BE570 = TOP   bound  = gScreenPosCurrentY.whole - 0x70  (0x70 = 112)
    //   D_800BE574 = BOTTOM bound = gScreenPosCurrentY.whole + 0x70
    // (Camera_UpdateViewBounds @ 0x800462F0 + func_800463C0 in decomp
    // src/438E0.c:290. Overlay readers, e.g. func_8019B314, do
    // `slt actorX, D_800BE568; beqz skip` and `slt D_800BE56C, actorX; beqz
    // skip` — a straight draw-cull test.) The half-extents 0x90/0x70 are
    // baked as addiu immediates, so the lever is the OUTPUT variables: widen
    // them at runtime and actors past the 4:3 edge stop being culled and get
    // redrawn live into the wings.
    //
    // The game overwrites these bounds every frame (Camera_UpdateViewBounds
    // runs in the per-frame camera update), so a one-shot poke won't persist.
    // Mirror the AI_LEN pattern: a detached ~1ms host thread recomputes them
    // from the LIVE camera position with a widened half-extent. At 60 fps
    // (16 ms/frame) several pokes land inside each frame's draw window, so the
    // widened bound reaches the cull test. Self-terminates on `exited` (rdram
    // is munmap'd only after the event threads join, same teardown safety as
    // the AI_LEN mirror).
    //
    // Gate: env MM_WIDE=1 enables. MM_WIDE_HALF (default 0x100=256) sets the
    // horizontal half-extent in pixels (game default 0x90=144; screen half is
    // 0xA0=160, so anything > 0xA0 fills the 4:3 edge + wings). MM_WIDE_Y_HALF
    // (default 0 = leave Y to the game) optionally widens the vertical bounds.
    const char* wide_env = getenv("MM_WIDE");
    if (wide_env != nullptr && wide_env[0] != '0') {
        // rdram offsets = vaddr - 0x80000000. All in committed .data, aligned.
        constexpr uintptr_t kCamXOff = 0x800BE558u - 0x80000000u; // gScreenPosCurrentX
        constexpr uintptr_t kCamYOff = 0x800BE55Cu - 0x80000000u; // gScreenPosCurrentY
        constexpr uintptr_t kLeftOff = 0x800BE568u - 0x80000000u; // D_800BE568
        constexpr uintptr_t kRightOff = 0x800BE56Cu - 0x80000000u; // D_800BE56C
        constexpr uintptr_t kTopOff = 0x800BE570u - 0x80000000u; // D_800BE570
        constexpr uintptr_t kBotOff = 0x800BE574u - 0x80000000u; // D_800BE574

        const char* half_env = getenv("MM_WIDE_HALF");
        int half = half_env ? std::atoi(half_env) : 0x100; // 256 px; > 0xA0 screen half
        if (half < 0xA0) half = 0xA0; // never narrower than the screen half
        const char* yhalf_env = getenv("MM_WIDE_Y_HALF");
        int yhalf = yhalf_env ? std::atoi(yhalf_env) : 0; // 0 = leave vertical untouched

        std::fprintf(stderr, "[mm_wide] on: half=%dpx (game 0x90=144), y_half=%dpx\n",
                     half, yhalf);

        // FixedCoord is Q16.16: raw s32, .whole (integer) = bits 16..31. The
        // cull test reads .whole via a sign-extended halfword load, so only the
        // integer part matters; we rewrite .whole and keep the low 16 (frac).
        // MEM_W is a plain aligned host word load (no swizzle), so a host s32
        // store is read back verbatim by the recompiled lw/sh. volatile: these
        // are game-visible cull registers; the store must issue every tick.
        auto write_whole = [](volatile int32_t* p, int32_t whole) {
            *p = (*p & 0x0000FFFF) | (static_cast<uint32_t>(static_cast<int16_t>(whole)) << 16);
        };
        auto read_whole = [](volatile const int32_t* p) -> int16_t {
            return static_cast<int16_t>((*p >> 16) & 0xFFFF);
        };

        std::thread([=]() {
            while (!exited.load(std::memory_order_relaxed)) {
                volatile int32_t* camX = reinterpret_cast<volatile int32_t*>(rdram + kCamXOff);
                volatile int32_t* left = reinterpret_cast<volatile int32_t*>(rdram + kLeftOff);
                volatile int32_t* right = reinterpret_cast<volatile int32_t*>(rdram + kRightOff);
                int16_t cx = read_whole(camX); // camera center, integer px
                write_whole(left,  static_cast<int32_t>(cx) - half);
                write_whole(right, static_cast<int32_t>(cx) + half);
                if (yhalf > 0) {
                    volatile int32_t* camY = reinterpret_cast<volatile int32_t*>(rdram + kCamYOff);
                    volatile int32_t* top = reinterpret_cast<volatile int32_t*>(rdram + kTopOff);
                    volatile int32_t* bot = reinterpret_cast<volatile int32_t*>(rdram + kBotOff);
                    int16_t cy = read_whole(camY);
                    write_whole(top, static_cast<int32_t>(cy) - yhalf);
                    write_whole(bot, static_cast<int32_t>(cy) + yhalf);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}

} // namespace troublemakers
