#ifndef MM_GRAPHICS_H
#define MM_GRAPHICS_H

// mm_graphics — RT64-backed RDP renderer glue for Trouble Makers: Recompiled
// recomp. Implements ultramodern::renderer::RendererContext (the abstract
// render-context interface the runtime calls into) on top of RT64, handing
// F3DEX display lists to RT64's interpreter. See PHASE2_NOTES_w3.md.
//
// Host wiring (src/game): plug mm::graphics::register_callbacks() in before
// recomp::start(), or set renderer_callbacks.create_render_context =
// mm::graphics::create_render_context directly. The runtime then drives
// send_dl / update_screen / update_config / shutdown through the interface.

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace mm::graphics {
    // Creates the concrete RT64-backed render context. Matches the signature
    // of ultramodern::renderer::callbacks_t::create_render_context_t.
    std::unique_ptr<ultramodern::renderer::RendererContext>
        create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // Registers create_render_context (and the API-name helper) with the
    // ultramodern renderer layer. Idempotent; call once before recomp::start().
    void register_callbacks();
}

#endif // MM_GRAPHICS_H
