// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCVR cursor layer. Renders a small floating dot at each hand's
// "pointer tip" position so the user can see where their controllers
// are aiming. The dot is a 2cm opaque white quad backed by a tiny
// shared XR swapchain that is cleared to white once at init and then
// re-displayed every frame without further GPU work.

#pragma once

#include "../XrPlatformIncludes.h"
#include "../windows/VkXrSwapchain.h"

#include <array>
#include <cstdint>

namespace vr_pcvr {

class VrFrameSubmitter;

class CursorLayer {
public:
    struct CursorSpec {
        XrVector3f position{0.0f, 0.0f, 0.0f};
        bool       visible = false;
        // When true the cursor is snapped onto the GameQuadLayer's plane.
        // That layer uses a per-eye horizontal offset (±kEyeOffsetX) to
        // place 2D content at infinite depth (zero stereo disparity); a
        // cursor sitting at one world position in LOCAL space would have
        // ~1.5m parallax and visually float in front of the screen pixel
        // it's pointing at. When this flag is set, BuildLayers emits two
        // per-eye quads with the same offset so the cursor matches the
        // screen's apparent depth. Non-snapped cursors (floating at the
        // controller tip, or snapped to the in-VR menu which uses normal
        // stereo depth) emit a single BOTH-eye quad.
        bool       snap_to_screen = false;

        // True when the cursor is hovering an interactive element
        // (wrist button, menu widget). Bumps the quad to 1.25x size so
        // the user gets a visible "this is clickable" cue without
        // needing a different texture.
        bool       over_interactive = false;

        // Click feedback pulse [0..1]. Caller bumps to 1.0 on a
        // trigger-press over an interactive target and decays it each
        // frame. Adds a brief +60% scale spike on top of any other
        // scaling so the click reads as a confirmed action even
        // without haptics.
        float      click_pulse = 0.0f;
    };

    static constexpr float kCursorSizeMeters = 0.025f;

    // Default per-hand reticle tints. Left = teal, right = amber.
    // The reticle's outer ring picks up the tint; the centre dot stays
    // white so the aim point is high-contrast on any background.
    struct Tint { float r, g, b; };
    static constexpr Tint kLeftTint  {0.30f, 0.85f, 0.95f};
    static constexpr Tint kRightTint {1.00f, 0.72f, 0.30f};

    CursorLayer()  = default;
    ~CursorLayer() { Shutdown(); }

    CursorLayer(const CursorLayer&)            = delete;
    CursorLayer& operator=(const CursorLayer&) = delete;

    // Allocate the shared swapchain and paint the reticle texture once.
    // Uses `submitter` for the upload command buffer; the submitter
    // remains owned by the caller and is reused for per-frame work.
    bool Init(XrSession session, VrFrameSubmitter& submitter,
              VkPhysicalDevice physical);
    void Shutdown();
    bool IsInitialised() const { return mSwapchains[0].Handle() != XR_NULL_HANDLE; }

    void SetCursors(const CursorSpec& left, const CursorSpec& right) {
        mCursors[0] = left;
        mCursors[1] = right;
    }
    void SetEyeOffset(float offset) { mEyeOffset = offset; }

    // Populate `outLayers` (capacity 4) with one or two quads per visible
    // cursor (two when snap_to_screen is set, see CursorSpec). Returns the
    // number of layers actually written (0..4).
    uint32_t BuildLayers(XrSpace space, XrCompositionLayerQuad outLayers[4]) const;

private:
    std::array<VkXrSwapchain, 2>   mSwapchains{};
    std::array<CursorSpec, 2>      mCursors{};
    float                          mEyeOffset = 0.0325f; // per-eye X offset matching GameQuadLayer

    VkDevice                        mDevice           = VK_NULL_HANDLE;
    std::array<VkBuffer, 2>         mStagingBuffers   {VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::array<VkDeviceMemory, 2>   mStagingMemories  {VK_NULL_HANDLE, VK_NULL_HANDLE};
};

} // namespace vr_pcvr
