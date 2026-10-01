# Pinned font assets used by the ImGui UI. Keep these revisions explicit so a
# normal Reg rebuild cannot silently change typography or icon glyphs.
set(REG_FONT_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/fonts")
file(MAKE_DIRECTORY "${REG_FONT_OUTPUT_DIR}")

set(_REG_ROBOTO_REV "9710da1eacb3be272583c3224dcb70f9da6eadbb")
set(_REG_MATERIAL_REV "bd8cb85bd4bad964fe6918f79665bb40c3a8efef")

set(_REG_ROBOTO_URL
    "https://raw.githubusercontent.com/google/fonts/${_REG_ROBOTO_REV}/ofl/roboto/Roboto%5Bwdth%2Cwght%5D.ttf")
set(_REG_MATERIAL_URL
    "https://raw.githubusercontent.com/google/material-design-icons/${_REG_MATERIAL_REV}/variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf")

function(_reg_download_font url destination minimum_size)
    set(_download TRUE)
    if(EXISTS "${destination}")
        file(SIZE "${destination}" _existing_size)
        if(_existing_size GREATER_EQUAL minimum_size)
            set(_download FALSE)
        endif()
    endif()

    if(_download)
        message(STATUS "Fetching UI font asset: ${destination}")
        file(
            DOWNLOAD
            "${url}"
            "${destination}.tmp"
            STATUS _status
            TLS_VERIFY ON
            SHOW_PROGRESS
        )
        list(GET _status 0 _status_code)
        list(GET _status 1 _status_message)
        if(NOT _status_code EQUAL 0)
            file(REMOVE "${destination}.tmp")
            message(FATAL_ERROR
                "Failed to download UI font asset ${url}: ${_status_message}")
        endif()

        file(SIZE "${destination}.tmp" _downloaded_size)
        if(_downloaded_size LESS minimum_size)
            file(REMOVE "${destination}.tmp")
            message(FATAL_ERROR
                "Downloaded UI font asset is unexpectedly small: ${url}")
        endif()

        file(RENAME "${destination}.tmp" "${destination}")
    endif()
endfunction()

_reg_download_font(
    "${_REG_ROBOTO_URL}"
    "${REG_FONT_OUTPUT_DIR}/Roboto.ttf"
    400000)
_reg_download_font(
    "${_REG_MATERIAL_URL}"
    "${REG_FONT_OUTPUT_DIR}/MaterialSymbolsOutlined.ttf"
    8000000)
