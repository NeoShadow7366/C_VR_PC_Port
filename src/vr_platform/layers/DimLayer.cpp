// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/DimLayer.h"
#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"

#include <algorithm>
#include <cmath>

namespace vr_pcvr {

namespace {

constexpr uint32_t kTexel = 8;

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
             VkAccessFlags srcAccess, VkAccessFlags dstAccess,
             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = srcAccess;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = oldL;
    b.newLayout           = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Acquire-clear-release the dim swapchain image to (r*a, g*a, b*a, a).
// Returns true on success. Caller is responsible for any layer-list
// gating; this just re-paints the texture.
bool ClearTo(VkXrSwapchain& sc, VrFrameSubmitter& submitter,
             float r, float g, float b, float a) {
    const uint32_t idx = sc.AcquireAndWait();
    if (idx >= sc.Images().size()) {
        ALOGE("DimLayer: AcquireAndWait failed");
        return false;
    }
    VkImage img = sc.Images()[idx];

    VkCommandBuffer cmd = submitter.Begin();
    if (cmd == VK_NULL_HANDLE) {
        sc.Release();
        return false;
    }

    Barrier(cmd, img,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Premultiplied alpha so SOURCE_ALPHA blend gives the right composite.
    VkClearColorValue color{};
    color.float32[0] = r * a;
    color.float32[1] = g * a;
    color.float32[2] = b * a;
    color.float32[3] = a;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    Barrier(cmd, img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    if (!submitter.Submit()) {
        ALOGE("DimLayer: submit failed");
        sc.Release();
        return false;
    }
    sc.Release();
    return true;
}

} // namespace

bool DimLayer::Init(XrSession session, VrFrameSubmitter& submitter,
                    float r, float g, float b, float a) {
    if (!mSwapchain.Create(session, kTexel, kTexel,
                           /*vkFormat=*/0, /*arraySize=*/1, /*sampleCount=*/1)) {
        ALOGE("DimLayer: swapchain creation failed");
        return false;
    }
    if (!submitter.IsInitialised()) {
        ALOGE("DimLayer: submitter not ready");
        Shutdown();
        return false;
    }

    mBaseR = r; mBaseG = g; mBaseB = b; mBaseA = a;

    if (!ClearTo(mSwapchain, submitter, r, g, b, a)) {
        Shutdown();
        return false;
    }
    mCurrentAlpha = 1.0f;
    ALOGI("DimLayer initialised (rgba=%.2f,%.2f,%.2f,%.2f)", r, g, b, a);
    return true;
}

void DimLayer::SetAlpha(VrFrameSubmitter& submitter, float alpha) {
    if (!IsInitialised()) return;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    // Only re-paint when the alpha changes by more than ~1/256 so we
    // don't burn a command buffer per frame on a static menu.
    if (std::abs(alpha - mCurrentAlpha) < 1.0f / 256.0f) return;
    if (ClearTo(mSwapchain, submitter, mBaseR, mBaseG, mBaseB, alpha * mBaseA)) {
        mCurrentAlpha = alpha;
    }
}

void DimLayer::Shutdown() {
    mSwapchain.Destroy();
    mVisible      = false;
    mCurrentAlpha = -1.0f;
}

bool DimLayer::BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const {
    if (!IsInitialised() || !mVisible || mCurrentAlpha <= 0.001f) return false;
    outLayer = {};
    outLayer.type                     = XR_TYPE_COMPOSITION_LAYER_QUAD;
    outLayer.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    outLayer.space                    = space;
    outLayer.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
    outLayer.subImage.swapchain       = mSwapchain.Handle();
    outLayer.subImage.imageRect       = {{0, 0},
                                         {static_cast<int32_t>(mSwapchain.Width()),
                                          static_cast<int32_t>(mSwapchain.Height())}};
    outLayer.subImage.imageArrayIndex = 0;
    outLayer.pose                     = mPose;
    outLayer.size                     = {mSizeW, mSizeH};
    return true;
}

} // namespace vr_pcvr
