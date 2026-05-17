// SPDX-License-Identifier: GPL-3.0-or-later
//
// WristMenuButton - a small coloured quad anchored above the user's
// left wrist that opens / closes the in-VR menu when pointed at and
// triggered with either hand. Provides a controller-button-independent
// way to summon the menu, useful when buttons are remapped or busy.
//
// Internals mirror CursorLayer: a tiny shared XR swapchain cleared
// once to an opaque accent colour at init, then re-displayed each
// frame as an XrCompositionLayerQuad at a caller-supplied pose.

#pragma once

#include "../XrPlatformIncludes.h"
#include "../windows/VkXrSwapchain.h"

#include <cstdint>

namespace vr_pcvr {

class VrFrameSubmitter;

// Glyph drawn in white on the centre of the button. Pure procedural
// (no font / asset dependency) so the button stays self-contained.
enum class WristIconKind {
    Hamburger,    // 3 horizontal bars  -> Menu
    PlayTriangle, // right-pointing triangle -> Start
    MinusBar,     // single thin horizontal bar -> Select
    ControllerL,  // stylised controller silhouette with "L" glyph
    ControllerR,  // stylised controller silhouette with "R" glyph
};

class WristMenuButton {
public:
    // World-space size of the button quad (square).
    static constexpr float kSizeMeters = 0.045f;

    WristMenuButton()  = default;
    ~WristMenuButton() { Shutdown(); }

    WristMenuButton(const WristMenuButton&)            = delete;
    WristMenuButton& operator=(const WristMenuButton&) = delete;

    bool Init(XrSession session, VrFrameSubmitter& submitter,
              VkPhysicalDevice physical, WristIconKind icon,
              float restR = 0.20f, float restG = 0.55f, float restB = 0.85f,
              float hovR  = 0.40f, float hovG  = 0.85f, float hovB  = 1.00f);
    void Shutdown();
    bool IsInitialised() const { return mSwapchain.Handle() != XR_NULL_HANDLE; }

    // Set externally each frame. visible=false omits the layer entirely.
    void SetPose(const XrPosef& pose, bool visible) {
        mPose    = pose;
        mVisible = visible;
    }
    // Optional: when hovered the quad re-paints to a brighter colour so
    // the user gets immediate hover feedback. State change is one-shot
    // per transition; safe to call every frame.
    void SetHovered(bool hovered, VrFrameSubmitter& submitter);

    bool IsVisible() const { return mVisible; }
    const XrPosef& Pose() const { return mPose; }

    // Populate `outLayer` and return true if the layer should be added
    // to xrEndFrame. `space` is the reference space the pose lives in
    // (typically LocalSpace).
    bool BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const;

private:
    bool Repaint(VrFrameSubmitter& submitter, float r, float g, float b);
    void DestroyStaging();
    bool CreateStaging(VkPhysicalDevice physical);

    VkXrSwapchain mSwapchain;
    XrPosef       mPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool          mVisible        = false;
    bool          mLastHovered    = false;
    WristIconKind mIcon           = WristIconKind::Hamburger;
    float         mRestR = 0.0f, mRestG = 0.0f, mRestB = 0.0f;
    float         mHovR  = 0.0f, mHovG  = 0.0f, mHovB  = 0.0f;

    // CPU-side staging used to upload the procedural icon RGBA texture.
    VkDevice       mDevice        = VK_NULL_HANDLE;
    VkBuffer       mStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mStagingMemory = VK_NULL_HANDLE;
};

} // namespace vr_pcvr
