// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny POD wrapper around an XR swapchain handle. Mirrors
// src/android/app/src/main/jni/vr/Swapchain.h so layer code can be
// shared between Android and Windows builds.

#pragma once

#include "XrPlatformIncludes.h"

#include <cstdint>

struct Swapchain {
    XrSwapchain mHandle = XR_NULL_HANDLE;
    uint32_t    mWidth  = 0;
    uint32_t    mHeight = 0;
};
