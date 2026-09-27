# Minimal FFmpeg finder for the libraries required by the MVP probe.
#
# Supported discovery paths:
#   1. pkg-config (typical Linux installation),
#   2. FFMPEG_ROOT / environment FFMPEG_ROOT containing include/ and lib/.
#
# Exposes:
#   FFmpeg::avcodec
#   FFmpeg::avformat
#   FFmpeg::avutil

include(FindPackageHandleStandardArgs)
find_package(PkgConfig QUIET)

set(_ffmpeg_roots)
if(DEFINED FFMPEG_ROOT)
    list(APPEND _ffmpeg_roots "${FFMPEG_ROOT}")
endif()
if(DEFINED ENV{FFMPEG_ROOT})
    list(APPEND _ffmpeg_roots "$ENV{FFMPEG_ROOT}")
endif()

if(PkgConfig_FOUND)
    pkg_check_modules(PC_AVCODEC QUIET libavcodec)
    pkg_check_modules(PC_AVFORMAT QUIET libavformat)
    pkg_check_modules(PC_AVUTIL QUIET libavutil)
endif()

set(_ffmpeg_include_hints
    ${PC_AVCODEC_INCLUDE_DIRS}
    ${PC_AVFORMAT_INCLUDE_DIRS}
    ${PC_AVUTIL_INCLUDE_DIRS}
)
set(_ffmpeg_library_hints
    ${PC_AVCODEC_LIBRARY_DIRS}
    ${PC_AVFORMAT_LIBRARY_DIRS}
    ${PC_AVUTIL_LIBRARY_DIRS}
)

foreach(_root IN LISTS _ffmpeg_roots)
    list(APPEND _ffmpeg_include_hints "${_root}/include" "${_root}")
    list(APPEND _ffmpeg_library_hints "${_root}/lib" "${_root}/lib64" "${_root}/bin" "${_root}")
endforeach()

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h libavformat/avformat.h libavutil/hwcontext_vulkan.h
    HINTS ${_ffmpeg_include_hints}
)

find_library(FFMPEG_AVCODEC_LIBRARY
    NAMES avcodec avcodec-63
    HINTS ${_ffmpeg_library_hints}
)

find_library(FFMPEG_AVFORMAT_LIBRARY
    NAMES avformat avformat-63
    HINTS ${_ffmpeg_library_hints}
)

find_library(FFMPEG_AVUTIL_LIBRARY
    NAMES avutil avutil-61
    HINTS ${_ffmpeg_library_hints}
)

find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS
        FFMPEG_INCLUDE_DIR
        FFMPEG_AVCODEC_LIBRARY
        FFMPEG_AVFORMAT_LIBRARY
        FFMPEG_AVUTIL_LIBRARY
)

if(FFmpeg_FOUND)
    foreach(_component IN ITEMS avcodec avformat avutil)
        string(TOUPPER "${_component}" _component_upper)
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${_component} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_${_component_upper}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
            )
        endif()
    endforeach()
endif()

mark_as_advanced(
    FFMPEG_INCLUDE_DIR
    FFMPEG_AVCODEC_LIBRARY
    FFMPEG_AVFORMAT_LIBRARY
    FFMPEG_AVUTIL_LIBRARY
)
