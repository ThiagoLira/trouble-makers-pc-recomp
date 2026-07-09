# src/graphics — RT64-backed RDP renderer glue (worker w3).
#
# Builds `mm_graphics`, a static library implementing
# ultramodern::renderer::RendererContext on top of RT64. The host app
# (src/game) plugs mm::graphics::register_callbacks() into the runtime's
# renderer callbacks before recomp::start(); the runtime then drives the
# F3DEX display-list / screen-update lifecycle through this interface.
#
# RT64 is a heavy dependency (HLSL->SPIR-V shader compilation via the bundled
# DXC, full Vulkan RHI). To keep `cmake -B build_w3` from ever breaking the
# tree for sibling workers, the whole component is gated behind
# MM_BUILD_GRAPHICS. It defaults ON when RT64's sources are present and SDL2
# is available, and degrades to a clear message + no target otherwise.

option(MM_BUILD_GRAPHICS "Build the RT64-backed graphics glue (mm_graphics)" ON)

if(NOT MM_BUILD_GRAPHICS)
    message(STATUS "mm_graphics: disabled (MM_BUILD_GRAPHICS=OFF). RDP rendering via RT64 is not built.")
    return()
endif()

# RT64 sources must be cloned into lib/rt64 by the worker (gitignored).
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/lib/rt64/CMakeLists.txt)
    message(WARNING
        "mm_graphics: lib/rt64/CMakeLists.txt not found — RT64 fork not cloned.\n"
        "  Clone the reference's pinned RT64 fork (https://github.com/rt64/rt64 @ 23cab603)\n"
        "  into lib/rt64 and init its submodules, or set -DMM_BUILD_GRAPHICS=OFF.\n"
        "  Skipping mm_graphics.")
    return()
endif()

# SDL2 is required by RT64 (and by ultramodern). The root CMakeLists already
# finds it before adding any Phase 2 component, but be explicit so this
# component is self-describing when built standalone.
if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 QUIET)
endif()
if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    message(WARNING
        "mm_graphics: SDL2 not found. RT64 and the ultramodern platform layer both require it.\n"
        "  Install SDL2 (e.g. 'pacman -S sdl2') or set -DMM_BUILD_GRAPHICS=OFF. Skipping mm_graphics.")
    return()
endif()

# ---------------------------------------------------------------------------
# RT64 — build as a static library with the SDL/Vulkan window backend, exactly
# as the reference (Zelda64Recomp) does. These variables must be set before
# add_subdirectory so RT64's own CMakeLists picks them up. RT64 bundles its
# own Vulkan headers and DXC shader compiler, so no system Vulkan SDK is
# required — only the Vulkan loader (libvulkan.so) at link/runtime.
# ---------------------------------------------------------------------------
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# RT64's own include_directories() are directory-scoped (not PUBLIC on the
# target), so they do not propagate to consumers. Its public headers
# (#include "vulkan/vulkan.h", vk_mem_alloc.h, hlslpp, imgui, ...) expect the
# full contrib include set on the path — mirror it here. The reference does
# the same duplication (see Zelda64Recomp CMakeLists.txt ~L217).
set(RT64_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/D3D12MemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/D3D12MemoryAllocator/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-core/src/api
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
)

# ---------------------------------------------------------------------------
# mm_graphics — the glue. RT64 headers require HLSL_CPU at compile time; the
# reference defines it globally before add_subdirectory(rt64). We scope it to
# this target only so it does not leak to sibling components.
# ---------------------------------------------------------------------------
add_library(mm_graphics STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rt64_render_context.cpp
)

target_include_directories(mm_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}                 # mm_graphics.h
)

target_include_directories(mm_graphics PRIVATE
    ${RT64_INCLUDE_DIRS}
)

target_compile_definitions(mm_graphics PRIVATE
    HLSL_CPU                                    # required by RT64's HLSL-derived headers
    RT64_SDL_WINDOW_VULKAN                     # RT64::RenderWindow -> SDL_Window* (matches ultramodern::renderer::WindowHandle on Linux)
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES            # RT64 loads Vulkan via volk at runtime
)

target_link_libraries(mm_graphics PUBLIC
    ultramodern                                 # RendererContext interface, ViRegs, GraphicsConfig (PUBLIC includes)
)

target_link_libraries(mm_graphics PRIVATE
    rt64
    SDL2::SDL2
)
