// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wraps a single XR colour swapchain backed by Vulkan images. Used by
// GameQuadLayer (and later UIQuadLayer) instead of the
// xrCreateSwapchainAndroidSurfaceKHR path used on Quest.

#pragma once

#include "../XrPlatformIncludes.h"

#include <cstdint>
#include <vector>

namespace vr_pcvr {

class VkXrSwapchain {
public:
    VkXrSwapchain() = default;
    ~VkXrSwapchain() { Destroy(); }

    VkXrSwapchain(const VkXrSwapchain&)            = delete;
    VkXrSwapchain& operator=(const VkXrSwapchain&) = delete;

    bool Create(XrSession session,
                uint32_t  width,
                uint32_t  height,
                int64_t   vkFormat = 0,   // 0 => pick a runtime-supported default
                uint32_t  arraySize = 1,
                uint32_t  sampleCount = 1);

    void Destroy();

    XrSwapchain Handle() const { return mHandle; }
    uint32_t    Width()  const { return mWidth;  }
    uint32_t    Height() const { return mHeight; }
    int64_t     Format() const { return mFormat; }
    const std::vector<VkImage>& Images() const { return mImages; }

    // Acquire / wait an image. Returns the index in Images() to render into.
    uint32_t AcquireAndWait();
    void     Release();

private:
    XrSession   mSession   = XR_NULL_HANDLE;
    XrSwapchain mHandle    = XR_NULL_HANDLE;
    uint32_t    mWidth     = 0;
    uint32_t    mHeight    = 0;
    int64_t     mFormat    = 0;
    std::vector<VkImage> mImages;
};

} // namespace vr_pcvr
