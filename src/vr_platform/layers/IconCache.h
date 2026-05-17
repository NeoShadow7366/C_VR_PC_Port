// SPDX-License-Identifier: GPL-3.0-or-later
//
// IconCache — small Vulkan texture cache for in-VR ROM cards. Owns one
// VkSampler and per-icon (VkImage / VkImageView / VkDeviceMemory /
// VkDescriptorSet) resources keyed by a 64-bit id (typically the SMDH
// program_id, falling back to a hash of the full path).
//
// Uploads run on the dedicated VR Vulkan queue and are bracketed by
// vkQueueWaitIdle so they cannot race the per-frame blit. Each ROM
// only uploads once and is then drawn via ImGui::Image() with a
// stable ImTextureID.
//
// Threading: all methods called from the XR thread (same as ImGuiLayer).

#pragma once

#include "../XrPlatformIncludes.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ImDrawList; // for ImTextureID typedef
typedef void* ImTextureID;

namespace vr_pcvr {

class IconCache {
public:
    IconCache()  = default;
    ~IconCache() { Shutdown(); }
    IconCache(const IconCache&)            = delete;
    IconCache& operator=(const IconCache&) = delete;

    bool Init(VkDevice         device,
              VkPhysicalDevice physical,
              VkQueue          queue,
              uint32_t         queueFamilyIndex);
    void Shutdown();

    // Returns a stable ImGui texture id for `key`, uploading on first
    // sight. Returns nullptr on failure (caller should draw a placeholder).
    // `rgba` must be `w*h*4` bytes; format is RGBA8 unorm.
    ImTextureID GetOrUpload(std::uint64_t key,
                            std::uint32_t w,
                            std::uint32_t h,
                            const std::vector<std::uint8_t>& rgba);

    // True when this key is already cached (no upload needed).
    bool Has(std::uint64_t key) const { return mIcons.find(key) != mIcons.end(); }

private:
    struct Icon {
        VkImage         image    = VK_NULL_HANDLE;
        VkImageView     view     = VK_NULL_HANDLE;
        VkDeviceMemory  memory   = VK_NULL_HANDLE;
        VkDescriptorSet descSet  = VK_NULL_HANDLE; // owned by ImGui_ImplVulkan_AddTexture
    };

    bool DoUpload(std::uint32_t w, std::uint32_t h,
                  const std::vector<std::uint8_t>& rgba,
                  Icon& out);

    VkDevice         mDevice         = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkQueue          mQueue          = VK_NULL_HANDLE;
    uint32_t         mQueueFamilyIdx = 0;

    VkSampler        mSampler        = VK_NULL_HANDLE;
    VkCommandPool    mPool           = VK_NULL_HANDLE;

    std::unordered_map<std::uint64_t, Icon> mIcons;
};

} // namespace vr_pcvr
