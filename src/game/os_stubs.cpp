// src/game/os_stubs.cpp — Phase 2 worker w1 (game core).
//
// Real host-side implementations of the two libultra OS-function wrappers that
// librecomp does NOT provide and that the translated game references (see
// PHASE1_NOTES.md "Genuine link-time gaps: exactly 2"). These are linked into
// the troublemakers executable (never hand-edited into RecompiledFuncs/).
//
// Standard wrapper signature (recomp.h): extern "C" void name(uint8_t* rdram,
// recomp_context* ctx). MIPS calling convention: integer args in $a0..$a3 =
// ctx->r4..r7, further args on the N64 stack; return value in $v0 = ctx->r2.

#include <cstdint>
#include <cstdio>

#include "recomp.h"

namespace {
// RDRAM is mapped at N64 virtual address 0x80000000. Size is 8 MiB (the
// allocation/mem_size used by ultramodern). Used only to bounds-check pointers
// handed to us by the game before dereferencing them on the host side.
constexpr uint32_t kRdramBase = 0x80000000u;
constexpr uint32_t kRdramSize = 0x00800000u; // 8 MiB

// Read one byte from an N64 virtual address, endianness-xored exactly like the
// recomp MEM_BU macro, with a bounds check. Returns false if out of range.
inline bool rdram_byte(uint8_t* rdram, uint32_t vaddr, uint8_t& out) {
    uint32_t off = vaddr - kRdramBase;
    if (off >= kRdramSize) {
        return false;
    }
    // N64 is big-endian; rdram is byte-swapped per-word (the ^3 in MEM_BU).
    out = rdram[(off ^ 3)];
    return true;
}
} // namespace

// rmonPrintf_recomp — N64 rmon (runtime monitor / debug) printf.
//
// The N64-side prototype is variadic: `void rmonPrintf(const char* fmt, ...)`.
// The recompiler maps calls to it onto this wrapper, with the format-string
// pointer in $a0 (ctx->r4) and subsequent arguments in $a1.. and on the N64
// stack. Properly decoding MIPS varargs through a host printf is fragile:
//   - %s arguments are N64 rdram pointers (would need rdram dereference),
//   - %d/%x arguments live in ctx->r5..r7 then the N64 stack at ctx->r29+,
//   - and the exact layout depends on the format specifier sequence.
// rmon output is debug-only (the rmon is not active in a retail boot), so we
// take the feasible middle ground: read the format string from rdram and
// forward it VERBATIM to host stderr, without decoding varargs. This surfaces
// the game's debug text (useful during bring-up) while staying safe. A future
// pass can implement full varargs decoding if a boot path actually depends on
// the formatted output.
extern "C" void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t fmt_addr = static_cast<uint32_t>(static_cast<int32_t>(ctx->r4));
    if (fmt_addr == 0) {
        return;
    }

    // Read the format string up to a null terminator or a sane cap, then print
    // it raw. (Printed verbatim, so any %s/%d placeholders pass through
    // untouched — we are NOT calling host printf with game-controlled args,
    // which would be both wrong and a format-string hazard.)
    char buf[1024];
    uint32_t len = 0;
    constexpr uint32_t kMaxLen = sizeof(buf) - 1;
    uint8_t ch = 0;
    while (len < kMaxLen && rdram_byte(rdram, fmt_addr + len, ch) && ch != 0) {
        buf[len] = static_cast<char>(ch);
        ++len;
    }
    buf[len] = '\0';

    if (len != 0) {
        std::fputs("[rmon] ", stderr);
        std::fputs(buf, stderr);
        std::fputc('\n', stderr);
    }
}

// ---------------------------------------------------------------------------
// Link-satisfaction stubs for N64Recomp-ignored functions called from the
// game's TRANSLATED rmon (debug monitor) code. The manual_funcs entries in
// troublemakers.us1.toml translate the rmon statics the ELF lost (they are
// jal-reachable from rmon code the recompiler already emitted), and those
// bodies call these OS/rmon wrappers, which N64Recomp never generates for
// ignored functions. None of this can execute: the rmon thread entry is a
// host no-op (register_overlays.cpp), so every stub is loud — if one ever
// fires, an assumption broke and we want to know immediately.
// ---------------------------------------------------------------------------
#define MM_UNREACHABLE_OS_STUB(name)                                          \
    extern "C" void name##_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {  \
        std::fprintf(stderr,                                                  \
            "[recomp] " #name "_recomp called — rmon debug path executed "    \
            "unexpectedly (thought unreachable). Returning 0.\n");            \
        ctx->r2 = 0;                                                          \
    }

MM_UNREACHABLE_OS_STUB(__osEnqueueThread)
MM_UNREACHABLE_OS_STUB(__osPopThread)
MM_UNREACHABLE_OS_STUB(__rmonGetExceptionStatus)
MM_UNREACHABLE_OS_STUB(__rmonGetThreadStatus)
MM_UNREACHABLE_OS_STUB(__rmonRCPrunning)
MM_UNREACHABLE_OS_STUB(__rmonReadWordAt)
MM_UNREACHABLE_OS_STUB(__rmonSendFault)
MM_UNREACHABLE_OS_STUB(__rmonSendReply)
MM_UNREACHABLE_OS_STUB(__rmonStepRCP)
MM_UNREACHABLE_OS_STUB(__rmonWriteWordTo)

// __osGetCause_recomp — read the MIPS CP0 `Cause` register.
//
// The Cause register holds the exception code (ExcCode, bits 2..6) and the
// interrupt-pending bits (IP, bits 8..15) among other fields. The game polls
// this to check for pending interrupts/exceptions. With no host-side exception
// model, returning 0 means "no exception pending, no IRQ bits set" — a safe
// placeholder so interrupt-polling code does not fault. Return value goes in
// $v0 = ctx->r2.
extern "C" void __osGetCause_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0;
}
