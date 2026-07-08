// mm_runtime_probe — link glue for the 2 libultra OS-function wrappers that
// librecomp does not yet implement. These are host-side stubs for the probe
// only (RecompiledFuncs is never hand-edited). Phase 2 should implement them
// properly inside librecomp (alongside ultra_stubs.cpp / ultra_translation.cpp):
//   - rmonPrintf_recomp : N64 rmon (runtime monitor / debug) printf. No-op here.
//   - __osGetCause_recomp : read MIPS CP0 Cause register (exception code + IP
//     interrupt-pending bits). Returns 0 (no exception, no pending IRQ) — a
//     safe placeholder so interrupt-polling code in the game doesn't fault.
// Return-value convention for _recomp wrappers is ctx->r2 (= MIPS $v0).
#include "recomp.h"

extern "C" void rmonPrintf_recomp(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {
    // Intentionally no-op: debug monitor output is non-essential for linking.
}

extern "C" void __osGetCause_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0; // Pretend no exception is pending and no IRQ bits are set.
}
