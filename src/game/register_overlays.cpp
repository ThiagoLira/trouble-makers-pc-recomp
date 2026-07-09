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

#include "../../RecompiledFuncs/recomp_overlays.inl"

#include "librecomp/overlays.hpp"

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

    // Sound_Update (decomp music.c, asm at 0x800023E8) reads the AI hardware
    // registers directly (lui 0xA450 / lw 4 = AI_LEN_REG) instead of calling
    // osAiGetLength. librecomp maps N64 addresses flat (addr - 0x80000000),
    // so that load hits rdram offset 0x24500000 — beyond the committed 512MB
    // mem_size but inside librecomp's 4GB PROT_NONE reservation. Commit that
    // one page as zero-filled host memory: AI_LEN reads 0 ("DMA drained"),
    // which is a safe bring-up answer, and stray writes are swallowed. This
    // is the game's ONLY raw MMIO touch — every other D_A4* access in the
    // decomp lives in libultra functions librecomp replaces wholesale.
    // TODO(phase3): trap-based MMIO emulation (SIGSEGV handler forwarding to
    // ultramodern's AI state) so AI_LEN reflects real queue depth and audio
    // pacing is correct.
    constexpr uint32_t kAiRegsVaddr = 0xA4500000u;
    void* ai_page = rdram + (kAiRegsVaddr - 0x80000000u);
    if (mprotect(ai_page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        std::perror("[recomp] mprotect(AI register page)");
    }
}

} // namespace troublemakers
