cmake_minimum_required(VERSION 3.28)

project(RegJetsonFrameIdentityProbe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${REG_ROOT}/cmake")

find_package(Vulkan 1.3 REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil)

add_executable(reg_jetson_probe
    main.cpp
    "${REG_ROOT}/src/media/CompressedVideoPacket.cpp"
    "${REG_ROOT}/src/media/FrameIdentity.cpp"
    "${REG_ROOT}/src/media/FrameIdentityAvFrame.cpp"
    "${REG_ROOT}/src/media/RtspDecoder.cpp"
    "${REG_ROOT}/src/media/VideoStreamDescriptor.cpp"
    "${REG_ROOT}/src/video/VideoFrame.cpp"
)

target_include_directories(reg_jetson_probe PRIVATE
    "${REG_ROOT}/src"
)

target_link_libraries(reg_jetson_probe PRIVATE
    Vulkan::Vulkan
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
)

if(MSVC)
    target_compile_options(reg_jetson_probe PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(reg_jetson_probe PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )
endif()
