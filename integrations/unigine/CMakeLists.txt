cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RegUnigineFrameIdentity LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

get_filename_component(
    REG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    ABSOLUTE
)

if(NOT TARGET reg_frame_identity_source)
    add_library(reg_frame_identity_source STATIC
        "${REG_ROOT}/src/media/FrameIdentity.cpp"
        "${REG_ROOT}/src/media/FrameIdentitySei.cpp"
        "${REG_ROOT}/src/source/FrameIdentityAccessUnitInjector.cpp"
    )

    target_include_directories(reg_frame_identity_source PUBLIC
        "${REG_ROOT}/src"
    )

    if(MSVC)
        target_compile_options(
            reg_frame_identity_source
            PRIVATE
            /W4
            /permissive-
            /EHsc
        )
    else()
        target_compile_options(
            reg_frame_identity_source
            PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    endif()
endif()

if(NOT TARGET reg_unigine_rtsp_frame_identity)
    add_library(
        reg_unigine_rtsp_frame_identity
        INTERFACE
    )

    target_include_directories(
        reg_unigine_rtsp_frame_identity
        INTERFACE
        "${REG_ROOT}"
        "${REG_ROOT}/src"
    )

    target_link_libraries(
        reg_unigine_rtsp_frame_identity
        INTERFACE
        reg_frame_identity_source
    )
endif()
