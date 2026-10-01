# NetImgui integration helpers for Reg.
#
# NetImgui is kept as a pinned submodule. Reg intentionally owns SDL/Vulkan
# presentation; the upstream NetImgui ServerApp renderer is not used here.

function(reg_add_netimgui_client_compat_target)
    set(_netimgui_root "${CMAKE_CURRENT_SOURCE_DIR}/netimgui")
    set(_netimgui_client "${_netimgui_root}/Code/Client")

    if(NOT EXISTS "${_netimgui_client}/NetImgui_Api.h")
        message(FATAL_ERROR
            "REG_ENABLE_NETIMGUI_REMOTE=ON requires the netimgui submodule.\n"
            "Run: git submodule update --init --recursive netimgui"
        )
    endif()

    # Upstream's NetImgui library project builds the complete Code/Client tree;
    # platform-specific translation units select themselves through NetImgui's
    # configuration macros. Keep the source set explicit enough to make upstream
    # changes visible during review while still picking up the current networking
    # backends shipped by the pinned revision.
    file(GLOB _netimgui_client_sources CONFIGURE_DEPENDS
        "${_netimgui_client}/Private/*.cpp"
    )

    add_library(reg_netimgui_client STATIC
        ${_netimgui_client_sources}
    )

    target_include_directories(reg_netimgui_client
        PUBLIC
            "${_netimgui_client}"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    )

    target_compile_definitions(reg_netimgui_client
        PUBLIC
            NETIMGUI_ENABLED=1
    )

    # NetImgui's client code calls Dear ImGui APIs but does not need a renderer
    # backend of its own. Linking Reg's ImGui target guarantees a single Dear
    # ImGui ABI/version inside Reg and makes this target an effective compatibility
    # smoke test for the pinned NetImgui revision.
    target_link_libraries(reg_netimgui_client PUBLIC reg_imgui)

    if(WIN32)
        target_link_libraries(reg_netimgui_client PRIVATE ws2_32)
    endif()

    if(MSVC)
        target_compile_options(reg_netimgui_client PRIVATE /W4 /permissive- /EHsc)
    else()
        # NetImgui is third-party code; don't promote its warnings to Reg policy.
        target_compile_options(reg_netimgui_client PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()
