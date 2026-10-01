# NetImgui integration helpers for Reg.
#
# NetImgui is kept as a pinned submodule. Reg intentionally owns SDL/Vulkan
# presentation; the upstream NetImgui ServerApp renderer is not used here.

function(reg_netimgui_third_party_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        # NetImgui is third-party code; don't promote its warnings to Reg policy.
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

function(reg_add_netimgui_client_compat_target)
    set(_netimgui_root "${CMAKE_CURRENT_SOURCE_DIR}/netimgui")
    set(_netimgui_client "${_netimgui_root}/Code/Client")

    if(NOT EXISTS "${_netimgui_client}/NetImgui_Api.h")
        message(FATAL_ERROR
            "REG_ENABLE_NETIMGUI_REMOTE=ON requires the netimgui submodule.\n"
            "Run: git submodule update --init --recursive netimgui"
        )
    endif()

    # Upstream NetImgui Server is intentionally built with 32-bit indices so it
    # can reconstruct frames produced by either 16-bit or 32-bit clients. The
    # definition is PUBLIC to keep every ImGui/ImPlot consumer in Reg ABI-identical.
    target_compile_definitions(reg_imgui PUBLIC ImDrawIdx=ImU32)

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

    reg_netimgui_third_party_warnings(reg_netimgui_client)

    reg_add_netimgui_server_core_target()
    reg_add_netimgui_remote_renderer_target()
endfunction()

function(reg_add_netimgui_server_core_target)
    set(_netimgui_root "${CMAKE_CURRENT_SOURCE_DIR}/netimgui")
    set(_netimgui_client "${_netimgui_root}/Code/Client")
    set(_netimgui_server "${_netimgui_root}/Code/ServerApp/Source")

    add_library(reg_netimgui_server_core STATIC
        "${_netimgui_server}/NetImguiServer_Network.cpp"
        "${_netimgui_server}/NetImguiServer_RemoteClient.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/NetImguiEmbeddedConfig.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/NetImguiEmbeddedApp.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/NetImguiHost.cpp"
    )

    target_include_directories(reg_netimgui_server_core
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PRIVATE
            "${_netimgui_client}"
            "${_netimgui_server}"
            "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    )

    # Keep upstream's server-side conditionals, but deliberately do not compile
    # any Win32/DX11, GLFW/OpenGL or Sokol ServerApp/HAL source files.
    target_compile_definitions(reg_netimgui_server_core
        PRIVATE
            IS_NETIMGUISERVER=1
    )

    target_link_libraries(reg_netimgui_server_core
        PUBLIC
            reg_netimgui_client
            reg_imgui
    )

    if(WIN32)
        target_link_libraries(reg_netimgui_server_core PRIVATE ws2_32)
    endif()

    reg_netimgui_third_party_warnings(reg_netimgui_server_core)

    add_executable(reg_netimgui_core_link_smoke
        "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/NetImguiCoreLinkSmoke.cpp"
    )
    target_link_libraries(reg_netimgui_core_link_smoke PRIVATE
        reg_netimgui_server_core
    )
    if(MSVC)
        target_compile_options(reg_netimgui_core_link_smoke PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_netimgui_core_link_smoke PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    endif()
endfunction()

function(reg_add_netimgui_remote_renderer_target)
    add_library(reg_remote_imgui STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/RemoteImGuiRenderer.cpp"
    )

    target_include_directories(reg_remote_imgui PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_link_libraries(reg_remote_imgui
        PUBLIC
            reg_netimgui_server_core
            reg_imgui
            Vulkan::Vulkan
    )

    if(MSVC)
        target_compile_options(reg_remote_imgui PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(reg_remote_imgui PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    endif()
endfunction()
