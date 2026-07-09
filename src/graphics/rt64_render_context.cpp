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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifndef HLSL_CPU
#define HLSL_CPU
#endif
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "mm_graphics.h"

namespace mm::graphics {

namespace {

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
        RT64::Application::Core core{};
        core.window = window; // WindowHandle is SDL_Window* on Linux, matching RT64's SDL backend.
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
        app_->userConfig.developerMode = developer_mode;

        setup_result = to_setup_result(app_->setup(0));
        chosen_api = from_rt64_api(app_->chosenGraphicsAPI);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            app_.reset();
            return;
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
        // TEMP DIAGNOSTIC (pixels bring-up): trace DL traffic.
        static uint64_t dl_count = 0;
        if (dl_count < 3 || dl_count % 10 == 0) {
            std::fprintf(stderr, "[gfx] send_dl #%llu ucode=%08X data_ptr=%08X data_size=%u\n",
                (unsigned long long)dl_count, task->t.ucode, task->t.data_ptr, task->t.data_size);
        }
        dl_count++;
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(physical(task->t.ucode), physical(task->t.ucode_data), true);
        app_->processDisplayLists(app_->core.RDRAM, physical(task->t.data_ptr), 0, true);
    }

    void update_screen() override {
        // TEMP DIAGNOSTIC (pixels bring-up): trace VI scanout state.
        static uint64_t vi_count = 0;
        if (vi_count < 3 || vi_count % 600 == 0) {
            ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
            std::fprintf(stderr, "[gfx] update_screen #%llu VI_ORIGIN=%08X VI_WIDTH=%u VI_STATUS=%08X\n",
                (unsigned long long)vi_count, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG, vi->VI_STATUS_REG);
        }
        vi_count++;
        app_->updateScreen();
    }

    void shutdown() override {
        if (app_ != nullptr) {
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
