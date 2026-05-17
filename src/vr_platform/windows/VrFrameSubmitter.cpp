// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"
#include "video_core/renderer_vulkan/vk_vr_hooks.h"

#include <mutex>

namespace vr_pcvr {

bool VrFrameSubmitter::Init(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex) {
    Destroy();
    mDevice = device;
    mQueue  = queue;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                         | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamilyIndex;
    if (vkCreateCommandPool(mDevice, &pci, nullptr, &mPool) != VK_SUCCESS) {
        ALOGE("VrFrameSubmitter: vkCreateCommandPool failed");
        Destroy();
        return false;
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = mPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kInFlightFrames;
    if (vkAllocateCommandBuffers(mDevice, &ai, mCmdBufs.data()) != VK_SUCCESS) {
        ALOGE("VrFrameSubmitter: vkAllocateCommandBuffers failed");
        Destroy();
        return false;
    }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kInFlightFrames; ++i) {
        if (vkCreateFence(mDevice, &fci, nullptr, &mFences[i]) != VK_SUCCESS) {
            ALOGE("VrFrameSubmitter: vkCreateFence failed");
            Destroy();
            return false;
        }
    }
    return true;
}

void VrFrameSubmitter::Destroy() {
    if (mDevice != VK_NULL_HANDLE) {
        // Wait for any outstanding work; fences may still be in-flight.
        for (auto f : mFences) {
            if (f != VK_NULL_HANDLE) {
                vkWaitForFences(mDevice, 1, &f, VK_TRUE, UINT64_MAX);
                vkDestroyFence(mDevice, f, nullptr);
            }
        }
        if (mPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(mDevice, mPool, nullptr);
        }
    }
    mFences.fill(VK_NULL_HANDLE);
    mCmdBufs.fill(VK_NULL_HANDLE);
    mPool      = VK_NULL_HANDLE;
    mQueue     = VK_NULL_HANDLE;
    mDevice    = VK_NULL_HANDLE;
    mCurrent   = 0;
    mRecording = false;
}

VkCommandBuffer VrFrameSubmitter::Begin(bool block) {
    if (mDevice == VK_NULL_HANDLE || mRecording) {
        return VK_NULL_HANDLE;
    }
    VkFence fence  = mFences[mCurrent];
    if (!block) {
        // Non-blocking: probe the fence. If GPU is still chewing on the
        // previous frame in this slot, bail out so the caller can drop
        // this frame's blit and keep the XR cadence intact.
        const VkResult st = vkGetFenceStatus(mDevice, fence);
        if (st == VK_NOT_READY) {
            return VK_NULL_HANDLE;
        }
        if (st != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
    } else if (vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    vkResetFences(mDevice, 1, &fence);

    VkCommandBuffer cmd = mCmdBufs[mCurrent];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    mRecording = true;
    return cmd;
}

bool VrFrameSubmitter::Submit(VkSemaphore waitSemaphore, VkPipelineStageFlags waitStage,
                              VkFence externalSignalFence) {
    if (!mRecording) {
        return false;
    }
    VkCommandBuffer cmd = mCmdBufs[mCurrent];
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        mRecording = false;
        return false;
    }

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    if (waitSemaphore != VK_NULL_HANDLE) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores    = &waitSemaphore;
        si.pWaitDstStageMask  = &waitStage;
    }
    const VkResult r = [&] {
        std::scoped_lock vr_lock{Vulkan::GetVrQueueMutex()};
        const VkResult r1 = vkQueueSubmit(mQueue, 1, &si, mFences[mCurrent]);
        if (r1 != VK_SUCCESS || externalSignalFence == VK_NULL_HANDLE) {
            return r1;
        }
        // Second empty submit on the same queue, signalling the caller's
        // fence. Submission order on a single queue guarantees this fence
        // is signalled only after the blit cmdbuf has retired - i.e.
        // after the source image has actually been read.
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        return vkQueueSubmit(mQueue, 1, &empty, externalSignalFence);
    }();
    mRecording = false;
    mCurrent   = (mCurrent + 1) % kInFlightFrames;
    if (r != VK_SUCCESS) {
        ALOGE("VrFrameSubmitter: vkQueueSubmit -> %d", static_cast<int>(r));
        return false;
    }
    return true;
}

} // namespace vr_pcvr
