# SPDX-License-Identifier: GPL-3.0-or-later
#
# citra_vr - native SteamVR/OpenXR entry point. Built only when the PCVR
# backend is enabled (VR_BACKEND_STEAMVR) and Vulkan is available.

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

add_executable(citra_vr
    citra_vr.cpp
)

create_target_directory_groups(citra_vr)

target_link_libraries(citra_vr PRIVATE
    citra_common
    citra_core
    input_common
    network
    video_core
    vr_platform_pcvr
    tsl::robin_map
)

target_link_libraries(citra_vr PRIVATE Threads::Threads)

if (WIN32)
    # MiniDumpWriteDump (crash dumps) + StackWalk symbol helpers.
    target_link_libraries(citra_vr PRIVATE dbghelp)
endif()

target_include_directories(citra_vr PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(citra_vr PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Match the SDL2 frontend's MSVC bundling so DLL deps land next to the exe.
if (MSVC)
    include(BundleTarget)
    bundle_target_in_place(citra_vr)
endif()

# Auto-sync the freshly built citra_vr.exe next to citra-qt.exe (if a
# parallel Qt build dir exists) so `citra-qt --vr` / "Launch in VR"
# always picks up the latest build. Skipped silently if the Qt build
# dir doesn't exist (e.g. on first build of a fresh clone). The companion
# build dir is conventionally `<repo>/build/bin/<config>/`.
if (WIN32)
    set(_citra_vr_qt_dir "${CMAKE_SOURCE_DIR}/build/bin/$<CONFIG>")
    add_custom_command(TARGET citra_vr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_citra_vr_qt_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:citra_vr>"
                "${_citra_vr_qt_dir}/$<TARGET_FILE_NAME:citra_vr>"
        COMMENT "Syncing citra_vr.exe to ${_citra_vr_qt_dir}"
        VERBATIM
    )
endif()

# Ship the in-VR menu font next to the exe (and into the parallel Qt
# build dir if present). The runtime menu loads it via
# AddFontFromFileTTF; if the copy is missing, ImGuiLayer falls back to
# the compile-time CITRA_VR_DEV_FONT_PATH absolute source path, and
# finally to ImGui's default ProggyClean.
set(_citra_vr_font_src "${CMAKE_SOURCE_DIR}/externals/imgui/misc/fonts/Roboto-Medium.ttf")
add_custom_command(TARGET citra_vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_citra_vr_font_src}"
            "$<TARGET_FILE_DIR:citra_vr>/Roboto-Medium.ttf"
    COMMENT "Copying Roboto-Medium.ttf next to citra_vr.exe"
    VERBATIM
)
if (WIN32)
    add_custom_command(TARGET citra_vr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_citra_vr_font_src}"
                "${_citra_vr_qt_dir}/Roboto-Medium.ttf"
        COMMENT "Syncing Roboto-Medium.ttf to ${_citra_vr_qt_dir}"
        VERBATIM
    )
endif()
