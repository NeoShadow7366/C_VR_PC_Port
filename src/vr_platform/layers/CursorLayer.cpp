// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/CursorLayer.h"
#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vr_pcvr {

namespace {

constexpr uint32_t kCursorTexel = 64;
constexpr VkDeviceSize kImageBytes = kCursorTexel * kCursorTexel * 4u;

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

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits,
                        VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

// Pack a premultiplied RGBA pixel for the swapchain's chosen format
// (RGBA8 vs BGRA8 swap order).
void Pack(int64_t format, float r, float g, float b, float a, uint8_t out[4]) {
    auto to8 = [](float f) {
        return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    out[3] = to8(a);
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        out[0] = to8(b * a); // B
        out[1] = to8(g * a); // G
        out[2] = to8(r * a); // R
    } else {
        out[0] = to8(r * a);
        out[1] = to8(g * a);
        out[2] = to8(b * a);
    }
}

// Generate a reticle: outer soft anti-aliased ring + small solid centre
// dot. Transparent everywhere else so the user can see what they're
// aiming at THROUGH the cursor. Premultiplied alpha so the OpenXR
// SOURCE_ALPHA blend produces the right composite.
void GenerateReticle(int64_t format, std::vector<uint8_t>& out,
                     float tintR, float tintG, float tintB) {
    out.assign(static_cast<size_t>(kImageBytes), 0);

    constexpr float cx = (kCursorTexel - 1) * 0.5f;
    constexpr float cy = (kCursorTexel - 1) * 0.5f;

    // Outer ring centered at radius rOuter, half-thickness ringHalf.
    constexpr float rOuter   = 26.0f;
    constexpr float ringHalf = 3.0f;
    // Centre dot half-width.
    constexpr float dotR     = 2.5f;
    // 1-texel feather at all edges.
    constexpr float feather  = 1.0f;

    for (uint32_t y = 0; y < kCursorTexel; ++y) {
        for (uint32_t x = 0; x < kCursorTexel; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);

            // Ring coverage: smooth band of width 2*ringHalf at radius rOuter.
            const float ringDist = std::abs(dist - rOuter);
            float ringA = std::clamp(1.0f - (ringDist - (ringHalf - feather)) / feather,
                                     0.0f, 1.0f);

            // Centre dot coverage: filled disc of radius dotR with feather.
            float dotA = std::clamp(1.0f - (dist - (dotR - feather)) / feather,
                                    0.0f, 1.0f);

            const float a = std::max(ringA, dotA);
            if (a <= 0.0f) continue;

            // Slight black 1-px outline outside the ring helps it stay
            // readable against bright backgrounds.
            const float outlineA = std::clamp(
                1.0f - (std::abs(dist - (rOuter + ringHalf)) - 0.5f) / feather,
                0.0f, 1.0f) * 0.6f;

            float r, g, b, alpha;
            if (outlineA > a) {
                r = 0.0f; g = 0.0f; b = 0.0f; alpha = outlineA;
            } else if (dotA >= ringA) {
                // Centre dot stays white -> high-contrast aim point.
                r = 1.0f; g = 1.0f; b = 1.0f; alpha = a;
            } else {
                // Ring picks up the per-hand tint.
                r = tintR; g = tintG; b = tintB; alpha = a;
            }

            const size_t off = (static_cast<size_t>(y) * kCursorTexel + x) * 4u;
            Pack(format, r, g, b, alpha, &out[off]);
        }
    }
}

} // namespace

bool CursorLayer::Init(XrSession session, VrFrameSubmitter& submitter,
                       VkPhysicalDevice physical) {
    if (!submitter.IsInitialised()) {
        ALOGE("CursorLayer: submitter not ready");
        return false;
    }
    mDevice = submitter.Device();

    const Tint tints[2] = {kLeftTint, kRightTint};
    const char* names[2] = {"left", "right"};

    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (!mSwapchains[hand].Create(session, kCursorTexel, kCursorTexel,
                                      /*vkFormat=*/0, /*arraySize=*/1, /*sampleCount=*/1)) {
            ALOGE("CursorLayer: %s swapchain creation failed", names[hand]);
            Shutdown();
            return false;
        }

        // ---- Staging buffer ------------------------------------------------
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = kImageBytes;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(mDevice, &bci, nullptr, &mStagingBuffers[hand]) != VK_SUCCESS) {
            ALOGE("CursorLayer: vkCreateBuffer failed (%s)", names[hand]);
            Shutdown();
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(mDevice, mStagingBuffers[hand], &req);
        const uint32_t typeIdx = FindMemoryType(
            physical, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (typeIdx == UINT32_MAX) {
            ALOGE("CursorLayer: no host-visible memory type");
            Shutdown();
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(mDevice, &mai, nullptr, &mStagingMemories[hand]) != VK_SUCCESS ||
            vkBindBufferMemory(mDevice, mStagingBuffers[hand],
                               mStagingMemories[hand], 0) != VK_SUCCESS) {
            ALOGE("CursorLayer: staging memory bind failed (%s)", names[hand]);
            Shutdown();
            return false;
        }

        // ---- CPU paint -> staging -----------------------------------------
        std::vector<uint8_t> pixels;
        GenerateReticle(mSwapchains[hand].Format(), pixels,
                        tints[hand].r, tints[hand].g, tints[hand].b);
        void* mapped = nullptr;
        if (vkMapMemory(mDevice, mStagingMemories[hand], 0, kImageBytes, 0, &mapped) != VK_SUCCESS) {
            ALOGE("CursorLayer: vkMapMemory failed (%s)", names[hand]);
            Shutdown();
            return false;
        }
        std::memcpy(mapped, pixels.data(), kImageBytes);
        vkUnmapMemory(mDevice, mStagingMemories[hand]);

        // ---- Acquire swapchain image + upload -----------------------------
        const uint32_t idx = mSwapchains[hand].AcquireAndWait();
        if (idx >= mSwapchains[hand].Images().size()) {
            ALOGE("CursorLayer: AcquireAndWait failed (%s)", names[hand]);
            Shutdown();
            return false;
        }
        VkImage img = mSwapchains[hand].Images()[idx];

        VkCommandBuffer cmd = submitter.Begin();
        if (cmd == VK_NULL_HANDLE) {
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }

        Barrier(cmd, img,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset       = {0, 0, 0};
        region.imageExtent       = {kCursorTexel, kCursorTexel, 1};
        vkCmdCopyBufferToImage(cmd, mStagingBuffers[hand], img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        Barrier(cmd, img,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        if (!submitter.Submit()) {
            ALOGE("CursorLayer: initial submit failed (%s)", names[hand]);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }
        mSwapchains[hand].Release();
    }

    ALOGI("CursorLayer initialised (%ux%u, per-hand tinted reticles)",
          kCursorTexel, kCursorTexel);
    return true;
}

void CursorLayer::Shutdown() {
    if (mDevice != VK_NULL_HANDLE) {
        for (uint32_t hand = 0; hand < 2; ++hand) {
            if (mStagingBuffers[hand] != VK_NULL_HANDLE) {
                vkDestroyBuffer(mDevice, mStagingBuffers[hand], nullptr);
                mStagingBuffers[hand] = VK_NULL_HANDLE;
            }
            if (mStagingMemories[hand] != VK_NULL_HANDLE) {
                vkFreeMemory(mDevice, mStagingMemories[hand], nullptr);
                mStagingMemories[hand] = VK_NULL_HANDLE;
            }
        }
    }
    mDevice = VK_NULL_HANDLE;
    for (auto& sc : mSwapchains) sc.Destroy();
    mCursors = {};
}

uint32_t CursorLayer::BuildLayers(XrSpace space, XrCompositionLayerQuad outLayers[4]) const {
    if (!IsInitialised()) return 0;

    static constexpr XrQuaternionf kIdentity{0.0f, 0.0f, 0.0f, 1.0f};
    static constexpr float kEyeOffsetX = 0.0325f;  // fallback (overridden by mEyeOffset)
    const float eyeOffset = mEyeOffset;

    auto fillCommon = [&](XrCompositionLayerQuad& q, uint32_t hand) {
        q                          = {};
        q.type                     = XR_TYPE_COMPOSITION_LAYER_QUAD;
        // Per-pixel premultiplied alpha so the transparent middle of
        // the reticle lets the underlying scene through.
        q.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space                    = space;
        q.subImage.swapchain       = mSwapchains[hand].Handle();
        q.subImage.imageRect       = {{0, 0},
                                      {static_cast<int32_t>(mSwapchains[hand].Width()),
                                       static_cast<int32_t>(mSwapchains[hand].Height())}};
        q.subImage.imageArrayIndex = 0;
        q.pose.orientation         = kIdentity;
        // Per-cursor scale: 1.0x at rest, 1.25x while hovering an
        // interactive element, plus an additive +60% spike during a
        // click pulse. Multiplied (not added) so a click on a hovered
        // target gives an even bigger pop.
        const float interactive = mCursors[hand].over_interactive ? 1.25f : 1.0f;
        const float pulse       = 1.0f + std::clamp(mCursors[hand].click_pulse, 0.0f, 1.0f) * 0.6f;
        const float side        = kCursorSizeMeters * interactive * pulse;
        q.size                     = {side, side};
    };

    uint32_t count = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!mCursors[i].visible) continue;
        if (mCursors[i].snap_to_screen) {
            for (int eye = 0; eye < 2; ++eye) {
                XrCompositionLayerQuad& q = outLayers[count++];
                fillCommon(q, i);
                q.eyeVisibility = (eye == 0) ? XR_EYE_VISIBILITY_LEFT
                                             : XR_EYE_VISIBILITY_RIGHT;
                const float xOffset = (eye == 0 ? -1.0f : 1.0f) * eyeOffset;
                q.pose.position = XrVector3f{mCursors[i].position.x + xOffset,
                                             mCursors[i].position.y,
                                             mCursors[i].position.z};
            }
        } else {
            XrCompositionLayerQuad& q = outLayers[count++];
            fillCommon(q, i);
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.pose.position = mCursors[i].position;
        }
    }
    return count;
}

} // namespace vr_pcvr
