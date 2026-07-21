// Low-overhead runtime telemetry. Hot game/audio/render paths only update
// atomics; a background reporter emits one aggregate snapshot every few
// seconds through stderr, which the session logger captures and timestamps.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::telemetry {

enum class Phase : std::uint8_t {
    Startup,
    Launcher,
    RomReady,
    RuntimeStarting,
    RendererStarting,
    RendererReady,
    GameStarting,
    Running,
    ShuttingDown,
    Stopped,
};

void start(std::uint32_t report_interval_ms = 5000);
void stop();
bool running();

void event(const char* subsystem, const char* format, ...);
void log_system_info();
void set_phase(Phase phase, const char* detail = nullptr);
Phase current_phase();

void record_vi();
void record_display_list(std::uint64_t cpu_time_us);
void record_screen_update(std::uint64_t cpu_time_us);
void record_audio_task(bool completed);
void record_scene(int game_state, int stage, int scene);

void record_audio_buffer(std::uint64_t queued_before_frames,
                         std::uint64_t queued_after_frames,
                         std::uint32_t output_rate,
                         std::size_t submitted_frames,
                         int peak_sample,
                         std::uint64_t clipped_samples,
                         std::int64_t sample_sum,
                         std::size_t measured_samples,
                         bool queue_error);
void set_audio_rates(std::uint32_t input_rate, std::uint32_t output_rate);

void set_display_state(std::uint32_t drawable_width,
                       std::uint32_t drawable_height,
                       std::uint32_t refresh_rate,
                       float resolution_scale);
void set_render_target_state(std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t downsample_multiplier,
                             std::uint32_t msaa_samples);
void set_display_target(std::uint32_t target_rate);

void set_renderer_state(std::uint32_t workload_queue_depth,
                        std::uint32_t present_queue_depth,
                        std::uint32_t shader_count,
                        std::uint64_t resident_textures,
                        std::uint64_t texture_slots,
                        std::uint64_t pending_texture_uploads);

} // namespace mm::telemetry
