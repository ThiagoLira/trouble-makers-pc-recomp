#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mm::presentation {

// State 12 owns stage completion, the map opening, and stage selection;
// state 14 is its adjoining records menu. Scene 35 is Missile Surf.
inline bool suppress_interpolation(int scene, int game_state) {
    return (scene == 35 && game_state == 6) || game_state == 12 || game_state == 14;
}

// Six words comprising E4/B4/B3 in the game's Fast3D tile renderer.
// Input X uses signed 10.2 coordinates, S uses 10.5, dS/dX uses 5.10.
inline void stretch_backdrop_rectangle(uint32_t (&words)[6], float scale) {
    if (scale <= 0.0f) {
        return;
    }
    uint32_t lower = words[0];
    uint32_t upper = words[1];
    const uint32_t derivatives = words[5];
    const int dsdx = derivatives >> 16;
    const bool copy_cycle = dsdx == 4096;
    const auto signed_x = [](uint32_t word) {
        const int x = (word >> 12) & 0xFFF;
        return x >= 0x800 ? x - 0x1000 : x;
    };
    const int original_left = signed_x(upper);
    const int left = std::max(original_left, 14 * 4);
    const int right = std::min(signed_x(lower) + (copy_cycle ? 4 : 0), 302 * 4);
    if (left >= right) {
        // Invert both edges so even inclusive copy-cycle rectangles are empty.
        words[0] = 0xE4000000u;
        words[1] = (4u << 12) | 4u;
        return;
    }
    const auto stretch_x = [scale](int x) {
        return static_cast<int>(std::lround(
            (x - 158 * 4) * scale + 160 * 4));
    };
    const int new_left = stretch_x(left);
    const int new_right = stretch_x(right) - (copy_cycle ? 4 : 0);
    words[0] = (lower & ~0xFFF000u) | ((new_right & 0xFFFu) << 12);
    words[1] = (upper & ~0xFFF000u) | ((new_left & 0xFFFu) << 12);
    const uint32_t st = words[3];
    const int s = static_cast<int16_t>(st >> 16) + (left - original_left) * 8;
    words[3] = (st & 0xFFFFu) | (static_cast<uint32_t>(s) << 16);
    const int new_dsdx = static_cast<int>(std::lround(dsdx / scale));
    words[5] = (derivatives & 0xFFFFu) | (new_dsdx << 16);
}

} // namespace mm::presentation
