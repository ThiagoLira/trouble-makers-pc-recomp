#include "presentation.h"

#include <array>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "presentation: %s\n", message);
        ++failures;
    }
}

int x(uint32_t word) {
    int value = (word >> 12) & 0xFFF;
    return value >= 0x800 ? value - 0x1000 : value;
}

void tile(uint32_t (&words)[6], int left, bool copy) {
    words[0] = 0xE4000000u | (((left + 32 - int(copy)) * 4 & 0xFFFu) << 12) | 252;
    words[1] = ((left * 4 & 0xFFFu) << 12) | 128;
    words[2] = 0xB4000000u;
    words[3] = 96; // Nonzero T must survive.
    words[4] = 0xB3000000u;
    words[5] = ((copy ? 4096u : 1024u) << 16) | 1024;
}
}

int main() {
    using namespace mm::presentation;
    for (int scene : {0, 31, 35, 36, 69}) {
        check(suppress_interpolation(scene, 12), "stage return and selection use native frames");
        check(suppress_interpolation(scene, 14), "records use native frames");
        check(suppress_interpolation(scene, 6) == (scene == 35), "gameplay restores user rate except Missile Surf");
        check(!suppress_interpolation(scene, 5), "loading restores user rate");
    }

    // 16:10, 16:9, and 21:9. Exercise signed left coordinates, both cycle
    // modes, clipping at both authored edges, and all internal tile joins.
    for (float aspect : {1.6f, 16.0f / 9.0f, 21.0f / 9.0f}) {
        const float scale = (aspect / (4.0f / 3.0f)) * 320.0f / 288.0f;
        for (bool copy : {false, true}) {
            int previous_right = 0;
            for (int col = 0; col < 10; ++col) {
                uint32_t words[6];
                tile(words, col * 32, copy);
                stretch_backdrop_rectangle(words, scale);
                check((words[0] >> 24) == 0xE4 && words[2] == 0xB4000000u &&
                    words[4] == 0xB3000000u, "Fast3D opcodes survive");
                check((words[0] & 0xFFF) == 252 && (words[1] & 0xFFF) == 128 &&
                    (words[3] & 0xFFFF) == 96 && (words[5] & 0xFFFF) == 1024,
                    "vertical geometry and texture coordinates survive");
                if (col == 0) {
                    check(x(words[1]) == std::lround(640 - aspect * 480), "panorama covers left window edge");
                    check((words[3] >> 16) == 14 * 32, "left crop advances texture S");
                }
                else {
                    check(x(words[1]) == previous_right, "adjacent tiles share a transformed edge");
                }
                previous_right = x(words[0]) + (copy ? 4 : 0);
                if (col == 9) {
                    check(previous_right == std::lround(640 + aspect * 480), "panorama covers right window edge");
                }
                const float step = float(words[5] >> 16) / (copy ? 4096 : 1024);
                check(std::abs(step * scale - 1.0f) < 0.002f, "UV step compensates for horizontal stretching");
            }
            uint32_t outside[6];
            tile(outside, 320, copy);
            stretch_backdrop_rectangle(outside, scale);
            check(x(outside[0]) + (copy ? 3 : 0) < x(outside[1]), "filler outside authored canvas is empty");
        }
    }
    uint32_t original[6];
    tile(original, -16, false);
    const auto saved = std::to_array(original);
    stretch_backdrop_rectangle(original, 0.0f);
    check(std::to_array(original) == saved, "disabled stretch is byte-identical");
    return failures ? 1 : 0;
}
