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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mm_audio_input.hpp"
#include "telemetry.h"

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
    mm::telemetry::set_audio_rates(g_sample_rate, kOutputSampleRate);
    // SDL_AudioCVT::len_ratio is scratch state in some SDL builds and has
    // produced uninitialised garbage in Windows diagnostics.  The rate ratio
    // is deterministic for this stereo-to-stereo conversion, so report it
    // directly instead of trusting that implementation detail.
    const double sample_rate_ratio = static_cast<double>(kOutputSampleRate) /
                                     static_cast<double>(g_sample_rate);
    mm::telemetry::event("audio",
        "converter input=%uHz output=%uHz conversion-needed=%s len-mult=%d ratio=%.6f",
        g_sample_rate, kOutputSampleRate, ret == 0 ? "no" : "yes",
        g_convert.len_mult, sample_rate_ratio);
}

// ultramodern passes sample_count = number of int16 samples (i.e. 2 per stereo
// frame). Swap each L/R pair to undo the N64 address-xor layout, resample to
// the device rate, and queue.
void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (g_audio_device == 0 || sample_count < kInputChannels) return;

    const std::uint64_t queued_before_frames =
        SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame;

    static std::vector<int16_t> buf;
    const size_t cap = (sample_count + 8u) * static_cast<size_t>(std::max(1, g_convert.len_mult));
    if (buf.size() < cap) buf.resize(cap);

    // Swap stereo channels: N64 delivers [R0 L0 R1 L1 ...], SDL wants [L R L R].
    for (size_t i = 0; i + 1 < sample_count; i += kInputChannels) {
        buf[i + 0] = audio_data[i + 1];
        buf[i + 1] = audio_data[i + 0];
    }

    const int16_t* queued_samples = buf.data();
    size_t queued_sample_count = sample_count;
    int queue_result = 0;
    if (g_convert_valid) {
        g_convert.buf = reinterpret_cast<Uint8*>(buf.data());
        g_convert.len = static_cast<int>(sample_count * sizeof(int16_t));
        if (SDL_ConvertAudio(&g_convert) < 0) {
            fprintf(stderr, "mm_audio: SDL_ConvertAudio: %s\n", SDL_GetError());
            return;
        }
        queued_sample_count = static_cast<size_t>(g_convert.len_cvt) / sizeof(int16_t);
        queue_result = SDL_QueueAudio(g_audio_device, buf.data(), g_convert.len_cvt);
    } else {
        // No converter (e.g. BuildAudioCVT failed): queue as-is at input rate.
        queue_result = SDL_QueueAudio(
            g_audio_device, buf.data(), sample_count * sizeof(int16_t));
    }

    int peak = 0;
    std::uint64_t clipped = 0;
    std::int64_t sum = 0;
    for (size_t i = 0; i < queued_sample_count; ++i) {
        const int sample = queued_samples[i];
        const int magnitude = sample == -32768 ? 32768 : std::abs(sample);
        peak = std::max(peak, magnitude);
        if (magnitude >= 32767) ++clipped;
        sum += sample;
    }

    if (queue_result < 0) {
        static std::atomic_bool reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "mm_audio: SDL_QueueAudio: %s\n", SDL_GetError());
        }
    }
    const std::uint64_t queued_after_frames =
        SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame;
    mm::telemetry::record_audio_buffer(
        queued_before_frames, queued_after_frames, kOutputSampleRate,
        queued_sample_count / kOutputChannels, peak, clipped, sum,
        queued_sample_count, queue_result < 0);
}

void set_frequency(uint32_t freq) {
    if (freq == 0) return;
    if (freq == g_sample_rate) return;
    const uint32_t previous = g_sample_rate;
    g_sample_rate = freq;
    mm::telemetry::event("audio", "input sample rate changed %uHz -> %uHz",
                         previous, g_sample_rate);
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
    // Input must remain available even when a machine has no usable audio
    // device (the launcher owns controller remapping independently of audio).
    init_input_subsystem();
    if (g_audio_device != 0) return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "mm_audio: SDL_InitSubSystem(AUDIO): %s\n", SDL_GetError());
    }

    const int playback_devices = SDL_GetNumAudioDevices(0);
    mm::telemetry::event("audio", "playback devices reported by SDL=%d",
                         playback_devices);
    for (int i = 0; i < std::min(playback_devices, 8); ++i) {
        const char* name = SDL_GetAudioDeviceName(i, 0);
        mm::telemetry::event("audio", "playback-device[%d]=%s", i,
                             name != nullptr ? name : "unknown");
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
    const double latency_ms = obtained.freq > 0
        ? static_cast<double>(obtained.samples) * 1000.0 / obtained.freq : 0.0;
    mm::telemetry::event("audio",
        "driver=%s requested=%dHz/0x%04X/%u-ch/%u-samples "
        "obtained=%dHz/0x%04X/%u-ch/%u-samples/%u-bytes latency=%.2fms",
        SDL_GetCurrentAudioDriver() != nullptr
            ? SDL_GetCurrentAudioDriver() : "unknown",
        desired.freq, static_cast<unsigned int>(desired.format),
        static_cast<unsigned int>(desired.channels),
        static_cast<unsigned int>(desired.samples), obtained.freq,
        static_cast<unsigned int>(obtained.format),
        static_cast<unsigned int>(obtained.channels),
        static_cast<unsigned int>(obtained.samples),
        static_cast<unsigned int>(obtained.size), latency_ms);
    SDL_PauseAudioDevice(g_audio_device, 0); // begin playback

    rebuild_converter();
}

ultramodern::audio_callbacks_t audio_callbacks() {
    return ultramodern::audio_callbacks_t{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
}

} // namespace mm_audio_input
