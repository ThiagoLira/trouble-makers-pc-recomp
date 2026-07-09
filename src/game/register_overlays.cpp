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
}

} // namespace troublemakers
