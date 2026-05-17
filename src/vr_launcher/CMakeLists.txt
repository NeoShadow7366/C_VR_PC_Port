# SPDX-FileCopyrightText: 2026 CitraVR / NeoXR Citra authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tiny WIN32 shim that ensures SteamVR is running, then invokes citra_vr.exe.
# Distributed alongside citra_vr.exe by the Inno Setup installer; this is the
# binary that the "NeoXR Citra VR" Steam shortcut points at.

add_executable(citra_vr_launcher WIN32
    citra_vr_launcher.cpp
)

set_target_properties(citra_vr_launcher PROPERTIES
    WIN32_EXECUTABLE TRUE
    FOLDER vr
)

target_compile_features(citra_vr_launcher PRIVATE cxx_std_17)

target_link_libraries(citra_vr_launcher PRIVATE
    shell32
)

# Embed NeoXR Citra icon when available (post-art-build).
set(_vr_launcher_icon "${CMAKE_SOURCE_DIR}/dist/steam_art/out/vr/icon.ico")
set(_vr_launcher_rc   "${CMAKE_CURRENT_BINARY_DIR}/citra_vr_launcher.rc")
if (EXISTS "${_vr_launcher_icon}")
    file(WRITE "${_vr_launcher_rc}"
        "IDI_ICON1 ICON DISCARDABLE \"${_vr_launcher_icon}\"\n")
    target_sources(citra_vr_launcher PRIVATE "${_vr_launcher_rc}")
endif()

# Drop the launcher next to citra_vr.exe in the build tree for local testing.
add_custom_command(TARGET citra_vr_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:citra_vr_launcher>"
        "$<TARGET_FILE_DIR:citra_vr>/citra_vr_launcher.exe"
    VERBATIM
)
