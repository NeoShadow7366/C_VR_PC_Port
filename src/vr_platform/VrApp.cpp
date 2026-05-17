// SPDX-License-Identifier: GPL-3.0-or-later

#include "VrApp.h"
#include "VrInputBridge.h"
#include "windows/EmuWindow_VR_Win.h"
#include "windows/WinPlatform.h"
#include "utils/LogUtils.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/vr_config.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "video_core/renderer_vulkan/vk_vr_hooks.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

namespace vr_pcvr {

namespace {
// Used by the VrWaitIdle hook so core.cpp can flush in-flight GPU work
// (vr_platform-owned submissions) before destroying renderer-owned
// images during save-state load. Set once during VrApp::Start.
VkDevice g_vr_device_for_wait_idle = VK_NULL_HANDLE;

void VrWaitIdleThunk() {
    if (g_vr_device_for_wait_idle != VK_NULL_HANDLE) {
        // Serialise with the VR queue's other submitters (xrEndFrame etc.).
        std::scoped_lock vr_lock{Vulkan::GetVrQueueMutex()};
        vkDeviceWaitIdle(g_vr_device_for_wait_idle);
    }
}

// Premium-styled tooltip helper. Replaces ad-hoc VrTip() calls
// in the menu draw with a uniformly padded, accent-bordered card sized
// for VR readability. Uses the active style's PopupBg + a 2 px cyan left
// stripe so tooltips read as part of the same theme system as the modals.
void VrTip(const char* text) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.13f, 0.18f, 0.97f));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    // 2 px cyan accent stripe down the left edge.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wmin = ImGui::GetWindowPos();
    const ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(),
                      wmin.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(wmin, ImVec2(wmin.x + 2.0f, wmax.y),
                      ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.0f)));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
} // namespace

// --- Haptic vocabulary -------------------------------------------------------
// Four named patterns used across all VR interactions.
namespace Haptic {
constexpr float      kTapAmp    = 0.25f;
constexpr XrDuration kTapDur    = 10'000'000LL;   // 10 ms
constexpr float      kTapFreq   = 200.0f;
constexpr float      kThumpAmp  = 0.50f;
constexpr XrDuration kThumpDur  = 40'000'000LL;   // 40 ms
constexpr float      kThumpFreq = 120.0f;
constexpr float      kErrorAmp  = 0.70f;
constexpr XrDuration kErrorDur  = 80'000'000LL;   // 80 ms
constexpr float      kErrorFreq = 60.0f;
// Success = double Thump 80 ms apart. Fire 1st pulse immediately, then store
// the deadline for the 2nd pulse in mHapticSuccessT2Ns[hand].
constexpr int64_t    kSuccessGapNs = 80'000'000LL;
} // namespace Haptic

VrApp::VrApp(WinPlatform& platform) : mPlatform(platform) {
    mShowPerfOverlay = Common::VRConfig::GetBool("show_perf_overlay", false);
    // Restore library sort preference (0 = Name, 1 = Recent, 2 = File).
    mLibrarySort     = Common::VRConfig::GetInt("library_sort", 0);
    if (mLibrarySort < 0 || mLibrarySort > 2) mLibrarySort = 0;
}
VrApp::~VrApp() {
    Vulkan::SetVrWaitIdleFn(nullptr);
    g_vr_device_for_wait_idle = VK_NULL_HANDLE;
    mSubmitter.Destroy();
    mImGuiLayer.Shutdown();
    mCursorLayer.Shutdown();
    mGameLayer.Shutdown();
    mController.Destroy();
}

void VrApp::ShowToast(const std::string& text, float duration) {
    if (mImGuiLayer.IsInitialised()) {
        mImGuiLayer.ShowToast(text, duration);
    }
}

bool VrApp::Start() {
    // Quad swapchain size = source EmuWindow render-target size when
    // available, so each XR swapchain image is a 1:1 copy of Citra's
    // off-screen frame (top stereo + bottom mono, side-by-side stereo).
    // Falls back to 2 x per-eye-recommended at supersample factor=2 when
    // no EmuWindow has been attached yet (early bring-up only).
    uint32_t swapchainW = 0;
    uint32_t swapchainH = 0;
    if (mEmuWindow != nullptr) {
        const auto fs = mEmuWindow->FrameSize();
        swapchainW = fs.width;
        swapchainH = fs.height;
    } else {
        const auto& vc = mPlatform.ViewConfig(0);
        const uint32_t factor = VRSettings::values.resolution_factor > 0
                                    ? VRSettings::values.resolution_factor
                                    : VRSettings::DefaultResolutionFactorFor(VRSettings::values.hmd_type);
        swapchainW = vc.recommendedImageRectWidth  * 2u;
        swapchainH = vc.recommendedImageRectHeight;
        // Cap at 4096 per dimension - SteamVR will happily allocate huge
        // swapchains and waste VRAM on the Beyond 2.
        const uint32_t cap = 4096u;
        swapchainW = std::min(swapchainW * std::max(factor, 1u), cap);
        swapchainH = std::min(swapchainH * std::max(factor, 1u), cap);
    }

    if (!mGameLayer.Init(mPlatform.Session(), swapchainW, swapchainH)) {
        ALOGE("GameQuadLayer init failed");
        return false;
    }
    if (!mController.Init(mPlatform.Instance(), mPlatform.Session(),
                          mPlatform.LocalSpace(),
                          VRSettings::values.enable_hand_tracking && mPlatform.HasHandTracking())) {
        ALOGE("XrController init failed");
        return false;
    }
    if (mPlatform.VkDevice_() != VK_NULL_HANDLE && !mSubmitter.Init(mPlatform.VkDevice_(),
                                                                   mPlatform.VkQueue_(),
                                                                   mPlatform.VkQueueFamily())) {
        ALOGE("VrFrameSubmitter init failed");
        return false;
    }
    // Register the wait-idle hook so core.cpp can flush in-flight VR
    // composite work before save-state load destroys renderer images.
    g_vr_device_for_wait_idle = mPlatform.VkDevice_();
    Vulkan::SetVrWaitIdleFn(&VrWaitIdleThunk);
    // Restore persisted user preferences.
    mShowControllers = Common::VRConfig::GetBool("show_controllers", true);
    // CursorLayer init is deferred until the session is actually running
    // (some OpenXR runtimes refuse swapchain Acquire/Wait/Release before
    // xrBeginSession). See VrApp::Frame for the lazy init.
    return true;
}

bool VrApp::PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(mPlatform.Instance(), &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            HandleSessionStateChanged(*reinterpret_cast<XrEventDataSessionStateChanged*>(&ev));
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            ALOGW("Runtime asked us to exit (instance loss)");
            mExitRequested.store(true);
            return false;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !mExitRequested.load();
}

void VrApp::HandleSessionStateChanged(const XrEventDataSessionStateChanged& ev) {
    mSessionState = ev.state;
    ALOGI("XR session state -> %d", static_cast<int>(ev.state));
    switch (ev.state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(mPlatform.Session(), &bi))) {
            mIsSessionRunning = true;
            ALOGI("xrBeginSession OK - session running");
        } else {
            ALOGE("xrBeginSession failed");
        }
        break;
    }
    case XR_SESSION_STATE_FOCUSED:
        mHmdOnHead = true;
        break;
    case XR_SESSION_STATE_VISIBLE:
        mHmdOnHead = false;
        break;
    case XR_SESSION_STATE_STOPPING:
        xrEndSession(mPlatform.Session());
        mIsSessionRunning = false;
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        mExitRequested.store(true);
        mIsSessionRunning = false;
        break;
    default:
        break;
    }
}

void VrApp::Frame(void (*renderCitraToImage)(void*), void* userdata) {
    if (!mIsSessionRunning) return;

    XrFrameWaitInfo  fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState     fs {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(mPlatform.Session(), &fwi, &fs))) return;

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(mPlatform.Session(), &fbi);

    if (mFirstFrame) {
        mPlatform.CreateRuntimeInitiatedSpaces(fs.predictedDisplayTime);
        // Capture the game-screen yaw that CreateRuntimeInitiatedSpaces
        // just baked into mHeadSpace so the touch raycast knows the game
        // quad's facing direction from the very first frame.
        if (mPlatform.ViewSpace() != XR_NULL_HANDLE
            && mPlatform.LocalSpace() != XR_NULL_HANDLE) {
            XrSpaceLocation initLoc{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(mPlatform.ViewSpace(),
                                           mPlatform.LocalSpace(),
                                           fs.predictedDisplayTime, &initLoc))
                && (initLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
                && (initLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                mGameOrigin = initLoc.pose.position;
                const auto& q = initLoc.pose.orientation;
                float fx = -2.0f * (q.x * q.z + q.w * q.y);
                float fz =  2.0f * (q.x * q.x + q.y * q.y) - 1.0f;
                const float fl = std::sqrt(fx*fx + fz*fz);
                if (fl > 1e-3f) { fx /= fl; fz /= fl; }
                else             { fx = 0.0f; fz = -1.0f; }
                // yaw such that yawQ=(0,sin(yaw/2),0,cos(yaw/2)) reproduces
                // the actual head rotation around +Y. rotate(yawQ,{0,0,-1})
                // = (-sin yaw, 0, -cos yaw), so we want -sin yaw = fx ->
                // yaw = atan2(-fx, -fz). Atan2(fx,-fz) gave the *opposite*
                // rotation and made the game quad / menu drift off-axis
                // whenever the user recentered while not facing straight.
                mGameYaw = std::atan2(-fx, -fz);
            }
        }
        mFirstFrame = false;
    }
    // Compute a real frame delta (seconds) from XR time. Used to drive
    // time-based UI animations (menu fade etc.). Guard against the
    // first-frame zero / runtime resume jumps by clamping to a sane
    // window.
    float dt = 0.0f;
    if (mLastDisplayTime != 0) {
        const int64_t deltaNs = static_cast<int64_t>(fs.predictedDisplayTime) -
                                static_cast<int64_t>(mLastDisplayTime);
        dt = std::clamp(static_cast<float>(deltaNs) * 1.0e-9f, 0.0f, 0.1f);
        // EWMA frame interval (ms) for the perf overlay.  Alpha 0.05 â‰ˆ a
        // 1-second window at 90 Hz; cheap and stable.
        const float intervalMs = dt * 1000.0f;
        if (mFrameIntervalMs <= 0.0f) {
            mFrameIntervalMs = intervalMs;
        } else {
            mFrameIntervalMs = mFrameIntervalMs * 0.95f + intervalMs * 0.05f;
        }
    }
    mLastDisplayTime = fs.predictedDisplayTime;
    ++mFrameCount;
    // Lazy-init the cursor layer on the first running frame so the
    // session is guaranteed to be in SYNCHRONIZED/VISIBLE/FOCUSED state
    // when we acquire/release its swapchain image.
    if (!mCursorLayer.IsInitialised() && mSubmitter.IsInitialised()) {
        if (!mCursorLayer.Init(mPlatform.Session(), mSubmitter,
                               mPlatform.VkPhysicalDevice_())) {
            ALOGW("CursorLayer init failed - controllers will not be visible");
        }
    }
    if (!mBeamLayer.IsInitialised() && mSubmitter.IsInitialised()) {
        if (!mBeamLayer.Init(mPlatform.Session(), mSubmitter,
                             mPlatform.VkPhysicalDevice_())) {
            ALOGW("AimBeamLayer init failed - controller beams will not be visible");
        }
    }
    // Lazy-init the modal dimmer (large semi-transparent dark quad
    // shown behind the menu while it's open). Improves text legibility
    // and reduces vergence fatigue from bright moving game content
    // showing through behind the panel.
    if (!mDimLayer.IsInitialised() && mSubmitter.IsInitialised()) {
        // base alpha = 1.0 so SetAlpha() maps [0,1] to true [0,1] opacity.
        // The menu-dim call site scales by 0.55 to preserve the original
        // 55% scrim; the launch fade uses the full 0â†’1 range.
        if (!mDimLayer.Init(mPlatform.Session(), mSubmitter,
                            0.0f, 0.0f, 0.0f, 1.0f)) {
            ALOGW("DimLayer init failed - menu will draw without backdrop");
        }
    }
    // Lazy-init the in-VR ImGui menu layer once the session is running
    // and our submitter is alive. Same rationale as CursorLayer.
    if (!mImGuiLayer.IsInitialised() && mSubmitter.IsInitialised()) {
        if (!mImGuiLayer.Init(mPlatform.Session(),
                              mPlatform.VkInstance_(),
                              mPlatform.VkPhysicalDevice_(),
                              mPlatform.VkDevice_(),
                              mPlatform.VkQueueFamily(),
                              mPlatform.VkQueue_(),
                              mSubmitter)) {
            ALOGW("ImGuiLayer init failed - in-VR menu disabled");
        } else if (mLauncherMode) {
            // Launcher mode (no ROM on the command line): open the menu
            // immediately so the user starts in the ROM browser.  When a
            // ROM IS specified we leave the menu closed so the game can
            // run; the wrist Menu button opens it on demand.
            mLauncherMode       = false;
            mOpenBrowserOnce    = true;
            mImGuiLayer.SetVisible(true);
            mMenuRecenterPending = true;
        }
    }
    // Lazy-init the wrist-anchored button bar. Independent of any
    // controller button binding: pointing at a button and pulling the
    // trigger fires its action. Three buttons in a row above the left
    // wrist: [Menu] (teal), [Start] (green), [Select] (amber).
    if (mSubmitter.IsInitialised()) {
        if (!mWristBtns[WB_Menu].IsInitialised()) {
            mWristBtns[WB_Menu].Init(mPlatform.Session(), mSubmitter,
                                     mPlatform.VkPhysicalDevice_(),
                                     WristIconKind::Hamburger,
                                     0.20f, 0.55f, 0.85f,  0.40f, 0.85f, 1.00f);
        }
        if (!mWristBtns[WB_Start].IsInitialised()) {
            mWristBtns[WB_Start].Init(mPlatform.Session(), mSubmitter,
                                      mPlatform.VkPhysicalDevice_(),
                                      WristIconKind::PlayTriangle,
                                      0.20f, 0.65f, 0.30f,  0.45f, 0.95f, 0.55f);
        }
        if (!mWristBtns[WB_Select].IsInitialised()) {
            mWristBtns[WB_Select].Init(mPlatform.Session(), mSubmitter,
                                       mPlatform.VkPhysicalDevice_(),
                                       WristIconKind::MinusBar,
                                       0.85f, 0.60f, 0.20f,  1.00f, 0.85f, 0.40f);
        }
        // Controller marker quads: visible at each grip pose so the user
        // can see where their virtual controllers are while a game is
        // rendering on the screen quad. Left = teal accent, Right = amber.
        if (!mControllerMarkers[0].IsInitialised()) {
            mControllerMarkers[0].Init(mPlatform.Session(), mSubmitter,
                                       mPlatform.VkPhysicalDevice_(),
                                       WristIconKind::ControllerL,
                                       0.18f, 0.42f, 0.62f,  0.30f, 0.72f, 0.95f);
        }
        if (!mControllerMarkers[1].IsInitialised()) {
            mControllerMarkers[1].Init(mPlatform.Session(), mSubmitter,
                                       mPlatform.VkPhysicalDevice_(),
                                       WristIconKind::ControllerR,
                                       0.62f, 0.44f, 0.18f,  0.95f, 0.72f, 0.30f);
        }
    }
    mController.SyncFrame(fs.predictedDisplayTime);

    if (fs.shouldRender && renderCitraToImage) {
        renderCitraToImage(userdata);
    }

    // Pull the most recent off-screen Citra frame for this XR tick and
    // composite it into the per-eye XR swapchain via vkCmdBlitImage.
    bool didNewBlit = false;
    // While the emulator core is mutating renderer-owned images (e.g.
    // a save-state load), skip the blit entirely - any VkImage we'd
    // pull from EmuWindow could be in the middle of being recreated.
    // try_lock so we never stall the XR thread; we'll just reuse the
    // last composited image until the core releases the lock.
    std::unique_lock<std::mutex> core_busy_lock{Vulkan::GetVrCoreBusyMutex(), std::try_to_lock};
    if (fs.shouldRender && mEmuWindow != nullptr && mSubmitter.IsInitialised() &&
        core_busy_lock.owns_lock()) {
        const auto src = mEmuWindow->AcquireLatestFrame();
        if (src.image != VK_NULL_HANDLE) {
            // Non-blocking: if the previous slot's GPU work hasn't
            // retired we drop this blit and let the XR runtime re-present
            // the last submitted swapchain image. Keeps the XR thread on
            // its xrWaitFrame cadence instead of stalling on a backed-up
            // GPU during a heavy game scene.
            VkCommandBuffer cmd = mSubmitter.Begin(/*block=*/false);
            if (cmd == VK_NULL_HANDLE) {
                ++mDroppedBlits;
            }
            if (cmd != VK_NULL_HANDLE) {
                if (mGameLayer.Blit(cmd, src)) {
                    // Wait on the renderer's render-ready semaphore so we
                    // don't blit a not-yet-rendered frame, and signal its
                    // present_done fence so the renderer only recycles
                    // the frame slot after our blit completes on the GPU.
                    mSubmitter.Submit(src.render_complete,
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      src.present_done);
                    mGameLayer.ReleaseAcquired();
                    didNewBlit       = true;
                    mEverComposited  = true;
                }
            }
            // Unblock the renderer so it can publish the next frame.
            // Even if the blit failed we must still release the publish
            // slot, otherwise the renderer thread will deadlock.
            mEmuWindow->ConsumeRenderer();
        }
    }
    (void)didNewBlit;
    // Allow layer submission as soon as the menu or wrist buttons have
    // something to show, even before the game framebuffer has ever been
    // blitted (e.g. launcher mode before a ROM is loaded).  The game
    // quad itself is only composited after mEverComposited is true, but
    // UI layers (menu, wrist buttons, cursors, beams) are always valid.
    const bool layersValid = true;

    // ---- Compute cursor positions + bottom-screen touch raycast --------
    // Screen geometry mirrors GameQuadLayer::BuildLayers so the touch
    // raycast always hits the actual rendered quad regardless of the
    // user's size/distance setting.
    const float kScale        = VRSettings::values.screen_scale;
    const float kDist         = VRSettings::values.screen_distance;
    const float kQuadHalfW    = 0.5f * kScale;
    const float kQuadHalfH    = kQuadHalfW * (5.0f / 6.0f);
    const float kBotHalfW     = kQuadHalfW * (320.0f / 400.0f);
    const float kBotHalfH     = kQuadHalfH;       // bottom panel = lower half
    const float kBotCenterY   = -kQuadHalfH * 0.5f;

    // Game screen world-space center and orientation (yaw-only).
    // Updated at launch and on every menu recenter so game + menu are
    // always co-aligned.  Y component follows the head height stored at
    // the last recenter.
    const float gcSY = std::sin(mGameYaw);
    const float gcCY = std::cos(mGameYaw);
    // rotate(yawQ, {0, 0, -kDist}):  x = -kDist*sin(yaw),  z = -kDist*cos(yaw)
    const float gcX = mGameOrigin.x - kDist * gcSY;
    const float gcY = mGameOrigin.y;          // Y = eye level at last recenter
    const float gcZ = mGameOrigin.z - kDist * gcCY;
    // Plane normal = rotate(yawQ, {0,0,1}) = {sin(yaw), 0, cos(yaw)} (facing away from user).
    const float plNx = gcSY, plNz = gcCY;

    auto rotate = [](const XrQuaternionf& q, const XrVector3f& v) -> XrVector3f {
        // Standard quaternion-vector rotation: v' = q * v * q^-1.
        const float xx = q.x * q.x;
        const float yy = q.y * q.y;
        const float zz = q.z * q.z;
        const float xy = q.x * q.y;
        const float xz = q.x * q.z;
        const float yz = q.y * q.z;
        const float wx = q.w * q.x;
        const float wy = q.w * q.y;
        const float wz = q.w * q.z;
        return XrVector3f{
            v.x * (1.0f - 2.0f * (yy + zz)) + v.y * 2.0f * (xy - wz)        + v.z * 2.0f * (xz + wy),
            v.x * 2.0f * (xy + wz)          + v.y * (1.0f - 2.0f * (xx + zz)) + v.z * 2.0f * (yz - wx),
            v.x * 2.0f * (xz - wy)          + v.y * 2.0f * (yz + wx)          + v.z * (1.0f - 2.0f * (xx + yy)),
        };
    };

    CursorLayer::CursorSpec cursors[2] = {};
    bool   touchValid = false;
    int    touchPxX   = 0;
    int    touchPxY   = 0;

    // While the in-VR menu is open the game is paused (RunLoop is
    // skipped) and the user is interacting with the menu, not the game
    // screens. Suppress the game-quad raycast entirely so neither hand
    // sets snap_to_screen=true on its cursor; otherwise sweeping the
    // right hand over the top screen toggles the right cursor between
    // 1 quad and 2 per-eye quads each frame, which makes SteamVR's
    // compositor re-shuffle layers and visibly flash the menu.
    const bool menuOpen = mImGuiLayer.IsInitialised() && mImGuiLayer.IsVisible();

    for (uint32_t hand = 0; hand < 2; ++hand) {
        const auto& s = mController.State(static_cast<Hand>(hand));
        if (!s.aim_pose_valid) continue;

        const XrVector3f forward = rotate(s.aim_pose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});

        // Default cursor: 5 m laser-pointer ray in front of controller.
        // This keeps the aim beam long and visible in free space so the user
        // can always see where they're pointing.  When the ray hits the game
        // screen the cursor snaps to the surface (snap_to_screen=true below).
        cursors[hand].position = XrVector3f{
            s.aim_pose.position.x + forward.x * 5.0f,
            s.aim_pose.position.y + forward.y * 5.0f,
            s.aim_pose.position.z + forward.z * 5.0f,
        };
        cursors[hand].visible = true;

        // Raycast against the game screen plane.
        // Plane equation: dot(P - center, N) = 0  where N = {plNx, 0, plNz}.
        // t = dot(center - aimOrigin, N) / dot(aimDir, N)
        if (!menuOpen) {
            const float denom = forward.x * plNx + forward.z * plNz;
            if (denom < -1e-4f) {   // negative = ray hits the front face
                const float num = (gcX - s.aim_pose.position.x) * plNx
                                + (gcZ - s.aim_pose.position.z) * plNz;
                const float t = num / denom;
                if (t > 0.0f) {
                    // World hit point.
                    const float pwX = s.aim_pose.position.x + forward.x * t;
                    const float pwY = s.aim_pose.position.y + forward.y * t;
                    const float pwZ = s.aim_pose.position.z + forward.z * t;
                    // Convert to quad-local 2-D coordinates.
                    // Quad local-X in world = {cos(yaw), 0, -sin(yaw)}.
                    const float dx = pwX - gcX, dz = pwZ - gcZ;
                    const float hx =  dx * gcCY - dz * gcSY;  // along quad-local +X
                    const float hy = pwY - gcY;                 // along quad-local +Y
                    // Inside the bottom-screen rect? (lower half of the quad,
                    // narrower since the bottom 3DS screen is 320 wide vs
                    // the top's 400.)
                    const bool inBotRect = (hx >= -kBotHalfW && hx <= kBotHalfW &&
                        hy >= (kBotCenterY - kBotHalfH * 0.5f) &&
                        hy <= (kBotCenterY + kBotHalfH * 0.5f));
                    // Inside the top-screen rect? (upper half of the quad,
                    // full quad width.) Visual snap only - no touch input.
                    const bool inTopRect = (!inBotRect &&
                        hx >= -kQuadHalfW && hx <= kQuadHalfW &&
                        hy >= 0.0f && hy <= kQuadHalfH);
                    if (inBotRect || inTopRect) {
                        // Snap cursor to the surface in LOCAL space.
                        // Surface point = center + rotate(yawQ, {hx, hy, 0}) + N_unit*0.001
                        // Quad local-Z in world = {sin(yaw), 0, cos(yaw)} (away from user).
                        // Slightly offset toward the user (opposite normal) so the cursor
                        // sits just in front of the quad surface.
                        cursors[hand].position = XrVector3f{
                            gcX + hx * gcCY - 0.001f * plNx,
                            gcY + hy,
                            gcZ - hx * gcSY - 0.001f * plNz,
                        };
                        // GameQuadLayer renders content at infinite stereo
                        // depth via per-eye xOffset; ask CursorLayer to do
                        // the same so the cursor visually aligns with the
                        // pixel it's pointing at instead of floating ~1.5m
                        // closer to the user.
                        cursors[hand].snap_to_screen = true;

                        // Convert to framebuffer pixels for Citra HID. Right-
                        // hand only - the left hand is reserved for ImGui /
                        // future menu interaction. Touch only fires when the
                        // ray actually hits the bottom screen.
                        if (inBotRect && hand == 1 && mEmuWindow != nullptr) {
                            const auto fs_size = mEmuWindow->FrameSize();
                            const float u = (hx + kBotHalfW) / (2.0f * kBotHalfW);
                            // The bottom 3DS screen occupies the lower half of
                            // the game quad: world y in [-kQuadHalfH, 0].
                            //   y =  0          -> v = 0 (top of bottom screen)
                            //   y = -kQuadHalfH -> v = 1 (bottom edge)
                            const float v = (-hy) / kQuadHalfH;
                            // Bottom panel position in framebuffer: centred
                            // horizontally at width kScreenBottomWidth*factor;
                            // vertically lower half of frame.
                            const uint32_t botFbW = (Core::kScreenBottomWidth * fs_size.width) /
                                                    Core::kScreenTopWidth;
                            const uint32_t botFbXOff = (fs_size.width - botFbW) / 2;
                            const uint32_t botFbYOff = fs_size.height / 2;
                            const uint32_t botFbH    = fs_size.height - botFbYOff;
                            touchPxX   = static_cast<int>(botFbXOff + u * botFbW);
                            touchPxY   = static_cast<int>(botFbYOff + v * botFbH);
                            touchValid = true;
                        }
                    }
                }
            }
        }
    }
    // Apply EMA smoothing to reduce hand-tremor jitter on the visible
    // cursor. Alpha is adaptive on motion magnitude: heavy smoothing for
    // tiny tremor, near-1:1 tracking for deliberate motion. This avoids
    // the visible "lag-then-snap" jitter that a fixed alpha + hard snap
    // threshold produces in the transition band.
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (!cursors[hand].visible) {
            mCursorSmoothedValid[hand] = false;
            continue;
        }
        const XrVector3f target = cursors[hand].position;
        if (!mCursorSmoothedValid[hand]) {
            mCursorSmoothed[hand]      = target;
            mCursorSmoothedValid[hand] = true;
        } else {
            const float dx = target.x - mCursorSmoothed[hand].x;
            const float dy = target.y - mCursorSmoothed[hand].y;
            const float dz = target.z - mCursorSmoothed[hand].z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            // Hard snap as a safety net for very large jumps (raycast
            // mode change, controller re-acquired, etc.).
            constexpr float kHardSnapDist2 = 0.10f * 0.10f; // 10 cm
            if (d2 > kHardSnapDist2) {
                mCursorSmoothed[hand] = target;
            } else {
                // Adaptive alpha: lerp from a smooth floor to 1.0 over a
                // motion window. Below kSlow we apply heavy smoothing
                // (kills hand tremor); above kFast we track 1:1
                // (no perceived lag). Smoothstep over the band.
                const float dist = std::sqrt(d2);
                constexpr float kSlow      = 0.005f; // 5 mm  -> alpha_floor
                constexpr float kFast      = 0.040f; // 4 cm  -> alpha=1.0
                const float alpha_floor    = cursors[hand].snap_to_screen ? 0.25f : 0.45f;
                float t = (dist - kSlow) / (kFast - kSlow);
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                t = t * t * (3.0f - 2.0f * t); // smoothstep
                const float alpha = alpha_floor + (1.0f - alpha_floor) * t;
                mCursorSmoothed[hand].x += dx * alpha;
                mCursorSmoothed[hand].y += dy * alpha;
                mCursorSmoothed[hand].z += dz * alpha;
            }
        }
        cursors[hand].position = mCursorSmoothed[hand];
    }
    mCursorLayer.SetCursors(cursors[0], cursors[1]);
    // Aim beams: thin elongated quads from controller origin to cursor endpoint.
    {
        AimBeamLayer::BeamSpec beams[2];
        for (uint32_t hand = 0; hand < 2; ++hand) {
            const auto& s = mController.State(static_cast<Hand>(hand));
            beams[hand].visible  = s.aim_pose_valid && cursors[hand].visible;
            beams[hand].origin   = s.aim_pose.position;
            beams[hand].endpoint = cursors[hand].position;
        }
        mBeamLayer.SetBeams(beams[0], beams[1]);
    }
    if (!mLoggedCursorOnce && (cursors[0].visible || cursors[1].visible)) {
        LOG_INFO(Frontend,
                 "VR cursors first visible: L({}, {:.2f},{:.2f},{:.2f})  R({}, {:.2f},{:.2f},{:.2f})  cursorLayerInit={}",
                 (int)cursors[0].visible, cursors[0].position.x, cursors[0].position.y, cursors[0].position.z,
                 (int)cursors[1].visible, cursors[1].position.x, cursors[1].position.y, cursors[1].position.z,
                 (int)mCursorLayer.IsInitialised());
        mLoggedCursorOnce = true;
    }

    // Right trigger drives touch-down on the bottom screen.
    if (mEmuWindow != nullptr) {
        const auto& rState = mController.State(Hand::Right);
        const bool wantTouch = touchValid && (rState.trigger_click || rState.trigger_value > 0.5f);

        if (wantTouch) {
            if (!mTouchHeld) {
                mEmuWindow->TouchPressed(static_cast<unsigned>(touchPxX),
                                         static_cast<unsigned>(touchPxY));
                mTouchHeld = true;
                // Light tactile confirmation that the touch was registered.
                mController.TriggerHaptic(Hand::Right, 0.35f, 25'000'000LL, 200.0f);
            } else {
                mEmuWindow->TouchMoved(static_cast<unsigned>(touchPxX),
                                       static_cast<unsigned>(touchPxY));
            }
        } else if (mTouchHeld) {
            mEmuWindow->TouchReleased();
            mTouchHeld = false;
        }
    }

    // ---- In-VR menu ---------------------------------------------------
    // Menu is opened/closed exclusively via the wrist Menu button above.
    // Left B is now a remappable gamepad button â€” no longer hardwired to
    // menu toggle. (Legacy binding removed 2026-05-02.)

    // C3: Deferred second pulse of the success double-thump haptic.
    // Fires 80 ms after the first pulse (e.g., triggered by Save state).
    {
        const auto chk_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int h = 0; h < 2; ++h) {
            if (mHapticSuccessT2Ns[h] > 0 && chk_ns >= mHapticSuccessT2Ns[h]) {
                mController.TriggerHaptic(static_cast<Hand>(h),
                                          Haptic::kThumpAmp, Haptic::kThumpDur,
                                          Haptic::kThumpFreq);
                mHapticSuccessT2Ns[h] = 0;
            }
        }
    }

    // ---- Wrist-anchored button bar (controller-button independent) -------
    // Three buttons (Menu / Start / Select) anchored above the left grip.
    // Either hand can point at a button and pull the trigger:
    //   - Menu  : trigger release toggles the in-VR menu;
    //             trigger held >= kWristHomeHoldNs fires 3DS Home.
    //   - Start : while held, drives 3DS Start.
    //   - Select: while held, drives 3DS Select.
    bool chordMenuHeld = false;         // D4: set when Menu is hovered+triggered
    bool anyWristHovered = false;
    bool btnHovered[WB_Count]  = {false, false, false};
    bool btnHeldNow[WB_Count]  = {false, false, false};
    // Per-hand: hovering ANY wrist button this frame? Used to drive
    // the cursor "over interactive" scale and click-pulse logic.
    bool handWristHover[2]     = {false, false};
    {
        const auto& lGrip = mController.State(Hand::Left);
        const bool gripOk = lGrip.grip_pose_valid;

        // ---- Drag-to-place: derive live offset from right-hand pose ----
        // Activated via the "Reposition wrist bar" button in the menu.
        // Right trigger release commits to vr_config.txt + exits drag mode.
        // Right A button cancels without saving.
        if (mWristDragMode && gripOk) {
            const auto& rState = mController.State(Hand::Right);
            if (rState.grip_pose_valid) {
                // World-space delta from left grip origin to right grip tip.
                const XrVector3f d{rState.grip_pose.position.x - lGrip.grip_pose.position.x,
                                   rState.grip_pose.position.y - lGrip.grip_pose.position.y,
                                   rState.grip_pose.position.z - lGrip.grip_pose.position.z};
                // Rotate world delta into LEFT-GRIP-LOCAL frame: apply the
                // left grip orientation inverse (= conjugate for unit quats).
                const XrQuaternionf& q = lGrip.grip_pose.orientation;
                const XrQuaternionf qi{-q.x, -q.y, -q.z, q.w};
                const XrVector3f local = rotate(qi, d);
                // Clamp to a sane range so a wild swing doesn't fling the
                // bar somewhere unreachable.
                auto clamp = [](float v, float lo, float hi) {
                    return v < lo ? lo : (v > hi ? hi : v);
                };
                mWristDragOffX = clamp(local.x, -0.30f, 0.30f);
                mWristDragOffY = clamp(local.y, -0.30f, 0.30f);
                mWristDragOffZ = clamp(local.z, -0.30f, 0.30f);
            }
            // Right trigger release => commit.
            const bool trig = rState.trigger_click || rState.trigger_value > 0.5f;
            if (mDragCommitLast && !trig) {
                VRSettings::values.wrist_offset_x = mWristDragOffX;
                VRSettings::values.wrist_offset_y = mWristDragOffY;
                VRSettings::values.wrist_offset_z = mWristDragOffZ;
                Common::VRConfig::Set("wrist_offset_x", std::to_string(mWristDragOffX));
                Common::VRConfig::Set("wrist_offset_y", std::to_string(mWristDragOffY));
                Common::VRConfig::Set("wrist_offset_z", std::to_string(mWristDragOffZ));
                mWristDragMode = false;
                ShowToast("Wrist bar position saved", 1.5f);
                mController.TriggerHaptic(Hand::Right, 0.6f, 60'000'000LL, 200.0f);
            }
            mDragCommitLast = trig;
            // Right A button => cancel.
            if (rState.a_click && !mDragCancelLast) {
                mWristDragMode = false;
                ShowToast("Wrist bar position reverted", 1.5f);
            }
            mDragCancelLast = rState.a_click;
        }

        if (gripOk) {
            // Build a common base pose anchored above-and-behind the wrist,
            // tilted around grip-X so it faces the user when arm hangs
            // naturally. Each button is then offset along the local X axis
            // of this base pose. All offsets / tilt come from VRSettings
            // so the user can reposition them via the in-VR menu.
            const float wox = mWristDragMode ? mWristDragOffX : VRSettings::values.wrist_offset_x;
            const float woy = mWristDragMode ? mWristDragOffY : VRSettings::values.wrist_offset_y;
            const float woz = mWristDragMode ? mWristDragOffZ : VRSettings::values.wrist_offset_z;
            const XrVector3f upLocal{wox, woy, woz};
            const XrVector3f upWorld = rotate(lGrip.grip_pose.orientation, upLocal);
            XrPosef base;
            base.position.x = lGrip.grip_pose.position.x + upWorld.x;
            base.position.y = lGrip.grip_pose.position.y + upWorld.y;
            base.position.z = lGrip.grip_pose.position.z + upWorld.z;
            const float tiltDeg = VRSettings::values.wrist_tilt_deg;
            const float kHalfAngle = (tiltDeg * 0.5f) * 0.01745329252f;
            const float qsin = std::sin(kHalfAngle);
            const float qcos = std::cos(kHalfAngle);
            const XrQuaternionf tilt{qsin, 0.0f, 0.0f, qcos};
            const XrQuaternionf& g = lGrip.grip_pose.orientation;
            base.orientation.x = g.w*tilt.x + g.x*tilt.w + g.y*tilt.z - g.z*tilt.y;
            base.orientation.y = g.w*tilt.y - g.x*tilt.z + g.y*tilt.w + g.z*tilt.x;
            base.orientation.z = g.w*tilt.z + g.x*tilt.y - g.y*tilt.x + g.z*tilt.w;
            base.orientation.w = g.w*tilt.w - g.x*tilt.x - g.y*tilt.y - g.z*tilt.z;

            const XrVector3f xAxisWorld = rotate(base.orientation, XrVector3f{1.0f, 0.0f, 0.0f});
            const XrVector3f n   = rotate(base.orientation, XrVector3f{0.0f, 0.0f, 1.0f});
            const XrVector3f ux  = xAxisWorld;
            const XrVector3f uy  = rotate(base.orientation, XrVector3f{0.0f, 1.0f, 0.0f});
            const float halfSz   = WristMenuButton::kSizeMeters * 0.5f;
            const float spacing  = WristMenuButton::kSizeMeters + 0.005f; // 5mm gap
            // Centre the row of 3 around the base position: offsets
            // -spacing, 0, +spacing along the X axis.
            const float xOff[WB_Count] = {-spacing, 0.0f, spacing};

            for (int btn = 0; btn < WB_Count; ++btn) {
                XrPosef pose = base;
                pose.position.x += xAxisWorld.x * xOff[btn];
                pose.position.y += xAxisWorld.y * xOff[btn];
                pose.position.z += xAxisWorld.z * xOff[btn];
                mWristBtns[btn].SetPose(pose, mWristBtns[btn].IsInitialised());

                // Per-hand aim raycast against this button's plane.
                for (uint32_t hand = 0; hand < 2; ++hand) {
                    const auto& s2 = mController.State(static_cast<Hand>(hand));
                    if (!s2.aim_pose_valid) continue;
                    const XrVector3f fwd = rotate(s2.aim_pose.orientation,
                                                  XrVector3f{0.0f, 0.0f, -1.0f});
                    const float denom = fwd.x * n.x + fwd.y * n.y + fwd.z * n.z;
                    if (std::abs(denom) < 1e-4f) continue;
                    const XrVector3f d{s2.aim_pose.position.x - pose.position.x,
                                       s2.aim_pose.position.y - pose.position.y,
                                       s2.aim_pose.position.z - pose.position.z};
                    const float t = -(d.x * n.x + d.y * n.y + d.z * n.z) / denom;
                    if (t <= 0.0f) continue;
                    const XrVector3f hit{s2.aim_pose.position.x + fwd.x * t - pose.position.x,
                                         s2.aim_pose.position.y + fwd.y * t - pose.position.y,
                                         s2.aim_pose.position.z + fwd.z * t - pose.position.z};
                    const float lx = hit.x * ux.x + hit.y * ux.y + hit.z * ux.z;
                    const float ly = hit.x * uy.x + hit.y * uy.y + hit.z * uy.z;
                    if (lx < -halfSz || lx > halfSz || ly < -halfSz || ly > halfSz) continue;
                    btnHovered[btn] = true;
                    anyWristHovered = true;
                    handWristHover[hand] = true;

                    const bool trig = s2.trigger_click || s2.trigger_value > 0.5f;
                    bool& last = mLastWristTrigger[btn][hand];

                    if (btn == WB_Menu) {
                        // Menu button: long-press = Home, release = toggle.
                        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (trig) {
                            chordMenuHeld = true;   // D4: track for Menu+Start chord
                            if (mWristHomeStartNs[hand] == 0) {
                                mWristHomeStartNs[hand] = now_ns;
                            } else if (!mWristHomeFired[hand] &&
                                       (now_ns - mWristHomeStartNs[hand]) >= kWristHomeHoldNs) {
                                mWristHomeFired[hand] = true;
                                if (mOnHome) {
                                    mOnHome();
                                    ALOGI("3DS Home fired (wrist long-press, hand=%u)", hand);
                                }
                                mController.TriggerHaptic(static_cast<Hand>(hand),
                                                          0.8f, 80'000'000LL, 160.0f);
                            }
                        } else {
                            if (last && !mWristHomeFired[hand] && mImGuiLayer.IsInitialised()) {
                                const bool willOpen = !mImGuiLayer.IsVisible();
                                mImGuiLayer.SetVisible(willOpen);
                                if (willOpen) mMenuRecenterPending = true;
                                ALOGI("In-VR menu %s (wrist Menu, hand=%u)",
                                      mImGuiLayer.IsVisible() ? "OPEN" : "CLOSED", hand);
                                mController.TriggerHaptic(static_cast<Hand>(hand),
                                                          0.5f, 30'000'000LL, 240.0f);
                            }
                            mWristHomeStartNs[hand] = 0;
                            mWristHomeFired[hand]   = false;
                        }
                    } else {
                        // Start / Select: held while trigger is down.
                        if (trig) btnHeldNow[btn] = true;
                        if (trig && !last) {
                            mController.TriggerHaptic(static_cast<Hand>(hand),
                                                      0.4f, 20'000'000LL, 220.0f);
                        }
                    }
                    last = trig;
                }
            }
        } else {
            // Left grip not tracked: hide all buttons and clear edges.
            for (int btn = 0; btn < WB_Count; ++btn) {
                mWristBtns[btn].SetPose(XrPosef{{0,0,0,1},{0,0,0}}, false);
            }
        }

        // Hover-state -> recolour, plus one-shot enter haptic.
        for (int btn = 0; btn < WB_Count; ++btn) {
            mWristBtns[btn].SetHovered(btnHovered[btn], mSubmitter);
            if (btnHovered[btn] && !mWristWasHovered[btn]) {
                mController.TriggerHaptic(Hand::Left,  0.20f, 12'000'000LL, 180.0f);
                mController.TriggerHaptic(Hand::Right, 0.20f, 12'000'000LL, 180.0f);
            }
            mWristWasHovered[btn] = btnHovered[btn];
        }
        // Publish held state for VrInputBridge consumption.
        mWristBtnHeld[WB_Start]  = btnHeldNow[WB_Start];
        mWristBtnHeld[WB_Select] = btnHeldNow[WB_Select];

        // Clear stale edge-state for any (btn, hand) NOT hovered this
        // frame so a held trigger off-button doesn't fire a phantom
        // release-edge when the user re-aims.
        for (int btn = 0; btn < WB_Count; ++btn) {
            if (btnHovered[btn]) continue;
            for (uint32_t hand = 0; hand < 2; ++hand) {
                const auto& s2 = mController.State(static_cast<Hand>(hand));
                mLastWristTrigger[btn][hand] = s2.trigger_click || s2.trigger_value > 0.5f;
            }
        }
        // If the Menu button isn't hovered, also clear its hold timers.
        if (!btnHovered[WB_Menu]) {
            mWristHomeStartNs[0] = mWristHomeStartNs[1] = 0;
            mWristHomeFired[0]   = mWristHomeFired[1]   = false;
        }
        (void)gripOk; (void)anyWristHovered;

        // D4: Quick-save chord â€” Menu + Start both triggered = save to slot 0.
        if (chordMenuHeld && btnHeldNow[WB_Start] && !mWristChordActive) {
            mWristChordActive = true;
            // Suppress menu-toggle release for both hands.
            mWristHomeFired[0] = mWristHomeFired[1] = true;
            Core::System::GetInstance().SendSignal(Core::System::Signal::Save, 0u);
            mImGuiLayer.SetVisible(false);
            Common::VRConfig::SetInt("last_slot", 0);
            ShowToast("Quick-saved (slot 0)");
            // C3: success double-thump haptic.
            const auto now_chord = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            mController.TriggerHaptic(Hand::Left,  Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
            mController.TriggerHaptic(Hand::Right, Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
            mHapticSuccessT2Ns[0] = now_chord + Haptic::kSuccessGapNs;
            mHapticSuccessT2Ns[1] = now_chord + Haptic::kSuccessGapNs;
        } else if (!chordMenuHeld || !btnHeldNow[WB_Start]) {
            mWristChordActive = false;
        }

        // L_Thumb (L3) is intentionally NOT bound to the menu toggle.
        // The wrist Menu button is the sole way to open/close the menu.
    }

    // C5: Right System button long-press (700 ms) \u2192 recenter view.
    {
        const auto& rState = mController.State(Hand::Right);
        const auto now_rc = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (rState.system_click) {
            if (mRecenterStartNs == 0) {
                mRecenterStartNs = now_rc;
            } else if (!mRecenterFired &&
                       (now_rc - mRecenterStartNs) >= kWristHomeHoldNs) {
                mRecenterFired       = true;
                mMenuRecenterPending = true;
                mRecenterFlash       = 1.0f;
                ShowToast("View recentered");
                mController.TriggerHaptic(Hand::Right, Haptic::kThumpAmp,
                                          Haptic::kThumpDur, Haptic::kThumpFreq);
            }
        } else {
            mRecenterStartNs = 0;
            mRecenterFired   = false;
        }
    }

    // Left-hand raycast against the menu quad (pose set by ImGuiLayer).
    int  menuPxX        = -1;
    int  menuPxY        = -1;
    bool menuPressed    = false;
    if (mImGuiLayer.IsInitialised() && mImGuiLayer.IsVisible()) {
        const auto&    lState   = mController.State(Hand::Left);
        const XrPosef& menuPose = mImGuiLayer.Pose();
        // Plane normal = menu pose forward (-Z in pose space).
        const XrVector3f n  = rotate(menuPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
        const XrVector3f ux = rotate(menuPose.orientation, XrVector3f{1.0f, 0.0f, 0.0f});
        const XrVector3f uy = rotate(menuPose.orientation, XrVector3f{0.0f, 1.0f, 0.0f});
        if (lState.aim_pose_valid) {
            const XrVector3f fwd = rotate(lState.aim_pose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
            // Default: 5 m laser-pointer ray for the left hand while the
            // menu is open.  Gives a visible beam that snaps to the menu
            // surface when the ray hits the panel.
            cursors[0].position = XrVector3f{
                lState.aim_pose.position.x + fwd.x * 5.0f,
                lState.aim_pose.position.y + fwd.y * 5.0f,
                lState.aim_pose.position.z + fwd.z * 5.0f,
            };
            cursors[0].visible        = true;
            // The menu quad is a normal stereo-depth layer, not the
            // infinite-depth game quad, so clear any snap flag the
            // game-quad raycast above may have set on the left cursor.
            cursors[0].snap_to_screen = false;
            // Ray vs plane: dot(P + t*fwd - C, n) = 0
            const XrVector3f d{lState.aim_pose.position.x - menuPose.position.x,
                               lState.aim_pose.position.y - menuPose.position.y,
                               lState.aim_pose.position.z - menuPose.position.z};
            const float denom = fwd.x * n.x + fwd.y * n.y + fwd.z * n.z;
            if (std::abs(denom) > 1e-4f) {
                const float t = -(d.x * n.x + d.y * n.y + d.z * n.z) / denom;
                if (t > 0.0f) {
                    const XrVector3f hit{lState.aim_pose.position.x + fwd.x * t - menuPose.position.x,
                                         lState.aim_pose.position.y + fwd.y * t - menuPose.position.y,
                                         lState.aim_pose.position.z + fwd.z * t - menuPose.position.z};
                    const float lx = hit.x * ux.x + hit.y * ux.y + hit.z * ux.z; // metres
                    const float ly = hit.x * uy.x + hit.y * uy.y + hit.z * uy.z;
                    const float halfW = ImGuiLayer::kQuadW * 0.5f;
                    const float halfH = ImGuiLayer::kQuadH * 0.5f;
                    if (lx >= -halfW && lx <= halfW && ly >= -halfH && ly <= halfH) {
                        // Map [-halfW, halfW] -> [0, kPixelW), [+halfH..-halfH] -> [0, kPixelH)
                        menuPxX = static_cast<int>(((lx + halfW) / ImGuiLayer::kQuadW) * ImGuiLayer::kPixelW);
                        menuPxY = static_cast<int>(((halfH - ly) / ImGuiLayer::kQuadH) * ImGuiLayer::kPixelH);
                        // Snap left cursor onto the menu surface.
                        cursors[0].position = XrVector3f{
                            lState.aim_pose.position.x + fwd.x * t,
                            lState.aim_pose.position.y + fwd.y * t,
                            lState.aim_pose.position.z + fwd.z * t,
                        };
                    }
                }
            }
            menuPressed = lState.trigger_click || lState.trigger_value > 0.5f;
        }
        mImGuiLayer.SetMouse(menuPxX, menuPxY, menuPressed);

        // ---- Cursor "over interactive" + click-pulse feedback -----------
        // Per-hand: hovering a wrist button or (left-only) hitting the
        // menu plane both count as "interactive". Bump cursor scale
        // and arm a click-pulse on trigger-press transitions.
        const bool leftOverMenu = (menuPxX >= 0);
        const bool overInteractive[2] = {
            handWristHover[0] || leftOverMenu,
            handWristHover[1],
        };
        for (uint32_t hand = 0; hand < 2; ++hand) {
            cursors[hand].over_interactive = overInteractive[hand];
            // Decay the existing pulse over ~180 ms.
            constexpr float kPulseSeconds = 0.18f;
            if (mCursorClickPulse[hand] > 0.0f && kPulseSeconds > 0.0f) {
                mCursorClickPulse[hand] = std::max(
                    0.0f, mCursorClickPulse[hand] - dt / kPulseSeconds);
            }
            // Re-arm on a trigger press transition while interactive.
            const auto& hs   = mController.State(static_cast<Hand>(hand));
            const bool trig  = hs.trigger_click || hs.trigger_value > 0.5f;
            if (trig && !mLastTriggerEdge[hand] && overInteractive[hand]) {
                mCursorClickPulse[hand] = 1.0f;
            }
            mLastTriggerEdge[hand]      = trig;
            cursors[hand].click_pulse   = mCursorClickPulse[hand];
        }
        mCursorLayer.SetCursors(cursors[0], cursors[1]);
    }

    // Begin / draw / end ImGui frame and render to the menu swapchain.
    // BeginFrame() returns true while the panel has any non-zero alpha
    // (fully open OR in mid open/close fade), AND also when a toast is
    // active so transient notifications render even with the menu closed.
    mImGuiLayer.Tick(dt);
    // ---- Right thumbstick â†’ menu scroll (injected before BeginFrame) ----
    // While the menu is visible, the right thumbstick Y axis scrolls the
    // content. AddScroll() accumulates and injects into io.MouseWheel
    // inside BeginFrame() (before ImGui::NewFrame consumes it).
    // Deadzone 0.15 to avoid drift from stick centre; scale maps full
    // deflection to ~1.5 ImGui wheel units per frame (comfortable at 90 fps).
    if (mImGuiLayer.IsInitialised() && mImGuiLayer.IsVisuallyVisible()) {
        const auto& rScroll = mController.State(Hand::Right);
        constexpr float kScrollDeadzone = 0.15f;
        const float stickY = rScroll.thumbstick.y;
        if (std::abs(stickY) > kScrollDeadzone) {
            // Negative: scrolls the window DOWN (positive Y = stick up = scroll up
            // = positive ImGui wheel). Match natural scroll direction.
            mImGuiLayer.AddScroll(stickY * 1.5f);
        }
    }
    // Push perf overlay text every frame; ImGuiLayer's BeginFrame keeps
    // returning true while perf is active, so the overlay renders even
    // when the main menu is hidden.
    if (mShowPerfOverlay) {
        const auto p = GetPerfSnapshot();
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "FPS %5.1f   %5.2f ms\nDropped blits: %llu",
                      p.fps, p.frame_interval_ms,
                      static_cast<unsigned long long>(p.dropped_blits));
        mImGuiLayer.SetPerfOverlay(true, buf);
    } else {
        mImGuiLayer.SetPerfOverlay(false, std::string());
    }

    if (mImGuiLayer.IsInitialised() && mImGuiLayer.BeginFrame()) {
        // Full CitraVR Menu: only rendered while the menu is visually
        // present (alpha > 0). Skipped during toast-only frames so the
        // background stays fully transparent behind the toast window.
        if (mImGuiLayer.IsVisuallyVisible()) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("CitraVR Menu", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 16));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));

        // ---- Hero banner -------------------------------------------------
        // Coloured accent bar + large title + dim subtitle. Replaces the
        // old single-line "CitraVR  -  Steam VR build" label.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 topL = ImGui::GetCursorScreenPos();
            const float  barW = ImGui::GetContentRegionAvail().x;
            const float  barH = 6.0f;
            // Horizontal gradient cyan â†’ indigo. AddRectFilledMultiColor
            // takes corner colours (TL, TR, BR, BL); set TL/BL = cyan and
            // TR/BR = indigo for a clean left-to-right ramp. GetColorU32
            // honours the global Style.Alpha so the bar fades with the
            // menu open/close animation.
            const ImU32 cCyan   = ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
            const ImU32 cIndigo = ImGui::GetColorU32(ImVec4(0.42f, 0.36f, 0.92f, 1.0f));
            const ImVec2 br(topL.x + barW, topL.y + barH);
            dl->AddRectFilledMultiColor(topL, br, cCyan, cIndigo, cIndigo, cCyan);
            ImGui::Dummy(ImVec2(0, barH + 4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::Text("CitraVR");
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
            // Baseline-align the subtitle to the display title bottom by
            // nudging the cursor down ~12 px (display 56 px - caption 22 px
            // â‰ˆ 34 px ascent gap, half-ish for visual balance).
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 26.0f);
            ImGui::Text("  SteamVR build");
            if (mImGuiLayer.FontCaption()) ImGui::PopFont();
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // Helper: teal-accented section separator label.
        auto section_label = [](const char* label) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
            ImGui::SeparatorText(label);
            ImGui::PopStyleColor();
        };

        // ---- Sidebar + content split ------------------------------------
        // Sidebar holds the tab buttons + a bottom-pinned Quit. Content
        // child holds the active tab's widgets. Each section below is
        // gated by an `if (mCurrentTab == MenuTab::X)` so unrelated
        // widgets aren't drawn (and don't capture input).
        constexpr float kSidebarW = 240.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.12f, 1.0f));
        // Sidebar shrinks vertically by 44 px to leave room for the
        // footer hint bar (drawn at window scope after content EndChild).
        ImGui::BeginChild("##sidebar", ImVec2(kSidebarW, -44.0f), true,
                          ImGuiWindowFlags_NoScrollbar);
        {
            // Tab buttons: dark base for both states; the active tab is
            // marked by a 6 px cyan accent stripe drawn on the left edge,
            // a slightly lifted background, and bright text. Looks like a
            // proper navigation rail rather than a coloured pill.
            //
            // Optional `badge` string renders as a small pill on the right
            // edge of the button — used to surface live counts (ROM
            // count, filled save slots, remapped bindings) so the user
            // doesn't have to dive into a tab to know what's there.
            auto tab_button = [&](MenuTab t, const char* label,
                                  const char* badge = nullptr) {
                const bool active = (mCurrentTab == t);
                constexpr ImVec4 kAccent  = ImVec4(0.30f, 0.72f, 0.95f, 1.00f);
                constexpr ImVec4 kBgDark  = ImVec4(0.10f, 0.13f, 0.17f, 1.00f);
                constexpr ImVec4 kBgHover = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
                constexpr ImVec4 kBgAct   = ImVec4(0.14f, 0.19f, 0.26f, 1.00f);
                ImGui::PushStyleColor(ImGuiCol_Button,        active ? kBgAct  : kBgDark);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kBgAct  : kBgHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kBgHover);
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.96f, 0.98f, 1.00f, 1.00f));
                }
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                if (ImGui::Button(label, ImVec2(-FLT_MIN, 60))) {
                    mCurrentTab = t;
                }
                const ImVec2 p1 = ImGui::GetItemRectMax();
                ImDrawList* dl  = ImGui::GetWindowDrawList();
                if (active) {
                    // 6 px stripe on the left edge of the button rect,
                    // drawn after the button so it sits on top of the
                    // (rounded) frame fill. Respects the open/close fade.
                    dl->AddRectFilled(
                        p0,
                        ImVec2(p0.x + 6.0f, p1.y),
                        ImGui::GetColorU32(kAccent),
                        3.0f);
                    ImGui::PopStyleColor(); // Text
                }
                ImGui::PopStyleColor(3);

                // Badge pill rendered after style pops so its colour
                // doesn't inherit the button's text-color override.
                if (badge && badge[0]) {
                    if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                    const ImVec2 ts = ImGui::CalcTextSize(badge);
                    constexpr float kPadX = 10.0f, kPadY = 4.0f;
                    const float w = ts.x + kPadX * 2.0f;
                    const float h = ts.y + kPadY * 2.0f;
                    const ImVec2 bMin(p1.x - w - 14.0f,
                                      p0.y + (60.0f - h) * 0.5f);
                    const ImVec2 bMax(bMin.x + w, bMin.y + h);
                    const ImVec4 bgCol = active
                        ? ImVec4(0.30f, 0.72f, 0.95f, 1.00f)
                        : ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
                    const ImVec4 fgCol = active
                        ? ImVec4(0.04f, 0.07f, 0.10f, 1.00f)
                        : ImVec4(0.78f, 0.83f, 0.89f, 1.00f);
                    dl->AddRectFilled(bMin, bMax,
                                      ImGui::GetColorU32(bgCol),
                                      h * 0.5f);
                    dl->AddText(ImVec2(bMin.x + kPadX, bMin.y + kPadY),
                                ImGui::GetColorU32(fgCol), badge);
                    if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                }
            };

            // ---- Pre-compute badge counts -------------------------------
            // Cheap to run every frame: snapshot count is a single size()
            // call, save-slot count uses the cached SlotInfo populated by
            // the Saves tab (may be 0 until first visit), bindings count
            // walks 10 enums.
            char badgeLib[16] = {0}, badgeSav[16] = {0}, badgeInp[16] = {0};
            {
                const std::size_t romN = mRomBrowser.EntriesSnapshot().size();
                if (romN > 0) std::snprintf(badgeLib, sizeof(badgeLib),
                                            "%zu", romN);
            }
            {
                using Bridge = vr_pcvr::VrInputBridge;
                static const Bridge::VrButtonId kIds[] = {
                    Bridge::Btn_A, Bridge::Btn_B, Bridge::Btn_X, Bridge::Btn_Y,
                    Bridge::Btn_L, Bridge::Btn_R, Bridge::Btn_ZL, Bridge::Btn_ZR,
                    Bridge::Btn_Start, Bridge::Btn_Select,
                };
                int remapped = 0;
                if (mInputBridge) {
                    for (auto id : kIds) {
                        if (mInputBridge->GetBinding(id) !=
                            Bridge::DefaultBinding(id)) ++remapped;
                    }
                }
                if (remapped > 0) std::snprintf(badgeInp, sizeof(badgeInp),
                                                "%d", remapped);
            }

            tab_button(MenuTab::Library, "Library", badgeLib);
            tab_button(MenuTab::Display, "Display");
            tab_button(MenuTab::Saves,   "Saves",   badgeSav);
            tab_button(MenuTab::Input,   "Input",   badgeInp);
            tab_button(MenuTab::Session, "Session");
            tab_button(MenuTab::About,   "About");

            // ---- Sidebar status panel ------------------------------------
            // Slim card pinned just above the Quit button showing whether
            // a game is currently running. "LIVE" pulse-dot when active;
            // dim "Idle" when not. Gives at-a-glance context regardless
            // of which tab the user is currently on.
            const float btnH      = 60.0f;
            constexpr float kStatusH = 78.0f;
            const float availY    = ImGui::GetWindowHeight();
            const float statusY   = availY - btnH - kStatusH - 16.0f;
            if (statusY > ImGui::GetCursorPosY()) {
                ImGui::SetCursorPosY(statusY);
            }
            {
                auto& sysS = Core::System::GetInstance();
                const bool live = sysS.IsPoweredOn();
                std::string title;
                if (live) sysS.GetAppLoader().ReadTitle(title);

                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                      ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                ImGui::BeginChild("##side_status", ImVec2(-FLT_MIN, kStatusH), true,
                                  ImGuiWindowFlags_NoScrollbar);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 sMin = ImGui::GetWindowPos();

                // Left-edge accent stripe (cyan = live, slate = idle).
                const ImVec4 stripeCol = live
                    ? ImVec4(0.30f, 0.85f, 0.50f, 1.0f)   // green = live
                    : ImVec4(0.20f, 0.26f, 0.33f, 0.85f); // slate = idle
                dl->AddRectFilled(sMin,
                                  ImVec2(sMin.x + 4.0f, sMin.y + kStatusH),
                                  ImGui::GetColorU32(stripeCol));

                // "LIVE" / "IDLE" caption + pulse-dot.
                ImGui::SetCursorPos(ImVec2(16.0f, 8.0f));
                if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      live ? ImVec4(0.45f, 0.92f, 0.62f, 1.0f)
                                           : ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
                ImGui::TextUnformatted(live ? "LIVE" : "IDLE");
                ImGui::PopStyleColor();
                if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                if (live) {
                    // Soft pulsing dot on the right (sin-wave alpha).
                    const float t     = static_cast<float>(ImGui::GetTime());
                    const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);
                    const ImVec2 dotP(sMin.x + ImGui::GetWindowWidth() - 18.0f,
                                      sMin.y + 18.0f);
                    dl->AddCircleFilled(dotP, 5.0f,
                        ImGui::GetColorU32(ImVec4(0.45f, 0.92f, 0.62f,
                                                  0.4f + 0.6f * pulse)));
                }

                // Title (truncated to fit).
                ImGui::SetCursorPos(ImVec2(16.0f, 36.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.92f, 0.95f, 0.99f, 1.0f));
                if (live && !title.empty()) {
                    // Truncate long titles to keep the line single-row.
                    std::string display = title;
                    if (display.size() > 22) {
                        display.resize(20);
                        display += "\xE2\x80\xA6"; // ellipsis
                    }
                    ImGui::TextUnformatted(display.c_str());
                } else if (live) {
                    ImGui::TextUnformatted("Untitled ROM");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
                    ImGui::TextUnformatted("No game running");
                    ImGui::PopStyleColor();
                }
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }

            // Bottom-pinned Quit (opens confirmation modal).
            ImGui::SetCursorPosY(availY - btnH - 8.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.82f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
            if (ImGui::Button("Quit", ImVec2(-FLT_MIN, btnH))) {
                // Defer the actual exit to the modal below so a stray
                // trigger pull doesn't kill the session.
                mQuitConfirmPending = true;
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(); // sidebar ChildBg

        // ---- Hairline tapered separator -------------------------------
        // Vertical 1 px gradient line between sidebar and content. Fades
        // in at top, peaks middle, fades out at bottom for a soft visual
        // anchor without competing with widget borders. Drawn at window
        // scope so its height matches the sidebar+content height (minus
        // the 44 px footer bar).
        {
            ImDrawList* dl  = ImGui::GetWindowDrawList();
            const ImVec2 wpos = ImGui::GetWindowPos();
            const float  wh   = ImGui::GetWindowHeight();
            const ImVec2 cur  = ImGui::GetCursorScreenPos();
            const float  x    = cur.x - 6.0f;
            const float  y0   = cur.y;
            const float  y1   = wpos.y + wh - 44.0f;
            const float  ymid = (y0 + y1) * 0.5f;
            const ImU32 cMid = ImGui::GetColorU32(
                ImVec4(0.30f, 0.72f, 0.95f, 0.45f));
            const ImU32 cEnd = ImGui::GetColorU32(
                ImVec4(0.30f, 0.72f, 0.95f, 0.0f));
            dl->AddRectFilledMultiColor(ImVec2(x,        y0),
                                        ImVec2(x + 1.0f, ymid),
                                        cEnd, cEnd, cMid, cMid);
            dl->AddRectFilledMultiColor(ImVec2(x,        ymid),
                                        ImVec2(x + 1.0f, y1),
                                        cMid, cMid, cEnd, cEnd);
        }

        ImGui::SameLine();
        // Reserve 44 px at the bottom for the footer hint bar drawn after
        // EndChild. Content child shrinks vertically so the bar never
        // overlaps tab content (Library's launch bar, About perf toggleâ€¦).
        // ---- Tab-switch fade microanimation ----------------------------
        // Whenever mCurrentTab changes, snapshot the new tab and reset the
        // fade timer so the new content fades in over ~150 ms. Advances on
        // wall-clock dt provided by ImGui (driven by the XR layer).
        if (mPrevTab != mCurrentTab) {
            mPrevTab     = mCurrentTab;
            mTabSwitchT  = 0.0f;
        }
        if (mTabSwitchT < 1.0f) {
            const float dtTab = ImGui::GetIO().DeltaTime;
            mTabSwitchT = std::min(1.0f, mTabSwitchT + dtTab * 6.7f); // ~150 ms
        }
        // Ease-out cubic for a snappier-feeling settle.
        const float fadeAlpha = [t = mTabSwitchT]() {
            const float k = 1.0f - t;
            return 1.0f - k * k * k;
        }();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlpha);
        ImGui::BeginChild("##content", ImVec2(0, -44.0f), false);

        // ---- Settings ----------------------------------------------------
        if (mCurrentTab == MenuTab::Display) {
        section_label("Settings");

        // ---- Settings search ------------------------------------------
        // Filter rows by substring of the row label (case-insensitive).
        // Empty filter shows everything. Drawn as a chrome-less InputText
        // with a faint search-icon glyph and a Clear button.
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.18f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.16f, 0.20f, 0.28f, 1.0f));
            ImGui::PushItemWidth(-110.0f);
            ImGui::InputTextWithHint("##settings_filter",
                                     "Search settings...",
                                     mSettingsFilter,
                                     IM_ARRAYSIZE(mSettingsFilter));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f, 0.20f, 0.26f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.28f, 0.36f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.36f, 0.44f, 1.0f));
            const bool hasFilter = mSettingsFilter[0] != '\0';
            if (!hasFilter) ImGui::BeginDisabled();
            if (ImGui::Button("Clear", ImVec2(100, 0))) {
                mSettingsFilter[0] = '\0';
            }
            if (!hasFilter) ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::PopStyleColor(3);
            ImGui::Spacing();
        }

        // ---- Reset all to defaults ------------------------------------
        // Counts overrides on the fly: shows a cyan caption with the
        // count when > 0, paired with a "Reset all" amber-stripe button
        // that opens a confirm modal. When everything matches defaults
        // the row collapses entirely so it doesn't add visual noise.
        {
            int overrides = 0;
            if (static_cast<int>(Settings::values.resolution_factor.GetValue()) !=
                static_cast<int>(Settings::values.resolution_factor.GetDefault())) ++overrides;
            if (static_cast<int>(Settings::values.frame_limit.GetValue()) !=
                static_cast<int>(Settings::values.frame_limit.GetDefault())) ++overrides;
            if (Settings::values.volume.GetValue() !=
                Settings::values.volume.GetDefault()) ++overrides;
            if (Settings::values.enable_audio_stretching.GetValue() !=
                Settings::values.enable_audio_stretching.GetDefault()) ++overrides;
            if (std::abs(VRSettings::values.screen_scale    - 1.0f)  > 0.001f) ++overrides;
            if (std::abs(VRSettings::values.screen_distance - 1.5f)  > 0.001f) ++overrides;
            if (std::abs(VRSettings::values.eye_offset * 1000.0f - 32.5f) > 0.05f) ++overrides;
            if (static_cast<int>(Settings::values.vr_immersive_mode.GetValue()) !=
                static_cast<int>(Settings::values.vr_immersive_mode.GetDefault())) ++overrides;
            if (static_cast<int>(Settings::values.factor_3d.GetValue()) !=
                static_cast<int>(Settings::values.factor_3d.GetDefault())) ++overrides;
            if (Settings::values.layout_option.GetValue() !=
                Settings::values.layout_option.GetDefault()) ++overrides;
            if (Settings::values.swap_screen.GetValue() !=
                Settings::values.swap_screen.GetDefault()) ++overrides;

            if (overrides > 0) {
                char msg[64];
                std::snprintf(msg, sizeof(msg),
                              "%d setting%s changed from default",
                              overrides, overrides == 1 ? "" : "s");
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
                if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                ImGui::TextUnformatted(msg);
                if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::SameLine();
                // Push the button to the right edge.
                const float btnW = 200.0f;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                                ImGui::GetCursorPosX() - btnW);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.40f, 0.30f, 0.10f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.42f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.50f, 0.18f, 1.0f));
                if (ImGui::Button("Reset all", ImVec2(btnW, 0))) {
                    mResetDisplayPending = true;
                    ImGui::OpenPopup("ResetDisplayConfirm");
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    VrTip("Restore every Display setting to its\nbuilt-in default value.");
                ImGui::Spacing();
            }
        }

        // Confirmation modal for Reset all.
        if (ImGui::BeginPopupModal("ResetDisplayConfirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoMove)) {
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::TextUnformatted("Reset Display Settings?");
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
            ImGui::TextUnformatted("This restores every Display setting to its\n"
                                   "compiled-in default. Saved games are not\n"
                                   "affected.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Spacing();
            const float w = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
            if (ImGui::Button("Cancel", ImVec2(w, 60))) {
                mResetDisplayPending = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.42f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.50f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.58f, 0.22f, 1.0f));
            if (ImGui::Button("Reset all", ImVec2(w, 60))) {
                Settings::values.resolution_factor =
                    Settings::values.resolution_factor.GetDefault();
                Settings::values.frame_limit =
                    Settings::values.frame_limit.GetDefault();
                Settings::values.volume =
                    Settings::values.volume.GetDefault();
                Settings::values.enable_audio_stretching =
                    Settings::values.enable_audio_stretching.GetDefault();
                Settings::values.vr_immersive_mode =
                    Settings::values.vr_immersive_mode.GetDefault();
                Settings::values.factor_3d =
                    Settings::values.factor_3d.GetDefault();
                Settings::values.layout_option =
                    Settings::values.layout_option.GetDefault();
                Settings::values.swap_screen =
                    Settings::values.swap_screen.GetDefault();
                VRSettings::values.vr_immersive_mode = 0;
                VRSettings::values.vr_factor_3d = 0;
                VRSettings::values.screen_scale = 1.0f;
                VRSettings::values.screen_distance = 1.5f;
                VRSettings::values.eye_offset = 0.0325f;
                Common::VRConfig::Set("screen_scale",    "1.000000");
                Common::VRConfig::Set("screen_distance", "1.500000");
                Common::VRConfig::Set("eye_offset",      "0.032500");
                Common::VRConfig::SetInt("vr_immersive_mode", 0);
                Common::VRConfig::SetInt("vr_factor_3d", 0);
                Core::System::GetInstance().ApplySettings();
                mImGuiLayer.ShowToast("Display settings reset to defaults",
                                      ImGuiLayer::ToastKind::Warning);
                mResetDisplayPending = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::EndPopup();
        }

        // Substring-match lambda used by every row helper. Lowercases both
        // sides on the fly so the filter is case-insensitive.
        auto match = [this](const char* label) -> bool {
            if (mSettingsFilter[0] == '\0') return true;
            // Lowercase the needle once per call (filter is short).
            char needle[64];
            std::size_t ni = 0;
            for (; ni + 1 < sizeof(needle) && mSettingsFilter[ni]; ++ni) {
                needle[ni] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(mSettingsFilter[ni])));
            }
            needle[ni] = '\0';
            if (ni == 0) return true;
            for (const char* p = label; *p; ++p) {
                if (std::tolower(static_cast<unsigned char>(*p)) ==
                    static_cast<unsigned char>(needle[0])) {
                    std::size_t k = 1;
                    for (; k < ni && p[k]; ++k) {
                        if (std::tolower(static_cast<unsigned char>(p[k])) !=
                            static_cast<unsigned char>(needle[k]))
                            break;
                    }
                    if (k == ni) return true;
                }
            }
            return false;
        };

        // Helper to render a "label  [- value +]" stepper row. ImGui sliders
        // are hard to use precisely with a 1024Ã—1024 quad and trigger
        // jitter, so every numeric setting in the VR menu uses discrete
        // steppers. Layout is driven by a 2-column table per row so the
        // label column always lines up regardless of menu width or font
        // metrics. Returns true on the frame the value changed.
        // `modified` (when true) draws a 6 px cyan dot inside the label
        // column gutter, signalling the value differs from its compiled
        // default. Mirrors how Steam / OBS settings flag user overrides.
        auto stepper_int = [&match](const char* label, int& v, int vmin, int vmax, int step,
                                    const char* fmt, bool modified = false) -> bool {
            if (!match(label)) return false;
            bool changed = false;
            ImGui::PushID(label);
            constexpr ImVec4 kTextDim = ImVec4(0.63f, 0.69f, 0.76f, 1.00f);
            constexpr ImVec4 kFrameBg = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
            if (ImGui::BeginTable("##row", 2,
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthFixed, 380.0f);
                ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (modified) {
                    const ImVec2 cp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(cp.x + 4.0f, cp.y + ImGui::GetFrameHeight() * 0.5f),
                        4.0f,
                        ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.00f)));
                    ImGui::Dummy(ImVec2(14.0f, 0.0f));
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (ImGui::Button("-", ImVec2(80, 60))) {
                    if (v - step >= vmin) { v -= step; changed = true; }
                    else if (v != vmin)   { v = vmin;  changed = true; }
                }
                ImGui::SameLine(0, 4);
                char buf[32];
                std::snprintf(buf, sizeof(buf), fmt, v);
                // Value chip: non-interactive button styled as a flat frame.
                ImGui::PushStyleColor(ImGuiCol_Button,        kFrameBg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFrameBg);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kFrameBg);
                ImGui::Button(buf, ImVec2(220, 60));
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 4);
                if (ImGui::Button("+", ImVec2(80, 60))) {
                    if (v + step <= vmax) { v += step; changed = true; }
                    else if (v != vmax)   { v = vmax;  changed = true; }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            return changed;
        };
        auto stepper_float = [&match](const char* label, float& v, float vmin, float vmax, float step,
                                      const char* fmt, bool modified = false) -> bool {
            if (!match(label)) return false;
            bool changed = false;
            ImGui::PushID(label);
            constexpr ImVec4 kTextDim = ImVec4(0.63f, 0.69f, 0.76f, 1.00f);
            constexpr ImVec4 kFrameBg = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
            if (ImGui::BeginTable("##row", 2,
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthFixed, 380.0f);
                ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (modified) {
                    const ImVec2 cp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(cp.x + 4.0f, cp.y + ImGui::GetFrameHeight() * 0.5f),
                        4.0f,
                        ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.00f)));
                    ImGui::Dummy(ImVec2(14.0f, 0.0f));
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (ImGui::Button("-", ImVec2(80, 60))) {
                    v -= step; if (v < vmin) v = vmin;
                    changed = true;
                }
                ImGui::SameLine(0, 4);
                char buf[32];
                std::snprintf(buf, sizeof(buf), fmt, v);
                ImGui::PushStyleColor(ImGuiCol_Button,        kFrameBg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFrameBg);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kFrameBg);
                ImGui::Button(buf, ImVec2(220, 60));
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 4);
                if (ImGui::Button("+", ImVec2(80, 60))) {
                    v += step; if (v > vmax) v = vmax;
                    changed = true;
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            return changed;
        };

        // Segmented-control row: 2..N buttons abut with zero spacing and
        // square inner edges so the group reads as a single pill. The
        // active option is filled with the cyan accent + white text; the
        // others share the dark frame fill. After the row, an outer
        // rounded outline traces the union rect for an extra dose of
        // "designed". Returns the index that was clicked this frame, or
        // -1 if nothing was. Caller renders the leading dim label
        // separately (so multiple rows can share label widths via
        // SameLine(380)).
        auto segmented_row = [](const char* const* labels, int count, int active,
                                ImVec2 btn_size) -> int {
            constexpr ImVec4 kAccent  = ImVec4(0.30f, 0.72f, 0.95f, 1.00f);
            constexpr ImVec4 kAccentH = ImVec4(0.45f, 0.85f, 1.00f, 1.00f);
            constexpr ImVec4 kAccentA = ImVec4(0.55f, 0.92f, 1.00f, 1.00f);
            constexpr ImVec4 kBgDark  = ImVec4(0.12f, 0.16f, 0.21f, 1.00f);
            constexpr ImVec4 kBgHover = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
            constexpr ImVec4 kBgAct   = ImVec4(0.22f, 0.28f, 0.36f, 1.00f);
            constexpr ImVec4 kTextDim = ImVec4(0.78f, 0.83f, 0.89f, 1.00f);

            int clicked = -1;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

            ImVec2 firstMin{}, lastMax{};
            for (int i = 0; i < count; ++i) {
                const bool isActive = (i == active);
                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentH);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccentA);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.04f, 0.07f, 0.10f, 1.00f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kBgDark);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBgHover);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kBgAct);
                    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                }
                ImGui::PushID(i);
                if (ImGui::Button(labels[i], btn_size) && !isActive) {
                    clicked = i;
                }
                ImGui::PopID();
                if (i == 0)         firstMin = ImGui::GetItemRectMin();
                if (i == count - 1) lastMax  = ImGui::GetItemRectMax();
                ImGui::PopStyleColor(4);
                if (i < count - 1) ImGui::SameLine();
            }
            ImGui::PopStyleVar(2);

            // Outer rounded outline tracing the whole group.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRect(firstMin, lastMax,
                        ImGui::GetColorU32(ImVec4(0.20f, 0.26f, 0.33f, 0.85f)),
                        8.0f, 0, 1.5f);
            return clicked;
        };

        // Inline modified-dot for non-stepper rows (segmented controls,
        // checkboxes). Call immediately before the row's label text so
        // the cyan dot lines up in the same gutter the steppers use.
        auto mod_dot = [](bool modified) {
            if (!modified) return;
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(cp.x + 4.0f, cp.y + ImGui::GetFrameHeight() * 0.5f),
                4.0f,
                ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.00f)));
            ImGui::Dummy(ImVec2(14.0f, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
        };

        {
            int res = static_cast<int>(Settings::values.resolution_factor.GetValue());
            if (res < 1) res = 1;
            const bool mod_res =
                res != static_cast<int>(Settings::values.resolution_factor.GetDefault());
            if (stepper_int("Internal res", res, 1, 4, 1, "%dx", mod_res)) {
                Settings::values.resolution_factor = static_cast<u32>(res);
                Core::System::GetInstance().ApplySettings();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Render resolution multiplier (1-4x).\nHigher = sharper; costs GPU.");
        }

        // Frame limit %  (0 = unlimited). Steps of 5 for fine control.
        {
            int fl = static_cast<int>(Settings::values.frame_limit.GetValue());
            const bool mod_fl =
                fl != static_cast<int>(Settings::values.frame_limit.GetDefault());
            if (stepper_int("Frame limit", fl, 0, 200, 5,
                            fl == 0 ? "Unlimited" : "%d%%", mod_fl)) {
                Settings::values.frame_limit = static_cast<u16>(fl);
                Core::System::GetInstance().ApplySettings();
            }
            // Reset sits on its own row, indented to align with the
            // stepper's control column (~380 px label width).
            if (match("Frame limit")) {
                ImGui::Indent(380.0f);
                if (ImGui::Button("Reset to 100%##fl", ImVec2(220, 60))) {
                    Settings::values.frame_limit = static_cast<u16>(100);
                    Core::System::GetInstance().ApplySettings();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    VrTip("Reset to 100% (native speed).");
                ImGui::Unindent(380.0f);
            }
        }

        // Audio volume + stretching.
        {
            float vol = Settings::values.volume.GetValue();
            const bool mod_vol = vol != Settings::values.volume.GetDefault();
            if (stepper_float("Volume", vol, 0.0f, 1.0f, 0.1f, "%.1f", mod_vol)) {
                Settings::values.volume = vol;
                Core::System::GetInstance().ApplySettings();
            }
            bool stretch = Settings::values.enable_audio_stretching.GetValue();
            if (match("Audio stretching")) {
                mod_dot(stretch != Settings::values.enable_audio_stretching.GetDefault());
                if (ImGui::Checkbox("Audio stretching", &stretch)) {
                    Settings::values.enable_audio_stretching = stretch;
                    Core::System::GetInstance().ApplySettings();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    VrTip("Stretches audio to prevent crackling\nat non-native frame rates.");
            }
        }

        // ---- Quick-access quality presets --------------------------------
        // Single-tap to apply a bundle of settings tuned for different
        // GPU budgets. Each fires a toast confirming the change. Rendered
        // as a segmented row to mirror the Immersive / Layout pickers.
        ImGui::Spacing();
        if (match("Preset")) {
            struct Preset { const char* name; int res; int fl; };
            static const Preset kPresets[3] = {
                {"Performance", 1,  80},
                {"Balanced",    2, 100},
                {"Quality",     3,   0},
            };
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            ImGui::TextUnformatted("Preset");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Apply a bundle of settings optimised\nfor the selected quality/perf trade-off.");
            ImGui::SameLine(380.0f);
            const char* presetLabels[3] = {kPresets[0].name, kPresets[1].name, kPresets[2].name};
            // -1 = no current selection (presets are momentary actions).
            int hit = segmented_row(presetLabels, 3, -1, ImVec2(200, 55));
            if (hit >= 0) {
                const auto& p = kPresets[hit];
                Settings::values.resolution_factor = static_cast<u32>(p.res);
                Settings::values.frame_limit       = static_cast<u16>(p.fl);
                Core::System::GetInstance().ApplySettings();
                mImGuiLayer.ShowToast(std::string("Preset: ") + p.name,
                                      ImGuiLayer::ToastKind::Success);
            }
        }

        // ---- Virtual screen geometry -------------------------------------
        ImGui::Spacing();
        {
            float scale = VRSettings::values.screen_scale;
            if (stepper_float("Screen size", scale, 0.5f, 3.0f, 0.1f, "%.1fx",
                              std::abs(scale - 1.0f) > 0.001f)) {
                VRSettings::values.screen_scale = scale;
                Common::VRConfig::Set("screen_scale", std::to_string(scale));
                mImGuiLayer.ShowToast("Screen size adjusted");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Width of the virtual screen (0.5x - 3.0x).\nBase = 1.0 m wide.");
        }
        {
            float dist = VRSettings::values.screen_distance;
            if (stepper_float("Screen dist", dist, 0.5f, 4.0f, 0.1f, "%.1f m",
                              std::abs(dist - 1.5f) > 0.001f)) {
                VRSettings::values.screen_distance = dist;
                Common::VRConfig::Set("screen_distance", std::to_string(dist));
                mImGuiLayer.ShowToast("Screen distance adjusted");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Distance to the virtual screen in metres.\nDefault = 1.5 m.");
        }
        {
            // Eye offset = per-eye horizontal shift of the two game quads.
            // Displayed in mm for clarity; stored in metres internally.
            float offset_mm = VRSettings::values.eye_offset * 1000.0f;
            if (stepper_float("Eye offset", offset_mm, 10.0f, 50.0f, 1.0f, "%.0f mm",
                              std::abs(offset_mm - 32.5f) > 0.05f)) {
                VRSettings::values.eye_offset = offset_mm / 1000.0f;
                Common::VRConfig::Set("eye_offset",
                                      std::to_string(VRSettings::values.eye_offset));
                mImGuiLayer.ShowToast("Eye offset adjusted");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Per-eye horizontal shift of the virtual screen.\n"
                                  "Matches your physical IPD. Default = 32.5 mm.");
        }

        // Game Display section continues inside the Display tab block.
        // ---- Game Display -----------------------------------------------
        ImGui::Spacing();
        section_label("Game Display");

        // Immersive mode: Off / High / Ultra.
        // Controls the VR depth warp shader (how 3D the game geometry feels
        // inside the headset) AND sets the 3DS hardware 3D slider state.
        if (match("Immersive mode")) {
            const char* const kImmNames[3] = {"Off", "High", "Ultra"};
            int imm = static_cast<int>(Settings::values.vr_immersive_mode.GetValue());
            if (imm < 0 || imm > 2) imm = 0;
            ImGui::AlignTextToFramePadding();
            mod_dot(imm != static_cast<int>(Settings::values.vr_immersive_mode.GetDefault()));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            ImGui::TextUnformatted("Immersive mode");
            ImGui::PopStyleColor();
            ImGui::SameLine(380.0f);
            const int hit = segmented_row(kImmNames, 3, imm, ImVec2(170, 55));
            if (hit >= 0) {
                Settings::values.vr_immersive_mode  = static_cast<u32>(hit);
                VRSettings::values.vr_immersive_mode = hit;
                Common::VRConfig::SetInt("vr_immersive_mode", hit);
                Core::System::GetInstance().ApplySettings();
                mImGuiLayer.ShowToast(std::string("Immersive: ") + kImmNames[hit]);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("VR depth warp intensity.\nOff = flat rendering; Ultra = full 3D geometry.");
        }

        // 3D depth: how much stereo separation the 3DS game itself renders.
        // Meaningful in Immersive modes > Off; also drives the HID 3D slider
        // so games that adapt their graphics to it (e.g. OoT) respond.
        {
            int d3 = static_cast<int>(Settings::values.factor_3d.GetValue());
            const bool mod_d3 =
                d3 != static_cast<int>(Settings::values.factor_3d.GetDefault());
            if (stepper_int("3D depth", d3, 0, 100, 5, "%d%%", mod_d3)) {
                Settings::values.factor_3d = static_cast<u32>(d3);
                VRSettings::values.vr_factor_3d = d3;
                Common::VRConfig::SetInt("vr_factor_3d", d3);
                Core::System::GetInstance().ApplySettings();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("In-game 3D stereoscopy depth (0 = off, 100 = max).\nAlso sets the 3DS hardware slider state.");
        }

        // Display layout: how Citra packs the top + bottom 3DS screens
        // into the framebuffer quad.  Options most useful in VR:
        //   Default      - standard dual screen (top above bottom)
        //   Large top    - top screen taller, bottom smaller
        //   Top only     - bottom screen hidden; good for cut-scene games
        //   Side by side - screens side-by-side (alternative placements)
        if (match("Layout")) {
            using L = Settings::LayoutOption;
            struct LayoutEntry { L opt; const char* label; };
            static const LayoutEntry kLayouts[] = {
                {L::Default,      "Default"},
                {L::LargeScreen,  "Large top"},
                {L::SingleScreen, "Top only"},
                {L::SideScreen,   "Side"},
            };
            L cur = Settings::values.layout_option.GetValue();
            ImGui::AlignTextToFramePadding();
            mod_dot(cur != Settings::values.layout_option.GetDefault());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            ImGui::TextUnformatted("Layout");
            ImGui::PopStyleColor();
            ImGui::SameLine(380.0f);

            constexpr int kLayoutCount = static_cast<int>(std::size(kLayouts));
            const char* labels[kLayoutCount];
            int curIdx = 0;
            for (int i = 0; i < kLayoutCount; ++i) {
                labels[i] = kLayouts[i].label;
                if (kLayouts[i].opt == cur) curIdx = i;
            }
            const int hit = segmented_row(labels, kLayoutCount, curIdx, ImVec2(155, 55));
            if (hit >= 0) {
                Settings::values.layout_option = kLayouts[hit].opt;
                Core::System::GetInstance().ApplySettings();
                mImGuiLayer.ShowToast(std::string("Layout: ") + kLayouts[hit].label);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("How the two 3DS screens are arranged\nin the virtual display quad.");
        }

        // Swap screens: exchange which physical 3DS screen appears on top.
        if (match("Swap top/bottom screens")) {
            bool sw = Settings::values.swap_screen.GetValue();
            mod_dot(sw != Settings::values.swap_screen.GetDefault());
            if (ImGui::Checkbox("Swap top/bottom screens", &sw)) {
                Settings::values.swap_screen = sw;
                Core::System::GetInstance().ApplySettings();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Put the touch (bottom) screen in the top\nhalf of the display quad.");
        }

        ImGui::Spacing();
        } // end Display tab

        if (mCurrentTab == MenuTab::Library) {
        section_label("Library");
        (void)mOpenBrowserOnce;  // legacy; library is now its own tab
        mOpenBrowserOnce = false;

        // Kick off a background scan on first paint.
        if (!mRomBrowser.HasScanned() && !mRomBrowser.Root().empty() &&
            !mRomBrowser.IsScanning()) {
            mRomBrowser.RescanAsync();
        }

        // Header strip: currently loaded + root + refresh + status.
        if (!mRomBrowser.CurrentRom().empty()) {
            const auto& cur = mRomBrowser.CurrentRom();
            const auto slash = cur.find_last_of("/\\");
            const auto base = (slash == std::string::npos) ? cur
                                                           : cur.substr(slash + 1);
            const auto dot  = base.find_last_of('.');
            const std::string stem =
                (dot == std::string::npos) ? base : base.substr(0, dot);
            ImGui::TextWrapped("Currently loaded: %s", stem.c_str());
            ImGui::Spacing();
        }
        ImGui::TextWrapped("Root: %s",
                           mRomBrowser.Root().empty() ? "(unset - set CITRA_VR_ROM_DIR)"
                                                      : mRomBrowser.Root().c_str());
        const bool scanning = mRomBrowser.IsScanning();
        if (scanning) ImGui::BeginDisabled();
        if (ImGui::Button("Refresh", ImVec2(180, 50))) {
            mRomBrowser.RescanAsync();
            mSelectedRomIdx = -1; // clear stale selection on rescan
        }
        if (scanning) ImGui::EndDisabled();
        ImGui::SameLine();
        if (scanning) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.95f, 0.78f, 0.30f, 1.0f));
            ImGui::Text("Scanning...");
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("(%d found)", static_cast<int>(mRomBrowser.EntryCount()));
        }
        ImGui::Separator();

        // ---- Card grid ---------------------------------------------------
        // Tapping a card selects it (highlighted border). A single Launch
        // bar pinned to the bottom of the content area confirms the choice.
        // This two-step prevents accidental launches from laser pointer drift.
        const auto entries = mRomBrowser.EntriesSnapshot();

        // Clamp stale selection (list may have changed after a rescan).
        if (mSelectedRomIdx >= static_cast<int>(entries.size())) {
            mSelectedRomIdx = -1;
        }

        // ---- Search + sort header --------------------------------------
        // InputTextWithHint mirrors the Display tab's settings filter, plus
        // a small segmented sort control on the right (Name / Recent /
        // File). Sort choice persists across launches via VRConfig.
        if (!entries.empty()) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.18f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.16f, 0.20f, 0.28f, 1.0f));
            // Reserve room for the 3-button sort group on the right.
            const float sortGroupW = 360.0f;
            ImGui::PushItemWidth(-sortGroupW - 12.0f);
            ImGui::InputTextWithHint("##library_filter",
                                     "Search library...",
                                     mLibraryFilter,
                                     IM_ARRAYSIZE(mLibraryFilter));
            ImGui::PopItemWidth();
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            // Mini segmented control for sort. Reuses the look-and-feel of
            // the Display tab segmented_row (cyan active, slate inactive).
            const char* const kSortLabels[3] = {"Name", "Recent", "File"};
            constexpr ImVec4 kAccent  = ImVec4(0.30f, 0.72f, 0.95f, 1.00f);
            constexpr ImVec4 kAccentH = ImVec4(0.45f, 0.85f, 1.00f, 1.00f);
            constexpr ImVec4 kBgDark  = ImVec4(0.12f, 0.16f, 0.21f, 1.00f);
            constexpr ImVec4 kBgHover = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            for (int i = 0; i < 3; ++i) {
                const bool act = (mLibrarySort == i);
                ImGui::PushStyleColor(ImGuiCol_Button,        act ? kAccent  : kBgDark);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, act ? kAccentH : kBgHover);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      act ? ImVec4(0.04f, 0.07f, 0.10f, 1.0f)
                                          : ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
                ImGui::PushID(i);
                if (ImGui::Button(kSortLabels[i], ImVec2(110, 0))) {
                    if (mLibrarySort != i) {
                        mLibrarySort = i;
                        Common::VRConfig::SetInt("library_sort", i);
                    }
                }
                ImGui::PopID();
                ImGui::PopStyleColor(3);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleVar(2);
            ImGui::Spacing();
        }

        // Build a view (vector of indices) over the entries snapshot,
        // applying the active filter and sort. Cheap for 100-500 ROMs and
        // re-evaluated each frame so edits propagate instantly.
        std::vector<int> view;
        view.reserve(entries.size());
        {
            char needle[64];
            std::size_t ni = 0;
            for (; ni + 1 < sizeof(needle) && mLibraryFilter[ni]; ++ni) {
                needle[ni] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(mLibraryFilter[ni])));
            }
            needle[ni] = '\0';
            const bool hasFilter = ni > 0;
            auto match_lib = [&](const std::string& s) -> bool {
                if (!hasFilter) return true;
                for (std::size_t i = 0; i + ni <= s.size(); ++i) {
                    std::size_t k = 0;
                    for (; k < ni; ++k) {
                        if (std::tolower(static_cast<unsigned char>(s[i + k])) !=
                            static_cast<unsigned char>(needle[k]))
                            break;
                    }
                    if (k == ni) return true;
                }
                return false;
            };
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                const std::string& nm =
                    !e.long_title.empty() ? e.long_title : e.display_name;
                if (match_lib(nm) || match_lib(e.display_name)) {
                    view.push_back(static_cast<int>(i));
                }
            }
            // Pinned-first ordering: build a parallel pin lookup keyed
            // by the same hash used for last-played stamps, then bias
            // every comparator so pinned entries float to the top.
            auto is_pinned = [&](int idx) -> bool {
                const auto& e = entries[idx];
                const std::uint64_t key = (e.program_id != 0)
                    ? e.program_id
                    : std::hash<std::string>{}(e.full_path);
                char k[40];
                std::snprintf(k, sizeof(k), "pin_%016llX",
                              static_cast<unsigned long long>(key));
                return Common::VRConfig::GetBool(k, false);
            };
            if (mLibrarySort == 0) {
                std::sort(view.begin(), view.end(), [&](int a, int b) {
                    const bool pa = is_pinned(a), pb = is_pinned(b);
                    if (pa != pb) return pa;
                    const auto& ea = entries[a];
                    const auto& eb = entries[b];
                    const std::string& na = !ea.long_title.empty()
                                                ? ea.long_title : ea.display_name;
                    const std::string& nb = !eb.long_title.empty()
                                                ? eb.long_title : eb.display_name;
                    // Case-insensitive compare so "Zelda" sits with "zelda".
                    const std::size_t mn = std::min(na.size(), nb.size());
                    for (std::size_t i = 0; i < mn; ++i) {
                        const int ca = std::tolower(static_cast<unsigned char>(na[i]));
                        const int cb = std::tolower(static_cast<unsigned char>(nb[i]));
                        if (ca != cb) return ca < cb;
                    }
                    return na.size() < nb.size();
                });
            } else if (mLibrarySort == 1) {
                // Newest played first; un-played sorted to the end.
                std::sort(view.begin(), view.end(), [&](int a, int b) {
                    const bool pa = is_pinned(a), pb = is_pinned(b);
                    if (pa != pb) return pa;
                    auto ts = [&](int idx) -> long long {
                        const auto& e = entries[idx];
                        const std::uint64_t key = (e.program_id != 0)
                            ? e.program_id
                            : std::hash<std::string>{}(e.full_path);
                        char k[40];
                        std::snprintf(k, sizeof(k), "lp_%016llX",
                                      static_cast<unsigned long long>(key));
                        const std::string s = Common::VRConfig::Get(k);
                        return s.empty() ? 0LL : std::strtoll(s.c_str(), nullptr, 10);
                    };
                    return ts(a) > ts(b);
                });
            } else {
                std::sort(view.begin(), view.end(), [&](int a, int b) {
                    const bool pa = is_pinned(a), pb = is_pinned(b);
                    if (pa != pb) return pa;
                    return entries[a].display_name < entries[b].display_name;
                });
            }
        }

        // ---- Recently played strip --------------------------------------
        // Top 4 entries by persisted "lp_<key>" timestamp, rendered as a
        // single horizontal row of mini-cards (icon + dim caption). Tapping
        // one selects the matching grid entry so the existing Launch bar
        // confirms the choice -- same two-step gesture as the main grid.
        if (!entries.empty()) {
            struct Recent { int idx; long long ts; };
            std::vector<Recent> recents;
            recents.reserve(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                const std::uint64_t key = (e.program_id != 0)
                    ? e.program_id
                    : std::hash<std::string>{}(e.full_path);
                char k[40];
                std::snprintf(k, sizeof(k), "lp_%016llX",
                              static_cast<unsigned long long>(key));
                const std::string s = Common::VRConfig::Get(k);
                if (s.empty()) continue;
                const long long ts = std::strtoll(s.c_str(), nullptr, 10);
                if (ts > 0) recents.push_back({static_cast<int>(i), ts});
            }
            if (!recents.empty()) {
                std::sort(recents.begin(), recents.end(),
                          [](const Recent& a, const Recent& b) { return a.ts > b.ts; });
                if (recents.size() > 4) recents.resize(4);

                // Section heading using the existing label helper, then a
                // fixed-height child so the strip never steals vertical
                // space from the main grid below.
                section_label("Recently played");
                constexpr float kMiniW = 150.0f;
                constexpr float kMiniH = 130.0f;
                constexpr float kMiniIcon = 72.0f;
                ImGui::BeginChild("##recents", ImVec2(0, kMiniH + 16.0f), false,
                                  ImGuiWindowFlags_NoScrollbar);
                for (std::size_t r = 0; r < recents.size(); ++r) {
                    const int idx = recents[r].idx;
                    const auto& e = entries[idx];
                    if (r > 0) ImGui::SameLine();
                    ImGui::PushID(1000 + idx);
                    ImGui::BeginChild("##rmini", ImVec2(kMiniW, kMiniH), true,
                                      ImGuiWindowFlags_NoScrollbar);
                    const ImVec2 cMin = ImGui::GetWindowPos();
                    const ImVec2 cMax(cMin.x + ImGui::GetWindowWidth(),
                                      cMin.y + ImGui::GetWindowHeight());
                    const ImVec2 hit = ImGui::GetContentRegionAvail();
                    if (ImGui::InvisibleButton("##t", hit)) {
                        mSelectedRomIdx = idx;
                    }
                    const bool hov = ImGui::IsItemHovered();
                    {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        if (hov) {
                            dl->AddRectFilled(cMin, cMax,
                                ImGui::GetColorU32(ImVec4(1, 1, 1, 0.04f)));
                        }
                        // Always-on cyan top stripe so recents read as a
                        // distinct surface from the main grid (which uses
                        // a dim stripe when idle).
                        dl->AddRectFilled(cMin,
                            ImVec2(cMax.x, cMin.y + 3.0f),
                            ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.0f)),
                            2.0f);
                    }
                    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
                                               ImGui::GetStyle().WindowPadding.y));
                    const float innerW = ImGui::GetContentRegionAvail().x;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                         + (innerW - kMiniIcon) * 0.5f);
                    ImTextureID tex = nullptr;
                    if (!e.icon_rgba.empty() && e.icon_w > 0 && e.icon_h > 0) {
                        const std::uint64_t key = (e.program_id != 0)
                            ? e.program_id
                            : std::hash<std::string>{}(e.full_path);
                        tex = mImGuiLayer.Icons().GetOrUpload(
                            key, e.icon_w, e.icon_h, e.icon_rgba);
                    }
                    if (tex) ImGui::Image(tex, ImVec2(kMiniIcon, kMiniIcon));
                    else     ImGui::Dummy(ImVec2(kMiniIcon, kMiniIcon));

                    if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.85f, 0.89f, 0.93f, 1.0f));
                    const std::string& nm = !e.long_title.empty()
                                                ? e.long_title : e.display_name;
                    // Truncate title to fit one mini-card line.
                    std::string short_nm = nm;
                    if (short_nm.size() > 18) short_nm = short_nm.substr(0, 17) + "\xE2\x80\xA6";
                    ImGui::TextUnformatted(short_nm.c_str());
                    ImGui::PopStyleColor();
                    if (mImGuiLayer.FontCaption()) ImGui::PopFont();

                    ImGui::EndChild();
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::Spacing();
            }
        }

        const float availW = ImGui::GetContentRegionAvail().x;
        constexpr float kCardW  = 320.0f;
        constexpr float kCardH  = 240.0f;
        constexpr float kIconPx = 128.0f;
        constexpr float kLaunchBarH = 72.0f;  // bottom-pinned Launch bar
        int cols = std::max(1, static_cast<int>(availW / (kCardW + 12.0f)));

        // Grid scrollable area leaves room for the Launch bar.
        const float gridH = ImGui::GetContentRegionAvail().y
                            - (mSelectedRomIdx >= 0 ? kLaunchBarH + 8.0f : 0.0f);
        ImGui::BeginChild("rom_grid", ImVec2(0, gridH), false);
        // ---- Empty / scanning state ------------------------------------
        // When the grid is empty we either show a centred spinner (active
        // scan in progress) or a friendly setup hint. The spinner is drawn
        // with the window draw list -- 12 dots arranged on a circle, dot
        // brightness tied to (i - tick) so it appears to chase its tail.
        if (entries.empty()) {
            const float regionW = ImGui::GetContentRegionAvail().x;
            const float regionH = ImGui::GetContentRegionAvail().y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 centre(origin.x + regionW * 0.5f,
                                origin.y + regionH * 0.35f);
            if (scanning) {
                constexpr int kDots = 12;
                constexpr float kRadius = 36.0f;
                const float t = static_cast<float>(ImGui::GetTime());
                const int   lead = static_cast<int>(t * 12.0f) % kDots;
                for (int i = 0; i < kDots; ++i) {
                    const float a = (float(i) / kDots) * 6.2831853f;
                    const ImVec2 p(centre.x + std::cos(a) * kRadius,
                                   centre.y + std::sin(a) * kRadius);
                    const int dist = (i - lead + kDots) % kDots;
                    const float k = 1.0f - (float(dist) / kDots);
                    dl->AddCircleFilled(p, 5.0f,
                        ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, k)));
                }
                const char* msg = "Scanning library\xE2\x80\xA6";
                ImFont* fc = mImGuiLayer.FontCaption();
                const float fsz = fc ? fc->FontSize : ImGui::GetFontSize();
                const ImVec2 ts = fc
                    ? fc->CalcTextSizeA(fsz, FLT_MAX, 0.0f, msg)
                    : ImGui::CalcTextSize(msg);
                const ImVec2 tp(centre.x - ts.x * 0.5f,
                                centre.y + kRadius + 18.0f);
                const ImU32 tc = ImGui::GetColorU32(
                    ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
                if (fc) dl->AddText(fc, fsz, tp, tc, msg);
                else    dl->AddText(tp, tc, msg);
            } else {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
                ImGui::TextWrapped("No ROMs found. Set CITRA_VR_ROM_DIR to a folder "
                                   "containing .3ds / .cci / .cxi / .app files, "
                                   "then press Refresh.");
                ImGui::PopStyleColor();
            }
        } else if (view.empty()) {
            // Library has ROMs but the active filter excluded them all.
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            ImGui::TextWrapped("No matches for \"%s\". Adjust the search "
                               "or clear it to see every ROM.",
                               mLibraryFilter);
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginTable("##cards", cols,
                              ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoHostExtendX)) {
            // Iterate the filtered/sorted view rather than the raw
            // snapshot. `i` is the original entries[] index used as the
            // selection identifier, so the bottom Launch bar still works.
            for (std::size_t v = 0; v < view.size(); ++v) {
                const std::size_t i = static_cast<std::size_t>(view[v]);
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                const auto& e = entries[i];
                const bool selected = (static_cast<int>(i) == mSelectedRomIdx);

                // Draw selection border behind the child window by pushing
                // the border colour before BeginChild. Hover gets a softer
                // teal border (1 px) so cards feel responsive without
                // jumping size when you sweep the laser pointer over them.
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Border,
                                          ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 3.0f);
                }
                ImGui::BeginChild("##card", ImVec2(0, kCardH), true,
                                  ImGuiWindowFlags_NoScrollbar);

                // Cache the card-rect bounds for hover-driven decoration
                // (top accent bar, drop-shadow). ImGui's hover query for
                // a child window must be against the InvisibleButton tap
                // hit-area below, not this scope.
                const ImVec2 cardMin = ImGui::GetWindowPos();
                const ImVec2 cardMax = ImVec2(cardMin.x + ImGui::GetWindowWidth(),
                                              cardMin.y + ImGui::GetWindowHeight());

                // Selected-state tint overlay drawn first (behind content).
                if (selected) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(cardMin, cardMax,
                                      ImGui::GetColorU32(
                                          ImVec4(0.30f, 0.72f, 0.95f, 0.10f)));
                }

                // Invisible button covering the whole card to capture taps.
                const ImVec2 cardSize = ImGui::GetContentRegionAvail();
                if (ImGui::InvisibleButton("##tap", cardSize)) {
                    mSelectedRomIdx = selected ? -1 : static_cast<int>(i);
                }
                const bool hovered = ImGui::IsItemHovered();

                // Hover: subtle white wash + a 4 px accent strip across
                // the top edge. Selected cards keep the strip lit even
                // without hover so the chosen item is obvious at a glance.
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (hovered && !selected) {
                        dl->AddRectFilled(cardMin, cardMax,
                                          ImGui::GetColorU32(
                                              ImVec4(1.0f, 1.0f, 1.0f, 0.04f)));
                    }
                    constexpr float kStripeH = 4.0f;
                    const ImVec4 stripeCol = (selected || hovered)
                        ? ImVec4(0.30f, 0.72f, 0.95f, 1.00f)
                        : ImVec4(0.20f, 0.26f, 0.33f, 0.65f);
                    dl->AddRectFilled(
                        cardMin,
                        ImVec2(cardMax.x, cardMin.y + kStripeH),
                        ImGui::GetColorU32(stripeCol),
                        2.0f);
                }

                // Rewind cursor so the icon/title render on top of the hit area.
                ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
                                           ImGui::GetStyle().WindowPadding.y));

                // Centred icon.
                const float innerW = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     (innerW - kIconPx) * 0.5f);
                ImTextureID tex = nullptr;
                if (!e.icon_rgba.empty() && e.icon_w > 0 && e.icon_h > 0) {
                    const std::uint64_t key = (e.program_id != 0)
                        ? e.program_id
                        : std::hash<std::string>{}(e.full_path);
                    tex = mImGuiLayer.Icons().GetOrUpload(
                        key, e.icon_w, e.icon_h, e.icon_rgba);
                }
                if (tex) {
                    ImGui::Image(tex, ImVec2(kIconPx, kIconPx));
                } else {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(p,
                                      ImVec2(p.x + kIconPx, p.y + kIconPx),
                                      ImGui::GetColorU32(
                                          ImVec4(0.18f, 0.22f, 0.28f, 1.0f)),
                                      8.0f);
                    dl->AddRect(p,
                                ImVec2(p.x + kIconPx, p.y + kIconPx),
                                ImGui::GetColorU32(
                                    ImVec4(0.30f, 0.72f, 0.95f, 0.7f)),
                                8.0f, 0, 2.0f);
                    ImGui::Dummy(ImVec2(kIconPx, kIconPx));
                }

                // Title.
                ImGui::Spacing();
                const std::string& title = !e.long_title.empty()
                                               ? e.long_title
                                               : e.display_name;
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(title.c_str());
                ImGui::PopTextWrapPos();

                // ---- Metadata caption -------------------------------
                // One dim line under the title with the file extension
                // (uppercase), trailing 8 hex of the 3DS title-id (when
                // available), and a relative "last played" stamp pulled
                // from VRConfig (recorded at launch, see Launch button).
                {
                    const auto& fp  = e.full_path;
                    const auto  dot = fp.find_last_of('.');
                    std::string ext;
                    if (dot != std::string::npos && dot + 1 < fp.size()) {
                        ext = fp.substr(dot + 1);
                        for (auto& c : ext)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    }

                    // Read persisted last-played timestamp and turn it
                    // into a friendly relative string ("2h ago", "3d ago").
                    char lpKey[40];
                    const std::uint64_t lpHash = (e.program_id != 0)
                        ? e.program_id
                        : std::hash<std::string>{}(e.full_path);
                    std::snprintf(lpKey, sizeof(lpKey), "lp_%016llX",
                                  static_cast<unsigned long long>(lpHash));
                    const std::string lpStr = Common::VRConfig::Get(lpKey);
                    char lpRel[24] = {0};
                    if (!lpStr.empty()) {
                        const long long then = std::strtoll(lpStr.c_str(), nullptr, 10);
                        const long long now  = static_cast<long long>(std::time(nullptr));
                        const long long ago  = (now > then) ? (now - then) : 0;
                        if      (ago < 60)        std::snprintf(lpRel, sizeof(lpRel), "just now");
                        else if (ago < 3600)      std::snprintf(lpRel, sizeof(lpRel), "%lldm ago", ago / 60);
                        else if (ago < 86400)     std::snprintf(lpRel, sizeof(lpRel), "%lldh ago", ago / 3600);
                        else if (ago < 86400*30)  std::snprintf(lpRel, sizeof(lpRel), "%lldd ago", ago / 86400);
                        else                      std::snprintf(lpRel, sizeof(lpRel), "%lldmo ago", ago / (86400*30));
                    }

                    char meta[96];
                    const char* extStr = ext.empty() ? "ROM" : ext.c_str();
                    if (e.program_id != 0 && lpRel[0]) {
                        std::snprintf(meta, sizeof(meta),
                                      "%s  \xC2\xB7  ID %08X  \xC2\xB7  %s",
                                      extStr,
                                      static_cast<unsigned>(e.program_id & 0xFFFFFFFFu),
                                      lpRel);
                    } else if (e.program_id != 0) {
                        std::snprintf(meta, sizeof(meta), "%s  \xC2\xB7  ID %08X",
                                      extStr,
                                      static_cast<unsigned>(e.program_id & 0xFFFFFFFFu));
                    } else if (lpRel[0]) {
                        std::snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %s",
                                      extStr, lpRel);
                    } else {
                        std::snprintf(meta, sizeof(meta), "%s", extStr);
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
                    if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                    ImGui::TextUnformatted(meta);
                    if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                    ImGui::PopStyleColor();
                }

                // ---- Pin/star toggle (top-right corner) -------------
                // Submitted AFTER the big ##tap InvisibleButton so its
                // small hit-rect claims hover priority within the card.
                // Pinned ROMs always sort to the top regardless of mode.
                {
                    char pinKey[40];
                    const std::uint64_t pinHash = (e.program_id != 0)
                        ? e.program_id
                        : std::hash<std::string>{}(e.full_path);
                    std::snprintf(pinKey, sizeof(pinKey), "pin_%016llX",
                                  static_cast<unsigned long long>(pinHash));
                    const bool pinned = Common::VRConfig::GetBool(pinKey, false);

                    constexpr float kStarSz = 36.0f;
                    const ImVec2 starPos(cardMax.x - kStarSz - 6.0f,
                                         cardMin.y + 6.0f);
                    ImGui::SetCursorScreenPos(starPos);
                    if (ImGui::InvisibleButton("##pin",
                                               ImVec2(kStarSz, kStarSz))) {
                        Common::VRConfig::SetBool(pinKey, !pinned);
                    }
                    const bool pinHov = ImGui::IsItemHovered();
                    if (pinHov) VrTip(pinned ? "Unpin from top"
                                             : "Pin to top of library");

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 c(starPos.x + kStarSz * 0.5f,
                                   starPos.y + kStarSz * 0.5f);
                    const ImVec4 starCol = pinned
                        ? ImVec4(1.00f, 0.82f, 0.30f, 1.00f)
                        : (pinHov ? ImVec4(0.92f, 0.95f, 0.99f, 0.90f)
                                  : ImVec4(0.55f, 0.62f, 0.70f, 0.55f));
                    const ImU32 col = ImGui::GetColorU32(starCol);

                    // 5-point star: 10 perimeter points alternating
                    // outer/inner radius, fan-triangulated from centre.
                    ImVec2 pts[10];
                    constexpr float kRout = 13.0f, kRin = 5.5f;
                    for (int k = 0; k < 10; ++k) {
                        const float a = -1.5707963f +
                                        k * (3.1415926f / 5.0f);
                        const float r = (k & 1) ? kRin : kRout;
                        pts[k] = ImVec2(c.x + std::cos(a) * r,
                                        c.y + std::sin(a) * r);
                    }
                    for (int k = 0; k < 10; ++k) {
                        dl->AddTriangleFilled(c, pts[k], pts[(k + 1) % 10], col);
                    }
                }

                ImGui::EndChild();
                if (selected) {
                    ImGui::PopStyleVar();   // ChildBorderSize
                    ImGui::PopStyleColor(); // Border
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild(); // rom_grid

        // ---- Bottom-pinned Launch bar ------------------------------------
        // Only shown when a card is selected.
        if (mSelectedRomIdx >= 0 &&
            mSelectedRomIdx < static_cast<int>(entries.size())) {
            const auto& sel = entries[static_cast<std::size_t>(mSelectedRomIdx)];
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.45f, 0.85f, 1.00f, 1.0f));
            const std::string launchLabel =
                std::string("Launch  ") +
                (!sel.long_title.empty() ? sel.long_title : sel.display_name);
            if (ImGui::Button(launchLabel.c_str(),
                              ImVec2(-FLT_MIN, kLaunchBarH))) {
                if (mOnLoadRom) {
                    ALOGI("ROM browser selected: %s", sel.full_path.c_str());
                    // Persist the launch timestamp so the card can show
                    // a "Last played" caption next time the menu opens.
                    // Keyed on title-id (preferred, stable across paths)
                    // or path hash as a fallback.
                    {
                        const std::uint64_t key = (sel.program_id != 0)
                            ? sel.program_id
                            : std::hash<std::string>{}(sel.full_path);
                        char k[40];
                        std::snprintf(k, sizeof(k), "lp_%016llX",
                                      static_cast<unsigned long long>(key));
                        Common::VRConfig::Set(
                            k, std::to_string(static_cast<long long>(std::time(nullptr))));
                    }
                    mOnLoadRom(sel.full_path);
                    mImGuiLayer.SetVisible(false);
                }
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::Spacing();
        } // end Library tab

        if (mCurrentTab == MenuTab::Saves) {
        section_label("Save states");

        // ---- Save state slots --------------------------------------------
        // Save runs in-process. Load is routed through the launcher
        // (sentinel-restart) because in-process LoadState destroys the
        // Vulkan renderer mid-XR-session and dangles every cached
        // VkDevice/VkImage handle the VR layer holds. The launcher .bat
        // re-launches the exe with --loadslot N, and citra_vr applies
        // the LoadState BEFORE binding the XR session.

        // Build a slot -> (timestamp, valid) lookup so each row can show
        // the file mtime instead of a bare slot number. Cached for ~2s
        // because ListSaveStates hits disk for every slot.
        struct SlotInfo { bool exists; bool revision_match; u64 time; };
        static thread_local SlotInfo s_slotInfo[Core::SaveStateSlotCount + 1] = {};
        static thread_local long long s_slotInfoNextRefreshNs = 0;
        const auto now_save_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_save_ns >= s_slotInfoNextRefreshNs) {
            for (auto& si : s_slotInfo) si = {};
            auto& sysSS = Core::System::GetInstance();
            if (sysSS.IsPoweredOn()) {
                u64 title_id = 0;
                if (sysSS.GetAppLoader().ReadProgramId(title_id) == Loader::ResultStatus::Success) {
                    const u64 movie_id = sysSS.Movie().GetCurrentMovieID();
                    for (const auto& info : Core::ListSaveStates(title_id, movie_id)) {
                        if (info.slot < Core::SaveStateSlotCount + 1) {
                            s_slotInfo[info.slot] = {
                                true,
                                info.status == Core::SaveStateInfo::ValidationStatus::OK,
                                info.time};
                        }
                    }
                }
            }
            s_slotInfoNextRefreshNs = now_save_ns + 2'000'000'000LL;
        }

        // ---- Empty-state guard -------------------------------------------
        // When no game is running, save/load buttons would all be dead
        // weight. Show a friendly hint pointing the user back to Library.
        const bool gameRunning = Core::System::GetInstance().IsPoweredOn();
        if (!gameRunning) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            ImGui::TextWrapped(
                "No game is currently running. Launch a ROM from the "
                "Library tab to enable save states.");
            ImGui::PopStyleColor();
        }

        // Last-saved slot is highlighted with a cyan accent stripe so
        // returning users instantly see where to load from.
        const int lastSlot = Common::VRConfig::GetInt("last_slot", 0);

        for (u32 slot = 1; slot <= 9; ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            const SlotInfo& si = s_slotInfo[slot];
            const bool isLast  = (static_cast<int>(slot) == lastSlot && si.exists);

            // ---- Slot card (single row, ~64 px tall) ---------------------
            // Layout: [#N badge] [status / timestamp] [Save] [Load].
            // Drawn as a child window so we can paint the background +
            // accent stripe without affecting later widgets.
            constexpr float kRowH = 64.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  si.exists ? ImVec4(0.10f, 0.13f, 0.18f, 1.0f)
                                            : ImVec4(0.07f, 0.10f, 0.14f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::BeginChild("##slot_row", ImVec2(0, kRowH), true,
                              ImGuiWindowFlags_NoScrollbar);

            // Background decorations (accent stripe + cyan tint for the
            // most recently saved slot).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 rMin = ImGui::GetWindowPos();
            const ImVec2 rMax(rMin.x + ImGui::GetWindowWidth(),
                              rMin.y + ImGui::GetWindowHeight());
            const ImVec4 stripeCol =
                isLast        ? ImVec4(0.30f, 0.72f, 0.95f, 1.0f) :   // cyan
                !si.exists    ? ImVec4(0.20f, 0.26f, 0.33f, 0.65f) :  // dim slate
                !si.revision_match ? ImVec4(1.00f, 0.72f, 0.30f, 1.0f) // amber warn
                                   : ImVec4(0.30f, 0.66f, 0.45f, 1.0f); // green
            dl->AddRectFilled(rMin,
                              ImVec2(rMin.x + 4.0f, rMax.y),
                              ImGui::GetColorU32(stripeCol));
            if (isLast) {
                dl->AddRectFilled(rMin, rMax,
                                  ImGui::GetColorU32(
                                      ImVec4(0.30f, 0.72f, 0.95f, 0.06f)));
            }

            // Slot number badge.
            ImGui::SetCursorPos(ImVec2(18.0f, (kRowH - ImGui::GetFontSize()) * 0.5f - 12.0f));
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  si.exists ? ImVec4(0.92f, 0.95f, 0.99f, 1.0f)
                                            : ImVec4(0.40f, 0.46f, 0.54f, 1.0f));
            ImGui::Text("%u", slot);
            ImGui::PopStyleColor();
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();

            // Status / timestamp text.
            ImGui::SameLine(80.0f);
            ImGui::SetCursorPosY((kRowH - ImGui::GetFontSize()) * 0.5f - 4.0f);
            if (si.exists) {
                std::time_t t = static_cast<std::time_t>(si.time);
                std::tm tm{};
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                char ts[32];
                std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d  %02d:%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.92f, 0.95f, 0.99f, 1.0f));
                ImGui::TextUnformatted(ts);
                ImGui::PopStyleColor();
                if (!si.revision_match || isLast) {
                    ImGui::SameLine();
                    if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                    if (!si.revision_match) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
                        ImGui::TextUnformatted("  \xE2\x80\xA2  other build");
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
                        ImGui::TextUnformatted("  \xE2\x80\xA2  last saved");
                        ImGui::PopStyleColor();
                    }
                    if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.60f, 0.66f, 1.0f));
                ImGui::TextUnformatted("Empty");
                ImGui::PopStyleColor();
            }

            // Save / Load buttons pinned to the right edge.
            constexpr float kBtnW = 120.0f, kBtnH = 44.0f;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float btnY = (kRowH - kBtnH) * 0.5f - 4.0f;
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetWindowWidth() - (kBtnW * 2.0f + gap) - 12.0f, btnY));

            if (!gameRunning) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.15f, 0.55f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.30f, 0.85f, 0.50f, 1.0f));
            if (ImGui::Button("Save", ImVec2(kBtnW, kBtnH))) {
                Core::System::GetInstance().SendSignal(Core::System::Signal::Save, slot);
                mImGuiLayer.SetVisible(false);
                ALOGI("Save state slot %u requested", slot);
                Common::VRConfig::SetInt("last_slot", static_cast<int>(slot));
                s_slotInfoNextRefreshNs = 0;
                const auto now_t2 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                mController.TriggerHaptic(Hand::Left,  Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
                mController.TriggerHaptic(Hand::Right, Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
                mHapticSuccessT2Ns[0] = now_t2 + Haptic::kSuccessGapNs;
                mHapticSuccessT2Ns[1] = now_t2 + Haptic::kSuccessGapNs;
            }
            ImGui::PopStyleColor(3);
            if (!gameRunning) ImGui::EndDisabled();

            ImGui::SameLine();
            const bool load_enabled = static_cast<bool>(mOnLoadState) && si.exists;
            if (!load_enabled) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.75f, 0.45f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.92f, 0.58f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(1.00f, 0.70f, 0.28f, 1.0f));
            if (ImGui::Button("Load", ImVec2(kBtnW, kBtnH)) && load_enabled) {
                ALOGI("Load state slot %u requested (relaunch)", slot);
                mOnLoadState(static_cast<int>(slot));
                mImGuiLayer.SetVisible(false);
            }
            ImGui::PopStyleColor(3);
            if (!load_enabled) ImGui::EndDisabled();

            ImGui::EndChild();
            ImGui::PopStyleVar();   // ChildRounding
            ImGui::PopStyleColor(); // ChildBg
            ImGui::PopID();
        }

        ImGui::Spacing();
        } // end Saves tab

        if (mCurrentTab == MenuTab::Input) {
        section_label("Input");

        // ---- Wrist bar position ----------------------------------------
        // Drag-to-place: enable the toggle, point with the right hand,
        // pull and release the right trigger to commit. Right A cancels.
        ImGui::SeparatorText("Wrist Bar Position");
        if (mWristDragMode) {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
                               "Drag mode: point right hand at desired spot,");
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.30f, 1.0f),
                               "press + release Right Trigger to save.");
            ImGui::Text("Right A button cancels.");
            ImGui::Text("Live offset (left-grip-local): X=%.3f  Y=%.3f  Z=%.3f m",
                        mWristDragOffX, mWristDragOffY, mWristDragOffZ);
            if (ImGui::Button("Cancel##wrist_drag", ImVec2(180, 56))) {
                mWristDragMode = false;
                ShowToast("Wrist bar position reverted", 1.2f);
            }
        } else {
            ImGui::Text("Current offset (left-grip-local): X=%.3f  Y=%.3f  Z=%.3f m",
                        VRSettings::values.wrist_offset_x,
                        VRSettings::values.wrist_offset_y,
                        VRSettings::values.wrist_offset_z);
            ImGui::Text("Tilt: %.0f deg", VRSettings::values.wrist_tilt_deg);
            if (ImGui::Button("Reposition Wrist Bar", ImVec2(260, 64))) {
                mWristDragMode      = true;
                mWristDragOffX      = VRSettings::values.wrist_offset_x;
                mWristDragOffY      = VRSettings::values.wrist_offset_y;
                mWristDragOffZ      = VRSettings::values.wrist_offset_z;
                mDragCancelLast     = false;
                mDragCommitLast     = false;
                ShowToast("Point with right hand, pull trigger to place", 3.0f);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Live drag the wrist button bar with your right hand.\n"
                                  "Right trigger press+release saves; Right A cancels.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Default##wrist", ImVec2(180, 64))) {
                VRSettings::values.wrist_offset_x = 0.00f;
                VRSettings::values.wrist_offset_y = 0.02f;
                VRSettings::values.wrist_offset_z = 0.08f;
                VRSettings::values.wrist_tilt_deg = -45.0f;
                Common::VRConfig::Set("wrist_offset_x", "0.000000");
                Common::VRConfig::Set("wrist_offset_y", "0.020000");
                Common::VRConfig::Set("wrist_offset_z", "0.080000");
                Common::VRConfig::Set("wrist_tilt_deg", "-45.000000");
                ShowToast("Wrist bar reset to default", 1.2f);
            }
            // Tilt slider (always live-editable).
            float tilt = VRSettings::values.wrist_tilt_deg;
            ImGui::PushItemWidth(280);
            if (ImGui::SliderFloat("Tilt##wrist_tilt", &tilt, -90.0f, 0.0f, "%.0f deg")) {
                VRSettings::values.wrist_tilt_deg = tilt;
                Common::VRConfig::Set("wrist_tilt_deg", std::to_string(tilt));
            }
            ImGui::PopItemWidth();
        }
        ImGui::Spacing();

        // ---- Controller visibility -------------------------------------
        ImGui::SeparatorText("Controllers");
        bool showCtrl = mShowControllers;
        if (ImGui::Checkbox("Show controller markers in game", &showCtrl)) {
            mShowControllers = showCtrl;
            Common::VRConfig::SetBool("show_controllers", showCtrl);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Show small floating markers at each hand pose so\n"
                              "you can see where your physical controllers are\n"
                              "while a game is rendering on the virtual screen.");
        }
        ImGui::Spacing();

        if (mInputBridge != nullptr) {
            using Bridge = vr_pcvr::VrInputBridge;
            // Combo entries (display label + Src). Listed in physical
            // groups so the popup is easy to scan in VR. Hand prefix is
            // separated visually so the popup sorts by side at a glance.
            struct Entry { Bridge::Src s; const char* label; };
            static const Entry kEntries[] = {
                {Bridge::Src::None,      "(none)"},
                {Bridge::Src::L_A,       "Left A"},
                {Bridge::Src::L_B,       "Left B"},
                {Bridge::Src::R_A,       "Right A"},
                {Bridge::Src::R_B,       "Right B"},
                {Bridge::Src::L_Trigger, "Left Trigger"},
                {Bridge::Src::R_Trigger, "Right Trigger"},
                {Bridge::Src::L_Grip,    "Left Grip"},
                {Bridge::Src::R_Grip,    "Right Grip"},
                {Bridge::Src::L_Thumb,   "Left Thumb Click"},
                {Bridge::Src::R_Thumb,   "Right Thumb Click"},
                {Bridge::Src::L_System,  "Left System"},
                {Bridge::Src::R_System,  "Right System"},
            };
            auto entry_label = [](Bridge::Src s) -> const char* {
                for (const auto& e : kEntries) if (e.s == s) return e.label;
                return "(none)";
            };
            // Compact glyph for the source-side chip on each row.
            auto entry_short = [](Bridge::Src s) -> const char* {
                switch (s) {
                    case Bridge::Src::None:      return "\xE2\x80\x94"; // em dash
                    case Bridge::Src::L_A:       return "L\xC2\xB7""A";
                    case Bridge::Src::L_B:       return "L\xC2\xB7""B";
                    case Bridge::Src::R_A:       return "R\xC2\xB7""A";
                    case Bridge::Src::R_B:       return "R\xC2\xB7""B";
                    case Bridge::Src::L_Trigger: return "L\xC2\xB7TRG";
                    case Bridge::Src::R_Trigger: return "R\xC2\xB7TRG";
                    case Bridge::Src::L_Grip:    return "L\xC2\xB7GRP";
                    case Bridge::Src::R_Grip:    return "R\xC2\xB7GRP";
                    case Bridge::Src::L_Thumb:   return "L\xC2\xB7THM";
                    case Bridge::Src::R_Thumb:   return "R\xC2\xB7THM";
                    case Bridge::Src::L_System:  return "L\xC2\xB7SYS";
                    case Bridge::Src::R_System:  return "R\xC2\xB7SYS";
                    default:                     return "?";
                }
            };

            struct Row { Bridge::VrButtonId id; const char* label; };
            static const Row kFace[] = {
                {Bridge::Btn_A, "3DS A"},
                {Bridge::Btn_B, "3DS B"},
                {Bridge::Btn_X, "3DS X"},
                {Bridge::Btn_Y, "3DS Y"},
            };
            static const Row kShoulder[] = {
                {Bridge::Btn_L,  "3DS L"},
                {Bridge::Btn_R,  "3DS R"},
                {Bridge::Btn_ZL, "3DS ZL"},
                {Bridge::Btn_ZR, "3DS ZR"},
            };
            static const Row kSystem[] = {
                {Bridge::Btn_Start,  "3DS Start"},
                {Bridge::Btn_Select, "3DS Select"},
            };

            bool changed = false;

            // Reusable card + combo renderer. Each row is its own rounded
            // child window with the source label rendered as a coloured
            // chip on the right (cyan when bound, dim slate when "(none)").
            auto bind_row = [&](const Row& row) {
                ImGui::PushID(row.label);
                constexpr float kRowH = 56.0f;
                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                      ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                ImGui::BeginChild("##bind_row", ImVec2(0, kRowH), true,
                                  ImGuiWindowFlags_NoScrollbar);

                const Bridge::Src cur = mInputBridge->GetBinding(row.id);
                const bool isDefault  = (cur == Bridge::DefaultBinding(row.id));
                const bool isBound    = (cur != Bridge::Src::None);

                // Left-edge accent stripe: cyan when remapped, dim when
                // at default. Mirrors the Saves slot accent system.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 rMin = ImGui::GetWindowPos();
                const ImVec2 rMax(rMin.x + ImGui::GetWindowWidth(),
                                  rMin.y + ImGui::GetWindowHeight());
                const ImVec4 stripeCol = !isBound
                    ? ImVec4(0.55f, 0.20f, 0.20f, 1.0f)              // unbound red
                    : (isDefault ? ImVec4(0.20f, 0.26f, 0.33f, 0.85f)
                                 : ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
                dl->AddRectFilled(rMin,
                                  ImVec2(rMin.x + 4.0f, rMax.y),
                                  ImGui::GetColorU32(stripeCol));

                // 3DS button label.
                ImGui::SetCursorPos(ImVec2(18.0f,
                                           (kRowH - ImGui::GetFontSize()) * 0.5f - 4.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.92f, 0.95f, 0.99f, 1.0f));
                ImGui::TextUnformatted(row.label);
                ImGui::PopStyleColor();

                // Right-side combo (320 px) + chip preview overlap.
                constexpr float kComboW = 320.0f;
                ImGui::SetCursorPos(ImVec2(
                    ImGui::GetWindowWidth() - kComboW - 14.0f,
                    (kRowH - ImGui::GetFrameHeight()) * 0.5f - 4.0f));
                ImGui::SetNextItemWidth(kComboW);
                ImGui::PushStyleColor(ImGuiCol_FrameBg,
                                      ImVec4(0.13f, 0.16f, 0.21f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                                      ImVec4(0.17f, 0.21f, 0.27f, 1.0f));
                if (ImGui::BeginCombo("##bind", entry_label(cur))) {
                    for (const auto& e : kEntries) {
                        const bool sel = (e.s == cur);
                        if (ImGui::Selectable(e.label, sel)) {
                            mInputBridge->SetBinding(row.id, e.s);
                            changed = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopStyleColor(2);

                // Tiny chip on the far left side showing the short label
                // (helps eyes parse the row at-a-glance from across the
                // VR room without reading the full combo string).
                {
                    if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                    const char* shortLabel = entry_short(cur);
                    const ImVec2 ts = ImGui::CalcTextSize(shortLabel);
                    const ImVec2 chipPos(180.0f,
                                         (kRowH - ts.y) * 0.5f - 8.0f);
                    const ImVec2 chipMin(rMin.x + chipPos.x - 8.0f,
                                         rMin.y + chipPos.y - 4.0f);
                    const ImVec2 chipMax(chipMin.x + ts.x + 16.0f,
                                         chipMin.y + ts.y + 8.0f);
                    const ImVec4 chipCol = !isBound
                        ? ImVec4(0.18f, 0.10f, 0.10f, 1.0f)
                        : (isDefault ? ImVec4(0.16f, 0.20f, 0.26f, 1.0f)
                                     : ImVec4(0.10f, 0.28f, 0.40f, 1.0f));
                    dl->AddRectFilled(chipMin, chipMax,
                                      ImGui::GetColorU32(chipCol), 4.0f);
                    dl->AddRect(chipMin, chipMax,
                                ImGui::GetColorU32(
                                    isBound && !isDefault
                                        ? ImVec4(0.30f, 0.72f, 0.95f, 0.85f)
                                        : ImVec4(0.30f, 0.36f, 0.43f, 0.55f)),
                                4.0f, 0, 1.0f);
                    const ImU32 chipText = ImGui::GetColorU32(
                        !isBound ? ImVec4(0.92f, 0.55f, 0.55f, 1.0f)
                                 : ImVec4(0.92f, 0.95f, 0.99f, 1.0f));
                    dl->AddText(ImVec2(rMin.x + chipPos.x,
                                       rMin.y + chipPos.y),
                                chipText, shortLabel);
                    if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                }

                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::PopID();
            };

            auto group = [&](const char* title, const Row* rows, std::size_t n) {
                if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                ImGui::Spacing();
                for (std::size_t i = 0; i < n; ++i) bind_row(rows[i]);
                ImGui::Spacing();
            };

            group("Face buttons",     kFace,     IM_ARRAYSIZE(kFace));
            group("Shoulder buttons", kShoulder, IM_ARRAYSIZE(kShoulder));
            group("System buttons",   kSystem,   IM_ARRAYSIZE(kSystem));

            ImGui::Spacing();
            // ---- Reset (with confirm) -----------------------------------
            // Confirmation prevents an accidental laser-pointer tap from
            // wiping a carefully tuned binding set.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.26f, 0.32f, 0.40f, 1.0f));
            if (ImGui::Button("Reset to defaults", ImVec2(360, 60))) {
                ImGui::OpenPopup("##reset_bind_confirm");
            }
            ImGui::PopStyleColor(2);
            ImGui::SetNextWindowSize(ImVec2(520, 240), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("##reset_bind_confirm", nullptr,
                                       ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize   |
                                       ImGuiWindowFlags_NoMove)) {
                if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
                ImGui::TextUnformatted("Reset bindings?");
                if (mImGuiLayer.FontDisplay()) ImGui::PopFont();
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
                ImGui::TextWrapped("Restores every face / shoulder / system "
                                   "button to its default mapping.");
                ImGui::PopStyleColor();
                ImGui::Spacing();
                const float bw = (ImGui::GetContentRegionAvail().x -
                                  ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::Button("Cancel", ImVec2(bw, 60))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(0.82f, 0.28f, 0.28f, 1.0f));
                if (ImGui::Button("Reset", ImVec2(bw, 60))) {
                    for (const auto& r : kFace)     mInputBridge->SetBinding(r.id, Bridge::DefaultBinding(r.id));
                    for (const auto& r : kShoulder) mInputBridge->SetBinding(r.id, Bridge::DefaultBinding(r.id));
                    for (const auto& r : kSystem)   mInputBridge->SetBinding(r.id, Bridge::DefaultBinding(r.id));
                    mInputBridge->SaveBindings();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor(2);
                ImGui::EndPopup();
            }

            if (changed) {
                mInputBridge->SaveBindings();
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
            ImGui::TextWrapped("D-pad and analog sticks are fixed. "
                               "Wrist Start/Select buttons always work.");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        } // end Input tab

        if (mCurrentTab == MenuTab::Session) {
        section_label("Session");

        // ---- Now-playing hero card --------------------------------------
        // When a game is running, surface its title + program ID + last
        // saved slot in a tinted hero panel. Mirrors the Library hero
        // gradient so the user instantly recognises this as the active
        // game's session controls. When no game is running, show a dim
        // "Idle" panel pointing back to the Library tab.
        {
            auto& sysS  = Core::System::GetInstance();
            const bool running = sysS.IsPoweredOn();
            std::string title;
            u64 pid = 0;
            if (running) {
                sysS.GetAppLoader().ReadTitle(title);
                sysS.GetAppLoader().ReadProgramId(pid);
            }

            constexpr float kHeroH = 110.0f;
            ImDrawList* dl    = ImGui::GetWindowDrawList();
            const ImVec2 topL = ImGui::GetCursorScreenPos();
            const float  hw   = ImGui::GetContentRegionAvail().x;
            const ImVec2 br(topL.x + hw, topL.y + kHeroH);
            if (running) {
                const ImU32 cCyan   = ImGui::GetColorU32(ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
                const ImU32 cIndigo = ImGui::GetColorU32(ImVec4(0.42f, 0.36f, 0.92f, 1.0f));
                dl->AddRectFilledMultiColor(topL, br, cCyan, cIndigo, cIndigo, cCyan);
            } else {
                dl->AddRectFilled(topL, br,
                                  ImGui::GetColorU32(ImVec4(0.10f, 0.13f, 0.18f, 1.0f)),
                                  4.0f);
            }
            dl->AddRect(topL, br,
                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)),
                        4.0f, 0, 1.5f);

            // Caption row ("NOW PLAYING" or "IDLE").
            ImGui::SetCursorScreenPos(ImVec2(topL.x + 24.0f, topL.y + 16.0f));
            if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  running ? ImVec4(0.92f, 0.95f, 1.00f, 0.85f)
                                          : ImVec4(0.55f, 0.61f, 0.70f, 1.00f));
            ImGui::TextUnformatted(running ? "NOW PLAYING" : "IDLE");
            ImGui::PopStyleColor();
            if (mImGuiLayer.FontCaption()) ImGui::PopFont();

            // Title row.
            ImGui::SetCursorScreenPos(ImVec2(topL.x + 24.0f, topL.y + 40.0f));
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.98f, 0.99f, 1.00f, 1.00f));
            if (running) {
                ImGui::TextUnformatted(!title.empty() ? title.c_str()
                                                     : "Untitled ROM");
            } else {
                ImGui::TextUnformatted("No game running");
            }
            ImGui::PopStyleColor();
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();

            // Sub row (program id / last slot, or hint).
            ImGui::SetCursorScreenPos(ImVec2(topL.x + 24.0f, topL.y + kHeroH - 30.0f));
            if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  running ? ImVec4(0.92f, 0.95f, 1.00f, 0.75f)
                                          : ImVec4(0.55f, 0.61f, 0.70f, 1.00f));
            if (running) {
                const int lastSlot = Common::VRConfig::GetInt("last_slot", 0);
                char sub[96];
                if (pid != 0 && lastSlot > 0) {
                    std::snprintf(sub, sizeof(sub),
                                  "Title ID %08X  \xE2\x80\xA2  Last save slot %d",
                                  static_cast<unsigned>(pid & 0xFFFFFFFFu),
                                  lastSlot);
                } else if (pid != 0) {
                    std::snprintf(sub, sizeof(sub),
                                  "Title ID %08X",
                                  static_cast<unsigned>(pid & 0xFFFFFFFFu));
                } else {
                    std::snprintf(sub, sizeof(sub), "Running");
                }
                ImGui::TextUnformatted(sub);
            } else {
                ImGui::TextUnformatted("Pick a ROM from the Library tab to begin a session.");
            }
            ImGui::PopStyleColor();
            if (mImGuiLayer.FontCaption()) ImGui::PopFont();

            ImGui::Dummy(ImVec2(0, kHeroH + 12.0f));
        }

        // ---- Primary action row -----------------------------------------
        // Resume = teal accent (matches hero banner), Quit = red. Equal
        // widths so they read as a balanced pair regardless of label
        // length. Resume disabled when no game is running.
        const bool gameRunning = Core::System::GetInstance().IsPoweredOn();
        const float aw = ImGui::GetContentRegionAvail().x;
        const float bw = (aw - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

        if (!gameRunning) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.45f, 0.85f, 1.00f, 1.0f));
        if (ImGui::Button("Resume", ImVec2(bw, 70))) {
            mImGuiLayer.SetVisible(false);
        }
        ImGui::PopStyleColor(3);
        if (!gameRunning) ImGui::EndDisabled();
        ImGui::SameLine();

        // C5: Recenter View button - snaps menu and dim layer to current gaze.
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.18f, 0.38f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.26f, 0.52f, 0.74f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.38f, 0.66f, 0.92f, 1.0f));
        if (ImGui::Button("Recenter View", ImVec2(bw, 70))) {
            mMenuRecenterPending = true;
            mRecenterFlash       = 1.0f;
            ShowToast("View recentered");
            mController.TriggerHaptic(Hand::Left,  Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
            mController.TriggerHaptic(Hand::Right, Haptic::kThumpAmp, Haptic::kThumpDur, Haptic::kThumpFreq);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            VrTip("Re-centres this menu in front of your current\ngaze. Also: hold Right System button 0.7 s.");
        ImGui::PopStyleColor(3);
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.82f, 0.28f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
        if (ImGui::Button("Quit", ImVec2(bw, 70))) {
            mQuitConfirmPending = true;
        }
        ImGui::PopStyleColor(3);

        ImGui::Spacing();
        ImGui::Spacing();

        // ---- Secondary settings card ------------------------------------
        // Auto-resume + future per-session toggles live in a tinted card
        // so the destructive Quit button above stays the visual anchor.
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              ImVec4(0.10f, 0.13f, 0.18f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::BeginChild("##session_opts", ImVec2(0, 110.0f), true);
        if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
        ImGui::TextUnformatted("SESSION OPTIONS");
        ImGui::PopStyleColor();
        if (mImGuiLayer.FontCaption()) ImGui::PopFont();
        ImGui::Spacing();
        {
            bool ar = VRSettings::values.auto_resume;
            if (ImGui::Checkbox("Auto-resume last session on start", &ar)) {
                VRSettings::values.auto_resume = ar;
                Common::VRConfig::SetBool("auto_resume", ar);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Automatically loads the last save state\non next launch with the same ROM.");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        } // end Session tab

        if (mCurrentTab == MenuTab::About) {
        section_label("About");
        {
            // Cached one-shot system info (runtime + GPU name + version
            // numbers). XR / Vulkan property fetches are cheap but doing
            // them every frame would still be wasteful.
            struct SysInfo {
                std::string runtime;        // "SteamVR  v1.27.5"
                std::string gpu;            // "NVIDIA GeForce RTX 4080"
                std::string viewRes;        // "2160 x 2160 / eye"
                std::string refresh;        // "120 Hz" or "n/a"
                std::string configPath;     // VRConfig::Path()
            };
            static SysInfo s;
            static bool sLoaded = false;
            if (!sLoaded) {
                XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
                if (mPlatform.Instance() != XR_NULL_HANDLE &&
                    xrGetInstanceProperties(mPlatform.Instance(), &ip) == XR_SUCCESS) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s  v%u.%u.%u",
                                  ip.runtimeName,
                                  XR_VERSION_MAJOR(ip.runtimeVersion),
                                  XR_VERSION_MINOR(ip.runtimeVersion),
                                  XR_VERSION_PATCH(ip.runtimeVersion));
                    s.runtime = buf;
                } else {
                    s.runtime = "(unknown)";
                }
                if (mPlatform.VkPhysicalDevice_() != VK_NULL_HANDLE) {
                    VkPhysicalDeviceProperties pd{};
                    vkGetPhysicalDeviceProperties(mPlatform.VkPhysicalDevice_(), &pd);
                    s.gpu = pd.deviceName;
                } else {
                    s.gpu = "(unknown)";
                }
                {
                    const auto& vc = mPlatform.ViewConfig(0);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%u x %u / eye",
                                  vc.recommendedImageRectWidth,
                                  vc.recommendedImageRectHeight);
                    s.viewRes = buf;
                }
                if (mPlatform.HasDisplayRefreshRate()) {
                    PFN_xrGetDisplayRefreshRateFB pfn = nullptr;
                    xrGetInstanceProcAddr(mPlatform.Instance(),
                                          "xrGetDisplayRefreshRateFB",
                                          reinterpret_cast<PFN_xrVoidFunction*>(&pfn));
                    float hz = 0.0f;
                    if (pfn && mPlatform.Session() != XR_NULL_HANDLE &&
                        pfn(mPlatform.Session(), &hz) == XR_SUCCESS && hz > 0.0f) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
                        s.refresh = buf;
                    } else {
                        s.refresh = "n/a";
                    }
                } else {
                    s.refresh = "n/a";
                }
                s.configPath = Common::VRConfig::Path();
                sLoaded = true;
            }

            // ---- Hero card with gradient banner --------------------------
            // Matches the Display tab's hero treatment so About feels
            // like part of the same product, not a leftover debug page.
            constexpr float kHeroH = 120.0f;
            ImDrawList* dl    = ImGui::GetWindowDrawList();
            const ImVec2 topL = ImGui::GetCursorScreenPos();
            const float  hw   = ImGui::GetContentRegionAvail().x;
            const ImVec2 br(topL.x + hw, topL.y + kHeroH);
            const ImU32 cCyan   = ImGui::GetColorU32(ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
            const ImU32 cIndigo = ImGui::GetColorU32(ImVec4(0.42f, 0.36f, 0.92f, 1.0f));
            dl->AddRectFilledMultiColor(topL, br, cCyan, cIndigo, cIndigo, cCyan);
            // Subtle inner border for a touch of depth.
            dl->AddRect(topL, br,
                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)),
                        4.0f, 0, 1.5f);

            // Title block on top of the gradient.
            ImGui::SetCursorScreenPos(ImVec2(topL.x + 24.0f, topL.y + 18.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.98f, 0.99f, 1.00f, 1.00f));
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::TextUnformatted("CitraVR");
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(topL.x + 24.0f, topL.y + 72.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.92f, 0.95f, 1.00f, 0.85f));
            if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
            ImGui::TextUnformatted(
                "Nintendo 3DS emulation in SteamVR");
            if (mImGuiLayer.FontCaption()) ImGui::PopFont();
            ImGui::PopStyleColor();

            // Skip past the hero so subsequent widgets stack below it.
            ImGui::Dummy(ImVec2(0, kHeroH + 8.0f));

            // ---- Stat grid -----------------------------------------------
            // Two-column key/value table. Keys dim, values bright. Wraps
            // long values (config path, GPU name) gracefully.
            auto stat_row = [&](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
                if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
                ImGui::TextUnformatted(k);
                if (mImGuiLayer.FontCaption()) ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.92f, 0.95f, 0.99f, 1.0f));
                ImGui::TextWrapped("%s", v.c_str());
                ImGui::PopStyleColor();
            };

            ImGui::PushStyleColor(ImGuiCol_TableBorderLight,
                                  ImVec4(0.18f, 0.22f, 0.28f, 0.45f));
            if (ImGui::BeginTable("##about_stats", 2,
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_RowBg         |
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 220.0f);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
                stat_row("OpenXR runtime",    s.runtime);
                stat_row("GPU",               s.gpu);
                stat_row("View resolution",   s.viewRes);
                stat_row("Display refresh",   s.refresh);
                stat_row("Config file",       s.configPath);
                ImGui::EndTable();
            }
            ImGui::PopStyleColor();

            ImGui::Spacing(); ImGui::Spacing();

            // ---- Diagnostic toggles --------------------------------------
            bool sp = mShowPerfOverlay;
            if (ImGui::Checkbox("Show perf overlay (FPS / frame time / drops)", &sp)) {
                mShowPerfOverlay = sp;
                Common::VRConfig::SetBool("show_perf_overlay", sp);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                VrTip("Renders a small green HUD in the top-left\nof the menu quad. Visible only while the menu is open.");

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.55f, 0.61f, 0.70f, 1.0f));
            ImGui::TextWrapped("CitraVR is a fork of Citra \xE2\x80\x94 GPLv3. "
                               "See license.txt and NOTICE in the install directory.");
            ImGui::PopStyleColor();
        }

        } // end About tab
        ImGui::EndChild(); // ##content
        ImGui::PopStyleVar();   // tab-switch fade Alpha

        ImGui::PopStyleVar(2);  // FramePadding, ItemSpacing pushed at top

        // ---- Footer hint bar ---------------------------------------------
        // Slim 36 px row at the bottom of the menu window listing the
        // primary input verbs. Drawn directly via the window draw list so
        // it sits flush with the bottom edge regardless of the active
        // tab's content height. Hint text uses the caption font for
        // readability without competing with the body.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 winPos  = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            constexpr float kHintH = 44.0f;
            const ImVec2 hMin(winPos.x, winPos.y + winSize.y - kHintH);
            const ImVec2 hMax(winPos.x + winSize.x, winPos.y + winSize.y);
            // Translucent panel (slightly darker than ChildBg so it
            // visually anchors the menu without grabbing focus).
            dl->AddRectFilled(hMin, hMax,
                              ImGui::GetColorU32(ImVec4(0.04f, 0.06f, 0.09f, 0.85f)));
            dl->AddLine(ImVec2(hMin.x, hMin.y),
                        ImVec2(hMax.x, hMin.y),
                        ImGui::GetColorU32(ImVec4(0.20f, 0.26f, 0.33f, 0.85f)),
                        1.0f);

            // Right-aligned hint text rendered with the caption font.
            constexpr const char* kHint =
                "Trigger: Select   Wrist Menu: Close   Right Stick: Scroll";
            ImFont* fc = mImGuiLayer.FontCaption();
            const float fontSize = fc ? fc->FontSize : ImGui::GetFontSize();
            const ImVec2 textSize = fc
                ? fc->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, kHint)
                : ImGui::CalcTextSize(kHint);
            const ImVec2 textPos(hMax.x - textSize.x - 24.0f,
                                 hMin.y + (kHintH - textSize.y) * 0.5f);
            const ImU32  textCol = ImGui::GetColorU32(
                ImVec4(0.63f, 0.69f, 0.76f, 1.0f));
            if (fc) {
                dl->AddText(fc, fontSize, textPos, textCol, kHint);
            } else {
                dl->AddText(textPos, textCol, kHint);
            }
        }

        // ---- First-launch welcome card -----------------------------------
        // Three-page onboarding: Welcome â†’ Controls â†’ Settings hint.
        // Triggered once by citra_vr.cpp on first-ever launch. Persists
        // until the user dismisses the last page; resets to page 0 on open.
        if (mWelcomePending) {
            ImGui::OpenPopup("##welcome_modal");
            mWelcomePending = false;
        }
        ImGui::SetNextWindowSize(ImVec2(820, 600), ImGuiCond_Appearing);
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,
                              ImVec4(0.02f, 0.04f, 0.07f, 0.70f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
        if (ImGui::BeginPopupModal("##welcome_modal", nullptr,
                                   ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize   |
                                   ImGuiWindowFlags_NoMove)) {
            // Page state lives inside the modal scope (resets implicitly
            // when the popup is closed and reopened on the next launch).
            static int sWelcomePage = 0;
            constexpr int kWelcomePages = 3;

            // Per-page metadata: a glyph (rendered in the hero bubble),
            // headline + subhead. Subhead replaces the dense paragraph
            // body of the original modal — punchier and reads at glance.
            struct WPage {
                const char* glyph;     // big bubble glyph
                const char* badge;     // small caption above title
                const char* title;
                const char* body;
            };
            static const WPage pages[kWelcomePages] = {
                {"VR", "STEP 1 OF 3", "Welcome to CitraVR",
                 "You're playing 3DS games inside SteamVR.\n"
                 "The virtual screen floats in front of you and follows the "
                 "world, not your head — so you can lean and step around it.\n\n"
                 "If anything ever feels off, the menu is one wrist-button "
                 "tap away."},
                {"::", "STEP 2 OF 3", "Your controllers",
                 "Wrist menu button   open / close this menu\n"
                 "Hold wrist menu     emulate the 3DS Home button\n"
                 "Right trigger       touch the bottom screen\n"
                 "Left trigger        click in the menu\n\n"
                 "Buttons map automatically to A/B/X/Y, D-Pad and shoulders."},
                {"OK", "STEP 3 OF 3", "Make it yours",
                 "Display tab tunes screen size, distance and resolution.\n"
                 "Input tab remaps every controller binding.\n"
                 "Library tab loads ROMs from CITRA_VR_ROM_DIR.\n\n"
                 "Tip: try the Performance / Balanced / Quality presets first "
                 "if frame timing feels off."},
            };
            const WPage& page = pages[sWelcomePage];

            // ---- Hero band: gradient + bubble glyph + title --------
            constexpr float kHeroH = 200.0f;
            const ImVec2 wMin = ImGui::GetWindowPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 hMin = wMin;
            const ImVec2 hMax(wMin.x + ImGui::GetWindowSize().x, wMin.y + kHeroH);
            const ImU32 cL = ImGui::GetColorU32(ImVec4(0.13f, 0.30f, 0.46f, 1.0f));
            const ImU32 cR = ImGui::GetColorU32(ImVec4(0.30f, 0.18f, 0.50f, 1.0f));
            dl->AddRectFilledMultiColor(hMin, hMax, cL, cR, cR, cL);
            // Soft inner highlight along the top edge.
            dl->AddRectFilled(hMin, ImVec2(hMax.x, hMin.y + 2.0f),
                              ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)));

            // Icon bubble on the left of the hero.
            constexpr float kBubR = 56.0f;
            const ImVec2 bC(hMin.x + 80.0f, hMin.y + kHeroH * 0.5f);
            dl->AddCircleFilled(bC, kBubR + 4.0f,
                                ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f)));
            dl->AddCircleFilled(bC, kBubR,
                                ImGui::GetColorU32(ImVec4(0.06f, 0.10f, 0.14f, 0.80f)));
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            const ImVec2 gSz = ImGui::CalcTextSize(page.glyph);
            dl->AddText(ImVec2(bC.x - gSz.x * 0.5f, bC.y - gSz.y * 0.5f),
                        ImGui::GetColorU32(ImVec4(0.30f, 0.72f, 0.95f, 1.0f)),
                        page.glyph);
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();

            // Title + step badge.
            const float textX = bC.x + kBubR + 28.0f;
            if (auto* fc = mImGuiLayer.FontCaption()) ImGui::PushFont(fc);
            dl->AddText(ImVec2(textX, hMin.y + 56.0f),
                        ImGui::GetColorU32(ImVec4(0.78f, 0.86f, 1.0f, 0.85f)),
                        page.badge);
            if (mImGuiLayer.FontCaption()) ImGui::PopFont();

            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            dl->AddText(ImVec2(textX, hMin.y + 86.0f),
                        ImGui::GetColorU32(ImVec4(0.96f, 0.98f, 1.0f, 1.0f)),
                        page.title);
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();

            // Push cursor below the hero band and apply normal padding for
            // the body region.
            ImGui::SetCursorPos(ImVec2(36.0f, kHeroH + 24.0f));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.89f, 0.93f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetWindowSize().x - 36.0f);
            ImGui::TextWrapped("%s", page.body);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            // Push the nav row to the bottom of the modal.
            const float navH = 75.0f;
            const float dotsH = 22.0f;
            const float footerY = ImGui::GetWindowSize().y - 36.0f - navH - dotsH - 8.0f;
            ImGui::SetCursorPosY(footerY);

            // Page-indicator dots, centred. Active dot is a longer pill.
            {
                const float dotR = 5.0f;
                const float gap  = 22.0f;
                const float totW = (kWelcomePages - 1) * gap;
                const ImVec2 p   = ImGui::GetCursorScreenPos();
                const float  cx  = p.x + ImGui::GetContentRegionAvail().x * 0.5f;
                const float  cy  = p.y + dotsH * 0.5f;
                for (int i = 0; i < kWelcomePages; ++i) {
                    const float x = cx - totW * 0.5f + i * gap;
                    const bool active = (i == sWelcomePage);
                    const ImU32 col = ImGui::GetColorU32(active
                            ? ImVec4(0.30f, 0.72f, 0.95f, 1.0f)
                            : ImVec4(0.30f, 0.36f, 0.44f, 0.9f));
                    if (active) {
                        dl->AddRectFilled(ImVec2(x - 12.0f, cy - dotR),
                                          ImVec2(x + 12.0f, cy + dotR),
                                          col, dotR);
                    } else {
                        dl->AddCircleFilled(ImVec2(x, cy), dotR, col);
                    }
                }
                ImGui::Dummy(ImVec2(0, dotsH));
            }
            ImGui::SetCursorPosX(36.0f);
            // Nav row: Back (dim) | Next/Done (cyan), 50/50 split.
            const float bw = (ImGui::GetWindowSize().x - 72.0f -
                              ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            const bool isFirst = (sWelcomePage == 0);
            const bool isLast  = (sWelcomePage == kWelcomePages - 1);

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.26f, 0.32f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.32f, 0.40f, 0.48f, 1.0f));
            if (isFirst) ImGui::BeginDisabled();
            if (ImGui::Button("Back", ImVec2(bw, navH))) {
                if (sWelcomePage > 0) --sWelcomePage;
            }
            if (isFirst) ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.20f, 0.55f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.30f, 0.72f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.45f, 0.85f, 1.00f, 1.0f));
            const char* nextLabel = isLast ? "Got it, let's play!" : "Next";
            if (ImGui::Button(nextLabel, ImVec2(bw, navH))) {
                if (isLast) {
                    ImGui::CloseCurrentPopup();
                    Common::VRConfig::SetBool("seen_welcome", true);
                    sWelcomePage = 0;
                } else {
                    ++sWelcomePage;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // ---- Quit confirmation modal -------------------------------------
        // Triggered by the sidebar Quit button. Uses a translucent
        // backdrop dim and a large red Confirm button so the destructive
        // action requires a deliberate second tap.
        if (mQuitConfirmPending) {
            ImGui::OpenPopup("##quit_confirm");
            mQuitConfirmPending = false;
        }
        ImGui::SetNextWindowSize(ImVec2(620, 320), ImGuiCond_Appearing);
        // Dim background behind the modal for a 3 % extra dose of polish.
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,
                              ImVec4(0.02f, 0.04f, 0.07f, 0.65f));
        if (ImGui::BeginPopupModal("##quit_confirm", nullptr,
                                   ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize   |
                                   ImGuiWindowFlags_NoMove)) {
            if (auto* fd = mImGuiLayer.FontDisplay()) ImGui::PushFont(fd);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.96f, 0.42f, 0.42f, 1.0f));
            ImGui::TextUnformatted("Quit CitraVR?");
            ImGui::PopStyleColor();
            if (mImGuiLayer.FontDisplay()) ImGui::PopFont();
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.78f, 0.83f, 0.89f, 1.0f));
            ImGui::TextWrapped(
                "Unsaved progress will be lost. Use the Saves tab to create a "
                "save state before quitting.");
            ImGui::PopStyleColor();
            ImGui::Spacing(); ImGui::Spacing();

            const float bw = (ImGui::GetContentRegionAvail().x -
                              ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.26f, 0.32f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.32f, 0.40f, 0.48f, 1.0f));
            if (ImGui::Button("Cancel", ImVec2(bw, 70))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.82f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.95f, 0.40f, 0.40f, 1.0f));
            if (ImGui::Button("Quit CitraVR", ImVec2(bw, 70))) {
                mExitRequested.store(true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(); // ModalWindowDimBg

        ImGui::End();
        } // end if (mImGuiLayer.IsVisuallyVisible())
        // Toast: rendered every frame when active, independent of the
        // main menu visibility. Appears at the bottom of the ImGui quad.
        mImGuiLayer.DrawToast();
        mImGuiLayer.DrawPerfOverlay();
        mImGuiLayer.EndFrameAndRender(mSubmitter);
    }

    // ---- Locate head pose once per frame -------------------------------
    // Both the menu re-centre and the head-tracking motion derivation
    // need the View-in-Local pose. Cache the lookup so we don't pay for
    // two xrLocateSpace calls per frame.
    XrSpaceLocation headLoc{XR_TYPE_SPACE_LOCATION};
    bool            headLocValid = false;
    if (mPlatform.ViewSpace() != XR_NULL_HANDLE
        && mPlatform.LocalSpace() != XR_NULL_HANDLE
        && XR_SUCCEEDED(xrLocateSpace(mPlatform.ViewSpace(), mPlatform.LocalSpace(),
                                      fs.predictedDisplayTime, &headLoc))) {
        headLocValid = true;
    }

    // ---- Re-centre menu in front of current gaze on every open ---------
    // Snapshots the user's view pose in LocalSpace, strips out pitch/roll
    // (yaw-only). The game screen recenters to face the user's current gaze;
    // the menu appears parallel to the game screen (same yaw, same eye-level
    // height) but slightly closer so it overlays naturally.
    if (mMenuRecenterPending && headLocValid
        && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        {
            const XrSpaceLocation& viewLoc = headLoc;

            // Build horizontal forward vector from view orientation.
            const XrQuaternionf& q = viewLoc.pose.orientation;
            XrVector3f fwd = rotate(q, XrVector3f{0.0f, 0.0f, -1.0f});
            fwd.y = 0.0f;
            const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
            if (len > 1e-3f) {
                fwd.x /= len; fwd.z /= len;
            } else {
                fwd = XrVector3f{0.0f, 0.0f, -1.0f};
            }

            // Right-handed +Y rotation convention (see first-frame comment).
            const float yaw = std::atan2(-fwd.x, -fwd.z);
            XrQuaternionf yawQ{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};

            // Recenter the game screen first. layerSpace is re-read after
            // this call (see below) so BuildLayers picks up the fresh handle.
            mGameYaw    = yaw;
            mGameOrigin = viewLoc.pose.position;
            mPlatform.RecreateHeadSpace(yaw, viewLoc.pose.position);

            // Menu: same yaw as game screen, eye-level height (no rise),
            // placed 0.30m closer to the user than the game screen so it
            // sits in front of it. Clamp so the menu is never closer than
            // 0.5m even if the user has set a very short screen distance.
            const float gameDist  = VRSettings::values.screen_distance;
            const float menuDist  = std::max(0.50f, gameDist - 0.30f);
            XrPosef pose;
            pose.orientation = yawQ;
            pose.position    = XrVector3f{viewLoc.pose.position.x + fwd.x * menuDist,
                                          viewLoc.pose.position.y,
                                          viewLoc.pose.position.z + fwd.z * menuDist};
            mImGuiLayer.SetPose(pose);
            // Dim layer sits between menu and game screen (halfway between
            // them) at eye level, covering the peripheral field of view.
            const float dimDist = (menuDist + gameDist) * 0.5f;
            XrPosef dimPose;
            dimPose.orientation = yawQ;
            dimPose.position    = XrVector3f{viewLoc.pose.position.x + fwd.x * dimDist,
                                             viewLoc.pose.position.y,
                                             viewLoc.pose.position.z + fwd.z * dimDist};
            mDimLayer.SetSize(3.0f, 2.2f);
            mDimLayer.SetPose(dimPose, true);
            ALOGI("Menu re-centred at (%.2f, %.2f, %.2f) yaw=%.1f deg gameDist=%.2f menuDist=%.2f",
                  pose.position.x, pose.position.y, pose.position.z,
                  yaw * 57.2957795f, gameDist, menuDist);
        }
        mMenuRecenterPending = false;
    }

    // ---- Lazy menu yaw-follow ------------------------------------------
    // While the menu is open, softly rotate it toward the user's current
    // head yaw. Uses a deadband + damped slerp so small head-shakes are
    // ignored but a body turn causes the menu to glide around to face the
    // user again. The dim scrim follows the menu.
    //
    // Tunables:
    //   kFollowDeadbandRad - menu doesn't move while head-yaw vs menu-yaw
    //                        differs by less than this angle.
    //   kFollowSpeedHz     - characteristic frequency of the exponential
    //                        slerp (higher = snappier, lower = lazier).
    //   kFollowMaxRad      - target travel per frame (avoids snapping on
    //                        very long dt spikes).
    if (mImGuiLayer.IsInitialised() && mImGuiLayer.IsVisuallyVisible()
        && !mMenuRecenterPending
        && headLocValid
        && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
        && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        constexpr float kFollowDeadbandRad = 0.35f; // ~20 deg
        constexpr float kFollowSpeedHz     = 2.0f;  // e-fold per ~0.5 s
        constexpr float kFollowMaxRad      = 0.10f; // <= ~5.7 deg per frame

        const XrQuaternionf& hq = headLoc.pose.orientation;
        XrVector3f fwdH = rotate(hq, XrVector3f{0.0f, 0.0f, -1.0f});
        fwdH.y = 0.0f;
        const float lenH = std::sqrt(fwdH.x * fwdH.x + fwdH.z * fwdH.z);
        if (lenH > 1e-3f) {
            fwdH.x /= lenH; fwdH.z /= lenH;
            // Match the +Y-rotation convention used by mGameYaw / yawQ.
            const float headYaw = std::atan2(-fwdH.x, -fwdH.z);
            // Wrap (headYaw - mGameYaw) into [-PI, +PI].
            float dyaw = headYaw - mGameYaw;
            while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
            while (dyaw < -3.14159265f) dyaw += 6.28318531f;
            const float adyaw = std::abs(dyaw);
            if (adyaw > kFollowDeadbandRad) {
                // Past the deadband: ease toward head yaw. `dt` is the
                // function-level frame delta computed at the top of
                // Frame() (already clamped to [0, 0.1] s).
                const float dtSec = (dt > 0.0f) ? dt : 0.016f;
                // Aim to close 50% of the over-deadband error per (1/kFollowSpeedHz) s.
                const float alpha = 1.0f - std::exp(-kFollowSpeedHz * dtSec);
                // Only correct the part of the error past the deadband so
                // the menu never overshoots into the opposite deadband.
                const float over = adyaw - kFollowDeadbandRad;
                float step = (dyaw > 0.0f ? +over : -over) * alpha;
                if (step >  kFollowMaxRad) step =  kFollowMaxRad;
                if (step < -kFollowMaxRad) step = -kFollowMaxRad;
                mGameYaw += step;

                const float yaw = mGameYaw;
                const XrQuaternionf yawQ{0.0f, std::sin(yaw * 0.5f),
                                         0.0f, std::cos(yaw * 0.5f)};

                // Re-derive menu and dim poses around the user's head
                // (head position drifts as the user walks, so use the
                // live position not the snapshot taken on open).
                const float gameDist = VRSettings::values.screen_distance;
                const float menuDist = std::max(0.50f, gameDist - 0.30f);
                const float dimDist  = (menuDist + gameDist) * 0.5f;
                // For our yaw convention, rotate(yawQ,{0,0,-1}) =
                // (-sin yaw, 0, -cos yaw); this is the user's forward.
                const XrVector3f fwdYaw{-std::sin(yaw), 0.0f, -std::cos(yaw)};

                XrPosef mp;
                mp.orientation = yawQ;
                mp.position    = XrVector3f{headLoc.pose.position.x + fwdYaw.x * menuDist,
                                            headLoc.pose.position.y,
                                            headLoc.pose.position.z + fwdYaw.z * menuDist};
                mImGuiLayer.SetPose(mp);

                if (mDimLayer.IsInitialised()) {
                    XrPosef dp;
                    dp.orientation = yawQ;
                    dp.position    = XrVector3f{
                        headLoc.pose.position.x + fwdYaw.x * dimDist,
                        headLoc.pose.position.y,
                        headLoc.pose.position.z + fwdYaw.z * dimDist};
                    mDimLayer.SetPose(dp, true);
                }
            }
        }
    }

    // ---- Head-tracking motion (gyro + accelerometer) -------------------
    // Locate the view space in local space every frame to get the current
    // head orientation, then derive a gravity vector (accelerometer) and
    // angular velocity (gyroscope) in the 3DS coordinate frame.
    // We use a quat-multiply helper for the angular-rate computation.
    if (mInputBridge != nullptr
        && headLocValid
        && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        {

            const XrQuaternionf& q = headLoc.pose.orientation;

            // Conjugate (= inverse for unit quaternions).
            const XrQuaternionf q_inv = {-q.x, -q.y, -q.z, q.w};

            // Gravity in head-local frame â€” same as motion_emu's approach.
            const XrVector3f gravity_local = rotate(q_inv, XrVector3f{0.0f, -1.0f, 0.0f});
            const float accel[3] = {gravity_local.x, gravity_local.y, gravity_local.z};

            // Angular velocity (deg/sec) from quaternion derivative.
            // dq = q_curr - q_prev (component-wise first-difference).
            // Ï‰_world = 2 * dq * q_inv  (world-space angular velocity quat)
            // Ï‰_local = rotate(q_inv, Ï‰_world.xyz)
            float gyro[3] = {0.0f, 0.0f, 0.0f};
            const float dt_ns = static_cast<float>(
                fs.predictedDisplayTime - mPrevDisplayTime);
            if (mPrevDisplayTime != 0 && dt_ns > 0.0f) {
                const float dt_s = dt_ns * 1e-9f;
                const float dx = q.x - mPrevHeadQ.x;
                const float dy = q.y - mPrevHeadQ.y;
                const float dz = q.z - mPrevHeadQ.z;
                const float dw = q.w - mPrevHeadQ.w;
                // Hamilton product  (dx,dy,dz,dw) Ã— q_inv = (dx,dy,dz,dw) Ã— (-qx,-qy,-qz,qw)
                const float rx = dw*(-q.x) + q.w*dx + dy*(-q.z) - dz*(-q.y);
                const float ry = dw*(-q.y) + q.w*dy + dz*(-q.x) - dx*(-q.z);
                const float rz = dw*(-q.z) + q.w*dz + dx*(-q.y) - dy*(-q.x);
                // Scale to deg/sec: * 2 / dt_s * (180 / PI)
                const float kToDegSec = 2.0f * (180.0f / 3.14159265358979323846f);
                const float scale = kToDegSec / dt_s;
                // Rotate into head-local frame (already in world frame via dq*q_inv)
                const XrVector3f omega_world{rx * scale, ry * scale, rz * scale};
                const XrVector3f omega_local = rotate(q_inv, omega_world);
                gyro[0] = omega_local.x;
                gyro[1] = omega_local.y;
                gyro[2] = omega_local.z;
            }

            mInputBridge->UpdateMotion(accel, gyro);
            mPrevHeadQ       = q;
            mLastHeadPose    = headLoc.pose;  // used by launch-fade positioning
            mPrevDisplayTime = fs.predictedDisplayTime;
        }
    }

    XrCompositionLayerQuad gameQuads[2]   = {};
    XrCompositionLayerQuad cursorQuads[4] = {};
    XrCompositionLayerQuad beamQuads[2]   = {};
    XrCompositionLayerQuad menuQuad       = {};
    XrCompositionLayerQuad dimQuad        = {};
    XrCompositionLayerQuad wristQuads[WB_Count] = {};
    bool                   wristVis[WB_Count]   = {false, false, false};
    XrCompositionLayerQuad ctrlQuads[2]         = {};
    bool                   ctrlVis[2]           = {false, false};
    // Push the current screen geometry into the game layer so it matches
    // the touch raycast constants computed above.
    // Re-read HeadSpace here — after any RecreateHeadSpace call in the
    // recenter handler the earlier handle is destroyed; reading it fresh
    // guarantees BuildLayers uses the valid, current handle.
    XrSpace layerSpace = mPlatform.HeadSpace() != XR_NULL_HANDLE
                             ? mPlatform.HeadSpace()
                             : mPlatform.LocalSpace();
    mGameLayer.SetGeometry(kScale, VRSettings::values.screen_distance);
    mGameLayer.SetEyeOffset(VRSettings::values.eye_offset);
    mGameLayer.BuildLayers(layerSpace, gameQuads);
    // Cursors live in LOCAL space (world-anchored) so they don't drift
    // when the user moves their head. CursorLayer may emit up to 2 quads
    // per visible cursor (one per eye, with GameQuadLayer-matching
    // xOffset) when the cursor is snapped to the game screen.
    mCursorLayer.SetEyeOffset(VRSettings::values.eye_offset);
    const uint32_t cursorCount = mCursorLayer.BuildLayers(mPlatform.LocalSpace(), cursorQuads);
    const uint32_t beamCount   = mBeamLayer.BuildLayers(mPlatform.LocalSpace(), beamQuads);
    // Menu lives in LocalSpace (world-anchored). It's re-posed in front
    // of the user's gaze on every open via the recenter block above, so
    // it stays put while the user looks around / leans in.
    // Menu lives in LocalSpace (world-anchored). It's re-posed in front
    // of the user's gaze on every open via the recenter block above, so
    // it stays put while the user looks around / leans in.
    // Use IsVisuallyVisible() (alpha > epsilon) rather than IsVisible()
    // (target state) so the menu's quad is still composited during the
    // close-fade animation.
    const bool menuVisible = mImGuiLayer.IsInitialised() &&
                             mImGuiLayer.BuildLayer(mPlatform.LocalSpace(), menuQuad);
    // Dim layer is only emitted while the menu is visually present.
    // Re-paint its scrim alpha each frame so it ramps smoothly with
    // the menu fade (instead of popping at the open/close edges).
    if (mDimLayer.IsInitialised()) {
        // Launch fade-in: ramp from full black to transparent over 1.5 s.
        // While active, track the head each frame so the blackout quad
        // always covers the full FOV regardless of where the user looks.
        if (mLaunchFade > 0.0f) {
            mLaunchFade = std::max(0.0f, mLaunchFade - dt / 1.5f);
            if (!mImGuiLayer.IsVisible()) {
                const XrVector3f fwd = rotate(mLastHeadPose.orientation,
                                              XrVector3f{0.0f, 0.0f, -1.0f});
                XrPosef fadePose;
                fadePose.orientation = mLastHeadPose.orientation;
                fadePose.position    = {mLastHeadPose.position.x + fwd.x * 0.5f,
                                        mLastHeadPose.position.y + fwd.y * 0.5f,
                                        mLastHeadPose.position.z + fwd.z * 0.5f};
                mDimLayer.SetSize(2.5f, 2.5f);
                mDimLayer.SetPose(fadePose, /*visible=*/true);
            }
        }
        // Menu scrim: squared curve up to 55% opacity (same visual as before).
        // Scale by 0.55 because DimLayer now uses base alpha = 1.0.
        const float menuFade = mImGuiLayer.Alpha();
        // Recenter flash: spikes to 0.7, decays over 150 ms.
        if (mRecenterFlash > 0.0f)
            mRecenterFlash = std::max(0.0f, mRecenterFlash - dt / 0.15f);
        mDimLayer.SetAlpha(mSubmitter,
                           std::max({menuFade * menuFade * 0.55f,
                                     mLaunchFade,
                                     mRecenterFlash * 0.7f}));
    }
    bool dimVisible = false;
    if (mDimLayer.IsInitialised()) {
        dimVisible = mDimLayer.BuildLayer(mPlatform.LocalSpace(), dimQuad);
    }
    // Wrist buttons (LOCAL space, anchored to controller).
    for (int btn = 0; btn < WB_Count; ++btn) {
        wristVis[btn] = mWristBtns[btn].BuildLayer(mPlatform.LocalSpace(), wristQuads[btn]);
    }
    // Controller markers (LOCAL space). Anchored to each grip pose,
    // offset slightly along the grip's forward axis so the quad sits
    // ahead of the user's fist rather than clipping into it. The
    // quad's own orientation is the grip orientation so it reads as
    // a "wand pointing along your aim direction".
    if (mShowControllers) {
        for (uint32_t hand = 0; hand < 2; ++hand) {
            const auto& s = mController.State(static_cast<Hand>(hand));
            if (!s.grip_pose_valid || !mControllerMarkers[hand].IsInitialised()) {
                mControllerMarkers[hand].SetPose(XrPosef{{0,0,0,1},{0,0,0}}, false);
                continue;
            }
            // Push the marker 4cm along grip-local -Z (forward of the
            // fist) and 2cm along grip-local +Y (raised slightly) so it
            // doesn't sit inside the user's hand mesh.
            const XrVector3f off = rotate(s.grip_pose.orientation,
                                          XrVector3f{0.0f, 0.02f, -0.04f});
            XrPosef p;
            p.orientation = s.grip_pose.orientation;
            p.position.x  = s.grip_pose.position.x + off.x;
            p.position.y  = s.grip_pose.position.y + off.y;
            p.position.z  = s.grip_pose.position.z + off.z;
            mControllerMarkers[hand].SetPose(p, true);
            ctrlVis[hand] = mControllerMarkers[hand].BuildLayer(
                mPlatform.LocalSpace(), ctrlQuads[hand]);
        }
    } else {
        for (uint32_t hand = 0; hand < 2; ++hand) {
            mControllerMarkers[hand].SetPose(XrPosef{{0,0,0,1},{0,0,0}}, false);
        }
    }

    // Layer order (back to front): game â†’ beams â†’ dim â†’ menu â†’ wrist â†’ controllers â†’ cursors.
    // Wrist buttons, controllers and cursors are composited AFTER the dim
    // scrim and menu panel so they remain fully visible while the menu is open.
    // game(2) + beams(0..2) + optional dim(1) + menu(1) + wrist(0..3) + ctrls(0..2) + cursors(0..4) = up to 15.
    const XrCompositionLayerBaseHeader* layerPtrs[15] = {};
    uint32_t layerIdx = 0;
    // Game quads are only valid after at least one blit has completed;
    // before that the swapchain images are uninitialised.
    if (mEverComposited) {
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&gameQuads[0]);
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&gameQuads[1]);
    }
    // Beams drawn on top of game content, behind the dim scrim.
    for (uint32_t i = 0; i < beamCount && layerIdx < 15; ++i) {
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&beamQuads[i]);
    }
    if (dimVisible && layerIdx < 15) {
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&dimQuad);
    }
    if (menuVisible && layerIdx < 15) {
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuQuad);
    }
    // Wrist buttons after menu so the dim scrim doesn't hide them.
    for (int btn = 0; btn < WB_Count; ++btn) {
        if (wristVis[btn] && layerIdx < 15) {
            layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&wristQuads[btn]);
        }
    }
    // Controller markers (one per hand when grip pose is tracked).
    for (uint32_t i = 0; i < 2; ++i) {
        if (ctrlVis[i] && layerIdx < 15) {
            layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&ctrlQuads[i]);
        }
    }
    // Cursors last â€” always on top.
    for (uint32_t i = 0; i < cursorCount && layerIdx < 15; ++i) {
        layerPtrs[layerIdx++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&cursorQuads[i]);
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime          = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount           = layersValid ? layerIdx : 0u;
    fei.layers               = fei.layerCount ? layerPtrs : nullptr;
    {
        // SteamVR's xrEndFrame submits work on Citra's graphics queue;
        // serialise it with renderer-side vkQueueSubmit.
        std::scoped_lock vr_lock{Vulkan::GetVrQueueMutex()};
        xrEndFrame(mPlatform.Session(), &fei);
    }
}

} // namespace vr_pcvr
