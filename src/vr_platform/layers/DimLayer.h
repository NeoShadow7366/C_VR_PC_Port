// SPDX-License-Identifier: GPL-3.0-or-later
//
// DimLayer - a large, semi-transparent dark quad shown behind the in-VR
// menu while it's open. Acts as a modal scrim so menu text isn't read
// against bright moving game content (which would cause vergence
// fatigue and make small UI text harder to parse).
//
// Implementation mirrors CursorLayer / WristMenuButton: a tiny shared
// swapchain cleared once at init to a chosen colour+alpha and then
// re-displayed each frame as an XrCompositionLayerQuad with
// SOURCE_ALPHA blending.

#pragma once

#include "../XrPlatformIncludes.h"
#include "../windows/VkXrSwapchain.h"

#include <cstdint>

namespace vr_pcvr {

class VrFrameSubmitter;

class DimLayer {
public:
    DimLayer()  = default;
    ~DimLayer() { Shutdown(); }

    DimLayer(const DimLayer&)            = delete;
    DimLayer& operator=(const DimLayer&) = delete;

    bool Init(XrSession session, VrFrameSubmitter& submitter,
              float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 0.55f);
    void Shutdown();
    bool IsInitialised() const { return mSwapchain.Handle() != XR_NULL_HANDLE; }

    // Re-paint the swapchain image to the configured RGB at a new
    // alpha. Cheap (one tiny command buffer, single clear, no staging
    // buffer); call it each frame the menu's fade alpha changes so the
    // scrim ramps in/out together with the menu instead of popping.
    // No-op if `alpha` matches the last applied value.
    void SetAlpha(VrFrameSubmitter& submitter, float alpha);
    float Alpha() const { return mCurrentAlpha; }

    void SetPose(const XrPosef& pose, bool visible) {
        mPose    = pose;
        mVisible = visible;
    }
    void SetSize(float widthMeters, float heightMeters) {
        mSizeW = widthMeters;
        mSizeH = heightMeters;
    }

    bool BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const;

private:
    VkXrSwapchain mSwapchain;
    XrPosef       mPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool          mVisible      = false;
    float         mSizeW        = 2.4f;   // metres
    float         mSizeH        = 1.6f;
    float         mBaseR        = 0.0f;
    float         mBaseG        = 0.0f;
    float         mBaseB        = 0.0f;
    float         mBaseA        = 0.55f;
    float         mCurrentAlpha = -1.0f;  // last alpha applied via SetAlpha
};

} // namespace vr_pcvr
