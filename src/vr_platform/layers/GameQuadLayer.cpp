// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/GameQuadLayer.h"
#include "utils/LogUtils.h"

#include "core/3ds.h"

#include <cstdlib>

namespace vr_pcvr {

namespace {

// Image-memory barrier helper using core Vulkan (vkCmd...) - we cannot
// use vulkan.hpp here because the VR module deliberately stays in the
// C ABI to avoid coupling to the renderer's vulkan.hpp dispatch loader.
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

} // namespace

bool GameQuadLayer::Init(XrSession session, uint32_t width, uint32_t height) {
    mWidth  = width;
    mHeight = height;
    return mSwapchain.Create(session, width, height,
                             VK_FORMAT_R8G8B8A8_SRGB, /*arraySize=*/1, /*sampleCount=*/1);
}

void GameQuadLayer::Shutdown() {
    mSwapchain.Destroy();
    mWidth = mHeight = 0;
}

bool GameQuadLayer::Blit(VkCommandBuffer cmd, const EmuWindow_VR_Win::PublishedFrame& src) {
    if (cmd == VK_NULL_HANDLE || src.image == VK_NULL_HANDLE || !IsInitialised()) {
        return false;
    }

    // Acquire the next XR swapchain image and look it up.
    const uint32_t dstIdx = mSwapchain.AcquireAndWait();
    if (dstIdx >= mSwapchain.Images().size()) {
        return false;
    }
    VkImage dstImage = mSwapchain.Images()[dstIdx];

    // ---- Layout transitions before blit -------------------------------
    // src.image is published in TRANSFER_SRC_OPTIMAL by the renderer
    // (RendererVulkan's PresentWindow already ends a frame in that
    // layout). We still emit a barrier on the read side to flush colour
    // writes -> transfer reads.
    Barrier(cmd, src.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(cmd, dstImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // ---- Compute source sub-rectangles --------------------------------
    // EmuWindow render target layout (see EmuWindow_VR_Win.cpp):
    //   - Top half  (y in [0, srcTopH))   = MONO top 3DS screen,
    //                                       fills the full src width.
    //   - Bottom    (y in [srcTopH, srcH)) = MONO bottom 3DS screen,
    //                                       centred within the same
    //                                       width with side borders.
    // Both eyes get the same source pixels - stereo separation comes
    // from the per-eye composition-layer offset, not from pixel-side
    // stereoscopy.
    const int32_t srcW       = static_cast<int32_t>(src.width);
    const int32_t srcH       = static_cast<int32_t>(src.height);
    const int32_t srcTopH    = srcH / 2;
    const int32_t srcBotW    = (Core::kScreenBottomWidth * srcW) / Core::kScreenTopWidth;
    const int32_t srcBotXOff = (srcW - srcBotW) / 2;

    // ---- Compute destination sub-rectangles ---------------------------
    // The XR swapchain is laid out side-by-side stereo (left half = left
    // eye, right half = right eye). Within each eye half, we stack the
    // 3DS top screen above the bottom touch panel using the same vertical
    // ratio (1:1) as the source.
    const int32_t dstW         = static_cast<int32_t>(mWidth);
    const int32_t dstH         = static_cast<int32_t>(mHeight);
    const int32_t dstEyeW      = dstW / 2;
    const int32_t dstTopH      = dstH / 2;
    // Bottom panel in dst keeps the same horizontal-centre layout as src.
    const int32_t dstBotW      = (Core::kScreenBottomWidth * dstEyeW) / Core::kScreenTopWidth;
    const int32_t dstBotXOff   = (dstEyeW - dstBotW) / 2;

    auto MakeBlit = [](int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                       int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1) {
        VkImageBlit b{};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.dstSubresource.layerCount = 1;
        b.srcOffsets[0] = {sx0, sy0, 0};
        b.srcOffsets[1] = {sx1, sy1, 1};
        b.dstOffsets[0] = {dx0, dy0, 0};
        b.dstOffsets[1] = {dx1, dy1, 1};
        return b;
    };

    // Four blits: top-L, top-R, bottom-L (mono), bottom-R (mono).
    const VkImageBlit blits[4] = {
        // Top-Left eye: full src top stripe -> left eye top.
        MakeBlit(0,             0,        srcW,        srcTopH,
                 0,             0,        dstEyeW,     dstTopH),
        // Top-Right eye: full src top stripe -> right eye top.
        MakeBlit(0,             0,        srcW,        srcTopH,
                 dstEyeW,       0,        dstW,        dstTopH),
        // Bottom mono into left eye bottom (centred horizontally).
        MakeBlit(srcBotXOff,    srcTopH,  srcBotXOff + srcBotW, srcH,
                 dstBotXOff,    dstTopH,  dstBotXOff + dstBotW, dstH),
        // Bottom mono into right eye bottom.
        MakeBlit(srcBotXOff,    srcTopH,  srcBotXOff + srcBotW, srcH,
                 dstEyeW + dstBotXOff,  dstTopH,
                 dstEyeW + dstBotXOff + dstBotW, dstH),
    };
    vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage,     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   4, blits, VK_FILTER_LINEAR);

    // Transition the XR swapchain image into a layout the runtime accepts.
    // The OpenXR spec requires colour swapchain images to be in
    // COLOR_ATTACHMENT_OPTIMAL when released back to the runtime.
    Barrier(cmd, dstImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Note: xrReleaseSwapchainImage is a CPU-side call but its semantics
    // are that the GPU work that wrote the image must have been *submitted*
    // before the release. The caller is expected to call submitter.Submit()
    // after this function returns, then mSwapchain.Release().
    return true;
}

void GameQuadLayer::ReleaseAcquired() {
    mSwapchain.Release();
}

void GameQuadLayer::BuildLayers(XrSpace space, XrCompositionLayerQuad outLayers[2]) const {
    if (!IsInitialised()) {
        outLayers[0] = {};
        outLayers[1] = {};
        return;
    }
    // Common geometry — driven by runtime values so the user can adjust
    // screen size and distance from the in-VR menu without restarting.
    const float kQuadWidth   = 1.0f * mScale;
    constexpr float kAspect  = 5.0f / 6.0f;              // 3DS dual-screen (400x480 -> 5:6)
    const float kQuadHeight  = kQuadWidth * kAspect;
    const float kDistance    = -mDistance;
    const float kEyeOffsetX  = mEyeOffset;

    for (int eye = 0; eye < 2; ++eye) {
        XrCompositionLayerQuad& q = outLayers[eye];
        q                        = {};
        q.type                   = XR_TYPE_COMPOSITION_LAYER_QUAD;
        // No BLEND_TEXTURE_SOURCE_ALPHA: Citra's renderer leaves the
        // framebuffer alpha at 0 (3DS games don't write alpha), which
        // would make the quad fully transparent if we asked the runtime
        // to alpha-blend. Treat the layer as opaque.
        q.layerFlags             = 0;
        q.space                  = space;
        q.eyeVisibility          = (eye == 0) ? XR_EYE_VISIBILITY_LEFT
                                              : XR_EYE_VISIBILITY_RIGHT;
        q.subImage.swapchain     = mSwapchain.Handle();
        q.subImage.imageRect.offset = {static_cast<int32_t>((mWidth / 2) * eye), 0};
        q.subImage.imageRect.extent = {static_cast<int32_t>(mWidth / 2),
                                       static_cast<int32_t>(mHeight)};
        q.subImage.imageArrayIndex = 0;

        const float xOffset      = (eye == 0 ? -1.0f : 1.0f) * kEyeOffsetX;
        q.pose                   = XrPosef{{0, 0, 0, 1}, {xOffset, 0.0f, kDistance}};
        q.size                   = XrExtent2Df{kQuadWidth, kQuadHeight};
    }
}

} // namespace vr_pcvr
