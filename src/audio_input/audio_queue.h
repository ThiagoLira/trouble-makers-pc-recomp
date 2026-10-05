#pragma once

#include <algorithm>
#include <cstdint>

namespace mm_audio_input::queue_policy {

constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint32_t kBytesPerFrame = 4;
// A suspended device must not retain seconds of obsolete game audio.
constexpr uint32_t kMaxQueuedBytes = kOutputSampleRate * kBytesPerFrame / 4;

inline uint32_t remaining_game_frames(uint32_t queued_bytes, uint32_t sample_rate) {
    const uint64_t output_frames =
        std::min(queued_bytes, kMaxQueuedBytes) / kBytesPerFrame;
    uint64_t input_frames = output_frames * sample_rate / kOutputSampleRate;
    const uint32_t backoff = sample_rate / 60;
    input_frames = input_frames > backoff ? input_frames - backoff : 0;
    // Sound_Update narrows (target - remaining + 96) to s16 before applying
    // its minimum. Larger backlogs can wrap into huge positive requests and
    // overwrite the audio heap. Keep the reported value in that signed range
    // even for an unexpectedly high input sample rate.
    return static_cast<uint32_t>(std::min<uint64_t>(input_frames, 0x7FFF));
}

} // namespace mm_audio_input::queue_policy
