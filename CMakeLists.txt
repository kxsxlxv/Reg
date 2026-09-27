cmake_minimum_required(VERSION 3.28)

project(Reg VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REG_FETCH_SDL3 "Fetch a pinned SDL3 release instead of using a system package" ON)
option(REG_ENABLE_VALIDATION "Enable Vulkan validation-layer integration in development builds" ON)
option(REG_BUILD_TESTS "Build lightweight unit tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Vulkan 1.3 REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil)

if(REG_FETCH_SDL3)
    include(FetchContent)

    # OpenGL and OpenGL ES are intentionally disabled. The project is Vulkan-only.
    set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.16
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
else()
    find_package(SDL3 CONFIG REQUIRED)
endif()

if(Vulkan_GLSLC_EXECUTABLE)
    set(REG_GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(
        REG_GLSLC_EXECUTABLE
        NAMES glslc
        HINTS "$ENV{VULKAN_SDK}"
        PATH_SUFFIXES Bin bin
        REQUIRED
    )
endif()

set(REG_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${REG_SHADER_OUTPUT_DIR}")

set(REG_SHADER_OUTPUTS)
foreach(_shader IN ITEMS video.vert video.frag)
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}")
    set(_output "${REG_SHADER_OUTPUT_DIR}/${_shader}.spv")

    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${REG_GLSLC_EXECUTABLE}" "${_source}" -o "${_output}"
        DEPENDS "${_source}"
        COMMENT "Compiling Vulkan shader ${_shader}"
        VERBATIM
    )
    list(APPEND REG_SHADER_OUTPUTS "${_output}")
endforeach()

add_custom_target(reg_shaders DEPENDS ${REG_SHADER_OUTPUTS})

add_executable(reg_probe
    src/main.cpp
    src/app/CommandLine.cpp
    src/platform/SDLPlatform.cpp
    src/vulkan/VulkanContext.cpp
    src/vulkan/Swapchain.cpp
    src/vulkan/VulkanVideoFrameAccess.cpp
    src/vulkan/VideoRenderer.cpp
    src/media/FrameIdentity.cpp
    src/media/VulkanHwDevice.cpp
    src/media/RtspDecoder.cpp
    src/video/VideoFrame.cpp
)

add_dependencies(reg_probe reg_shaders)

target_include_directories(reg_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(reg_probe PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
)

target_compile_definitions(reg_probe PRIVATE
    REG_ENABLE_VALIDATION=$<BOOL:${REG_ENABLE_VALIDATION}>
    REG_SHADER_DIR=\"${REG_SHADER_OUTPUT_DIR}\"
)

if(MSVC)
    target_compile_options(reg_probe PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()


if(REG_BUILD_TESTS)
    enable_testing()

    add_executable(reg_frame_identity_tests
        tests/frame_identity_tests.cpp
        src/media/FrameIdentity.cpp
    )
    target_include_directories(reg_frame_identity_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_frame_identity_tests PRIVATE FFmpeg::avutil)

    if(MSVC)
        target_compile_options(reg_frame_identity_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_frame_identity_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME frame_identity COMMAND reg_frame_identity_tests)

    add_executable(reg_frame_sync_tests
        tests/frame_sync_tests.cpp
        src/metadata/MetadataStore.cpp
    )
    target_include_directories(reg_frame_sync_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_frame_sync_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_frame_sync_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME frame_sync COMMAND reg_frame_sync_tests)

    add_executable(reg_metadata_protocol_tests
        tests/metadata_protocol_tests.cpp
        src/metadata/MetadataProtocol.cpp
    )
    target_include_directories(reg_metadata_protocol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_metadata_protocol_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_metadata_protocol_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME metadata_protocol COMMAND reg_metadata_protocol_tests)

    add_executable(reg_packet_sequence_tests
        tests/packet_sequence_tests.cpp
        src/metadata/PacketSequenceTracker.cpp
    )
    target_include_directories(reg_packet_sequence_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_packet_sequence_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_packet_sequence_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME packet_sequence COMMAND reg_packet_sequence_tests)

    find_package(Threads REQUIRED)

    add_executable(reg_udp_metadata_receiver_tests
        tests/udp_metadata_receiver_tests.cpp
        src/metadata/UdpMetadataReceiver.cpp
        src/metadata/MetadataProtocol.cpp
        src/metadata/PacketSequenceTracker.cpp
    )
    target_include_directories(reg_udp_metadata_receiver_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_udp_metadata_receiver_tests PRIVATE Threads::Threads)

    if(WIN32)
        target_link_libraries(reg_udp_metadata_receiver_tests PRIVATE ws2_32)
    endif()

    if(MSVC)
        target_compile_options(reg_udp_metadata_receiver_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_udp_metadata_receiver_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME udp_metadata_receiver COMMAND reg_udp_metadata_receiver_tests)
endif()


find_package(Threads REQUIRED)

add_executable(reg_metadata_probe
    src/tools/metadata_udp_probe.cpp
    src/metadata/UdpMetadataReceiver.cpp
    src/metadata/MetadataProtocol.cpp
    src/metadata/PacketSequenceTracker.cpp
)

target_include_directories(reg_metadata_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(reg_metadata_probe PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(reg_metadata_probe PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(reg_metadata_probe PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_metadata_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
