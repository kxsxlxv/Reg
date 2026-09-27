cmake_minimum_required(VERSION 3.28)

project(Reg VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REG_FETCH_SDL3 "Fetch a pinned SDL3 release instead of using a system package" ON)
option(REG_ENABLE_VALIDATION "Enable Vulkan validation-layer integration in development builds" ON)

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

add_executable(reg_probe
    src/main.cpp
    src/app/CommandLine.cpp
    src/platform/SDLPlatform.cpp
    src/vulkan/VulkanContext.cpp
    src/media/FrameIdentity.cpp
    src/media/VulkanHwDevice.cpp
    src/media/RtspDecoder.cpp
    src/video/VideoFrame.cpp
)

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
)

if(MSVC)
    target_compile_options(reg_probe PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
