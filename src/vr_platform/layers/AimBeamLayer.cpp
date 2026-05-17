// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/AimBeamLayer.h"
#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vr_pcvr {

namespace {

// Per-hand tint: left = teal, right = amber (matches CursorLayer).
struct BeamTint { float r, g, b; };
static constexpr BeamTint kTints[2] = {
    {0.30f, 0.85f, 0.95f},   // left  — teal
    {1.00f, 0.72f, 0.30f},   // right — amber
};

// Clamp a float to [0,1] and convert to uint8.
static uint8_t To8(float f) {
    return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Build a 1×4 RGBA8 gradient: full tint at top, fading to transparent at
// bottom.  Premultiplied alpha.  4 rows keeps the texture trivially small.
static void PaintBeamGradient(int64_t fmt, float r, float g, float b,
                               uint8_t out[4 * 4]) {
    // Alpha steps: 0.50, 0.40, 0.28, 0.10  (bright near origin, fade out)
    static const float kAlphas[4] = {0.50f, 0.40f, 0.28f, 0.10f};
    for (int row = 0; row < 4; ++row) {
        const float a = kAlphas[row];
        uint8_t* p = out + row * 4;
        // Premultiplied: store (r*a, g*a, b*a, a).
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) {
            p[0] = To8(b * a); p[1] = To8(g * a); p[2] = To8(r * a); p[3] = To8(a);
        } else {
            p[0] = To8(r * a); p[1] = To8(g * a); p[2] = To8(b * a); p[3] = To8(a);
        }
    }
}

// Quaternion that rotates the +Y axis to `dir` (unit vector).
static XrQuaternionf QuatFromYToDir(const XrVector3f& dir) {
    // dot(+Y, dir) = dir.y
    const float d = dir.y;
    if (d > 0.9999f) return {0.0f, 0.0f, 0.0f, 1.0f};
    if (d < -0.9999f) return {0.0f, 0.0f, 1.0f, 0.0f};  // 180° around Z
    // cross(+Y, dir) = (dir.z, 0, -dir.x)
    const float cx = dir.z, cy = 0.0f, cz = -dir.x;
    const float len = std::sqrt(cx * cx + cz * cz);
    const float halfA = std::acos(std::clamp(d, -1.0f, 1.0f)) * 0.5f;
    const float s = std::sin(halfA) / len;
    return {cx * s, cy * s, cz * s, std::cos(halfA)};
}

} // namespace

bool AimBeamLayer::Init(XrSession session, VrFrameSubmitter& submitter,
                        VkPhysicalDevice physical) {
    if (!submitter.IsInitialised()) {
        ALOGE("AimBeamLayer: submitter not ready");
        return false;
    }

    for (uint32_t hand = 0; hand < 2; ++hand) {
        // 1×4 swapchain — tiny, just enough for the alpha gradient.
        if (!mSwapchains[hand].Create(session, 1, 4,
                                       /*vkFormat=*/0, /*arraySize=*/1,
                                       /*sampleCount=*/1)) {
            ALOGE("AimBeamLayer: swapchain[%u] creation failed", hand);
            Shutdown();
            return false;
        }

        const uint32_t idx = mSwapchains[hand].AcquireAndWait();
        if (idx >= mSwapchains[hand].Images().size()) {
            ALOGE("AimBeamLayer: AcquireAndWait[%u] failed", hand);
            Shutdown();
            return false;
        }
        VkImage img = mSwapchains[hand].Images()[idx];
        VkDevice dev = submitter.Device();

        // Staging buffer for the 4-pixel gradient.
        constexpr VkDeviceSize kBytes = 1 * 4 * 4; // 1w × 4h × 4bpp
        VkBuffer     stageBuf  = VK_NULL_HANDLE;
        VkDeviceMemory stageMem = VK_NULL_HANDLE;

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = kBytes;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bci, nullptr, &stageBuf) != VK_SUCCESS) {
            ALOGE("AimBeamLayer: vkCreateBuffer[%u] failed", hand);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, stageBuf, &req);
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
        uint32_t typeIdx = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                typeIdx = i; break;
            }
        }
        if (typeIdx == UINT32_MAX) {
            ALOGE("AimBeamLayer: no host-visible memory");
            vkDestroyBuffer(dev, stageBuf, nullptr);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(dev, &mai, nullptr, &stageMem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, stageBuf, stageMem, 0) != VK_SUCCESS) {
            ALOGE("AimBeamLayer: staging memory bind[%u] failed", hand);
            vkDestroyBuffer(dev, stageBuf, nullptr);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }

        // Paint gradient into staging memory.
        uint8_t pixels[4 * 4] = {};
        PaintBeamGradient(mSwapchains[hand].Format(),
                          kTints[hand].r, kTints[hand].g, kTints[hand].b,
                          pixels);
        void* mapped = nullptr;
        vkMapMemory(dev, stageMem, 0, kBytes, 0, &mapped);
        std::memcpy(mapped, pixels, kBytes);
        vkUnmapMemory(dev, stageMem);

        VkCommandBuffer cmd = submitter.Begin();
        if (cmd == VK_NULL_HANDLE) {
            vkFreeMemory(dev, stageMem, nullptr);
            vkDestroyBuffer(dev, stageBuf, nullptr);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }

        // Transition → upload → transition back.
        auto barrier = [&](VkImageLayout oldL, VkImageLayout newL,
                           VkAccessFlags src, VkAccessFlags dst,
                           VkPipelineStageFlags sp, VkPipelineStageFlags dp) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = src; b.dstAccessMask = dst;
            b.oldLayout = oldL;   b.newLayout = newL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, sp, dp, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 4, 1};
        vkCmdCopyBufferToImage(cmd, stageBuf, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        if (!submitter.Submit()) {
            ALOGE("AimBeamLayer: submit[%u] failed", hand);
            vkFreeMemory(dev, stageMem, nullptr);
            vkDestroyBuffer(dev, stageBuf, nullptr);
            mSwapchains[hand].Release();
            Shutdown();
            return false;
        }
        // Wait for the GPU copy to finish before freeing the staging buffer.
        // Submit() is asynchronous; destroying the staging VkDeviceMemory while
        // the GPU is still executing vkCmdCopyBufferToImage causes UB / device-lost.
        vkQueueWaitIdle(submitter.Queue());
        mSwapchains[hand].Release();

        // Safe to free now that the GPU has finished reading from staging.
        vkFreeMemory(dev, stageMem, nullptr);
        vkDestroyBuffer(dev, stageBuf, nullptr);
    }

    ALOGI("AimBeamLayer initialised (2 beams, tinted gradient)");
    return true;
}

void AimBeamLayer::Shutdown() {
    for (auto& sc : mSwapchains) sc.Destroy();
}

uint32_t AimBeamLayer::BuildLayers(XrSpace space,
                                    XrCompositionLayerQuad outLayers[2]) const {
    if (!IsInitialised()) return 0;

    uint32_t count = 0;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        const BeamSpec& b = mBeams[hand];
        if (!b.visible) continue;

        const float dx = b.endpoint.x - b.origin.x;
        const float dy = b.endpoint.y - b.origin.y;
        const float dz = b.endpoint.z - b.origin.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.01f) continue;  // sub-centimetre: skip

        const XrVector3f dir{dx / len, dy / len, dz / len};

        XrCompositionLayerQuad& q = outLayers[count++];
        q                          = {};
        q.type                     = XR_TYPE_COMPOSITION_LAYER_QUAD;
        q.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space                    = space;
        q.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain       = mSwapchains[hand].Handle();
        q.subImage.imageRect       = {{0, 0}, {1, 4}};
        q.subImage.imageArrayIndex = 0;

        // Beam quad: 4 mm wide, full ray length tall.
        // Positioned at the midpoint of origin→endpoint, oriented so
        // +Y points along the ray direction.
        q.size = {0.004f, len};
        q.pose.orientation = QuatFromYToDir(dir);
        q.pose.position    = {b.origin.x + dx * 0.5f,
                               b.origin.y + dy * 0.5f,
                               b.origin.z + dz * 0.5f};
    }
    return count;
}

} // namespace vr_pcvr
