// SPDX-License-Identifier: GPL-3.0-or-later
//
// VrFrameSubmitter owns a small ring of single-use Vulkan command buffers
// + fences used by the OpenXR thread to record per-frame blits from the
// off-screen Citra render target into the XR composition swapchain.
//
// It deliberately reuses Citra's existing VkDevice / VkQueue (obtained
// from WinPlatform) instead of opening a second device. The ring depth
// (kInFlightFrames) matches a typical XR runtime's swapchain image count
// so we never block on a fence under steady-state.

#pragma once

#include "../XrPlatformIncludes.h"

#include <array>
#include <cstdint>

namespace vr_pcvr {

class VrFrameSubmitter {
public:
    static constexpr uint32_t kInFlightFrames = 3;

    VrFrameSubmitter()  = default;
    ~VrFrameSubmitter() { Destroy(); }

    VrFrameSubmitter(const VrFrameSubmitter&)            = delete;
    VrFrameSubmitter& operator=(const VrFrameSubmitter&) = delete;

    bool Init(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex);
    void Destroy();

    bool IsInitialised() const { return mDevice != VK_NULL_HANDLE; }

    // Accessors for callers that need to do their own one-shot Vulkan
    // work (e.g. staging-buffer uploads) on the same device/queue/family
    // we were initialised with.
    VkDevice Device() const { return mDevice; }
    VkQueue  Queue()  const { return mQueue;  }

    // Begin recording into the next free command buffer. Waits on its
    // associated fence first (so the buffer's previous submission has
    // fully retired). Returns VK_NULL_HANDLE on failure.
    //
    // When `block` is false the call is non-blocking: if the next
    // buffer's fence has not yet retired we return VK_NULL_HANDLE
    // immediately so the XR thread can drop this frame's blit and keep
    // its xrEndFrame cadence rather than stalling on a slow GPU.
    VkCommandBuffer Begin(bool block = true);

    // Submit the previously-Begin'd command buffer. `waitSemaphore` may
    // be VK_NULL_HANDLE (no external wait). Signals the per-frame fence
    // that Begin() will wait on next time around. If `externalSignalFence`
    // is non-null, it is signalled by an additional empty submit on the
    // same queue, so it fires only after the recorded blit has retired.
    bool Submit(VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT,
                VkFence externalSignalFence = VK_NULL_HANDLE);

private:
    VkDevice                                       mDevice = VK_NULL_HANDLE;
    VkQueue                                        mQueue  = VK_NULL_HANDLE;
    VkCommandPool                                  mPool   = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kInFlightFrames>   mCmdBufs{};
    std::array<VkFence,         kInFlightFrames>   mFences{};
    uint32_t                                       mCurrent = 0;
    bool                                           mRecording = false;
};

} // namespace vr_pcvr
