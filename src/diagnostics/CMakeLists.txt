add_library(mm_diagnostics STATIC telemetry.cpp)

target_include_directories(mm_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(mm_diagnostics PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(mm_diagnostics PRIVATE kernel32 psapi)
endif()

if(BUILD_TESTING)
    add_executable(mm_telemetry_tests tests/telemetry_test.cpp)
    target_link_libraries(mm_telemetry_tests PRIVATE mm_diagnostics)
    add_test(NAME mm_telemetry COMMAND mm_telemetry_tests)
endif()
