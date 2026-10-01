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

include(RegFonts)

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

add_library(reg_imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(reg_imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)

target_link_libraries(reg_imgui PUBLIC Vulkan::Vulkan)

add_library(reg_implot STATIC
    implot/implot.cpp
    implot/implot_items.cpp
)

target_include_directories(reg_implot PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/implot"
)

target_link_libraries(reg_implot PUBLIC reg_imgui)

add_library(reg_frame_identity_source STATIC
    src/media/FrameIdentity.cpp
    src/media/FrameIdentitySei.cpp
    src/source/FrameIdentityAccessUnitInjector.cpp
)

target_include_directories(reg_frame_identity_source PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(MSVC)
    target_compile_options(reg_frame_identity_source PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_frame_identity_source PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(reg_metadata_sender STATIC
    src/metadata/Protocol.cpp
    src/metadata/MetadataSender.cpp
    src/platform/UdpSender.cpp
)

target_include_directories(reg_metadata_sender PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(WIN32)
    target_link_libraries(reg_metadata_sender PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(reg_metadata_sender PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_metadata_sender PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(reg_jetson_metadata_bridge STATIC
    src/media/FrameIdentity.cpp
    src/media/FrameIdentityAvFrame.cpp
    src/jetson/MetadataBridge.cpp
)

target_include_directories(reg_jetson_metadata_bridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(reg_jetson_metadata_bridge PUBLIC
    reg_metadata_sender
    FFmpeg::avutil
)

if(MSVC)
    target_compile_options(reg_jetson_metadata_bridge PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_jetson_metadata_bridge PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(Vulkan_GLSLC_EXECUTABLE)
    set(REG_GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(
        REG_GLSLC_EXECUTABLE
        NAMES glslc
        HINTS "$ENV{VULKAN_SDK}"
        PATH_SUFFIXES Bin bin
    )
endif()

if(NOT REG_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "glslc was not found. Reg requires the Shaderc compiler for Vulkan shaders.\n"
        "Windows/MSYS2 UCRT64: install mingw-w64-ucrt-x86_64-shaderc and configure "
        "from the UCRT64 shell (or run scripts/windows/bootstrap-ucrt64.ps1).\n"
        "LunarG Vulkan SDK is also supported when VULKAN_SDK points to the SDK root."
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
    src/capture/BmpWriter.cpp
    src/app/CommandLine.cpp
    src/platform/SDLPlatform.cpp
    src/vulkan/VulkanContext.cpp
    src/vulkan/RenderWindow.cpp
    src/vulkan/Swapchain.cpp
    src/vulkan/VulkanVideoFrameAccess.cpp
    src/vulkan/VideoRenderer.cpp
    src/vulkan/VideoRendererBlank.cpp
    src/media/CompressedVideoPacket.cpp
    src/media/FrameIdentity.cpp
    src/media/FrameIdentityAvFrame.cpp
    src/media/VideoStreamDescriptor.cpp
    src/media/VulkanHwDevice.cpp
    src/media/RtspDecoder.cpp
    src/metadata/Protocol.cpp
    src/metadata/MetadataStore.cpp
    src/metadata/MetadataReceiver.cpp
    src/metadata/TrackHistory.cpp
    src/platform/UdpSocket.cpp
    src/video/VideoFrame.cpp
    src/video/OverlayFrameBuffer.cpp
    src/video/FrameSynchronizer.cpp
    src/video/VideoTransform.cpp
    src/render/TargetOverlayBuilder.cpp
    src/render/ImGuiTheme.cpp
    src/render/ImGuiOverlayRenderer.cpp
    src/render/ImGuiTelemetryRenderer.cpp
    src/telemetry/TelemetryModel.cpp
    src/recorder/SegmentedMkvWriter.cpp
    src/recorder/MetadataJournal.cpp
    src/recorder/BlackboxRecorder.cpp
)

add_dependencies(reg_probe reg_shaders)

target_include_directories(reg_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(reg_probe PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    reg_imgui
    reg_implot
)

if(WIN32)
    target_link_libraries(reg_probe PRIVATE ws2_32)
endif()

target_compile_definitions(reg_probe PRIVATE
    REG_ENABLE_VALIDATION=$<BOOL:${REG_ENABLE_VALIDATION}>
    REG_SHADER_DIR=\"${REG_SHADER_OUTPUT_DIR}\"
    REG_FONT_DIR=\"${REG_FONT_OUTPUT_DIR}\"
)

if(MSVC)
    target_compile_options(reg_probe PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(reg_replay
    src/replay_main.cpp
    src/capture/BmpWriter.cpp
    src/platform/SDLPlatform.cpp
    src/vulkan/VulkanContext.cpp
    src/vulkan/RenderWindow.cpp
    src/vulkan/Swapchain.cpp
    src/vulkan/VulkanVideoFrameAccess.cpp
    src/vulkan/VideoRenderer.cpp
    src/media/CompressedVideoPacket.cpp
    src/media/FrameIdentity.cpp
    src/media/FrameIdentityAvFrame.cpp
    src/media/VideoStreamDescriptor.cpp
    src/media/VulkanHwDevice.cpp
    src/media/RtspDecoder.cpp
    src/metadata/Protocol.cpp
    src/metadata/TrackHistory.cpp
    src/video/VideoFrame.cpp
    src/video/VideoTransform.cpp
    src/render/TargetOverlayBuilder.cpp
    src/render/ImGuiTheme.cpp
    src/render/ImGuiOverlayRenderer.cpp
    src/recorder/MetadataJournal.cpp
    src/replay/ReplayMetadataIndex.cpp
    src/replay/ReplayClock.cpp
)

add_dependencies(reg_replay reg_shaders)

target_include_directories(reg_replay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(reg_replay PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    reg_imgui
)

if(WIN32)
    target_link_libraries(reg_replay PRIVATE ws2_32)
endif()

target_compile_definitions(reg_replay PRIVATE
    REG_ENABLE_VALIDATION=$<BOOL:${REG_ENABLE_VALIDATION}>
    REG_SHADER_DIR=\"${REG_SHADER_OUTPUT_DIR}\"
    REG_FONT_DIR=\"${REG_FONT_OUTPUT_DIR}\"
)

if(MSVC)
    target_compile_options(reg_replay PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_replay PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(REG_BUILD_TESTS)
    enable_testing()

    add_executable(reg_source_frame_identity_injector_tests
        tests/source_frame_identity_injector_tests.cpp
    )
    target_include_directories(reg_source_frame_identity_injector_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_source_frame_identity_injector_tests PRIVATE
        reg_frame_identity_source
    )
    if(MSVC)
        target_compile_options(reg_source_frame_identity_injector_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_source_frame_identity_injector_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME source_frame_identity_injector COMMAND reg_source_frame_identity_injector_tests)

    add_executable(reg_frame_identity_tests
        tests/frame_identity_tests.cpp
        src/media/FrameIdentity.cpp
        src/media/FrameIdentityAvFrame.cpp
        src/media/FrameIdentitySei.cpp
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
        src/metadata/Protocol.cpp
        src/metadata/MetadataStore.cpp
        src/metadata/MetadataReceiver.cpp
        src/platform/UdpSocket.cpp
        src/video/OverlayFrameBuffer.cpp
        src/video/FrameSynchronizer.cpp
    )
    target_include_directories(reg_frame_sync_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_frame_sync_tests PRIVATE FFmpeg::avutil Vulkan::Vulkan)
    if(WIN32)
        target_link_libraries(reg_frame_sync_tests PRIVATE ws2_32)
    endif()

    if(MSVC)
        target_compile_options(reg_frame_sync_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_frame_sync_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME frame_sync COMMAND reg_frame_sync_tests)

    add_executable(reg_metadata_sender_tests
        tests/metadata_sender_tests.cpp
        src/platform/UdpSocket.cpp
    )
    target_include_directories(reg_metadata_sender_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_metadata_sender_tests PRIVATE reg_metadata_sender)

    if(MSVC)
        target_compile_options(reg_metadata_sender_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_metadata_sender_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME metadata_sender COMMAND reg_metadata_sender_tests)

    add_executable(reg_jetson_metadata_bridge_tests
        tests/jetson_metadata_bridge_tests.cpp
        src/platform/UdpSocket.cpp
    )
    target_include_directories(reg_jetson_metadata_bridge_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_jetson_metadata_bridge_tests PRIVATE
        reg_jetson_metadata_bridge
        FFmpeg::avutil
    )

    if(MSVC)
        target_compile_options(reg_jetson_metadata_bridge_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_jetson_metadata_bridge_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME jetson_metadata_bridge COMMAND reg_jetson_metadata_bridge_tests)

    add_executable(reg_overlay_model_tests
        tests/overlay_model_tests.cpp
        src/metadata/TrackHistory.cpp
        src/video/VideoTransform.cpp
        src/render/TargetOverlayBuilder.cpp
    )
    target_include_directories(reg_overlay_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_overlay_model_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_overlay_model_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME overlay_model COMMAND reg_overlay_model_tests)

    add_executable(reg_blackbox_recorder_tests
        tests/blackbox_recorder_tests.cpp
        src/media/CompressedVideoPacket.cpp
        src/media/VideoStreamDescriptor.cpp
        src/metadata/Protocol.cpp
        src/recorder/SegmentedMkvWriter.cpp
        src/recorder/MetadataJournal.cpp
    )
    target_include_directories(reg_blackbox_recorder_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_blackbox_recorder_tests PRIVATE
        FFmpeg::avcodec
        FFmpeg::avformat
        FFmpeg::avutil
    )

    if(MSVC)
        target_compile_options(reg_blackbox_recorder_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_blackbox_recorder_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME blackbox_recorder COMMAND reg_blackbox_recorder_tests)

    add_executable(reg_telemetry_model_tests
        tests/telemetry_model_tests.cpp
        src/telemetry/TelemetryModel.cpp
    )
    target_include_directories(reg_telemetry_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_telemetry_model_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_telemetry_model_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME telemetry_model COMMAND reg_telemetry_model_tests)

    add_executable(reg_replay_foundation_tests
        tests/replay_foundation_tests.cpp
        src/metadata/Protocol.cpp
        src/recorder/MetadataJournal.cpp
        src/replay/ReplayMetadataIndex.cpp
        src/replay/ReplayClock.cpp
    )
    target_include_directories(reg_replay_foundation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_replay_foundation_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_replay_foundation_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME replay_foundation COMMAND reg_replay_foundation_tests)

    add_executable(reg_vulkan_error_tests
        tests/vulkan_error_tests.cpp
    )
    target_include_directories(reg_vulkan_error_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_vulkan_error_tests PRIVATE Vulkan::Vulkan)

    if(MSVC)
        target_compile_options(reg_vulkan_error_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_vulkan_error_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME vulkan_error COMMAND reg_vulkan_error_tests)

    add_executable(reg_bmp_writer_tests
        tests/bmp_writer_tests.cpp
        src/capture/BmpWriter.cpp
    )
    target_include_directories(reg_bmp_writer_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MSVC)
        target_compile_options(reg_bmp_writer_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_bmp_writer_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME bmp_writer COMMAND reg_bmp_writer_tests)
endif()
