// SPDX-License-Identifier: GPL-3.0-or-later
//
// Single include used everywhere VR code touches OpenXR. The order is
// load-bearing: openxr_platform.h declares structs that reference
// LARGE_INTEGER, IUnknown (Win32) and VkInstance/VkImage (Vulkan), so
// windows.h and vulkan.h must already be visible.
//
// XR_USE_GRAPHICS_API_VULKAN and XR_USE_PLATFORM_WIN32 are set as
// PUBLIC compile definitions on the vr_platform_pcvr CMake target, so
// we do NOT redefine them here.

#pragma once

#if defined(_WIN32) && !defined(_WINDOWS_)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <unknwn.h>     // IUnknown
#endif

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
