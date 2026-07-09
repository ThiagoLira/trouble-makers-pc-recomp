// Audio backend for ultramodern's audio_callbacks_t, backed by SDL2's
// queue-based audio device. ultramodern calls queue_samples() with int16
// stereo data pulled from RDRAM, get_frames_remaining() to gauge buffer
// fill, and set_frequency() when the game sets the AI DAC rate.
//
// Modeled on reference/Zelda64Recomp's SDL audio glue, but simplified to
// AUDIO_S16 output (no float volume scaling) while keeping the two things
// that matter for correctness: the N64 L/R channel swap (the AI DMA delivers
// stereo pairs swapped relative to a little-endian interleaved stream) and
// sample-rate conversion from the game's rate to the output device rate.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mm_audio_input.hpp"

namespace mm_audio_input {
// Defined in input.cpp — brings up the game-controller subsystem and opens
// the first pad. init() calls it so the host has a single setup entry point.
void init_input_subsystem();

namespace {

// Output device runs at a fixed 48 kHz; SDL resamples the game's stream to it.
constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint32_t kInputChannels = 2;
constexpr uint32_t kOutputChannels = 2;
constexpr uint32_t kBytesPerFrame = kInputChannels * sizeof(int16_t);

SDL_AudioDeviceID g_audio_device = 0;
// Current game (input) sample rate, set via set_frequency(). Defaults to the
// output rate until the game tells us otherwise.
uint32_t g_sample_rate = kOutputSampleRate;
SDL_AudioCVT g_convert{};
bool g_convert_valid = false;

void rebuild_converter() {
    SDL_zero(g_convert);
    int ret = SDL_BuildAudioCVT(&g_convert,
        AUDIO_S16LSB, kInputChannels, g_sample_rate,
        AUDIO_S16LSB, kOutputChannels, kOutputSampleRate);
    if (ret < 0) {
        fprintf(stderr, "mm_audio: SDL_BuildAudioCVT(%u): %s\n", g_sample_rate, SDL_GetError());
        g_convert_valid = false;
        return;
    }
    g_convert_valid = true;
}

// ultramodern passes sample_count = number of int16 samples (i.e. 2 per stereo
// frame). Swap each L/R pair to undo the N64 address-xor layout, resample to
// the device rate, and queue.
void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (g_audio_device == 0 || sample_count < kInputChannels) return;

    static std::vector<int16_t> buf;
    const size_t cap = (sample_count + 8u) * static_cast<size_t>(std::max(1, g_convert.len_mult));
    if (buf.size() < cap) buf.resize(cap);

    // Swap stereo channels: N64 delivers [R0 L0 R1 L1 ...], SDL wants [L R L R].
    for (size_t i = 0; i + 1 < sample_count; i += kInputChannels) {
        buf[i + 0] = audio_data[i + 1];
        buf[i + 1] = audio_data[i + 0];
    }

    if (g_convert_valid) {
        g_convert.buf = reinterpret_cast<Uint8*>(buf.data());
        g_convert.len = static_cast<int>(sample_count * sizeof(int16_t));
        if (SDL_ConvertAudio(&g_convert) < 0) {
            fprintf(stderr, "mm_audio: SDL_ConvertAudio: %s\n", SDL_GetError());
            return;
        }
        SDL_QueueAudio(g_audio_device, buf.data(), g_convert.len_cvt);
    } else {
        // No converter (e.g. BuildAudioCVT failed): queue as-is at input rate.
        SDL_QueueAudio(g_audio_device, buf.data(), sample_count * sizeof(int16_t));
    }
}

void set_frequency(uint32_t freq) {
    if (freq == 0) return;
    if (freq == g_sample_rate) return;
    g_sample_rate = freq;
    rebuild_converter();
}

} // namespace

// Report remaining buffer in stereo *frames* at the game's sample rate, so
// ultramodern's get_remaining_audio_bytes() (which multiplies by 2*sizeof(s16))
// stays consistent. Back off ~1 VI of samples so transient underruns don't pop.
// Declared in the public header so the host can mirror it into AI_LEN (see
// src/game/register_overlays.cpp); it is also the audio_callbacks value.
size_t get_frames_remaining() {
    if (g_audio_device == 0) return 0;
    uint64_t out_frames = SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame;
    // Rescale device-rate frames back to the game's sample-rate frame count.
    uint64_t in_frames = out_frames * g_sample_rate / kOutputSampleRate;

    const uint32_t frames_per_vi = g_sample_rate / 60;
    const uint64_t backoff = 1u * frames_per_vi;
    if (in_frames > backoff) in_frames -= backoff;
    else in_frames = 0;
    return static_cast<size_t>(in_frames);
}

void init() {
    if (g_audio_device != 0) return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "mm_audio: SDL_InitSubSystem(AUDIO): %s\n", SDL_GetError());
    }

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(kOutputSampleRate);
    desired.format = AUDIO_S16LSB;
    desired.channels = kOutputChannels;
    desired.samples = 0x100; // small, to keep latency low
    desired.callback = nullptr;
    desired.userdata = nullptr;

    SDL_AudioSpec obtained{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (g_audio_device == 0) {
        fprintf(stderr, "mm_audio: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_audio_device, 0); // begin playback

    rebuild_converter();
    init_input_subsystem();
}

ultramodern::audio_callbacks_t audio_callbacks() {
    return ultramodern::audio_callbacks_t{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
}

} // namespace mm_audio_input
