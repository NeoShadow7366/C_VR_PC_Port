// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/IconCache.h"
#include "utils/LogUtils.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstring>

namespace vr_pcvr {

namespace {

// Pick a host-visible memory type for the staging buffer, or a
// device-local one for the image. Returns UINT32_MAX on failure.
uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                        VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

bool IconCache::Init(VkDevice         device,
                     VkPhysicalDevice physical,
                     VkQueue          queue,
                     uint32_t         queueFamilyIndex) {
    mDevice         = device;
    mPhysicalDevice = physical;
    mQueue          = queue;
    mQueueFamilyIdx = queueFamilyIndex;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod       = 0.0f;
    sci.maxLod       = 1.0f;
    sci.maxAnisotropy = 1.0f;
    if (vkCreateSampler(mDevice, &sci, nullptr, &mSampler) != VK_SUCCESS) {
        ALOGE("IconCache: vkCreateSampler failed");
        return false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                           VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = mQueueFamilyIdx;
    if (vkCreateCommandPool(mDevice, &pci, nullptr, &mPool) != VK_SUCCESS) {
        ALOGE("IconCache: vkCreateCommandPool failed");
        return false;
    }
    return true;
}

void IconCache::Shutdown() {
    if (mDevice == VK_NULL_HANDLE) return;
    // Make sure no in-flight queue work is using these descriptors.
    vkDeviceWaitIdle(mDevice);
    for (auto& kv : mIcons) {
        Icon& ic = kv.second;
        if (ic.descSet != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(ic.descSet);
            ic.descSet = VK_NULL_HANDLE;
        }
        if (ic.view   != VK_NULL_HANDLE) vkDestroyImageView(mDevice, ic.view,   nullptr);
        if (ic.image  != VK_NULL_HANDLE) vkDestroyImage    (mDevice, ic.image,  nullptr);
        if (ic.memory != VK_NULL_HANDLE) vkFreeMemory      (mDevice, ic.memory, nullptr);
    }
    mIcons.clear();
    if (mPool    != VK_NULL_HANDLE) vkDestroyCommandPool(mDevice, mPool,    nullptr);
    if (mSampler != VK_NULL_HANDLE) vkDestroySampler    (mDevice, mSampler, nullptr);
    mPool    = VK_NULL_HANDLE;
    mSampler = VK_NULL_HANDLE;
    mDevice  = VK_NULL_HANDLE;
}

ImTextureID IconCache::GetOrUpload(std::uint64_t key,
                                   std::uint32_t w,
                                   std::uint32_t h,
                                   const std::vector<std::uint8_t>& rgba) {
    if (mDevice == VK_NULL_HANDLE) return nullptr;
    if (auto it = mIcons.find(key); it != mIcons.end()) {
        return reinterpret_cast<ImTextureID>(it->second.descSet);
    }
    if (w == 0 || h == 0 || rgba.size() < std::size_t(w) * h * 4) {
        return nullptr;
    }
    Icon icon;
    if (!DoUpload(w, h, rgba, icon)) {
        // Best-effort cleanup of any partially created handles.
        if (icon.view   != VK_NULL_HANDLE) vkDestroyImageView(mDevice, icon.view,   nullptr);
        if (icon.image  != VK_NULL_HANDLE) vkDestroyImage    (mDevice, icon.image,  nullptr);
        if (icon.memory != VK_NULL_HANDLE) vkFreeMemory      (mDevice, icon.memory, nullptr);
        return nullptr;
    }
    icon.descSet = ImGui_ImplVulkan_AddTexture(
        mSampler, icon.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (icon.descSet == VK_NULL_HANDLE) {
        ALOGE("IconCache: ImGui_ImplVulkan_AddTexture failed for key 0x%llx",
              static_cast<unsigned long long>(key));
        vkDestroyImageView(mDevice, icon.view,   nullptr);
        vkDestroyImage    (mDevice, icon.image,  nullptr);
        vkFreeMemory      (mDevice, icon.memory, nullptr);
        return nullptr;
    }
    mIcons.emplace(key, icon);
    return reinterpret_cast<ImTextureID>(icon.descSet);
}

bool IconCache::DoUpload(std::uint32_t w, std::uint32_t h,
                         const std::vector<std::uint8_t>& rgba,
                         Icon& out) {
    constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;

    // ---- Image ------------------------------------------------------
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = kFmt;
    ici.extent      = {w, h, 1};
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(mDevice, &ici, nullptr, &out.image) != VK_SUCCESS) {
        ALOGE("IconCache: vkCreateImage failed"); return false;
    }
    VkMemoryRequirements mreq{};
    vkGetImageMemoryRequirements(mDevice, out.image, &mreq);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = mreq.size;
    mai.memoryTypeIndex = FindMemoryType(
        mPhysicalDevice, mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        ALOGE("IconCache: no DEVICE_LOCAL memory type"); return false;
    }
    if (vkAllocateMemory(mDevice, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        ALOGE("IconCache: vkAllocateMemory(image) failed"); return false;
    }
    vkBindImageMemory(mDevice, out.image, out.memory, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = kFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(mDevice, &vci, nullptr, &out.view) != VK_SUCCESS) {
        ALOGE("IconCache: vkCreateImageView failed"); return false;
    }

    // ---- Staging buffer (host-visible) ------------------------------
    VkBuffer       staging       = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size        = bytes;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(mDevice, &bci, nullptr, &staging) != VK_SUCCESS) {
        ALOGE("IconCache: vkCreateBuffer(staging) failed"); return false;
    }
    VkMemoryRequirements smreq{};
    vkGetBufferMemoryRequirements(mDevice, staging, &smreq);
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize  = smreq.size;
    smai.memoryTypeIndex = FindMemoryType(
        mPhysicalDevice, smreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (smai.memoryTypeIndex == UINT32_MAX) {
        ALOGE("IconCache: no HOST_VISIBLE memory type");
        vkDestroyBuffer(mDevice, staging, nullptr);
        return false;
    }
    if (vkAllocateMemory(mDevice, &smai, nullptr, &stagingMemory) != VK_SUCCESS) {
        ALOGE("IconCache: vkAllocateMemory(staging) failed");
        vkDestroyBuffer(mDevice, staging, nullptr);
        return false;
    }
    vkBindBufferMemory(mDevice, staging, stagingMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(mDevice, stagingMemory, 0, bytes, 0, &mapped);
    std::memcpy(mapped, rgba.data(), bytes);
    vkUnmapMemory(mDevice, stagingMemory);

    // ---- Record + submit one-shot command buffer --------------------
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool        = mPool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(mDevice, &cbi, &cmd) != VK_SUCCESS) {
        ALOGE("IconCache: vkAllocateCommandBuffers failed");
        vkDestroyBuffer(mDevice, staging, nullptr);
        vkFreeMemory   (mDevice, stagingMemory, nullptr);
        return false;
    }
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bbi);

    // UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image               = out.image;
    toDst.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask       = 0;
    toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier toSh{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSh.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSh.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSh.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSh.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSh.image               = out.image;
    toSh.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSh.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSh.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toSh);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    // Synchronous submit: wait for the upload to retire so the staging
    // buffer is safe to free immediately. Icons upload at most once
    // each so this never lands on the steady-state hot path.
    vkQueueSubmit(mQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(mQueue);

    vkFreeCommandBuffers(mDevice, mPool, 1, &cmd);
    vkDestroyBuffer(mDevice, staging, nullptr);
    vkFreeMemory   (mDevice, stagingMemory, nullptr);
    return true;
}

} // namespace vr_pcvr
