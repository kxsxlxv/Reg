# Independent of Reg's Vulkan, FFmpeg, SDL and RTSP requirements.
# Configure directly with: cmake -S src/diagnostics/stats -B build/diag-core
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(RegDiagnosticsCore LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(REG_BUILD_TESTS "Build diagnostics tests" ON)
    enable_testing()
endif()

get_filename_component(REG_STATS_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(REG_STATS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(reg_diagnostics_core STATIC Statistics.cpp)
target_include_directories(reg_diagnostics_core PUBLIC "${REG_STATS_SRC_ROOT}")
target_compile_features(reg_diagnostics_core PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(reg_diagnostics_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_diagnostics_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(REG_BUILD_TESTS)
    add_executable(reg_diagnostic_statistics_tests
        "${REG_STATS_REPO_ROOT}/tests/diagnostic_statistics_tests.cpp")
    target_link_libraries(reg_diagnostic_statistics_tests PRIVATE reg_diagnostics_core)
    if(MSVC)
        target_compile_options(reg_diagnostic_statistics_tests PRIVATE /W4 /permissive- /EHsc /UNDEBUG)
    else()
        target_compile_options(reg_diagnostic_statistics_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -UNDEBUG)
    endif()
    add_test(NAME diagnostic_statistics COMMAND reg_diagnostic_statistics_tests)
endif()
