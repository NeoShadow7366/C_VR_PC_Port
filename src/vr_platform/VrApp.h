// SPDX-License-Identifier: GPL-3.0-or-later
//
// VrApp - platform-agnostic OpenXR frame loop. The Android port and the
// new Windows / SteamVR port both drive their session from this class;
// platform-specific bring-up (event pump, graphics binding) lives in
// IVrPlatform implementations.

#pragma once

#include "RomBrowser.h"
#include "VrSettings.h"
#include "XrController.h"
#include "XrPlatformIncludes.h"
#include "layers/AimBeamLayer.h"
#include "layers/CursorLayer.h"
#include "layers/DimLayer.h"
#include "layers/GameQuadLayer.h"
#include "layers/ImGuiLayer.h"
#include "layers/WristMenuButton.h"
#include "windows/VrFrameSubmitter.h"

#include <atomic>
#include <functional>
#include <string>

namespace vr_pcvr {

class WinPlatform;
class EmuWindow_VR_Win;

class VrApp {
public:
    explicit VrApp(WinPlatform& platform);
    ~VrApp();

    // Attach the Citra EmuWindow that owns the off-screen render target.
    // The XR thread will pull the latest published frame from it on each
    // xrEndFrame iteration. Lifetime: the caller owns the EmuWindow and
    // must outlive this VrApp.
    void SetEmuWindow(EmuWindow_VR_Win* window) { mEmuWindow = window; }
    EmuWindow_VR_Win* GetEmuWindow() const { return mEmuWindow; }

    VrApp(const VrApp&)            = delete;
    VrApp& operator=(const VrApp&) = delete;

    // After WinPlatform::InitSession has succeeded, set up the action set,
    // the per-eye swapchains and the game / UI / cursor layers.
    bool Start();

    // Pump XR events; returns false when the runtime asks us to exit.
    bool PollEvents();

    // Equivalent to one iteration of Android's vr_main.cpp main loop:
    //  xrWaitFrame -> xrBeginFrame -> SyncFrame -> render -> xrEndFrame.
    // `renderCitraToImage` is the renderer hook that draws one Citra
    // frame into the off-screen Vulkan image which GameQuadLayer
    // then blits into the XR swapchain. It may be null while the
    // renderer is still being wired up.
    void Frame(void (*renderCitraToImage)(void*) = nullptr, void* userdata = nullptr);

    bool IsRunning() const { return mIsSessionRunning; }

    // Set externally (e.g. by the menu "Quit" button) to ask the XR
    // thread to exit cleanly.
    void RequestExit() { mExitRequested.store(true); }
    bool ExitRequested() const { return mExitRequested.load(); }

    ImGuiLayer&         Menu()       { return mImGuiLayer; }
    const ImGuiLayer&   Menu() const { return mImGuiLayer; }

    XrController&       Input()       { return mController; }
    const XrController& Input() const { return mController; }

    // Lightweight per-frame state queries used by VrInputBridge to
    // avoid leaking menu / touch interactions into 3DS HID.
    bool IsMenuOpen()         const { return mImGuiLayer.IsInitialised() && mImGuiLayer.IsVisible(); }
    bool IsRightTouchActive() const { return mTouchHeld; }
    bool IsWristStartHeld()   const { return mWristBtnHeld[1]; }
    bool IsWristSelectHeld()  const { return mWristBtnHeld[2]; }
    // C6: true while the HMD is on the user's head (session FOCUSED).
    bool IsHmdOnHead()        const { return mHmdOnHead; }

    // ROM browser configuration. Pass the root directory to scan and a
    // callback fired when the user picks a ROM in the in-VR menu. The
    // callback runs on the XR thread; implementations should not block.
    RomBrowser&       Browser()       { return mRomBrowser; }
    const RomBrowser& Browser() const { return mRomBrowser; }
    void SetOnLoadRom(std::function<void(const std::string&)> cb) {
        mOnLoadRom = std::move(cb);
    }
    // Fired by the in-VR Save-state Load buttons. Implementations
    // typically write a sentinel and request app exit so the launcher
    // can relaunch with --loadslot N (in-process LoadState destroys the
    // renderer, which dangles the XR session's VkDevice).
    void SetOnLoadState(std::function<void(int /*slot*/)> cb) {
        mOnLoadState = std::move(cb);
    }
    // Fired once when the user holds a controller trigger on the wrist
    // menu button for >= kWristHomeHoldNs (700 ms). Bound by citra_vr to
    // VrInputBridge::FireHome so 3DS HID gets a single-frame Home press.
    void SetOnHome(std::function<void()> cb) { mOnHome = std::move(cb); }

    // Optional: hand the in-VR menu access to the live VrInputBridge so
    // the "Controls" pane can read/write the remappable button bindings.
    void SetInputBridge(class VrInputBridge* bridge) { mInputBridge = bridge; }

    // Show a transient toast notification in the VR space (works whether
    // the in-VR menu is open or closed).
    void ShowToast(const std::string& text, float duration = 1.5f);

    // Backend perf snapshot for the in-VR perf overlay.
    struct PerfSnapshot {
        float    frame_interval_ms;
        float    fps;
        uint64_t dropped_blits;
        uint64_t frame_count;
    };
    PerfSnapshot GetPerfSnapshot() const {
        return {mFrameIntervalMs,
                mFrameIntervalMs > 0.0f ? 1000.0f / mFrameIntervalMs : 0.0f,
                mDroppedBlits, mFrameCount};
    }

    void SetShowPerfOverlay(bool v) { mShowPerfOverlay = v; }
    bool IsShowPerfOverlay() const  { return mShowPerfOverlay; }

    // Call once from citra_vr.cpp if vr_config.txt has never been seen
    // (seen_welcome != 1). The welcome card will appear on the first
    // menu-open and be dismissable with a single button click.
    void SetShowWelcome(bool v) { mWelcomePending = v; }

    // Launcher mode: called when the app started with no ROM on the command
    // line (auto-selected a boot ROM). Opens the in-VR menu to the ROM
    // browser automatically so the user can pick their game in-headset.
    void SetLauncherMode(bool v) { mLauncherMode = v; }

private:
    void HandleSessionStateChanged(const XrEventDataSessionStateChanged& ev);

    WinPlatform&     mPlatform;
    XrSessionState   mSessionState     = XR_SESSION_STATE_UNKNOWN;
    bool             mIsSessionRunning = false;
    bool             mFirstFrame       = true;
    std::atomic<bool> mExitRequested{false};
    bool             mEverComposited = false;

    XrController       mController;
    GameQuadLayer      mGameLayer;
    CursorLayer        mCursorLayer;
    AimBeamLayer       mBeamLayer;
    DimLayer           mDimLayer;
    ImGuiLayer         mImGuiLayer;
    // Wrist button bar: [Menu] [Start] [Select]. Same primitive (8x8
    // tinted quad) instantiated three times. Index 0 keeps the existing
    // menu-toggle / long-press-Home semantics; indices 1/2 just drive
    // a transient "held" flag consumed by VrInputBridge.
    enum WristBtn { WB_Menu = 0, WB_Start = 1, WB_Select = 2, WB_Count = 3 };
    WristMenuButton    mWristBtns[WB_Count];
    // Per-hand controller marker quads (also instances of the same flat
    // textured-quad primitive). Anchored to each grip pose so the user
    // can always see where their virtual controllers are while a game
    // is rendered on the screen quad. [0]=left, [1]=right.
    WristMenuButton    mControllerMarkers[2];
    // User toggle: show controller marker quads while a game is running.
    bool               mShowControllers = true;
    VrFrameSubmitter   mSubmitter;
    EmuWindow_VR_Win*  mEmuWindow = nullptr;

    // Touch state for the right-hand aim raycast against the bottom screen.
    bool               mTouchHeld        = false;
    bool               mLoggedCursorOnce = false;
    bool               mLoggedTouchOnce  = false;
    int                mTouchAttempts    = 0;

    // Per-hand smoothed cursor position (EMA). Applied to the visible
    // cursor only; the touch raycast still uses the raw pose so input
    // stays responsive.
    XrVector3f         mCursorSmoothed[2]      = {};
    bool               mCursorSmoothedValid[2] = {false, false};

    // Menu toggle edge-detect state (left B button + wrist-button hover).

    // Set to true on every menu-open (false->true) transition so the
    // next Frame() can re-pose the menu quad in front of the user's
    // current gaze direction. Avoids the "menu is behind me" UX trap
    // when the user has rotated since the last open.
    bool               mMenuRecenterPending = false;
    // Yaw angle (radians) of the game screen in LOCAL space, and the
    // head position stored at the last recenter (also in LOCAL space).
    // Updated at launch and on every menu recenter so the game screen
    // always shares the same horizontal facing direction as the menu.
    float              mGameYaw    = 0.0f;
    XrVector3f         mGameOrigin = {0.0f, 0.0f, 0.0f};
    // Previous frame's XR predictedDisplayTime, used to derive a real
    // dt for time-based animations (e.g. menu open/close fade).
    XrTime             mLastDisplayTime     = 0;
    // Per-hand cursor click pulse [0..1]. Bumped to 1.0 on a trigger
    // press over any interactive target (wrist button or menu hit),
    // decayed each frame in CursorLayer-friendly units. Drives the
    // cursor scale spike for click feedback.
    float              mCursorClickPulse[2] = {0.0f, 0.0f};
    bool               mLastTriggerEdge[2]  = {false, false};
    // Per-(wrist-button, hand) trigger edge state. [btn][hand].
    bool               mLastWristTrigger[3][2] = {{false,false},{false,false},{false,false}};
    bool               mWristWasHovered[3] = {false, false, false};
    // True while ANY hand is holding the trigger over Start (idx 1) or
    // Select (idx 2). Consumed by VrInputBridge so HID sees a real press.
    bool               mWristBtnHeld[3]    = {false, false, false};

    // ---- Wrist-bar drag-to-place mode ---------------------------------
    // While true, the right-controller aim raycast against an XY plane
    // through the left grip is interpreted as the new wrist-bar offset.
    // Confirm with right trigger release; cancel with right A button.
    // Persisted to vr_config.txt via wrist_offset_{x,y,z} + wrist_tilt_deg.
    bool               mWristDragMode      = false;
    // Latched values during the drag (live preview before commit).
    float              mWristDragOffX      = 0.0f;
    float              mWristDragOffY      = 0.0f;
    float              mWristDragOffZ      = 0.0f;
    // Edge-detect helper for the right-controller A-button (cancel).
    bool               mDragCancelLast     = false;
    // Right-trigger press during drag = commit + exit drag mode.
    bool               mDragCommitLast     = false;
    // Wrist-button long-press (3DS Home synthesis). Per-hand start tick
    // of the trigger-while-hovered hold; 0 = not currently held. Once a
    // single hand crosses kWristHomeHoldNs we fire OnHome() once and
    // suppress the menu-toggle release-edge for that hand.
    long long          mWristHomeStartNs[2] = {0, 0};
    bool               mWristHomeFired[2]   = {false, false};
    static constexpr long long kWristHomeHoldNs = 700'000'000LL;
    // C5: Right System button long-press → recenter view.
    long long          mRecenterStartNs  = 0;
    bool               mRecenterFired    = false;
    // Recenter dim-flash: set to 1.0 when recenter fires, decays over
    // 150 ms to give a SteamVR-style "blink" that signals repositioning.
    float              mRecenterFlash    = 0.0f;
    // C3: Per-hand deferred second haptic pulse for the Success double-thump.
    long long          mHapticSuccessT2Ns[2] = {0, 0};
    // D4: True while the wrist chord (Menu+Start simultaneously) is active.
    bool               mWristChordActive = false;
    // C6: True when the HMD is on the user's head (session FOCUSED vs VISIBLE).
    bool               mHmdOnHead        = true;

    // In-VR ROM browser (lazy first scan in Frame()).
    RomBrowser                                      mRomBrowser;
    std::function<void(const std::string&)>         mOnLoadRom;
    std::function<void(int)>                        mOnLoadState;
    std::function<void()>                           mOnHome;
    class VrInputBridge*                            mInputBridge = nullptr;
    // Welcome card shown once on first launch (triggered by SetShowWelcome).
    bool                                            mWelcomePending    = false;
    // Launcher mode: auto-open the ROM browser on the first rendered frame
    // so the user can pick their game from within VR.
    bool                                            mLauncherMode      = false;
    // Set alongside mLauncherMode to force-expand the ROM Browser header once.
    bool                                            mOpenBrowserOnce   = false;
    // Head-tracking motion: previous frame orientation + predicted display time.
    // Used to compute angular velocity for the 3DS gyroscope.
    XrQuaternionf mPrevHeadQ       = {0.0f, 0.0f, 0.0f, 1.0f};
    XrTime        mPrevDisplayTime  = 0;
    // Last known head pose in LocalSpace (updated every frame). Used to
    // position the launch-fade dim quad in front of the user's eyes.
    XrPosef       mLastHeadPose    = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    // Launch fade: ramps from 1.0 (full black) to 0.0 over 1.5 s so the
    // first frames fade in gracefully instead of snapping to the game image.
    float         mLaunchFade      = 1.0f;

    // ---- Backend perf counters (consumed by in-VR perf overlay) -------
    // EWMA of XR-thread frame interval (ms) and # of frames where the
    // VR blit was dropped because the previous fence had not yet retired.
    float         mFrameIntervalMs = 0.0f;          // EWMA, alpha=0.05
    uint64_t      mDroppedBlits    = 0;             // monotonically increasing
    uint64_t      mFrameCount      = 0;             // monotonically increasing
    // User-toggleable perf HUD (saved to vr_config.txt as show_perf_overlay).
    bool          mShowPerfOverlay = false;

    // Active sidebar tab in the in-VR menu. Default = Library so first
    // launch lands on the ROM browser.
    enum class MenuTab : int {
        Library = 0,
        Display = 1,
        Saves   = 2,
        Input   = 3,
        Session = 4,
        About   = 5,
    };
    MenuTab       mCurrentTab      = MenuTab::Library;
    // Index into the current EntriesSnapshot() of the selected ROM card.
    // -1 means nothing is selected.
    int           mSelectedRomIdx  = -1;
    // Quit-confirm modal: deferred until the user double-confirms via the
    // modal so a stray trigger pull doesn't kill the session.
    bool          mQuitConfirmPending = false;
    // Reset-Display-defaults confirm modal flag.
    bool          mResetDisplayPending = false;
    // Tab-switch fade microanimation. Ramps 0->1 over ~150 ms whenever
    // mCurrentTab changes; content is alpha-blended via PushStyleVar.
    MenuTab       mPrevTab            = MenuTab::Library;
    float         mTabSwitchT         = 1.0f;   // 1.0 = fully settled
    // Settings search filter (Display tab). Plain ASCII, lowered.
    char          mSettingsFilter[64] = {0};
    // Library search filter (Library tab). Substring match against the
    // ROM display name / long title.
    char          mLibraryFilter[64]  = {0};
    // Library sort order: 0 = Name (A-Z), 1 = Last played (newest first),
    // 2 = Filename. Persisted via VRConfig "library_sort".
    int           mLibrarySort        = 0;
};

} // namespace vr_pcvr
