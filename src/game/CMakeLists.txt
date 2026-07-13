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
    app_dirs.cpp
    register_overlays.cpp
    os_stubs.cpp
    widescreen.cpp
)

target_include_directories(mm_game PRIVATE
    ${RUNTIME_INCLUDES}                 # recomp.h, librecomp/*.hpp, ultramodern/*.hpp
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs # funcs.h, recomp_overlays.inl (siblings of the .c TUs)
    ${CMAKE_SOURCE_DIR}/include         # recomp_config.h (game shim)
)

# Component libraries are reached through the callback structs main() builds,
# so normal linking suffices for them — only the game + runtime need
# whole-archive (data-driven function tables, see above). The LINK_LIBRARY
# generator expression emits --whole-archive / /WHOLEARCHIVE / -force_load
# per linker (needs CMake 3.24; set at the top-level CMakeLists).
target_link_libraries(mm_game PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,mm_recompiled,librecomp,ultramodern>"
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
# GNU-ld flag; MSVC's link.exe already hard-errors on undefined symbols.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(mm_game PRIVATE -Wl,--no-undefined)
endif()

if(WIN32)
    # Ship the runtime DLLs next to the exe: SDL2 (fetched in the top-level
    # CMakeLists) and RT64's bundled DXC shader compiler. Same set the
    # reference copies.
    add_custom_command(TARGET mm_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:SDL2::SDL2,IMPORTED_LOCATION>"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:mm_game>)
    # GUI subsystem for release so no console window flashes; keep the
    # console in Debug for logs. mainCRTStartup keeps our plain main().
    set_target_properties(mm_game PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()
