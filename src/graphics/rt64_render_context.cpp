// mm_graphics: ultramodern::renderer::RendererContext implemented on RT64.
//
// SPDX-License-Identifier: MIT
// Written against the two MIT-licensed interfaces it bridges — ultramodern's
// renderer_context.hpp (the runtime side) and RT64's rt64_application.h (the
// renderer side). RT64 wants the N64's memory-mapped register file as a set
// of host pointers it can read and write while interpreting display lists;
// ultramodern owns the VI registers (its VI thread writes them every retrace)
// and this context supplies backing storage for the rest (SP memories, DPC,
// MI) that nothing else in the recomp models.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "gui/rt64_inspector.h"
#include "imgui.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "mm_graphics.h"
#include "telemetry.h"

namespace mm::graphics {

namespace {

std::atomic<OverlayDrawCallback> g_overlay_draw_callback{nullptr};
std::atomic<bool> g_vsync_enabled{true};

int ring_queue_depth(int thread_cursor, int write_cursor, int queue_size) {
    return (write_cursor - thread_cursor + queue_size) % queue_size;
}

double timing_average_ms(const RT64::PresentQueueTimingSnapshot& timing) {
    return timing.count == 0 ? 0.0
        : static_cast<double>(timing.sumMicroseconds) /
          static_cast<double>(timing.count) / 1000.0;
}

void log_present_telemetry(
        const RT64::PresentQueueTelemetrySnapshot& present) {
    if (present.presentedFrames == 0 && present.failedPresents == 0 &&
        present.queueSkippedPresents == 0) {
        return;
    }

    const double interval_average_ms = present.intervalCount == 0 ? 0.0
        : static_cast<double>(present.intervalSumMicroseconds) /
          static_cast<double>(present.intervalCount) / 1000.0;
    const double measured_rate = present.intervalSumMicroseconds == 0 ? 0.0
        : static_cast<double>(present.intervalCount) * 1'000'000.0 /
          static_cast<double>(present.intervalSumMicroseconds);
    mm::telemetry::event("present",
        "rate=%.1f/s target=%uHz source=%uHz "
        "interval-ms(avg/p95/p99/max)=%.2f/%.2f/%.2f/%.2f "
        "late/missed=%llu/%llu frames(base/interpolated/skipped)=%llu/%llu/%llu "
        "batches(interpolated/non-interpolated)=%llu/%llu queue-skips=%llu failures=%llu "
        "wait-ms(avg/max acquire/interpolation/gpu/swap/present)="
        "%.2f/%.2f %.2f/%.2f %.2f/%.2f %.2f/%.2f %.2f/%.2f "
        "pacing-ms(sleep/overshoot)=%.2f/%.2f %.2f/%.2f present-wait=%s "
        "render-target=%ux%u downsample=%ux msaa=%ux",
        measured_rate, present.effectiveTargetRate, present.originalRate,
        interval_average_ms,
        present.intervalP95Microseconds / 1000.0,
        present.intervalP99Microseconds / 1000.0,
        present.intervalMaximumMicroseconds / 1000.0,
        static_cast<unsigned long long>(present.lateIntervals),
        static_cast<unsigned long long>(present.missedDeadlines),
        static_cast<unsigned long long>(present.baseFrames),
        static_cast<unsigned long long>(present.interpolatedFrames),
        static_cast<unsigned long long>(present.skippedInterpolatedFrames),
        static_cast<unsigned long long>(present.interpolationBatches),
        static_cast<unsigned long long>(present.nonInterpolatedBatches),
        static_cast<unsigned long long>(present.queueSkippedPresents),
        static_cast<unsigned long long>(present.failedPresents),
        timing_average_ms(present.acquireWait),
        present.acquireWait.maximumMicroseconds / 1000.0,
        timing_average_ms(present.interpolationWait),
        present.interpolationWait.maximumMicroseconds / 1000.0,
        timing_average_ms(present.gpuWait),
        present.gpuWait.maximumMicroseconds / 1000.0,
        timing_average_ms(present.swapChainWait),
        present.swapChainWait.maximumMicroseconds / 1000.0,
        timing_average_ms(present.presentCall),
        present.presentCall.maximumMicroseconds / 1000.0,
        timing_average_ms(present.pacingSleep),
        present.pacingSleep.maximumMicroseconds / 1000.0,
        timing_average_ms(present.sleepOvershoot),
        present.sleepOvershoot.maximumMicroseconds / 1000.0,
        present.presentWaitEnabled ? "yes" : "no",
        present.renderTargetWidth, present.renderTargetHeight,
        present.downsampleMultiplier, present.msaaSamples);
}

// N64 bus addresses carry segment bits in the top byte; RT64 wants the
// physical offset into RDRAM.
constexpr uint32_t physical(uint32_t bus_addr) {
    return bus_addr & 0x3FFFFFFu;
}

// RT64 requires a non-null interrupt hook; the recomp runtime drives frame
// completion through ultramodern's own event plumbing instead.
void no_interrupts() {}

RT64::UserConfiguration::GraphicsAPI to_rt64_api(ultramodern::renderer::GraphicsApi api) {
    using From = ultramodern::renderer::GraphicsApi;
    using To = RT64::UserConfiguration::GraphicsAPI;
    switch (api) {
        case From::D3D12:  return To::D3D12;
        case From::Vulkan: return To::Vulkan;
        case From::Metal:  return To::Metal;
        default:           return To::Automatic;
    }
}

ultramodern::renderer::GraphicsApi from_rt64_api(RT64::UserConfiguration::GraphicsAPI api) {
    using From = RT64::UserConfiguration::GraphicsAPI;
    using To = ultramodern::renderer::GraphicsApi;
    switch (api) {
        case From::D3D12:  return To::D3D12;
        case From::Vulkan: return To::Vulkan;
        case From::Metal:  return To::Metal;
        default:           return To::Auto;
    }
}

const char* graphics_api_name(ultramodern::renderer::GraphicsApi api) {
    using Api = ultramodern::renderer::GraphicsApi;
    switch (api) {
        case Api::D3D12: return "D3D12";
        case Api::Vulkan: return "Vulkan";
        case Api::Metal: return "Metal";
        default: return "Automatic/unknown";
    }
}

ultramodern::renderer::SetupResult to_setup_result(RT64::Application::SetupResult result) {
    using From = RT64::Application::SetupResult;
    using To = ultramodern::renderer::SetupResult;
    switch (result) {
        case From::Success:                  return To::Success;
        case From::DynamicLibrariesNotFound: return To::DynamicLibrariesNotFound;
        case From::InvalidGraphicsAPI:       return To::InvalidGraphicsAPI;
        case From::GraphicsAPINotFound:      return To::GraphicsAPINotFound;
        case From::GraphicsDeviceNotFound:   return To::GraphicsDeviceNotFound;
    }
    return To::GraphicsDeviceNotFound;
}

RT64::UserConfiguration::Antialiasing to_rt64_msaa(ultramodern::renderer::Antialiasing aa) {
    using From = ultramodern::renderer::Antialiasing;
    using To = RT64::UserConfiguration::Antialiasing;
    switch (aa) {
        case From::MSAA2X: return To::MSAA2X;
        case From::MSAA4X: return To::MSAA4X;
        case From::MSAA8X: return To::MSAA8X;
        default:           return To::None;
    }
}

RT64::UserConfiguration::AspectRatio to_rt64_aspect(ultramodern::renderer::AspectRatio ar) {
    using From = ultramodern::renderer::AspectRatio;
    using To = RT64::UserConfiguration::AspectRatio;
    switch (ar) {
        case From::Expand: return To::Expand;
        case From::Manual: return To::Manual;
        default:           return To::Original;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64_refresh(ultramodern::renderer::RefreshRate rr) {
    using From = ultramodern::renderer::RefreshRate;
    using To = RT64::UserConfiguration::RefreshRate;
    switch (rr) {
        case From::Display: return To::Display;
        case From::Manual:  return To::Manual;
        default:            return To::Original;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64_color(ultramodern::renderer::HighPrecisionFramebuffer hpfb) {
    using From = ultramodern::renderer::HighPrecisionFramebuffer;
    using To = RT64::UserConfiguration::InternalColorFormat;
    switch (hpfb) {
        case From::On:  return To::High;
        case From::Off: return To::Standard;
        default:        return To::Automatic;
    }
}

// Push the runtime's graphics settings into RT64's user configuration.
void apply_config(RT64::Application& app, const ultramodern::renderer::GraphicsConfig& config) {
    const int downsample = std::max(config.ds_option, 1);

    switch (config.res_option) {
        case ultramodern::renderer::Resolution::Original:
            app.userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app.userConfig.resolutionMultiplier = downsample;
            break;
        case ultramodern::renderer::Resolution::Original2x:
            app.userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app.userConfig.resolutionMultiplier = 2.0 * downsample;
            break;
        default:
            app.userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            break;
    }
    app.userConfig.downsampleMultiplier = downsample;

    app.userConfig.aspectRatio = to_rt64_aspect(config.ar_option);
    app.userConfig.antialiasing = to_rt64_msaa(config.msaa_option);
    app.userConfig.refreshRate = to_rt64_refresh(config.rr_option);
    app.userConfig.refreshRateTarget = config.rr_manual_value;
    app.userConfig.internalColorFormat = to_rt64_color(config.hpfb_option);
    app.userConfig.graphicsAPI = to_rt64_api(config.api_option);
}

class Rt64Context final : public ultramodern::renderer::RendererContext {
public:
    Rt64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
        mm::telemetry::set_phase(mm::telemetry::Phase::RendererStarting,
                                 "RT64 setup beginning");
        RT64::Application::Core core{};
        // RT64's RenderWindow is HWND on Windows, SDL_Window* on Linux;
        // ultramodern's WindowHandle wraps HWND+thread_id on Windows. Same
        // split as the reference render context.
#if defined(_WIN32)
        core.window = window.window;
#else
        core.window = window;
#endif
        core.checkInterrupts = no_interrupts;

        // RT64 dereferences all of these unconditionally. RDRAM is the
        // recomp's flat game memory; the rest are backed by this context
        // (regs_ zero-initialized: idle RCP, no pending interrupts, blank
        // ROM header).
        core.HEADER = regs_.header;
        core.RDRAM = rdram;
        core.DMEM = regs_.dmem;
        core.IMEM = regs_.imem;
        core.MI_INTR_REG = &regs_.mi_intr;
        core.DPC_START_REG = &regs_.dpc[0];
        core.DPC_END_REG = &regs_.dpc[1];
        core.DPC_CURRENT_REG = &regs_.dpc[2];
        core.DPC_STATUS_REG = &regs_.dpc[3];
        core.DPC_CLOCK_REG = &regs_.dpc[4];
        core.DPC_BUFBUSY_REG = &regs_.dpc[5];
        core.DPC_PIPEBUSY_REG = &regs_.dpc[6];
        core.DPC_TMEM_REG = &regs_.dpc[7];

        // The VI register file belongs to ultramodern — its VI thread updates
        // these every retrace, and RT64 reads them to scan out.
        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration app_config;
        app_config.useConfigurationFile = false; // settings come from ultramodern, not rt64.json

        app_ = std::make_unique<RT64::Application>(core, app_config);

        apply_config(*app_, ultramodern::renderer::get_graphics_config());

        // The game stashes RDP-copied palette scratch in the framebuffer rows
        // a CRT never showed: one row above VI_ORIGIN plus the first scanned
        // out row or two (issue #37). The patched RT64 presenter skips the
        // pre-origin row unconditionally; this hides the remaining scratch
        // rows the way CRT overscan did. MM_VI_TOP_CROP overrides (0 shows
        // every scanned-out row again).
        uint32_t top_crop_rows = 2;
        if (const char* crop_env = std::getenv("MM_VI_TOP_CROP")) {
            const long value = std::strtol(crop_env, nullptr, 10);
            top_crop_rows = static_cast<uint32_t>(std::clamp(value, 0L, 16L));
        }
        app_->enhancementConfig.presentation.viTopCropRows = top_crop_rows;

        const uint32_t requested_msaa = app_->userConfig.msaaSampleCount();
        const int requested_ssaa = app_->userConfig.downsampleMultiplier;
        app_->userConfig.developerMode = developer_mode;

        setup_result = to_setup_result(app_->setup(0));
        chosen_api = from_rt64_api(app_->chosenGraphicsAPI);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            mm::telemetry::event("gfx", "RT64 setup failed result=%d api=%s",
                static_cast<int>(setup_result), graphics_api_name(chosen_api));
            app_.reset();
            return;
        }

        const uint32_t active_msaa = app_->userConfig.msaaSampleCount();
        const auto& device = app_->device->getDescription();
        std::fprintf(stderr,
            "[gfx] api=%s device=%s vendor=0x%04X driver=0x%llX vram=%llu MiB\n",
            graphics_api_name(chosen_api), device.name.c_str(),
            static_cast<unsigned int>(device.vendor),
            static_cast<unsigned long long>(device.driverVersion),
            static_cast<unsigned long long>(device.dedicatedVideoMemory / (1024 * 1024)));
        if ((chosen_api == ultramodern::renderer::GraphicsApi::Vulkan) &&
            ((device.name.find("llvmpipe") != std::string::npos) ||
             (device.name.find("lavapipe") != std::string::npos) ||
             (device.name.find("SwiftShader") != std::string::npos))) {
            std::fprintf(stderr,
                "[gfx] WARNING: software Vulkan device selected; performance will be extremely low\n");
        }
        std::fprintf(stderr,
            "[gfx] antialiasing msaa=%ux%s ssaa=%dx sample-locations=%s\n",
            active_msaa,
            active_msaa == requested_msaa ? "" : " (hardware fallback)",
            requested_ssaa,
            app_->device->getCapabilities().sampleLocations ? "yes" : "no");

        // The swap chain is created with vsync on. Apply the user's choice
        // before the first workload reaches the present queue: D3D12 switches
        // to Present(0) immediately, Vulkan flags the swap chain for
        // recreation in immediate mode (a no-op if the surface only supports
        // FIFO, in which case isVsyncEnabled stays yes).
        const bool vsync = g_vsync_enabled.load(std::memory_order_acquire);
        if (!vsync && app_->swapChain != nullptr) {
            app_->swapChain->setVsyncEnabled(false);
        }
        mm::telemetry::event("gfx", "vsync requested=%s swap-chain=%s",
            vsync ? "on" : "off",
            app_->swapChain != nullptr &&
                app_->swapChain->isVsyncEnabled() ? "yes" : "no");
        mm::telemetry::set_phase(mm::telemetry::Phase::RendererReady,
                                 "RT64 setup completed");

        // NVIDIA's 610-series Linux Vulkan driver was observed corrupting RT64's
        // specialized per-material SPIR-V on Blackwell: small sprites and glyphs
        // collapse into lines or solid blocks. RT64's ubershader consumes the
        // same draw data without the re-spirv specialization pass and renders
        // correctly.
        // Keep the workaround scoped to Blackwell. Applying it to every older
        // NVIDIA GPU on a 610-series driver turns a correctness workaround into
        // a substantial performance regression.
        // The environment override makes testing a future driver fix immediate.
        bool ubershaders_only = false;
#if defined(__linux__)
        if ((chosen_api == ultramodern::renderer::GraphicsApi::Vulkan) &&
            (app_->device != nullptr)) {
            const auto& device = app_->device->getDescription();
            constexpr uint64_t kNvidiaDriverMajorShift = 22;
            const uint32_t driver_major =
                static_cast<uint32_t>(device.driverVersion >> kNvidiaDriverMajorShift);
            const bool blackwell =
                (device.name.find("NVIDIA GeForce RTX 50") != std::string::npos) ||
                (device.name.find("Blackwell") != std::string::npos);
            ubershaders_only =
                (device.vendor == RT64::RenderDeviceVendor::NVIDIA) &&
                blackwell && (driver_major >= 610);
        }
#endif
        if (const char* env = std::getenv("MM_RT64_UBERSHADERS_ONLY")) {
            ubershaders_only = (std::atoi(env) != 0);
        }
        app_->workloadQueue->ubershadersOnly = ubershaders_only;
        if (ubershaders_only) {
            std::fprintf(stderr,
                "[gfx] using RT64 ubershaders (NVIDIA specialized-SPIR-V workaround)\n");
        }

        app_->setFullScreen(ultramodern::renderer::get_graphics_config().wm_option
                            == ultramodern::renderer::WindowMode::Fullscreen);

        // [MM] Opt-in widescreen wing clear. With --widescreen (AspectRatio::Expand)
        // the HD color target is wider than the game's 4:3 framebuffer; the side
        // "wings" are never written by the game's 2D draws and otherwise freeze
        // stale framebuffer content. MM_CLEAR_WINGS=1 makes RT64 clear those wing
        // rects at the start of each framebuffer's render pass. See
        // PHASE6_NOTES_b.md (lane b).
        bool clear_wings = ultramodern::renderer::get_graphics_config().ar_option
                           == ultramodern::renderer::AspectRatio::Expand; // default ON in widescreen
        if (const char* env = std::getenv("MM_CLEAR_WINGS")) {
            clear_wings = (std::atoi(env) != 0);
        }
        if (clear_wings) {
            app_->enhancementConfig.rect.clearWings = true;
            app_->updateEnhancementConfig();
        }
    }

    ~Rt64Context() override = default;

    bool valid() override {
        return app_ != nullptr;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (old_config == new_config) {
            return false;
        }
        if (old_config.wm_option != new_config.wm_option) {
            app_->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        }
        apply_config(*app_, new_config);
        app_->updateUserConfig(true);
        if (old_config.msaa_option != new_config.msaa_option) {
            app_->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        app_->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        // Fresh RSP state per task, load the task's microcode (gspFast3D for
        // this game), then let RT64 interpret the display list out of RDRAM.
        static std::atomic_bool first{true};
        if (first.exchange(false, std::memory_order_relaxed)) {
            mm::telemetry::event("gfx",
                "first display list ucode=%08X data-ptr=%08X data-size=%u",
                task->t.ucode, task->t.data_ptr, task->t.data_size);
        }
        const auto started = std::chrono::steady_clock::now();
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(physical(task->t.ucode), physical(task->t.ucode_data), true);
        app_->processDisplayLists(app_->core.RDRAM, physical(task->t.data_ptr), 0, true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        mm::telemetry::record_display_list(
            static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)));
    }

    void update_screen() override {
        static std::uint64_t screen_count = 0;
        if (screen_count == 0) {
            ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
            mm::telemetry::event("gfx",
                "first screen update VI_ORIGIN=%08X VI_WIDTH=%u VI_STATUS=%08X",
                vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG, vi->VI_STATUS_REG);
        }
        ++screen_count;
        const auto screen_started = std::chrono::steady_clock::now();

        // Reuse RT64's inspector renderer as a small, host-owned post-present
        // ImGui pass. Developer mode stays off, so State::inspect() never
        // builds RT64's own large inspector window; only our callback submits
        // draw data. PresentQueue composites it after the N64 image on both
        // Vulkan and D3D12.
        if (OverlayDrawCallback draw_overlay =
                g_overlay_draw_callback.load(std::memory_order_acquire)) {
            const std::scoped_lock lock(app_->presentQueue->inspectorMutex);
            if (app_->presentQueue->inspector == nullptr) {
                app_->presentQueue->inspector = std::make_unique<RT64::Inspector>(
                    app_->device.get(), app_->swapChain.get(),
                    app_->chosenGraphicsAPI, app_->appWindow->sdlWindow);
                ImGui::GetIO().IniFilename = nullptr;
            }
            RT64::Inspector* inspector = app_->presentQueue->inspector.get();
            inspector->newFrame(app_->framebufferGraphicsWorker.get());
            draw_overlay();
            inspector->endFrame();
        }
        app_->updateScreen();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - screen_started).count();
        mm::telemetry::record_screen_update(
            static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)));

        if ((screen_count % 60) == 0) {
            int workload_thread = 0;
            int workload_write = 0;
            {
                const std::scoped_lock lock(app_->workloadQueue->cursorMutex);
                workload_thread = app_->workloadQueue->threadCursor;
                workload_write = app_->workloadQueue->writeCursor;
            }
            int present_thread = 0;
            int present_write = 0;
            {
                const std::scoped_lock lock(app_->presentQueue->cursorMutex);
                present_thread = app_->presentQueue->threadCursor;
                present_write = app_->presentQueue->writeCursor;
            }
            std::uint64_t texture_slots = 0;
            std::uint64_t resident_textures = 0;
            {
                const std::scoped_lock lock(app_->textureCache->textureMapMutex);
                texture_slots = app_->textureCache->textureMap.textures.size();
                resident_textures = static_cast<std::uint64_t>(std::count_if(
                    app_->textureCache->textureMap.textures.begin(),
                    app_->textureCache->textureMap.textures.end(),
                    [](const RT64::Texture* texture) { return texture != nullptr; }));
            }
            std::uint64_t pending_texture_uploads = 0;
            {
                const std::scoped_lock lock(app_->textureCache->uploadQueueMutex);
                pending_texture_uploads = app_->textureCache->uploadQueue.size();
            }
            mm::telemetry::set_renderer_state(
                static_cast<std::uint32_t>(ring_queue_depth(
                    workload_thread, workload_write, WORKLOAD_QUEUE_SIZE)),
                static_cast<std::uint32_t>(ring_queue_depth(
                    present_thread, present_write, PRESENT_QUEUE_SIZE)),
                app_->rasterShaderCache->shaderCount(), resident_textures,
                texture_slots, pending_texture_uploads);
        }

        if ((screen_count % 300) == 0) {
            const RT64::PresentQueueTelemetrySnapshot present =
                app_->presentQueue->takeTelemetrySnapshot();
            mm::telemetry::set_render_target_state(
                present.renderTargetWidth, present.renderTargetHeight,
                present.downsampleMultiplier, present.msaaSamples);
            log_present_telemetry(present);
        }

        if (screen_count == 1 || screen_count % 300 == 0) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t refresh = 0;
            float scale_x = 1.0f;
            float scale_y = 1.0f;
            std::uint32_t downsample = 1;
            {
                const std::scoped_lock lock(
                    app_->sharedQueueResources->configurationMutex);
                width = app_->sharedQueueResources->swapChainWidth;
                height = app_->sharedQueueResources->swapChainHeight;
                refresh = app_->sharedQueueResources->swapChainRate;
                scale_x = app_->sharedQueueResources->resolutionScale.x;
                scale_y = app_->sharedQueueResources->resolutionScale.y;
                downsample = static_cast<std::uint32_t>(std::max(
                    app_->sharedQueueResources->userConfig.downsampleMultiplier,
                    1));

                // Some Wayland swap chains report 0x0 until their first
                // resize. The SDL client size is already final and is a safe
                // startup fallback for diagnostics and scale planning.
                if ((width == 0 || height == 0) &&
                    app_->appWindow->sdlWindow != nullptr) {
                    int window_width = 0;
                    int window_height = 0;
                    SDL_GetWindowSize(app_->appWindow->sdlWindow,
                                      &window_width, &window_height);
                    width = static_cast<std::uint32_t>(
                        std::max(window_width, 0));
                    height = static_cast<std::uint32_t>(
                        std::max(window_height, 0));
                }

                // Before the first workload, RT64 has not populated its
                // shared resolution vector yet. Derive the exact planned
                // window-integer scale so startup diagnostics do not report a
                // fictitious 1x target (or omit SSAA as the old log did).
                if (screen_count == 1 && height > 0 &&
                    app_->sharedQueueResources->userConfig.resolution ==
                        RT64::UserConfiguration::Resolution::WindowIntegerScale) {
                    constexpr float kN64Lines = 240.0f;
                    scale_y = std::max(std::ceil(height / kN64Lines), 1.0f) *
                              downsample;
                    scale_x = scale_y;
                    if (app_->sharedQueueResources->userConfig.aspectRatio ==
                            RT64::UserConfiguration::AspectRatio::Expand &&
                        width > 0) {
                        constexpr float kOriginalAspect = 4.0f / 3.0f;
                        const float output_aspect =
                            static_cast<float>(width) /
                            static_cast<float>(height);
                        scale_x *= std::max(
                            output_aspect / kOriginalAspect, 1.0f);
                    }
                }
            }
            mm::telemetry::set_display_state(width, height, refresh, scale_y);
            if (screen_count == 1) {
                constexpr float kReferenceWidth = 320.0f;
                constexpr float kReferenceHeight = 240.0f;
                mm::telemetry::event("display",
                    "RT64 drawable=%ux%u refresh=%uHz resolution-scale=%.2fx%.2f "
                    "reference-target=%ux%u ssaa=%ux",
                    width, height, refresh, scale_x, scale_y,
                    static_cast<std::uint32_t>(std::lround(
                        kReferenceWidth * scale_x)),
                    static_cast<std::uint32_t>(std::lround(
                        kReferenceHeight * scale_y)),
                    downsample);
            }
        }
    }

    void shutdown() override {
        if (app_ != nullptr) {
            log_present_telemetry(
                app_->presentQueue->takeTelemetrySnapshot());
            mm::telemetry::set_phase(mm::telemetry::Phase::ShuttingDown,
                                     "RT64 shutdown requested");
            mm::telemetry::event("gfx", "RT64 shutdown beginning");
            app_->end();
        }
    }

    uint32_t get_display_framerate() const override {
        return app_->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        // Scale relative to the N64's 240-line output.
        constexpr float kN64Lines = 240.0f;
        switch (app_->userConfig.resolution) {
            case RT64::UserConfiguration::Resolution::Manual:
                return static_cast<float>(app_->userConfig.resolutionMultiplier);
            case RT64::UserConfiguration::Resolution::WindowIntegerScale: {
                const uint32_t height = app_->sharedQueueResources->swapChainHeight;
                return height > 0 ? std::max(std::ceil(height / kN64Lines), 1.0f) : 1.0f;
            }
            default:
                return 1.0f;
        }
    }

private:
    // Backing storage for the RCP state RT64 expects pointers to and nothing
    // else in the recomp provides. Zero-initialized: RSP memories blank, DPC
    // pipeline idle, no MI interrupts pending.
    struct {
        uint8_t header[0x40]{};
        uint8_t dmem[0x1000]{};
        uint8_t imem[0x1000]{};
        uint32_t mi_intr{};
        uint32_t dpc[8]{};
    } regs_;

    std::unique_ptr<RT64::Application> app_;
};

} // namespace

void set_vsync_enabled(bool enabled) {
    g_vsync_enabled.store(enabled, std::memory_order_release);
}

void set_overlay_draw_callback(OverlayDrawCallback callback) {
    g_overlay_draw_callback.store(callback, std::memory_order_release);
}

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<Rt64Context>(rdram, window_handle, developer_mode);
}

void register_callbacks() {
    ultramodern::renderer::set_callbacks(ultramodern::renderer::callbacks_t{
        .create_render_context = create_render_context,
    });
}

} // namespace mm::graphics
