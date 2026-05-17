// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/WristMenuButton.h"
#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace vr_pcvr {

namespace {

constexpr uint32_t kTexel        = 64;
constexpr uint32_t kPixelStride  = 4;                          // RGBA8
constexpr VkDeviceSize kImageBytes = kTexel * kTexel * kPixelStride;

// Rounded-corner radius and icon stroke (in texels).
constexpr int kCornerRadius = 12;
constexpr int kStroke       = 5;

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

// ---- procedural CPU painting --------------------------------------------

struct PixelRGBA {
    uint8_t r, g, b, a;
};

// Whether the texel (x,y) sits inside the rounded-square mask. Returns a
// coverage value in [0,1] for a 1-texel-wide soft edge.
float RoundedMaskCoverage(int x, int y) {
    const int w = static_cast<int>(kTexel);
    const int h = static_cast<int>(kTexel);
    const int r = kCornerRadius;
    // Distance from the nearest "centre" of the rounded corner arcs.
    int cx = std::clamp(x, r, w - 1 - r);
    int cy = std::clamp(y, r, h - 1 - r);
    int dx = x - cx;
    int dy = y - cy;
    const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
    // Inside the inflated square: dist == 0 => fully inside the rect.
    // For corner texels: dist > 0 => circular falloff at radius r.
    if (dist <= static_cast<float>(r) - 0.5f) return 1.0f;
    if (dist >= static_cast<float>(r) + 0.5f) return 0.0f;
    return (static_cast<float>(r) + 0.5f - dist); // ~1px soft edge
}

bool IconHamburger(int x, int y) {
    // 3 horizontal bars at roughly y=18, 32, 46 (centred), width ~40px.
    const int bar_w_half = (kTexel - 24) / 2; // 20
    const int cx = kTexel / 2;
    if (x < cx - bar_w_half || x > cx + bar_w_half) return false;
    const int half = kStroke / 2;
    const std::array<int, 3> ys = {18, 32, 46};
    for (int yc : ys) {
        if (y >= yc - half && y <= yc + half) return true;
    }
    return false;
}

bool IconPlayTriangle(int x, int y) {
    // Right-pointing triangle with vertices (18,14), (18,50), (50,32).
    // Implicit form: inside if x >= 18, and (x-18) <= a*(32 - |y-32|) where
    // a = (50-18)/(32-14) = 32/18.
    if (x < 18) return false;
    const int dy = std::abs(y - 32);
    if (dy > 18) return false;
    const float allowed = (32.0f / 18.0f) * static_cast<float>(18 - dy);
    return static_cast<float>(x - 18) <= allowed;
}

bool IconMinusBar(int x, int y) {
    // Single horizontal bar centred, width ~28px, height kStroke.
    const int bar_w_half = 14;
    const int cx = kTexel / 2;
    if (x < cx - bar_w_half || x > cx + bar_w_half) return false;
    const int half = kStroke / 2;
    return y >= 32 - half && y <= 32 + half;
}

// Stylised controller silhouette: a vertical capsule body 36px tall x
// 20px wide centred, with a small triangle pointing up at the top
// (the "muzzle" / aim direction). Used for both ControllerL/R; the
// hand letter is overlaid separately.
bool IconControllerBody(int x, int y) {
    const int cx = kTexel / 2;
    // Vertical capsule: x in [cx-10, cx+10], y in [16, 52], rounded ends.
    const int dx = x - cx;
    if (std::abs(dx) > 10) return false;
    if (y < 16 || y > 52) return false;
    // Round the top (y < 22) and bottom (y > 46) caps.
    if (y < 22) {
        const int dy = 22 - y;
        return (dx * dx + dy * dy) <= 100; // r=10
    }
    if (y > 46) {
        const int dy = y - 46;
        return (dx * dx + dy * dy) <= 100;
    }
    return true;
}

// Letter "L" : vertical bar + bottom bar.
bool IconLetterL(int x, int y) {
    // vertical bar: x in [27..32], y in [22..42]
    if (x >= 27 && x <= 32 && y >= 22 && y <= 42) return true;
    // bottom bar: x in [27..40], y in [38..42]
    if (x >= 27 && x <= 40 && y >= 38 && y <= 42) return true;
    return false;
}

// Letter "R" : vertical bar + arc/top crossbar + diagonal leg.
bool IconLetterR(int x, int y) {
    // vertical bar
    if (x >= 24 && x <= 29 && y >= 22 && y <= 42) return true;
    // top crossbar
    if (x >= 24 && x <= 36 && y >= 22 && y <= 26) return true;
    // mid crossbar (closing the "P" loop)
    if (x >= 24 && x <= 36 && y >= 30 && y <= 34) return true;
    // right edge of the P loop
    if (x >= 33 && x <= 37 && y >= 22 && y <= 34) return true;
    // diagonal leg from (29,33) to (40,42)
    {
        const int dx = x - 29;
        const int dy = y - 33;
        if (dx >= 0 && dy >= 0 && dx <= 11 && dy <= 9) {
            if (std::abs(dx - dy) <= 2) return true;
        }
    }
    return false;
}

void PaintIcon(WristIconKind icon, int x, int y, bool& out_inside) {
    switch (icon) {
    case WristIconKind::Hamburger:    out_inside = IconHamburger(x, y); break;
    case WristIconKind::PlayTriangle: out_inside = IconPlayTriangle(x, y); break;
    case WristIconKind::MinusBar:     out_inside = IconMinusBar(x, y); break;
    case WristIconKind::ControllerL:
        out_inside = IconControllerBody(x, y) || IconLetterL(x, y);
        break;
    case WristIconKind::ControllerR:
        out_inside = IconControllerBody(x, y) || IconLetterR(x, y);
        break;
    }
}

// Pack a pixel for the swapchain's actual format. Both RGBA and BGRA
// UNORM/SRGB are stored as 4 bytes; the channel order differs.
PixelRGBA Pack(int64_t format, float r, float g, float b, float a) {
    auto to8 = [](float f) {
        return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    PixelRGBA p{};
    p.a = to8(a);
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) {
        p.r = to8(b); // first byte = B
        p.g = to8(g);
        p.b = to8(r); // third byte = R
    } else {
        p.r = to8(r);
        p.g = to8(g);
        p.b = to8(b);
    }
    return p;
}

// Generate the full RGBA texture for a given background colour and icon
// glyph. Premultiplied alpha (so SteamVR can blend with the
// SOURCE_ALPHA flag we set on the composition layer). When `hovered`
// is true an additional soft glow is painted in the texels OUTSIDE
// the rounded-square mask so the button reads as "raised / lit up".
void GenerateTexture(int64_t format, WristIconKind icon,
                     float bgR, float bgG, float bgB, bool hovered,
                     std::vector<uint8_t>& out) {
    out.assign(kImageBytes, 0);
    // Border = a slightly brighter shade of the background.
    const float borderR = std::min(1.0f, bgR + 0.20f);
    const float borderG = std::min(1.0f, bgG + 0.20f);
    const float borderB = std::min(1.0f, bgB + 0.20f);

    for (uint32_t y = 0; y < kTexel; ++y) {
        for (uint32_t x = 0; x < kTexel; ++x) {
            const float maskCov = RoundedMaskCoverage(static_cast<int>(x),
                                                     static_cast<int>(y));

            const int dx = static_cast<int>(x);
            const int dy = static_cast<int>(y);
            const int w = kTexel, h = kTexel, r = kCornerRadius;
            int cx = std::clamp(dx, r, w - 1 - r);
            int cy = std::clamp(dy, r, h - 1 - r);
            const float dist = std::sqrt(
                static_cast<float>((dx - cx) * (dx - cx) + (dy - cy) * (dy - cy)));

            // OUTSIDE the rounded mask: optionally paint a soft hover
            // halo. Falls off over ~kHaloRadius texels.
            if (maskCov <= 0.0f) {
                if (!hovered) continue;
                constexpr float kHaloRadius = 10.0f;
                // Distance from the rounded-square boundary outward.
                float outside = 0.0f;
                if (dx == cx && dy == cy) {
                    // Pure interior of the rect (shouldn't happen here).
                    continue;
                }
                outside = dist - static_cast<float>(r);
                if (outside <= 0.0f || outside >= kHaloRadius) continue;
                // Smooth squared falloff.
                const float t = 1.0f - (outside / kHaloRadius);
                const float haloA = t * t * 0.55f;
                // Halo colour = brighter background tint, premultiplied.
                const float hr = std::min(1.0f, bgR + 0.30f);
                const float hg = std::min(1.0f, bgG + 0.30f);
                const float hb = std::min(1.0f, bgB + 0.30f);
                PixelRGBA px = Pack(format, hr * haloA, hg * haloA, hb * haloA, haloA);
                const size_t off = (static_cast<size_t>(y) * kTexel + x) * kPixelStride;
                out[off + 0] = px.r;
                out[off + 1] = px.g;
                out[off + 2] = px.b;
                out[off + 3] = px.a;
                continue;
            }

            const bool isBorder =
                (dx == 0 || dy == 0 || dx == w - 1 || dy == h - 1)
                || dist >= static_cast<float>(r) - 1.5f;

            float pr = bgR, pg = bgG, pb = bgB;
            if (isBorder) {
                pr = borderR; pg = borderG; pb = borderB;
            }

            bool insideIcon = false;
            PaintIcon(icon, dx, dy, insideIcon);
            if (insideIcon) {
                pr = 1.0f; pg = 1.0f; pb = 1.0f;
            }

            const float a = maskCov; // soft alpha at the rounded edge
            // Premultiply colour by alpha for SOURCE_ALPHA blending.
            PixelRGBA px = Pack(format, pr * a, pg * a, pb * a, a);
            const size_t off = (static_cast<size_t>(y) * kTexel + x) * kPixelStride;
            out[off + 0] = px.r;
            out[off + 1] = px.g;
            out[off + 2] = px.b;
            out[off + 3] = px.a;
        }
    }
}

} // namespace

bool WristMenuButton::CreateStaging(VkPhysicalDevice physical) {
    if (mStagingBuffer != VK_NULL_HANDLE) return true;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size        = kImageBytes;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(mDevice, &bci, nullptr, &mStagingBuffer) != VK_SUCCESS) {
        ALOGE("WristMenuButton: vkCreateBuffer failed");
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(mDevice, mStagingBuffer, &req);
    const uint32_t typeIdx = FindMemoryType(
        physical, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (typeIdx == UINT32_MAX) {
        ALOGE("WristMenuButton: no host-visible+coherent memory type");
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(mDevice, &mai, nullptr, &mStagingMemory) != VK_SUCCESS) {
        ALOGE("WristMenuButton: vkAllocateMemory failed");
        return false;
    }
    if (vkBindBufferMemory(mDevice, mStagingBuffer, mStagingMemory, 0) != VK_SUCCESS) {
        ALOGE("WristMenuButton: vkBindBufferMemory failed");
        return false;
    }
    return true;
}

void WristMenuButton::DestroyStaging() {
    if (mDevice == VK_NULL_HANDLE) return;
    if (mStagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(mDevice, mStagingBuffer, nullptr);
        mStagingBuffer = VK_NULL_HANDLE;
    }
    if (mStagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(mDevice, mStagingMemory, nullptr);
        mStagingMemory = VK_NULL_HANDLE;
    }
}

bool WristMenuButton::Repaint(VrFrameSubmitter& submitter, float r, float g, float b) {
    if (!IsInitialised() || !submitter.IsInitialised()
        || mStagingBuffer == VK_NULL_HANDLE) return false;

    // CPU-paint the procedural texture into the staging buffer. Pass
    // the current hover state so the painter can add the outer glow.
    std::vector<uint8_t> pixels;
    GenerateTexture(mSwapchain.Format(), mIcon, r, g, b, mLastHovered, pixels);
    void* mapped = nullptr;
    if (vkMapMemory(mDevice, mStagingMemory, 0, kImageBytes, 0, &mapped) != VK_SUCCESS) {
        ALOGE("WristMenuButton: vkMapMemory failed");
        return false;
    }
    std::memcpy(mapped, pixels.data(), kImageBytes);
    vkUnmapMemory(mDevice, mStagingMemory);

    const uint32_t idx = mSwapchain.AcquireAndWait();
    if (idx >= mSwapchain.Images().size()) {
        ALOGE("WristMenuButton: AcquireAndWait failed");
        return false;
    }
    VkImage img = mSwapchain.Images()[idx];

    VkCommandBuffer cmd = submitter.Begin();
    if (cmd == VK_NULL_HANDLE) {
        mSwapchain.Release();
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
    region.imageExtent       = {kTexel, kTexel, 1};
    vkCmdCopyBufferToImage(cmd, mStagingBuffer, img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    Barrier(cmd, img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    if (!submitter.Submit()) {
        ALOGE("WristMenuButton: repaint submit failed");
        mSwapchain.Release();
        return false;
    }
    mSwapchain.Release();
    return true;
}

bool WristMenuButton::Init(XrSession session, VrFrameSubmitter& submitter,
                           VkPhysicalDevice physical, WristIconKind icon,
                           float restR, float restG, float restB,
                           float hovR,  float hovG,  float hovB) {
    if (!mSwapchain.Create(session, kTexel, kTexel,
                           /*vkFormat=*/0, /*arraySize=*/1, /*sampleCount=*/1)) {
        ALOGE("WristMenuButton: swapchain creation failed");
        return false;
    }
    mDevice = submitter.Device();
    mIcon   = icon;
    mRestR = restR; mRestG = restG; mRestB = restB;
    mHovR  = hovR;  mHovG  = hovG;  mHovB  = hovB;
    if (!CreateStaging(physical)) {
        Shutdown();
        return false;
    }
    if (!Repaint(submitter, mRestR, mRestG, mRestB)) {
        Shutdown();
        return false;
    }
    ALOGI("WristMenuButton initialised (%ux%u, icon=%d)", kTexel, kTexel,
          static_cast<int>(icon));
    return true;
}

void WristMenuButton::Shutdown() {
    DestroyStaging();
    mDevice = VK_NULL_HANDLE;
    mSwapchain.Destroy();
    mVisible     = false;
    mLastHovered = false;
}

void WristMenuButton::SetHovered(bool hovered, VrFrameSubmitter& submitter) {
    if (hovered == mLastHovered) return;
    mLastHovered = hovered;
    if (hovered) {
        Repaint(submitter, mHovR, mHovG, mHovB);
    } else {
        Repaint(submitter, mRestR, mRestG, mRestB);
    }
}

bool WristMenuButton::BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const {
    if (!IsInitialised() || !mVisible) return false;
    outLayer = {};
    outLayer.type                     = XR_TYPE_COMPOSITION_LAYER_QUAD;
    // Per-pixel alpha (premultiplied) so the rounded corners are
    // transparent and the icon background blends cleanly with whatever
    // the user is looking at.
    outLayer.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    outLayer.space                    = space;
    outLayer.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
    outLayer.subImage.swapchain       = mSwapchain.Handle();
    outLayer.subImage.imageRect       = {{0, 0},
                                         {static_cast<int32_t>(mSwapchain.Width()),
                                          static_cast<int32_t>(mSwapchain.Height())}};
    outLayer.subImage.imageArrayIndex = 0;
    outLayer.pose                     = mPose;
    outLayer.size                     = {kSizeMeters, kSizeMeters};
    return true;
}

} // namespace vr_pcvr
