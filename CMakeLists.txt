cmake_minimum_required(VERSION 3.20)

project(troublemakers_recomp LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output beside the build dir, not into source.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Path shortcuts into the runtime submodule.
set(RUNTIME_DIR ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
set(RUNTIME_INCLUDES
    ${RUNTIME_DIR}/N64Recomp/include        # recomp.h (macros, recomp_context, gpr, fpr)
    ${RUNTIME_DIR}/librecomp/include        # librecomp/*.hpp, sections.h (FuncEntry, SectionTableEntry)
    ${RUNTIME_DIR}/ultramodern/include      # ultramodern/*.hpp
)

# ----------------------------------------------------------------------------
# mm_recompiled — the whole translated game as host-native objects.
#
# Phase 1 success gate: this static library must compile every file under
# RecompiledFuncs/ with zero errors. The generated C is regeneration output;
# fixes belong in the recompiler config or these headers, never hand-edits to
# RecompiledFuncs/*. See PHASE1_NOTES.md.
#
# Layout of RecompiledFuncs/ (emitted by tools/N64Recomp):
#   funcs_NN.c       91 translation units, each #include "recomp.h" + "funcs.h"
#   funcs.h          declarations for every recompiled function (extern "C")
#   lookup.cpp       get_entrypoint_address() + get_rom_name() (librecomp glue)
#   recomp_overlays.inl  section/overlay lookup tables (NOT compiled here —
#                       #included by a runtime register_overlays() TU in Phase 2)
# ----------------------------------------------------------------------------
add_library(mm_recompiled STATIC)

target_compile_options(mm_recompiled PRIVATE
    -fno-strict-aliasing          # MEM_* macros type-pun rdram bytes
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-builtin-declaration-mismatch
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
    $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
)

target_include_directories(mm_recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include            # game-specific shim (recomp_config.h, etc.)
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs    # funcs.h, sibling of the .c TUs
    ${RUNTIME_INCLUDES}
)

file(GLOB FUNC_C_SOURCES   ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(mm_recompiled PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# ----------------------------------------------------------------------------
# Stretch: mm_runtime_probe — link the game against the runtime to surface the
# Phase 2 undefined-symbol work plan. Off by default so the Phase 1 gate build
# stays self-contained (no SDL2/RT64 coupling). Enable with -DMM_BUILD_PROBE=ON.
# ----------------------------------------------------------------------------
option(MM_BUILD_PROBE "Link mm_recompiled against librecomp/ultramodern to enumerate unresolved symbols" OFF)
if(MM_BUILD_PROBE)
    # The runtime platform layer (ultramodern/src/audio.cpp) pulls in SDL2.
    # The runtime's own CMakeLists doesn't find_package it — the host app is
    # expected to. Provide SDL2 on the include path before adding the runtime.
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})

    add_subdirectory(${RUNTIME_DIR} ${CMAKE_BINARY_DIR}/N64ModernRuntime)

    add_executable(mm_runtime_probe
        ${CMAKE_SOURCE_DIR}/src/probe_main.cpp
        ${CMAKE_SOURCE_DIR}/src/probe_glue.cpp
    )
    target_include_directories(mm_runtime_probe PRIVATE ${RUNTIME_INCLUDES})
    # Force-include every object from the static libs (not just the transitive
    # closure reachable from main()) so the *whole* game + whole runtime get
    # linked. With the 2 stubs in probe_glue.cpp every game/runtime reference
    # resolves, so this links cleanly under --no-undefined. (The original
    # undefined set — rmonPrintf_recomp, __osGetCause_recomp — is documented in
    # PHASE1_NOTES.md as the only genuine link-time gaps; the larger Phase 2
    # graphics/audio/RSP/entry work uses abstract interfaces that need runtime
    # *registration*, not link-time symbols, so it does not surface here.)
    target_link_libraries(mm_runtime_probe PRIVATE
        "-Wl,--whole-archive"
        mm_recompiled librecomp ultramodern
        "-Wl,--no-whole-archive"
        ${SDL2_LIBRARIES}
    )
    target_link_options(mm_runtime_probe PRIVATE -Wl,--no-undefined)
endif()
