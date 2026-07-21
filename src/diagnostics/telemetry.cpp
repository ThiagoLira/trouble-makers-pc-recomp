#include "telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#endif

namespace mm::telemetry {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kStallThresholdNs = 10ULL * kNanosecondsPerSecond;
constexpr std::size_t kFrameHistogramBuckets = 128;

struct TimingAtoms {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum_us{0};
    std::atomic<std::uint64_t> maximum_us{0};
    std::atomic<std::uint64_t> over_20ms{0};
    std::atomic<std::uint64_t> over_33ms{0};
    std::array<std::atomic<std::uint64_t>, kFrameHistogramBuckets> histogram{};
};

struct TimingWindow {
    std::uint64_t count = 0;
    std::uint64_t sum_us = 0;
    std::uint64_t maximum_us = 0;
    std::uint64_t over_20ms = 0;
    std::uint64_t over_33ms = 0;
    double p95_ms = 0.0;
};

std::mutex g_state_mutex;
std::mutex g_event_mutex;
std::mutex g_wait_mutex;
std::condition_variable g_wait_condition;
std::thread g_reporter;
std::atomic_bool g_running{false};
bool g_atexit_registered = false;
std::uint32_t g_report_interval_ms = 5000;

std::atomic<Phase> g_phase{Phase::Startup};
std::atomic<std::uint64_t> g_phase_changed_ns{0};
std::atomic<std::uint64_t> g_vi_total{0};
std::atomic<std::uint64_t> g_vi_window{0};
std::atomic<std::uint64_t> g_dl_total{0};
std::atomic<std::uint64_t> g_dl_window{0};
std::atomic<std::uint64_t> g_screen_total{0};
std::atomic<std::uint64_t> g_screen_window{0};
std::atomic<std::uint64_t> g_audio_task_total{0};
std::atomic<std::uint64_t> g_audio_task_window{0};
std::atomic<std::uint64_t> g_audio_task_failures{0};

std::atomic<std::uint64_t> g_last_vi_ns{0};
std::atomic<std::uint64_t> g_last_dl_ns{0};
std::atomic<std::uint64_t> g_last_screen_ns{0};
std::atomic<std::uint64_t> g_last_audio_task_ns{0};
std::atomic<std::uint64_t> g_last_frame_boundary_ns{0};

TimingAtoms g_frame_intervals;
TimingAtoms g_screen_cpu;
TimingAtoms g_dl_cpu;

std::atomic<std::uint64_t> g_audio_buffers_total{0};
std::atomic<std::uint64_t> g_audio_buffers_window{0};
std::atomic<std::uint64_t> g_audio_queue_current{0};
std::atomic<std::uint64_t> g_audio_queue_min{
    std::numeric_limits<std::uint64_t>::max()};
std::atomic<std::uint64_t> g_audio_queue_max{0};
std::atomic<std::uint64_t> g_audio_empty_events{0};
std::atomic<std::uint64_t> g_audio_queue_errors{0};
std::atomic<std::uint64_t> g_audio_submitted_frames{0};
std::atomic<std::uint64_t> g_audio_clipped_samples{0};
std::atomic<std::int64_t> g_audio_sample_sum{0};
std::atomic<std::uint64_t> g_audio_measured_samples{0};
std::atomic<std::uint64_t> g_audio_peak{0};
std::atomic<std::uint32_t> g_audio_input_rate{0};
std::atomic<std::uint32_t> g_audio_output_rate{0};

std::atomic<std::uint32_t> g_drawable_width{0};
std::atomic<std::uint32_t> g_drawable_height{0};
std::atomic<std::uint32_t> g_refresh_rate{0};
std::atomic<std::uint32_t> g_target_rate{60};
std::atomic<std::uint32_t> g_resolution_scale_milli{1000};
std::atomic<std::uint64_t> g_workload_queue_depth{0};
std::atomic<std::uint64_t> g_present_queue_depth{0};
std::atomic<std::uint64_t> g_shader_count{0};
std::atomic<std::uint64_t> g_resident_textures{0};
std::atomic<std::uint64_t> g_texture_slots{0};
std::atomic<std::uint64_t> g_pending_texture_uploads{0};

std::atomic<std::uint64_t> g_scene_state{
    std::numeric_limits<std::uint64_t>::max()};
std::uint32_t g_previous_stall_mask = 0;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

struct ProcessResourceSnapshot {
    std::uint64_t resident_kib = 0;
    std::uint64_t peak_resident_kib = 0;
    std::uint32_t thread_count = 0;
};

ProcessResourceSnapshot process_resource_snapshot() {
    ProcessResourceSnapshot snapshot{};
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        unsigned long long value = 0;
        unsigned int threads = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %llu kB", &value) == 1) {
            snapshot.resident_kib = value;
        } else if (std::sscanf(line.c_str(), "VmHWM: %llu kB", &value) == 1) {
            snapshot.peak_resident_kib = value;
        } else if (std::sscanf(line.c_str(), "Threads: %u", &threads) == 1) {
            snapshot.thread_count = threads;
        }
    }
#endif
    return snapshot;
}

const char* phase_name(Phase phase) {
    switch (phase) {
        case Phase::Startup: return "startup";
        case Phase::Launcher: return "launcher";
        case Phase::RomReady: return "rom-ready";
        case Phase::RuntimeStarting: return "runtime-starting";
        case Phase::RendererStarting: return "renderer-starting";
        case Phase::RendererReady: return "renderer-ready";
        case Phase::GameStarting: return "game-starting";
        case Phase::Running: return "running";
        case Phase::ShuttingDown: return "shutting-down";
        case Phase::Stopped: return "stopped";
    }
    return "unknown";
}

void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void update_min(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (current > value &&
           !target.compare_exchange_weak(current, value,
               std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void record_timing(TimingAtoms& timing, std::uint64_t microseconds) {
    timing.count.fetch_add(1, std::memory_order_relaxed);
    timing.sum_us.fetch_add(microseconds, std::memory_order_relaxed);
    update_max(timing.maximum_us, microseconds);
    if (microseconds > 20000) {
        timing.over_20ms.fetch_add(1, std::memory_order_relaxed);
    }
    if (microseconds > 33333) {
        timing.over_33ms.fetch_add(1, std::memory_order_relaxed);
    }
    const std::size_t bucket = std::min<std::size_t>(
        microseconds / 1000, kFrameHistogramBuckets - 1);
    timing.histogram[bucket].fetch_add(1, std::memory_order_relaxed);
}

TimingWindow take_timing_window(TimingAtoms& timing) {
    TimingWindow result;
    result.count = timing.count.exchange(0, std::memory_order_relaxed);
    result.sum_us = timing.sum_us.exchange(0, std::memory_order_relaxed);
    result.maximum_us = timing.maximum_us.exchange(0, std::memory_order_relaxed);
    result.over_20ms = timing.over_20ms.exchange(0, std::memory_order_relaxed);
    result.over_33ms = timing.over_33ms.exchange(0, std::memory_order_relaxed);

    const std::uint64_t wanted = result.count == 0
        ? 0 : (result.count * 95 + 99) / 100;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kFrameHistogramBuckets; ++i) {
        cumulative += timing.histogram[i].exchange(0, std::memory_order_relaxed);
        if (wanted != 0 && result.p95_ms == 0.0 && cumulative >= wanted) {
            result.p95_ms = static_cast<double>(i + 1);
        }
    }
    return result;
}

double average_ms(const TimingWindow& timing) {
    return timing.count == 0 ? 0.0
        : static_cast<double>(timing.sum_us) /
          static_cast<double>(timing.count) / 1000.0;
}

double age_seconds(std::uint64_t now, std::uint64_t last,
                   std::uint64_t fallback) {
    const std::uint64_t origin = last != 0 ? last : fallback;
    if (origin == 0 || origin > now) return 0.0;
    return static_cast<double>(now - origin) /
           static_cast<double>(kNanosecondsPerSecond);
}

void reset_timing(TimingAtoms& timing) {
    timing.count.store(0, std::memory_order_relaxed);
    timing.sum_us.store(0, std::memory_order_relaxed);
    timing.maximum_us.store(0, std::memory_order_relaxed);
    timing.over_20ms.store(0, std::memory_order_relaxed);
    timing.over_33ms.store(0, std::memory_order_relaxed);
    for (auto& bucket : timing.histogram) {
        bucket.store(0, std::memory_order_relaxed);
    }
}

void reset_statistics() {
    for (auto* value : {&g_vi_total, &g_vi_window, &g_dl_total, &g_dl_window,
                        &g_screen_total, &g_screen_window, &g_audio_task_total,
                        &g_audio_task_window, &g_audio_task_failures,
                        &g_last_vi_ns, &g_last_dl_ns, &g_last_screen_ns,
                        &g_last_audio_task_ns, &g_last_frame_boundary_ns,
                        &g_audio_buffers_total, &g_audio_buffers_window,
                        &g_audio_queue_current, &g_audio_queue_max,
                        &g_audio_empty_events, &g_audio_queue_errors,
                        &g_audio_submitted_frames, &g_audio_clipped_samples,
                        &g_audio_measured_samples, &g_audio_peak,
                        &g_workload_queue_depth, &g_present_queue_depth,
                        &g_shader_count, &g_resident_textures,
                        &g_texture_slots, &g_pending_texture_uploads}) {
        value->store(0, std::memory_order_relaxed);
    }
    g_audio_sample_sum.store(0, std::memory_order_relaxed);
    g_audio_queue_min.store(std::numeric_limits<std::uint64_t>::max(),
                            std::memory_order_relaxed);
    g_scene_state.store(std::numeric_limits<std::uint64_t>::max(),
                        std::memory_order_relaxed);
    reset_timing(g_frame_intervals);
    reset_timing(g_screen_cpu);
    reset_timing(g_dl_cpu);
    g_previous_stall_mask = 0;
}

void emit_report(double interval_seconds) {
    const auto phase = g_phase.load(std::memory_order_relaxed);
    const std::uint64_t vi = g_vi_window.exchange(0, std::memory_order_relaxed);
    const std::uint64_t dl = g_dl_window.exchange(0, std::memory_order_relaxed);
    const std::uint64_t screen =
        g_screen_window.exchange(0, std::memory_order_relaxed);
    const std::uint64_t audio_tasks =
        g_audio_task_window.exchange(0, std::memory_order_relaxed);
    const std::uint64_t audio_failures =
        g_audio_task_failures.exchange(0, std::memory_order_relaxed);
    const TimingWindow frames = take_timing_window(g_frame_intervals);
    const TimingWindow screen_cpu = take_timing_window(g_screen_cpu);
    const TimingWindow dl_cpu = take_timing_window(g_dl_cpu);

    const double seconds = std::max(interval_seconds, 0.001);
    const std::uint32_t width = g_drawable_width.load(std::memory_order_relaxed);
    const std::uint32_t height = g_drawable_height.load(std::memory_order_relaxed);
    const std::uint32_t refresh = g_refresh_rate.load(std::memory_order_relaxed);
    const std::uint32_t target = g_target_rate.load(std::memory_order_relaxed);
    const double scale = static_cast<double>(
        g_resolution_scale_milli.load(std::memory_order_relaxed)) / 1000.0;
    const ProcessResourceSnapshot resources = process_resource_snapshot();

    if (phase >= Phase::RuntimeStarting || vi != 0 || dl != 0 || screen != 0) {
        event("perf",
            "window=%.2fs phase=%s vi=%.1f/s screen=%.1f/s dl=%.1f/s "
            "audio-tasks=%.1f/s frame-ms(avg/p95/max)=%.2f/%.2f/%.2f "
            "slow(>20/>33)=%llu/%llu screen-cpu-ms(avg/max)=%.2f/%.2f "
            "dl-cpu-ms(avg/max)=%.2f/%.2f display=%ux%u@%uHz target=%uHz scale=%.2fx "
            "audio-task-errors=%llu renderer-queues=%llu/%llu shaders=%llu "
            "textures=%llu/%llu uploads=%llu rss-mib=%.1f peak-rss-mib=%.1f "
            "threads=%u",
            seconds, phase_name(phase), vi / seconds, screen / seconds,
            dl / seconds, audio_tasks / seconds, average_ms(frames), frames.p95_ms,
            static_cast<double>(frames.maximum_us) / 1000.0,
            static_cast<unsigned long long>(frames.over_20ms),
            static_cast<unsigned long long>(frames.over_33ms),
            average_ms(screen_cpu),
            static_cast<double>(screen_cpu.maximum_us) / 1000.0,
            average_ms(dl_cpu),
            static_cast<double>(dl_cpu.maximum_us) / 1000.0,
            width, height, refresh, target, scale,
            static_cast<unsigned long long>(audio_failures),
            static_cast<unsigned long long>(
                g_workload_queue_depth.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_present_queue_depth.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_shader_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_resident_textures.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_texture_slots.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_pending_texture_uploads.load(std::memory_order_relaxed)),
            resources.resident_kib / 1024.0,
            resources.peak_resident_kib / 1024.0,
            resources.thread_count);
    }

    const std::uint64_t audio_buffers =
        g_audio_buffers_window.exchange(0, std::memory_order_relaxed);
    const std::uint64_t queue_min = g_audio_queue_min.exchange(
        std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    const std::uint64_t queue_max =
        g_audio_queue_max.exchange(0, std::memory_order_relaxed);
    const std::uint64_t queue_current =
        g_audio_queue_current.load(std::memory_order_relaxed);
    const std::uint64_t empty_events =
        g_audio_empty_events.exchange(0, std::memory_order_relaxed);
    const std::uint64_t queue_errors =
        g_audio_queue_errors.exchange(0, std::memory_order_relaxed);
    const std::uint64_t submitted =
        g_audio_submitted_frames.exchange(0, std::memory_order_relaxed);
    const std::uint64_t clipped =
        g_audio_clipped_samples.exchange(0, std::memory_order_relaxed);
    const std::int64_t sample_sum =
        g_audio_sample_sum.exchange(0, std::memory_order_relaxed);
    const std::uint64_t measured =
        g_audio_measured_samples.exchange(0, std::memory_order_relaxed);
    const std::uint64_t peak = g_audio_peak.exchange(0, std::memory_order_relaxed);
    const std::uint32_t output_rate =
        g_audio_output_rate.load(std::memory_order_relaxed);
    const std::uint32_t input_rate =
        g_audio_input_rate.load(std::memory_order_relaxed);

    if (audio_buffers != 0 || queue_errors != 0) {
        const auto queue_ms = [output_rate](std::uint64_t frames_count) {
            return output_rate == 0 ? 0.0
                : static_cast<double>(frames_count) * 1000.0 / output_rate;
        };
        const std::uint64_t safe_min =
            queue_min == std::numeric_limits<std::uint64_t>::max()
                ? queue_current : queue_min;
        const double dc = measured == 0 ? 0.0
            : static_cast<double>(sample_sum) / static_cast<double>(measured);
        event("audio-stats",
            "callbacks=%.1f/s rates=%u->%uHz submitted=%llu frames "
            "queue-ms(current/min/max)=%.2f/%.2f/%.2f "
            "empty-after-warmup=%llu queue-errors=%llu peak=%llu clipped=%llu dc=%.2f",
            audio_buffers / seconds, input_rate, output_rate,
            static_cast<unsigned long long>(submitted), queue_ms(queue_current),
            queue_ms(safe_min), queue_ms(queue_max),
            static_cast<unsigned long long>(empty_events),
            static_cast<unsigned long long>(queue_errors),
            static_cast<unsigned long long>(peak),
            static_cast<unsigned long long>(clipped), dc);
    }

    const std::uint64_t now = now_ns();
    const std::uint64_t phase_changed =
        g_phase_changed_ns.load(std::memory_order_relaxed);
    const double vi_age = age_seconds(
        now, g_last_vi_ns.load(std::memory_order_relaxed), phase_changed);
    const double screen_age = age_seconds(
        now, g_last_screen_ns.load(std::memory_order_relaxed), phase_changed);
    const double dl_age = age_seconds(
        now, g_last_dl_ns.load(std::memory_order_relaxed), phase_changed);
    const double audio_age = age_seconds(
        now, g_last_audio_task_ns.load(std::memory_order_relaxed), phase_changed);

    if (phase >= Phase::RuntimeStarting && phase < Phase::ShuttingDown) {
        event("watchdog",
            "phase=%s progress-age-seconds vi=%.1f screen=%.1f dl=%.1f audio=%.1f",
            phase_name(phase), vi_age, screen_age, dl_age, audio_age);
    }

    std::uint32_t stall_mask = 0;
    if (phase >= Phase::GameStarting && phase < Phase::ShuttingDown) {
        if (vi_age * kNanosecondsPerSecond >= kStallThresholdNs) stall_mask |= 1u;
        if (screen_age * kNanosecondsPerSecond >= kStallThresholdNs) stall_mask |= 2u;
        if (dl_age * kNanosecondsPerSecond >= kStallThresholdNs) stall_mask |= 4u;
        if (g_audio_task_total.load(std::memory_order_relaxed) != 0 &&
            audio_age * kNanosecondsPerSecond >= kStallThresholdNs) stall_mask |= 8u;
    }
    if (stall_mask != g_previous_stall_mask) {
        if (stall_mask == 0) {
            event("watchdog", "progress recovered; prior stalled-mask=0x%X",
                  g_previous_stall_mask);
        } else {
            event("watchdog",
                "possible stall detected mask=0x%X (vi=0x1 screen=0x2 dl=0x4 audio=0x8)",
                stall_mask);
        }
        g_previous_stall_mask = stall_mask;
    }
}

void reporter_main() {
    auto previous = Clock::now();
    while (g_running.load(std::memory_order_acquire)) {
        std::unique_lock lock(g_wait_mutex);
        g_wait_condition.wait_for(lock,
            std::chrono::milliseconds(g_report_interval_ms), [] {
                return !g_running.load(std::memory_order_acquire);
            });
        lock.unlock();
        if (!g_running.load(std::memory_order_acquire)) break;

        const auto current = Clock::now();
        const double elapsed = std::chrono::duration<double>(current - previous).count();
        previous = current;
        emit_report(elapsed);
    }
}

std::string linux_cpu_name() {
#if defined(__linux__)
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        if (key.find("model name") == std::string::npos &&
            key.find("Hardware") == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(
                   static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        return value;
    }
#endif
    return "unknown";
}

} // namespace

void event(const char* subsystem, const char* format, ...) {
    std::array<char, 2048> message{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);

    std::lock_guard lock(g_event_mutex);
    std::fprintf(stderr, "[%s] %s\n",
                 subsystem != nullptr ? subsystem : "telemetry", message.data());
}

void start(std::uint32_t report_interval_ms) {
    std::lock_guard lock(g_state_mutex);
    if (g_running.load(std::memory_order_acquire)) return;
    reset_statistics();
    g_report_interval_ms = std::max<std::uint32_t>(report_interval_ms, 25);
    g_phase.store(Phase::Startup, std::memory_order_relaxed);
    g_phase_changed_ns.store(now_ns(), std::memory_order_relaxed);
    g_running.store(true, std::memory_order_release);
    g_reporter = std::thread(reporter_main);
    if (!g_atexit_registered) {
        std::atexit(stop);
        g_atexit_registered = true;
    }
    event("lifecycle", "telemetry started; aggregate interval=%u ms",
          g_report_interval_ms);
}

void stop() {
    std::unique_lock lock(g_state_mutex);
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    g_wait_condition.notify_all();
    lock.unlock();
    if (g_reporter.joinable()) g_reporter.join();
    set_phase(Phase::Stopped, "telemetry stopped");
}

bool running() {
    return g_running.load(std::memory_order_acquire);
}

void set_phase(Phase phase, const char* detail) {
    const Phase previous = g_phase.exchange(phase, std::memory_order_acq_rel);
    g_phase_changed_ns.store(now_ns(), std::memory_order_release);
    if (previous != phase || detail != nullptr) {
        event("lifecycle", "phase=%s%s%s", phase_name(phase),
              detail != nullptr ? " detail=" : "",
              detail != nullptr ? detail : "");
    }
}

Phase current_phase() {
    return g_phase.load(std::memory_order_acquire);
}

void log_system_info() {
#if defined(_WIN32)
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtl_get_version != nullptr) rtl_get_version(&version);

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    const char* cpu = std::getenv("PROCESSOR_IDENTIFIER");
    event("system", "os=Windows %lu.%lu build=%lu cpu=%s logical-cores=%u ram=%llu MiB",
          version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
          cpu != nullptr ? cpu : "unknown", std::thread::hardware_concurrency(),
          static_cast<unsigned long long>(memory.ullTotalPhys / (1024 * 1024)));
#elif defined(__linux__)
    utsname system{};
    uname(&system);
    struct sysinfo memory{};
    ::sysinfo(&memory);
    const unsigned long long ram_bytes =
        static_cast<unsigned long long>(memory.totalram) * memory.mem_unit;
    event("system", "os=%s kernel=%s machine=%s cpu=%s logical-cores=%u ram=%llu MiB",
          system.sysname, system.release, system.machine, linux_cpu_name().c_str(),
          std::thread::hardware_concurrency(), ram_bytes / (1024 * 1024));
#else
    event("system", "logical-cores=%u", std::thread::hardware_concurrency());
#endif
}

void record_vi() {
    const std::uint64_t now = now_ns();
    g_last_vi_ns.store(now, std::memory_order_relaxed);
    g_vi_window.fetch_add(1, std::memory_order_relaxed);
    if (g_vi_total.fetch_add(1, std::memory_order_relaxed) == 0) {
        event("lifecycle", "first VI tick");
    }
}

void record_display_list(std::uint64_t cpu_time_us) {
    const std::uint64_t now = now_ns();
    g_last_dl_ns.store(now, std::memory_order_relaxed);
    g_dl_window.fetch_add(1, std::memory_order_relaxed);
    record_timing(g_dl_cpu, cpu_time_us);
    if (g_dl_total.fetch_add(1, std::memory_order_relaxed) == 0) {
        event("lifecycle", "first display list completed");
    }
}

void record_screen_update(std::uint64_t cpu_time_us) {
    const std::uint64_t now = now_ns();
    const std::uint64_t previous =
        g_last_frame_boundary_ns.exchange(now, std::memory_order_relaxed);
    if (previous != 0 && now > previous) {
        record_timing(g_frame_intervals, (now - previous) / 1000);
    }
    g_last_screen_ns.store(now, std::memory_order_relaxed);
    g_screen_window.fetch_add(1, std::memory_order_relaxed);
    record_timing(g_screen_cpu, cpu_time_us);
    if (g_screen_total.fetch_add(1, std::memory_order_relaxed) == 0) {
        event("lifecycle", "first screen update completed");
    }
}

void record_audio_task(bool completed) {
    const std::uint64_t now = now_ns();
    g_last_audio_task_ns.store(now, std::memory_order_relaxed);
    g_audio_task_window.fetch_add(1, std::memory_order_relaxed);
    if (!completed) g_audio_task_failures.fetch_add(1, std::memory_order_relaxed);
    if (g_audio_task_total.fetch_add(1, std::memory_order_relaxed) == 0) {
        event("lifecycle", "first audio RSP task completed=%s",
              completed ? "yes" : "no");
    }
}

void record_scene(int game_state, int stage, int scene) {
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(game_state)) << 32) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(stage)) << 16) |
        static_cast<std::uint16_t>(scene);
    if (g_scene_state.exchange(packed, std::memory_order_relaxed) != packed) {
        event("scene", "game-state=%d stage=%d scene=%d", game_state, stage, scene);
        const Phase phase = current_phase();
        if (game_state != 0 && phase >= Phase::RuntimeStarting &&
            phase < Phase::Running) {
            set_phase(Phase::Running, "game state became active");
        }
    }
}

void record_audio_buffer(std::uint64_t queued_before_frames,
                         std::uint64_t queued_after_frames,
                         std::uint32_t output_rate,
                         std::size_t submitted_frames,
                         int peak_sample,
                         std::uint64_t clipped_samples,
                         std::int64_t sample_sum,
                         std::size_t measured_samples,
                         bool queue_error) {
    const std::uint64_t prior_buffers =
        g_audio_buffers_total.fetch_add(1, std::memory_order_relaxed);
    g_audio_buffers_window.fetch_add(1, std::memory_order_relaxed);
    g_audio_queue_current.store(queued_after_frames, std::memory_order_relaxed);
    update_min(g_audio_queue_min, queued_before_frames);
    update_max(g_audio_queue_max, queued_after_frames);
    if (prior_buffers >= 2 && queued_before_frames == 0) {
        g_audio_empty_events.fetch_add(1, std::memory_order_relaxed);
    }
    if (queue_error) g_audio_queue_errors.fetch_add(1, std::memory_order_relaxed);
    g_audio_submitted_frames.fetch_add(submitted_frames, std::memory_order_relaxed);
    g_audio_clipped_samples.fetch_add(clipped_samples, std::memory_order_relaxed);
    g_audio_sample_sum.fetch_add(sample_sum, std::memory_order_relaxed);
    g_audio_measured_samples.fetch_add(measured_samples, std::memory_order_relaxed);
    update_max(g_audio_peak, static_cast<std::uint64_t>(std::max(peak_sample, 0)));
    g_audio_output_rate.store(output_rate, std::memory_order_relaxed);
}

void set_audio_rates(std::uint32_t input_rate, std::uint32_t output_rate) {
    g_audio_input_rate.store(input_rate, std::memory_order_relaxed);
    g_audio_output_rate.store(output_rate, std::memory_order_relaxed);
}

void set_display_state(std::uint32_t drawable_width,
                       std::uint32_t drawable_height,
                       std::uint32_t refresh_rate,
                       float resolution_scale) {
    g_drawable_width.store(drawable_width, std::memory_order_relaxed);
    g_drawable_height.store(drawable_height, std::memory_order_relaxed);
    g_refresh_rate.store(refresh_rate, std::memory_order_relaxed);
    g_resolution_scale_milli.store(static_cast<std::uint32_t>(
        std::max(resolution_scale, 0.0f) * 1000.0f), std::memory_order_relaxed);
}

void set_display_target(std::uint32_t target_rate) {
    g_target_rate.store(target_rate, std::memory_order_relaxed);
}

void set_renderer_state(std::uint32_t workload_queue_depth,
                        std::uint32_t present_queue_depth,
                        std::uint32_t shader_count,
                        std::uint64_t resident_textures,
                        std::uint64_t texture_slots,
                        std::uint64_t pending_texture_uploads) {
    g_workload_queue_depth.store(workload_queue_depth, std::memory_order_relaxed);
    g_present_queue_depth.store(present_queue_depth, std::memory_order_relaxed);
    g_shader_count.store(shader_count, std::memory_order_relaxed);
    g_resident_textures.store(resident_textures, std::memory_order_relaxed);
    g_texture_slots.store(texture_slots, std::memory_order_relaxed);
    g_pending_texture_uploads.store(
        pending_texture_uploads, std::memory_order_relaxed);
}

} // namespace mm::telemetry
