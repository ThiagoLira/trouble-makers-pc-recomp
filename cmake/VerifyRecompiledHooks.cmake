# Host-side widescreen fixes depend on calls emitted by N64Recomp. Stale
# generated C can link successfully while silently dropping those fixes.
file(GLOB generated_sources "${MM_SOURCE_DIR}/RecompiledFuncs/funcs_*.c")
set(required_hooks
    mm_ws_capture_mid_fill_state
    mm_ws_repeat_wrapped_terrain
    mm_fix_rotation_material
    mm_ws_stretch_backdrop_rect)
foreach(source IN LISTS generated_sources)
    file(STRINGS "${source}" hook_lines REGEX "mm_(ws_|fix_rotation_material)")
    foreach(hook IN LISTS required_hooks)
        string(FIND "${hook_lines}" "${hook}(rdram, ctx)" hook_position)
        if(NOT hook_position EQUAL -1)
            list(REMOVE_ITEM required_hooks "${hook}")
        endif()
    endforeach()
endforeach()
if(required_hooks)
    message(FATAL_ERROR
        "Generated game code is missing widescreen hooks: ${required_hooks}. "
        "Run tools/N64Recomp/build/N64Recomp troublemakers.us1.toml from the "
        "source directory, then reconfigure and rebuild.")
endif()
