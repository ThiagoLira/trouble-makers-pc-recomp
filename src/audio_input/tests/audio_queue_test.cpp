#include "audio_queue.h"

#include <array>
#include <cstdio>
#include <limits>

namespace {

int check(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "audio queue test failed: %s\n", message);
    return 1;
}

// Sound_Update at 0x800022D0..0x80002310: subtract queued frames,
// align to 16, narrow to signed 16-bit, then apply the 352-frame minimum.
int game_request(uint32_t remaining) {
    const uint32_t bits = (368u - remaining + 96u) & 0xFFF0u;
    const int narrowed = bits < 0x8000u ? static_cast<int>(bits)
                                     : static_cast<int>(bits) - 0x10000;
    return std::max(narrowed, 352);
}

} // namespace

int main() {
    using mm_audio_input::queue_policy::remaining_game_frames;
    int result = 0;
    result |= check(game_request(33414) == 32576,
        "reproduce observed unbounded-backlog wraparound");
    result |= check(remaining_game_frames(0, 22050) == 0, "empty queue");
    result |= check(remaining_game_frames(3200, 48000) == 0, "one-VI backoff");
    result |= check(remaining_game_frames(3840, 22050) == 74,
        "normal resampling and backoff remain unchanged");
    result |= check(remaining_game_frames(7680, 48000) == 1120,
        "normal device-rate queue remains unchanged");

    const std::array<uint32_t, 7> rates = {
        0, 22050, 32000, 44100, 48000, 192000,
        std::numeric_limits<uint32_t>::max()};
    const std::array<uint32_t, 8> queues = {
        0, 4, 3840, 47996, 48000, 48004, 294152,
        std::numeric_limits<uint32_t>::max()};
    for (uint32_t rate : rates) {
        for (uint32_t bytes : queues) {
            const uint32_t remaining = remaining_game_frames(bytes, rate);
            result |= check(remaining <= 0x7FFF, "backlog fits the game's signed range");
            result |= check(game_request(remaining) >= 352 && game_request(remaining) <= 464,
                "paused or overfilled device cannot cause oversized synthesis");
        }
    }
    return result;
}
