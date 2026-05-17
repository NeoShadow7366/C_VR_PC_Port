// SPDX-License-Identifier: GPL-3.0-or-later
//
// Win32 / SteamVR replacement for src/android/app/src/main/jni/emu_window/emu_window_vk.{h,cpp}.
//
// On Quest the original CitraVR pumped Citra's video output into an
// `ANativeWindow` (an Android `Surface`) that was, in turn, the back-end of
// the GameSurfaceLayer's OpenXR Android-surface swapchain. There is no
// equivalent path on PCVR: SteamVR's OpenXR runtime expects the application
// to render into a vendor-neutral `XrSwapchain` of `VkImage`s.
//
// `EmuWindow_VR_Win` therefore advertises itself to the Citra core as a
// HEADLESS frontend - the renderer must not create a Win32 `VkSurfaceKHR`
// or a `vk::SwapchainKHR` against this window. Instead, the renderer is
// expected to draw the dual-screen 3DS frame into an off-screen `VkImage`
// of size `(2 * 400 * factor) x (240 * factor)` (top stereo + bottom mono
// stacked vertically), and publish the latest finished image back to this
// class via `PublishFrame`. The OpenXR thread (`vr_pcvr::VrApp::Frame`)
// then calls `AcquireLatestFrame` and blits sub-rectangles into the per-eye
// `XrSwapchain` images owned by `GameQuadLayer`.
//
// At this stage the renderer-side wiring is still TODO (it requires
// extending `Vulkan::PresentWindow` to support a headless target). All
// hooks are therefore in place but no `VkImage` is published yet, so
// `AcquireLatestFrame` returns VK_NULL_HANDLE and `GameQuadLayer::Blit`
// no-ops. The XR session will composite empty quads until that wiring
// is finished in a follow-up step.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "../XrPlatformIncludes.h"
#include "core/frontend/emu_window.h"

namespace Core {
class System;
}

namespace Vulkan {
class PresentWindow;
}

namespace vr_pcvr {

class WinPlatform;

// Resolution of the off-screen frame Citra renders into. Mirrors today's
// Android side-by-side layout exactly so that the existing Citra layout
// code (DefaultFrameLayout / SeparateWindowsLayout) stays usable.
//
//   +------------------+------------------+
//   |  TOP-LEFT (3DS)  |  TOP-RIGHT (3DS) |   240*factor px tall
//   +--------+---------+---------+--------+
//            |     BOTTOM (3DS)  |            240*factor px tall
//            +-------------------+
//
// Width  = 2 * 400 * factor
// Height = (240 + 240) * factor
struct EmuWindowSize {
    uint32_t width;
    uint32_t height;
};

class EmuWindow_VR_Win final : public Frontend::EmuWindow {
public:
    explicit EmuWindow_VR_Win(Core::System& system, WinPlatform& platform, uint32_t resolution_factor);
    ~EmuWindow_VR_Win() override;

    EmuWindow_VR_Win(const EmuWindow_VR_Win&)            = delete;
    EmuWindow_VR_Win& operator=(const EmuWindow_VR_Win&) = delete;

    // Frontend::EmuWindow
    void PollEvents() override;
    std::unique_ptr<Frontend::GraphicsContext> CreateSharedContext() const override;
    void MakeCurrent() override {}
    void DoneCurrent() override {}

    // Touch input forwarded from XrController (ray-cast hit on the bottom
    // panel quad). Coordinates are in framebuffer-space pixels relative to
    // the off-screen image returned by FrameSize().
    bool OnTouchEvent(int x, int y, bool pressed);
    void OnTouchMoved(int x, int y);

    // Off-screen render-target dimensions.
    EmuWindowSize FrameSize() const { return mFrameSize; }
    uint32_t      ResolutionFactor() const { return mResolutionFactor; }

    // ----- Renderer <-> XR thread hand-off --------------------------------
    //
    // `PublishFrame` is called by the renderer (RendererVulkan, on its
    // present thread) after the dual-screen frame has been drawn into
    // `image` and `render_complete` has been signalled. The image must
    // remain alive and in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` (or
    // `VK_IMAGE_LAYOUT_GENERAL`) until the next `PublishFrame` for the
    // same slot.
    //
    // `AcquireLatestFrame` is called by the XR thread on each frame to
    // grab the most recently published image. Returns VK_NULL_HANDLE if
    // no frame has been published yet.

    struct PublishedFrame {
        VkImage     image           = VK_NULL_HANDLE;
        VkSemaphore render_complete = VK_NULL_HANDLE;
        VkFence     present_done    = VK_NULL_HANDLE;
        uint32_t    width           = 0;
        uint32_t    height          = 0;
        uint64_t    serial          = 0;
    };

    void           PublishFrame(const PublishedFrame& frame);
    PublishedFrame AcquireLatestFrame();

    // Hook the renderer's PresentWindow up so each finished Citra frame
    // lands in this EmuWindow's publish slot. The XR thread must call
    // ConsumeRenderer() once per frame after queueing its blit so the
    // renderer is unblocked to produce the next frame.
    void BindToRenderer(Vulkan::PresentWindow& present_window);
    void ConsumeRenderer();

    // The off-screen render target should not be presented to a window
    // surface; tell anything that walks `GraphicsContext` that we're a
    // headless target.
    bool IsHeadless() const { return true; }

protected:
    void OnMinimalClientAreaChangeRequest(std::pair<uint32_t, uint32_t>) override {}

private:
    void RebuildFramebufferLayout();

    Core::System&  mSystem;
    WinPlatform&   mPlatform;
    uint32_t       mResolutionFactor;
    EmuWindowSize  mFrameSize{};

    Vulkan::PresentWindow* mPresentWindow = nullptr;

    std::mutex             mFrameMutex;
    PublishedFrame         mLatestFrame{};
    std::atomic<uint64_t>  mPublishedSerial{0};
    uint64_t               mLastClaimedSerial{0};
};

} // namespace vr_pcvr
