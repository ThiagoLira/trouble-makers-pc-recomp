# mm_audio_input — Phase 2 audio + input + save glue (worker w4).
#
# Backs ultramodern's audio_callbacks_t and input::callbacks_t with SDL2 and
# exposes the game's EEPROM save type. This is a STATIC lib: it leaves
# ultramodern/librecomp symbols undefined (resolved when w1's main links the
# runtime), so building the .a alone never fails the gate on missing symbols.
#
# SDL2 is found and put on the global include path by the root CMakeLists
# before this directory is added. Linking librecomp + ultramodern pulls in
# their PUBLIC include trees (thirdparty/ for json.hpp + concurrentqueue's
# lightweightsemaphore.h, used transitively by ultramodern.hpp).
add_library(mm_audio_input STATIC
    audio.cpp
    controls.cpp
    input.cpp
    save.cpp
)

target_include_directories(mm_audio_input PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # librecomp/game.hpp #includes "recomp.h", which lives here. librecomp links
    # the N64Recomp target PRIVATE, so this path isn't propagated to consumers —
    # add it ourselves (matches the root's RUNTIME_INCLUDES).
    ${RUNTIME_DIR}/N64Recomp/include
)

target_link_libraries(mm_audio_input PUBLIC
    librecomp
    ultramodern
    mm_diagnostics
    SDL2::SDL2
)

if(BUILD_TESTING)
    add_executable(mm_controls_tests tests/controls_test.cpp)
    target_compile_definitions(mm_controls_tests PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(mm_controls_tests PRIVATE mm_audio_input)
    add_test(NAME mm_controls COMMAND mm_controls_tests)
endif()
