# 3.24 for $<LINK_LIBRARY:WHOLE_ARCHIVE,...> (portable whole-archive across
# GNU ld / lld-link / ld64; see src/game/CMakeLists.txt).
cmake_minimum_required(VERSION 3.24)

project(troublemakers_recomp VERSION 0.3.4 LANGUAGES C CXX)
include(CTest)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release builds are configured from a version tag. Derive the user-visible
# version from that exact tag so logs and launcher metadata cannot drift from
# the downloadable release again. Source/development builds fall back to the
# latest declared project version while the commit field keeps them unique.
set(MM_PROJECT_VERSION "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v[0-9]*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE MM_EXACT_VERSION_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(MM_EXACT_VERSION_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(MM_PROJECT_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

if(WIN32)
    # Same workaround as the reference: opt out of the constexpr std::mutex
    # constructor so the binary doesn't require a bleeding-edge vcredist.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    # windows.h min/max macros break std::min/std::max everywhere (the
    # reference sets this too).
    add_compile_definitions(NOMINMAX)
endif()

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

if(NOT WIN32)
    target_compile_options(mm_recompiled PRIVATE
        # 30k+ flat extern-C symbols (including generic names like `send`, from
        # translated static functions) must never enter the executable's dynamic
        # symbol table: a linked shared lib with a matching undefined symbol (e.g.
        # libdbus's send(2)) would resolve to translated MIPS code and crash.
        # (Not a Windows concern: PE executables export nothing by default.)
        -fvisibility=hidden
        -fno-strict-aliasing          # MEM_* macros type-pun rdram bytes
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-builtin-declaration-mismatch
        $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
    )
else()
    # Windows builds require clang-cl (VS "C++ Clang Compiler" component, like
    # the reference project) — MSVC's cl is not supported for the generated C.
    # clang-cl accepts -W* natively; -f flags go through the /clang: prefix.
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(WARNING "mm_recompiled: build with clang-cl on Windows (cl.exe is unsupported for the generated code)")
    endif()
    target_compile_options(mm_recompiled PRIVATE
        /clang:-fno-strict-aliasing   # MEM_* macros type-pun rdram bytes
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-int-conversion>
    )
endif()

# GCC 11 miscompiles part of the translated game. The failure first becomes
# obvious when an NPC dialogue starts: the text box disappears and glyph tiles
# become framebuffer garbage. Lower optimization levels either retain the
# corruption or make the game too slow, while GCC 12 and newer render the same
# dialogue correctly at -O3. Refuse to create another silently broken build.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_C_COMPILER_VERSION VERSION_LESS 12)
    message(FATAL_ERROR
        "GCC 12 or newer is required; GCC 11 corrupts translated game graphics")
endif()

target_include_directories(mm_recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include            # game-specific shim (recomp_config.h, etc.)
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs    # funcs.h, sibling of the .c TUs
    ${RUNTIME_INCLUDES}
)

file(GLOB FUNC_C_SOURCES   ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(mm_recompiled PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# ----------------------------------------------------------------------------
# Phase 2 components — one directory per worker, each with its own
# CMakeLists.txt. Hooks are guarded on existence so the plain mm_recompiled
# gate build keeps working while components are absent. Workers: do NOT edit
# this file — your directory is already included below.
# ----------------------------------------------------------------------------
option(MM_BUILD_PROBE "Link mm_recompiled against librecomp/ultramodern to enumerate unresolved symbols" OFF)

# `game` must come last: its CMakeLists links whichever component targets
# exist (if(TARGET mm_graphics) etc.), so the components must be added first.
set(MM_PHASE2_COMPONENTS diagnostics rsp graphics audio_input game)
set(MM_ANY_COMPONENT OFF)
foreach(_c ${MM_PHASE2_COMPONENTS})
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/${_c}/CMakeLists.txt)
        set(MM_ANY_COMPONENT ON)
    endif()
endforeach()

if(MM_BUILD_PROBE OR MM_ANY_COMPONENT)
    # The runtime platform layer (ultramodern/src/audio.cpp) pulls in SDL2.
    # The runtime's own CMakeLists doesn't find_package it — the host app is
    # expected to. Provide SDL2 on the include path before adding the runtime.
    if(WIN32)
        # No system SDL2 on Windows: fetch the prebuilt VC devel package, as
        # the reference does, and expose it under the same names find_package
        # would (variables + imported targets; mm_graphics links SDL2::SDL2).
        # SDL2.dll is copied next to troublemakers in src/game/CMakeLists.txt.
        include(FetchContent)
        if(DEFINED ENV{SDL2_VERSION})
            set(SDL2_VERSION $ENV{SDL2_VERSION})
        else()
            set(SDL2_VERSION "2.30.3")
        endif()
        FetchContent_Declare(
            sdl2
            URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
        )
        FetchContent_MakeAvailable(sdl2)
        set(SDL2_INCLUDE_DIRS ${sdl2_SOURCE_DIR}/include)
        add_library(SDL2::SDL2 SHARED IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            IMPORTED_LOCATION ${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll
            IMPORTED_IMPLIB   ${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib
            INTERFACE_INCLUDE_DIRECTORIES ${sdl2_SOURCE_DIR}/include
        )
        add_library(SDL2::SDL2main STATIC IMPORTED)
        set_target_properties(SDL2::SDL2main PROPERTIES
            IMPORTED_LOCATION ${sdl2_SOURCE_DIR}/lib/x64/SDL2main.lib
        )
        set(SDL2_LIBRARIES SDL2::SDL2 SDL2::SDL2main)
        # Our code includes <SDL2/SDL.h>; the VC package unpacks headers flat
        # under include/. Mirror them into an SDL2/ prefix dir.
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/sdl2-compat-include/SDL2)
        file(GLOB _sdl2_headers ${sdl2_SOURCE_DIR}/include/*.h)
        file(COPY ${_sdl2_headers} DESTINATION ${CMAKE_BINARY_DIR}/sdl2-compat-include/SDL2)
        list(APPEND SDL2_INCLUDE_DIRS ${CMAKE_BINARY_DIR}/sdl2-compat-include)
    else()
        find_package(SDL2 REQUIRED)
    endif()
    include_directories(${SDL2_INCLUDE_DIRS})

    add_subdirectory(${RUNTIME_DIR} ${CMAKE_BINARY_DIR}/N64ModernRuntime)
endif()

foreach(_c ${MM_PHASE2_COMPONENTS})
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/${_c}/CMakeLists.txt)
        add_subdirectory(src/${_c})
    endif()
endforeach()

# ----------------------------------------------------------------------------
# Stretch: mm_runtime_probe — link the game against the runtime to surface the
# Phase 2 undefined-symbol work plan. Off by default so the Phase 1 gate build
# stays self-contained (no SDL2/RT64 coupling). Enable with -DMM_BUILD_PROBE=ON.
# ----------------------------------------------------------------------------
if(MM_BUILD_PROBE)
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
