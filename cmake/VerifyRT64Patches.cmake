# Used at configure time and before compiling RT64. Its checkout is ignored
# by the main repository, so changing branches does not update applied fixes.
find_package(Git REQUIRED QUIET)

file(GLOB rt64_patches "${MM_RT64_PATCH_DIR}/*.patch")
list(SORT rt64_patches)
if(NOT rt64_patches)
    message(FATAL_ERROR "No RT64 patches found in ${MM_RT64_PATCH_DIR}")
endif()

foreach(patch IN LISTS rt64_patches)
    # A reverse dry run verifies that the patch's changes are present without
    # writing to the checkout. Later patches can change earlier context lines
    # (e.g. the VI option beside rect.clearWings), so allow reduced context;
    # the actual added/removed lines must still match. Accept Windows CRLF.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check -C0 --ignore-whitespace "${patch}"
        WORKING_DIRECTORY "${MM_RT64_SOURCE_DIR}"
        RESULT_VARIABLE patch_result
        OUTPUT_QUIET
        ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
        get_filename_component(patch_name "${patch}" NAME)
        message(FATAL_ERROR
            "RT64 is missing or differs from required patch ${patch_name}.\n"
            "The ignored lib/rt64 checkout must include the project's current fixes.\n"
            "Apply the missing patch or reconcile local edits before rebuilding;\n"
            "see README.md, Building and running. No source files were changed.\n"
            "${patch_error}")
    endif()
endforeach()
