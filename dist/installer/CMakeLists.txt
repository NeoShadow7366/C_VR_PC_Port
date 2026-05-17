
if(WIN32)
    set(PLATFORM "windows")
elseif(APPLE)
    set(PLATFORM "mac")
elseif(UNIX)
    set(PLATFORM "linux")
else()
    message(FATAL_ERROR "Cannot build installer for this unsupported platform")
endif()

set(BUILD_DIR "${CMAKE_BINARY_DIR}/installer")
set(DIST_DIR "${BUILD_DIR}/dist")
set(TARGET_FILE "${DIST_DIR}/citra-setup-${PLATFORM}")

file(MAKE_DIRECTORY "${BUILD_DIR}" "${DIST_DIR}")

# Adds a custom target that will run the BuildInstaller.cmake file
# CMake can't just run a cmake function as a custom command, so this is a way around it.
# Calls the cmake command and runs a cmake file in "scripting" mode passing in variables with -D
add_custom_command(OUTPUT "${TARGET_FILE}"
    COMMAND ${CMAKE_COMMAND}
        -DBASE_DIR=${CMAKE_SOURCE_DIR}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBUILD_DIR=${BUILD_DIR}
        -DTARGET_FILE=${TARGET_FILE}
        -P ${CMAKE_SOURCE_DIR}/CMakeModules/BuildInstaller.cmake
    WORKING_DIRECTORY ${BUILD_DIR}
)

add_custom_target(installer DEPENDS ${TARGET_FILE})

# -----------------------------------------------------------------------------
# NeoXR Citra Inno Setup installer (Windows only).
# Builds Steam grid art + invokes ISCC to produce
#   dist/installer/out/NeoXRCitra-Setup-<ver>.exe
#
# Manual invocation:
#   cmake --build <build> --target neoxr_installer
# Requires:
#   - Inno Setup 6 (ISCC.exe on PATH or under Program Files (x86)\Inno Setup 6)
#   - Both the qt build (build/) and vr build (build-vr/) already produced.
# -----------------------------------------------------------------------------
if(WIN32)
    add_custom_target(neoxr_installer
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/dist/installer/Build-Installer.ps1"
            -RepoRoot "${CMAKE_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building NeoXR Citra [VR] Inno Setup installer"
        VERBATIM
    )
    set_target_properties(neoxr_installer PROPERTIES FOLDER installer)
endif()
