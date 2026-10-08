cmake_minimum_required(VERSION 3.28)

project(Reg VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REG_FETCH_SDL3 "Fetch a pinned SDL3 release instead of using a system package" ON)
option(REG_ENABLE_VALIDATION "Enable Vulkan validation-layer integration in development builds" ON)
option(REG_BUILD_TESTS "Build lightweight unit tests" ON)
option(REG_ENABLE_NETIMGUI_REMOTE "Build the experimental NetImgui remote-UI integration" OFF)

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

if(REG_ENABLE_NETIMGUI_REMOTE)
    include(RegNetImgui)
    reg_add_netimgui_client_compat_target()
endif()

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
    src/diagnostics/FrameTimingLogger.cpp
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

    add_executable(reg_reconnect_render_contract_tests
        tests/reconnect_render_contract_tests.cpp
    )
    target_include_directories(reg_reconnect_render_contract_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_reconnect_render_contract_tests
        PRIVATE Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(reg_reconnect_render_contract_tests
            PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_reconnect_render_contract_tests
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    add_test(NAME reconnect_render_contract COMMAND reg_reconnect_render_contract_tests)

    add_executable(reg_exact_sync_diagnostics_tests
        tests/exact_sync_diagnostics_tests.cpp
    )
    target_include_directories(reg_exact_sync_diagnostics_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(reg_exact_sync_diagnostics_tests
            PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_exact_sync_diagnostics_tests
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    add_test(NAME exact_sync_diagnostics COMMAND reg_exact_sync_diagnostics_tests)

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


# The launcher is deliberately isolated from the NVIDIA Vulkan Video runtime.
# It can display errors and edit profiles even when video decoding cannot start.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(reg_launcher_core STATIC
    src/launcher/LauncherConfig.cpp
    src/launcher/ProcessManager.cpp
)
target_include_directories(reg_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(reg_launcher_core PUBLIC nlohmann_json::nlohmann_json)
if(REG_BUILD_TESTS)
    add_executable(reg_window_identity_tests tests/window_identity_tests.cpp)
    target_include_directories(reg_window_identity_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(reg_window_identity_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_window_identity_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME window_identity COMMAND reg_window_identity_tests)
endif()
# Lock responsive dashboard breakpoints independently of SDL/ImGui.
if(REG_BUILD_TESTS)
    add_executable(reg_launcher_layout_tests tests/launcher_layout_tests.cpp)
    target_include_directories(reg_launcher_layout_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(reg_launcher_layout_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_launcher_layout_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME launcher_layout COMMAND reg_launcher_layout_tests)
endif()
# Regression: GitHub's releases API may be out of chronological order.
# Test release selection on both Windows and Linux, independently of WinHTTP.
if(REG_BUILD_TESTS)
    add_executable(reg_update_release_selection_tests
        tests/update_release_selection_tests.cpp
    )
    target_include_directories(reg_update_release_selection_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_update_release_selection_tests PRIVATE
        nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(reg_update_release_selection_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_update_release_selection_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME update_release_selection COMMAND reg_update_release_selection_tests)
endif()
if(WIN32)
    target_sources(reg_launcher_core PRIVATE
        src/launcher/UpdateManager.cpp
        src/launcher/UpdateShared.cpp
    )
    target_link_libraries(reg_launcher_core PUBLIC winhttp bcrypt)
    add_executable(reg_updater WIN32
        src/launcher/updater_main.cpp
        src/launcher/UpdateShared.cpp
    )
    target_include_directories(reg_updater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(reg_updater PRIVATE nlohmann_json::nlohmann_json bcrypt shell32)
    if(MINGW)
        # The updater is copied outside the install directory while replacing
        # DLLs. It must not need MinGW runtime DLLs from that install directory.
        target_link_options(reg_updater PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
if(WIN32)
    target_link_libraries(reg_launcher_core PUBLIC user32)
endif()

add_library(reg_launcher_imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(reg_launcher_imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)
target_link_libraries(reg_launcher_imgui PUBLIC SDL3::SDL3)

add_executable(reg_launcher WIN32
    src/launcher/main.cpp
    src/launcher/LauncherDisplay.cpp
    src/launcher/WindowTiling.cpp
)
target_link_libraries(reg_launcher PRIVATE reg_launcher_core reg_launcher_imgui)
if(WIN32)
    target_link_libraries(reg_launcher PRIVATE shell32 user32 dwmapi)
endif()
target_compile_definitions(reg_launcher PRIVATE
    REG_FONT_DIR=\"${REG_FONT_OUTPUT_DIR}\"
)

# Installed layout is relocatable: binaries find adjacent assets and private libs.
set_target_properties(reg_probe reg_replay reg_launcher PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
)
install(TARGETS reg_launcher reg_probe reg_replay RUNTIME DESTINATION bin)
if(WIN32)
    install(TARGETS reg_updater RUNTIME DESTINATION bin)
endif()
install(DIRECTORY "${REG_FONT_OUTPUT_DIR}/" DESTINATION bin/fonts)
install(DIRECTORY "${REG_SHADER_OUTPUT_DIR}/" DESTINATION bin/shaders)
if(UNIX AND NOT APPLE)
    # install(FILES) cannot follow soname symlink chains. This variant can.
    # Delay the copy until installation so the built SDL3 library exists.
    foreach(_reg_runtime_library IN ITEMS
        "$<TARGET_FILE:SDL3::SDL3>"
        "${FFMPEG_AVCODEC_LIBRARY}"
        "${FFMPEG_AVFORMAT_LIBRARY}"
        "${FFMPEG_AVUTIL_LIBRARY}")
        install(CODE "file(INSTALL \"${_reg_runtime_library}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\" FOLLOW_SYMLINK_CHAIN)")
    endforeach()
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/reg-launcher.desktop"
    DESTINATION share/applications OPTIONAL)

if(REG_BUILD_TESTS)
    add_executable(reg_command_line_options_tests
        tests/command_line_options_tests.cpp
        src/app/CommandLine.cpp
    )
    target_include_directories(reg_command_line_options_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(reg_command_line_options_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_command_line_options_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME command_line_options COMMAND reg_command_line_options_tests)

    add_executable(reg_launcher_config_tests
        tests/launcher_config_tests.cpp
    )
    target_link_libraries(reg_launcher_config_tests PRIVATE reg_launcher_core)
    if(MSVC)
        target_compile_options(reg_launcher_config_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_launcher_config_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME launcher_config COMMAND reg_launcher_config_tests)

    add_executable(reg_launcher_process_tests tests/launcher_process_tests.cpp)
    target_link_libraries(reg_launcher_process_tests PRIVATE reg_launcher_core)
    if(MSVC)
        target_compile_options(reg_launcher_process_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(reg_launcher_process_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME launcher_process COMMAND reg_launcher_process_tests)

    if(WIN32)
        add_executable(reg_update_protocol_tests
            tests/update_protocol_tests.cpp
            src/launcher/UpdateShared.cpp
        )
        target_include_directories(reg_update_protocol_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(reg_update_protocol_tests PRIVATE bcrypt)
        if(MSVC)
            target_compile_options(reg_update_protocol_tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(reg_update_protocol_tests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME update_protocol COMMAND reg_update_protocol_tests)
        add_test(NAME updater_install COMMAND reg_updater --self-test)
    endif()
endif()
