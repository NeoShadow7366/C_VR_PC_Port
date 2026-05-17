// SPDX-License-Identifier: GPL-3.0-or-later

// Match video_core's vk_common.h. Vulkan headers use include guards, so
// once vulkan.h is parsed without this define the beta-only enums vanish
// for the rest of the TU - which then trips vulkan.hpp. Defining it
// before any header guarantees the C and C++ wrappers see the same set.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "windows/EmuWindow_VR_Win.h"

#include "common/logging/log.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "windows/WinPlatform.h"

namespace vr_pcvr {

namespace {

// Stub graphics context. Citra's RendererVulkan does not require a
// platform-specific shared context the way the OpenGL renderer does, but
// the EmuWindow contract still expects one for background work.
class HeadlessVrContext final : public Frontend::GraphicsContext {};

} // namespace

EmuWindow_VR_Win::EmuWindow_VR_Win(Core::System& system, WinPlatform& platform,
                                   uint32_t resolution_factor)
    : mSystem(system), mPlatform(platform),
      mResolutionFactor(resolution_factor == 0 ? 1u : resolution_factor) {

    // Off-screen render target = stacked 3DS layout at exactly the
    // 3DS aspect ratio so the default landscape layout fills it
    // without pillarboxing. Top screen is 400x240, bottom 320x240;
    // we use kScreenTopWidth as the framebuffer width so the top
    // screen fills it edge-to-edge and the bottom screen centres
    // within the same width.
    const uint32_t width  = static_cast<uint32_t>(Core::kScreenTopWidth) * mResolutionFactor;
    const uint32_t height = static_cast<uint32_t>(Core::kScreenTopHeight + Core::kScreenBottomHeight)
                          * mResolutionFactor;
    mFrameSize = {width, height};

    // Headless: there is no Win32 HWND backing this window. The renderer
    // must skip surface creation when it sees this type.
    window_info.type                 = Frontend::WindowSystemType::Headless;
    window_info.render_surface       = nullptr;
    window_info.display_connection   = nullptr;
    window_info.render_surface_scale = 1.0f;

    // Vulkan does not need a "strict" frontend-managed context.
    strict_context_required = false;

    RebuildFramebufferLayout();

    LOG_INFO(Frontend,
             "EmuWindow_VR_Win created: factor={} render-target={}x{} (off-screen, headless)",
             mResolutionFactor, mFrameSize.width, mFrameSize.height);
}

EmuWindow_VR_Win::~EmuWindow_VR_Win() = default;

void EmuWindow_VR_Win::PollEvents() {
    // No native window event loop on this thread - SteamVR's event pump
    // lives on the dedicated XR thread (vr_pcvr::VrApp::PollEvents). The
    // emulator thread calls PollEvents purely as part of the frontend
    // contract; nothing to do here.
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_VR_Win::CreateSharedContext() const {
    return std::make_unique<HeadlessVrContext>();
}

bool EmuWindow_VR_Win::OnTouchEvent(int x, int y, bool pressed) {
    if (pressed) {
        return TouchPressed(static_cast<unsigned>(std::max(x, 0)),
                            static_cast<unsigned>(std::max(y, 0)));
    }
    TouchReleased();
    return true;
}

void EmuWindow_VR_Win::OnTouchMoved(int x, int y) {
    TouchMoved(static_cast<unsigned>(std::max(x, 0)),
               static_cast<unsigned>(std::max(y, 0)));
}

void EmuWindow_VR_Win::PublishFrame(const PublishedFrame& frame) {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    mLatestFrame = frame;
    mLatestFrame.serial = mPublishedSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
}

EmuWindow_VR_Win::PublishedFrame EmuWindow_VR_Win::AcquireLatestFrame() {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    // Only return a frame the consumer hasn't already taken. Returning the
    // same frame twice would re-wait on its (binary) render_complete
    // semaphore - which was already consumed on the first wait - causing
    // the consumer's queue submit to deadlock and freezing the pipeline.
    if (mLatestFrame.serial == 0 || mLatestFrame.serial == mLastClaimedSerial) {
        return PublishedFrame{};
    }
    mLastClaimedSerial = mLatestFrame.serial;
    return mLatestFrame;
}

void EmuWindow_VR_Win::BindToRenderer(Vulkan::PresentWindow& present_window) {
    mPresentWindow = &present_window;
    present_window.SetFramePublishCallback(
        [this](const Vulkan::PresentWindow::PublishedFrame& f) {
            PublishedFrame pf{};
            pf.image           = f.image;
            pf.render_complete = f.render_complete;
            pf.present_done    = f.present_done;
            pf.width           = f.width;
            pf.height          = f.height;
            PublishFrame(pf);
        });
}

void EmuWindow_VR_Win::ConsumeRenderer() {
    if (mPresentWindow != nullptr) {
        mPresentWindow->NotifyFrameConsumed();
    }
}

void EmuWindow_VR_Win::RebuildFramebufferLayout() {
    // Use the default landscape layout (top stereo + bottom mono) at our
    // off-screen render target's resolution. The XR thread reinterprets
    // the resulting image as four sub-rectangles (top-L, top-R, bottom-L,
    // bottom-R) when blitting into the per-eye XrSwapchain.
    UpdateCurrentFramebufferLayout(mFrameSize.width, mFrameSize.height,
                                   /*is_portrait_mode=*/false);
}

} // namespace vr_pcvr
