#ifndef MM_GRAPHICS_H
#define MM_GRAPHICS_H

// mm_graphics — RT64-backed RDP renderer glue for Trouble Makers: Recompiled
// recomp. Implements ultramodern::renderer::RendererContext (the abstract
// render-context interface the runtime calls into) on top of RT64, handing
// gspFast3D display lists to RT64's interpreter. See docs/README.md.
//
// Host wiring (src/game): plug mm::graphics::register_callbacks() in before
// recomp::start(), or set renderer_callbacks.create_render_context =
// mm::graphics::create_render_context directly. The runtime then drives
// send_dl / update_screen / update_config / shutdown through the interface.

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace mm::graphics {
    using OverlayDrawCallback = void (*)();

    // Creates the concrete RT64-backed render context. Matches the signature
    // of ultramodern::renderer::callbacks_t::create_render_context_t.
    std::unique_ptr<ultramodern::renderer::RendererContext>
        create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // Optional post-present UI callback. RT64's existing cross-API ImGui
    // renderer draws it after the N64 framebuffer and before presentation.
    // Set before recomp::start(); nullptr disables the overlay pass entirely.
    void set_overlay_draw_callback(OverlayDrawCallback callback);

    // Whether presentation waits for vertical sync (default on). Set before
    // recomp::start(); the render context applies it to the swap chain during
    // setup. Off removes the display-sync wait (D3D12 Present(0), Vulkan
    // immediate mode where the surface supports it).
    void set_vsync_enabled(bool enabled);

    // Temporarily present only native game frames while leaving the user's
    // saved refresh-rate selection untouched. Intended for a narrowly scoped
    // scene compatibility fallback; changing the value is thread-safe.
    void set_interpolation_suppressed(bool suppressed);

    // Window width relative to the original 4:3 canvas, published by the
    // renderer for game-thread background layout. Never less than one.
    float get_widescreen_scale();

    // Registers create_render_context (and the API-name helper) with the
    // ultramodern renderer layer. Idempotent; call once before recomp::start().
    void register_callbacks();
}

#endif // MM_GRAPHICS_H
