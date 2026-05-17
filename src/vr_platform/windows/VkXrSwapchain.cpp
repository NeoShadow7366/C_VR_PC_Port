// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows/VkXrSwapchain.h"
#include "utils/LogUtils.h"
#include "video_core/renderer_vulkan/vk_vr_hooks.h"

#include <mutex>

namespace vr_pcvr {

bool VkXrSwapchain::Create(XrSession session,
                           uint32_t  width,
                           uint32_t  height,
                           int64_t   vkFormat,
                           uint32_t  arraySize,
                           uint32_t  sampleCount) {
    Destroy();
    mSession = session;
    mWidth   = width;
    mHeight  = height;

    // Pick a runtime-supported colour format. Citra's RendererVulkan writes
    // its frame in linear space, so we prefer UNORM over SRGB to avoid a
    // double gamma conversion in the SteamVR compositor. Fall back to
    // whatever the runtime advertises first.
    if (vkFormat == 0) {
        uint32_t fmtCount = 0;
        if (XR_FAILED(xrEnumerateSwapchainFormats(mSession, 0, &fmtCount, nullptr)) ||
            fmtCount == 0) {
            ALOGE("xrEnumerateSwapchainFormats returned 0 formats");
            return false;
        }
        std::vector<int64_t> fmts(fmtCount);
        if (XR_FAILED(xrEnumerateSwapchainFormats(mSession, fmtCount, &fmtCount, fmts.data()))) {
            return false;
        }
        constexpr int64_t kPreferred[] = {
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SRGB,
        };
        for (int64_t want : kPreferred) {
            for (int64_t got : fmts) {
                if (want == got) { vkFormat = want; break; }
            }
            if (vkFormat != 0) break;
        }
        if (vkFormat == 0) {
            vkFormat = fmts[0]; // last-resort fallback
        }
        ALOGI("VkXrSwapchain selected VkFormat %lld", static_cast<long long>(vkFormat));
    }
    mFormat = vkFormat;

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format      = vkFormat;
    sci.sampleCount = sampleCount;
    sci.width       = width;
    sci.height      = height;
    sci.faceCount   = 1;
    sci.arraySize   = arraySize;
    sci.mipCount    = 1;

    if (XR_FAILED(xrCreateSwapchain(mSession, &sci, &mHandle))) {
        ALOGE("xrCreateSwapchain (Vulkan, %ux%u) failed", width, height);
        return false;
    }

    uint32_t imageCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(mHandle, 0, &imageCount, nullptr))) {
        return false;
    }
    std::vector<XrSwapchainImageVulkanKHR> images(
        imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            mHandle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
        return false;
    }
    mImages.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        mImages[i] = images[i].image;
    }
    ALOGI("VkXrSwapchain created %ux%u (%u images)", width, height, imageCount);
    return true;
}

uint32_t VkXrSwapchain::AcquireAndWait() {
    // SteamVR's xrAcquire/WaitSwapchainImage submit work to the same VkQueue
    // Citra renders into; serialise against the renderer's submits.
    std::scoped_lock vr_lock{Vulkan::GetVrQueueMutex()};
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(mHandle, &ai, &index))) {
        ALOGE("xrAcquireSwapchainImage failed");
        return 0;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(mHandle, &wi))) {
        ALOGE("xrWaitSwapchainImage failed");
    }
    return index;
}

void VkXrSwapchain::Release() {
    std::scoped_lock vr_lock{Vulkan::GetVrQueueMutex()};
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    (void)xrReleaseSwapchainImage(mHandle, &ri);
}

void VkXrSwapchain::Destroy() {
    if (mHandle != XR_NULL_HANDLE) {
        xrDestroySwapchain(mHandle);
        mHandle = XR_NULL_HANDLE;
    }
    mImages.clear();
    mWidth = mHeight = 0;
}

} // namespace vr_pcvr
