# src/game/CMakeLists.txt — Phase 2 worker w1 (game core).
#
# Builds `mm_game`, the host executable that wires the translated game
# (mm_recompiled) to the librecomp/ultramodern runtime and launches it.
#
# The root CMakeLists.txt already: provides RUNTIME_INCLUDES, finds SDL2, adds
# the N64ModernRuntime subdirectory, and add_subdirectory()s this directory
# when this file exists. So this file only declares the mm_game target.
#
# Link strategy mirrors the proven-working mm_runtime_probe (PHASE1_NOTES.md):
# mm_recompiled is whole-archive linked because the recompiled game is
# data-driven (recomp_overlays.inl's FuncEntry tables hold function pointers
# that no normal call graph reaches) — without --whole-archive the linker
# would strip nearly all of the game. librecomp + ultramodern are also
# whole-archive linked to match the probe's clean link under --no-undefined.
# The two OS wrappers the game needs (rmonPrintf_recomp, __osGetCause_recomp)
# are provided by os_stubs.cpp in this target.

add_executable(mm_game
    main.cpp
    register_overlays.cpp
    os_stubs.cpp
)

target_include_directories(mm_game PRIVATE
    ${RUNTIME_INCLUDES}                 # recomp.h, librecomp/*.hpp, ultramodern/*.hpp
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs # funcs.h, recomp_overlays.inl (siblings of the .c TUs)
    ${CMAKE_SOURCE_DIR}/include         # recomp_config.h (game shim)
)

# Component libraries are reached through the callback structs main() builds,
# so normal linking suffices for them — only the game + runtime need
# --whole-archive (data-driven function tables, see above).
target_link_libraries(mm_game PRIVATE
    "-Wl,--whole-archive"
    mm_recompiled librecomp ultramodern
    "-Wl,--no-whole-archive"
    mm_rsp
    mm_audio_input
    ${SDL2_LIBRARIES}
)

# The RT64 renderer is optional (MM_BUILD_GRAPHICS gates it; see
# src/graphics/CMakeLists.txt). When absent, main.cpp falls back to its stub
# renderer, which aborts the gfx thread cleanly at startup.
if(TARGET mm_graphics)
    target_link_libraries(mm_game PRIVATE mm_graphics)
    target_compile_definitions(mm_game PRIVATE MM_HAS_GRAPHICS)

    # Launcher splash screen (launcher.cpp): Dear ImGui + nativefiledialog-
    # extended, both vendored and compiled inside RT64 (rt64.a provides the
    # imgui core and the SDL2 platform backend; nfd is its own target). Only
    # the SDL_Renderer backend TU is missing from rt64, so compile it here.
    target_sources(mm_game PRIVATE
        launcher.cpp
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(mm_game PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/imgui                                      # imgui_impl_sdl2_custom.h
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    )
    target_link_libraries(mm_game PRIVATE nfd)
endif()

# Enforce no unresolved symbols (matches the probe; surfaces any new gap).
target_link_options(mm_game PRIVATE -Wl,--no-undefined)
