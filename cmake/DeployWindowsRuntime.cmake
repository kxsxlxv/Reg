# Copy dynamically linked non-system runtime DLLs next to the Windows binaries.
# Inputs: REG_BUILD_DIR, REG_FFMPEG_BIN, REG_UCRT_BIN.
foreach(_name IN ITEMS REG_BUILD_DIR REG_FFMPEG_BIN REG_UCRT_BIN)
    if(NOT DEFINED ${_name})
        message(FATAL_ERROR "Missing input ${_name}")
    endif()
endforeach()

set(_executables
    "${REG_BUILD_DIR}/reg_probe.exe"
    "${REG_BUILD_DIR}/reg_replay.exe"
    "${REG_BUILD_DIR}/reg_launcher.exe"
)
foreach(_exe IN LISTS _executables)
    if(NOT EXISTS "${_exe}")
        message(FATAL_ERROR "Missing executable ${_exe}")
    endif()
endforeach()

if(NOT EXISTS "${REG_BUILD_DIR}/SDL3.dll")
    file(GLOB_RECURSE _sdl_dll "${REG_BUILD_DIR}/_deps/*/SDL3.dll")
    if(NOT _sdl_dll)
        message(FATAL_ERROR "SDL3.dll not found")
    endif()
    list(GET _sdl_dll 0 _sdl_path)
    file(COPY_FILE "${_sdl_path}" "${REG_BUILD_DIR}/SDL3.dll")
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${_executables}
    DIRECTORIES "${REG_BUILD_DIR}" "${REG_FFMPEG_BIN}" "${REG_UCRT_BIN}"
    PRE_EXCLUDE_REGEXES "^api-ms-win-" "^ext-ms-"
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
)
foreach(_dll IN LISTS _resolved)
    get_filename_component(_folder "${_dll}" DIRECTORY)
    file(TO_CMAKE_PATH "${_folder}" _folder)
    file(TO_CMAKE_PATH "${REG_UCRT_BIN}" _ucrt)
    file(TO_CMAKE_PATH "${REG_FFMPEG_BIN}" _ffmpeg)
    string(TOLOWER "${_folder}" _folder)
    string(TOLOWER "${_ucrt}" _ucrt)
    string(TOLOWER "${_ffmpeg}" _ffmpeg)
    if(_folder STREQUAL _ucrt OR _folder STREQUAL _ffmpeg)
        get_filename_component(_filename "${_dll}" NAME)
        file(COPY_FILE "${_dll}" "${REG_BUILD_DIR}/${_filename}" ONLY_IF_DIFFERENT)
    endif()
endforeach()

if(_unresolved)
    message(WARNING "Unresolved DLL dependencies: ${_unresolved}")
endif()
