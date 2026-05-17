# SPDX-License-Identifier: GPL-3.0-or-later
#
# CitraVR PC / SteamVR backend.
#
# This builds an OpenXR-on-Vulkan VR runtime adapter that hosts the Citra
# emulator core in a native SteamVR application on Windows. It deliberately
# mirrors the layout of src/android/app/src/main/jni/vr/ so that the
# platform-agnostic logic (frame loop, input, layer composition) can later
# be hoisted into a shared module.

add_library(vr_platform_pcvr STATIC
    VrApp.cpp
    VrApp.h
    VrInputBridge.cpp
    VrInputBridge.h
    VrSettings.cpp
    VrSettings.h
    RomBrowser.cpp
    RomBrowser.h
    XrController.cpp
    XrController.h
    XrPlatformIncludes.h
    Swapchain.h
    layers/AimBeamLayer.cpp
    layers/AimBeamLayer.h
    layers/CursorLayer.cpp
    layers/CursorLayer.h
    layers/DimLayer.cpp
    layers/DimLayer.h
    layers/ImGuiLayer.cpp
    layers/ImGuiLayer.h
    layers/IconCache.cpp
    layers/IconCache.h
    layers/WristMenuButton.cpp
    layers/WristMenuButton.h
    layers/GameQuadLayer.cpp
    layers/GameQuadLayer.h
    layers/UIQuadLayer.cpp
    layers/UIQuadLayer.h
    utils/LogUtils.h
    utils/XrMath.h
    windows/EmuWindow_VR_Win.cpp
    windows/EmuWindow_VR_Win.h
    windows/VkXrSwapchain.cpp
    windows/VkXrSwapchain.h
    windows/VrFrameSubmitter.cpp
    windows/VrFrameSubmitter.h
    windows/WinPlatform.cpp
    windows/WinPlatform.h
)

target_include_directories(vr_platform_pcvr
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(vr_platform_pcvr
    PUBLIC
        XR_USE_GRAPHICS_API_VULKAN=1
        XR_USE_PLATFORM_WIN32=1
        VR_BACKEND_STEAMVR=1
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

# Dev-time fallback path to the bundled Roboto TTF used by the in-VR menu.
# Runtime first looks for "Roboto-Medium.ttf" next to citra_vr.exe (shipped
# via a POST_BUILD copy on the citra_vr target); this absolute path is used
# only when running from a non-standard location (e.g. ad-hoc test builds).
target_compile_definitions(vr_platform_pcvr
    PRIVATE
        CITRA_VR_DEV_FONT_PATH="${CMAKE_SOURCE_DIR}/externals/imgui/misc/fonts/Roboto-Medium.ttf"
)

target_compile_features(vr_platform_pcvr PUBLIC cxx_std_20)

# Vulkan: re-use Citra's bundled Vulkan headers, but also link the
# system Vulkan loader (vulkan-1.lib on Windows) so direct C-API calls
# like vkCmdBlitImage / vkQueueSubmit resolve. RendererVulkan loads
# symbols dynamically via vulkan.hpp's dispatch loader, but we
# deliberately stay on the C ABI here.
find_package(Vulkan)
target_link_libraries(vr_platform_pcvr
    PUBLIC
        openxr_loader
        vulkan-headers
        citra_common
        citra_core
        video_core
        input_common
        imgui
)
if (TARGET Vulkan::Vulkan)
    target_link_libraries(vr_platform_pcvr PUBLIC Vulkan::Vulkan)
elseif (DEFINED ENV{VULKAN_SDK})
    target_link_libraries(vr_platform_pcvr PUBLIC "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
else()
    message(FATAL_ERROR "vr_platform_pcvr: Vulkan loader not found - install the Vulkan SDK or set VULKAN_SDK.")
endif()
