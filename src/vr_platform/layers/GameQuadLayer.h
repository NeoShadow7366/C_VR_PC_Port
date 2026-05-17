// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game (3DS dual-screen) quad layer for the PCVR backend. Eventually
// composes the off-screen Citra Vulkan render target into a stereo
// pair of XrCompositionLayerQuad layers (one per eye, sharing a single
// swapchain image via XrSwapchainSubImage halves).
//
// At this point in the port the implementation is a stub - the build
// system, action set, and OpenXR session bring-up are validated first.

#pragma once

#include "Swapchain.h"
#include "XrPlatformIncludes.h"
#include "windows/EmuWindow_VR_Win.h"
#include "windows/VkXrSwapchain.h"

namespace vr_pcvr {

class GameQuadLayer {
public:
    GameQuadLayer()  = default;
    ~GameQuadLayer() = default;

    // width/height = full side-by-side stereo image (per-eye = width/2).
    bool Init(XrSession session, uint32_t width, uint32_t height);
    void Shutdown();

    // Blit the most recent off-screen Citra frame (published by
    // EmuWindow_VR_Win::PublishFrame) into the currently-acquired XR
    // swapchain image. `cmd` must be a command buffer recorded on a
    // queue compatible with both the source image (RendererVulkan) and
    // the XR swapchain image (the same VkDevice). Returns false if no
    // frame has been published yet, in which case the caller should
    // skip the layer for this XR frame.
    bool Blit(VkCommandBuffer cmd, const EmuWindow_VR_Win::PublishedFrame& src);

    // Release the XR swapchain image acquired by Blit(). Must be called
    // after the command buffer that recorded the blit has been submitted
    // to the queue (xrReleaseSwapchainImage requires the GPU work to be
    // in flight, not necessarily complete).
    void ReleaseAcquired();

    // Fill out the two quad-layer headers for xrEndFrame.
    // `space` is the head/local space the panels should follow.
    void BuildLayers(XrSpace space,
                     XrCompositionLayerQuad outLayers[2]) const;

    bool                IsInitialised() const { return mSwapchain.Handle() != XR_NULL_HANDLE; }
    const VkXrSwapchain& Swapchain()    const { return mSwapchain; }

    // Adjust the virtual screen geometry in real-time (changes take effect
    // on the next BuildLayers call and the matching VrApp touch raycast).
    void  SetGeometry(float scale, float distance) { mScale = scale; mDistance = distance; }
    void  SetEyeOffset(float offset) { mEyeOffset = offset; }
    float Scale()     const { return mScale; }
    float Distance()  const { return mDistance; }
    float EyeOffset() const { return mEyeOffset; }

private:
    VkXrSwapchain mSwapchain;
    uint32_t      mWidth     = 0;
    uint32_t      mHeight    = 0;
    float         mScale     = 1.0f;    // multiplier on base 1.0 m quad width
    float         mDistance  = 1.5f;    // metres in front of user
    float         mEyeOffset = 0.0325f; // per-eye X offset (half-IPD) in metres
};

} // namespace vr_pcvr
