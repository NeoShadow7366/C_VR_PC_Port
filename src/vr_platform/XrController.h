// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCVR controller / hand-tracking surface. Modelled after
// src/android/app/src/main/jni/vr/XrController.{h,cpp} but rebuilt for
// Valve Index "Knuckles" (full analogue trigger + grip force + thumbstick
// + trackpad + A/B + system) with a Touch fallback profile, plus
// XR_EXT_hand_tracking when available, plus xrApplyHapticFeedback.
//
// No JNI. The frontend wires `mState` straight into Citra's HID via
// the platform-agnostic VrApp glue layer.

#pragma once

#include "XrPlatformIncludes.h"

#include <array>
#include <cstdint>

namespace vr_pcvr {

enum class Hand : uint32_t { Left = 0, Right = 1, Count = 2 };

// One-frame snapshot of controller state, regardless of which interaction
// profile is currently active. Ranges are normalised: triggers / grip /
// trackpad force in [0..1], thumbstick in [-1..1].
struct ControllerState {
    bool   trigger_click  = false;
    float  trigger_value  = 0.0f;
    bool   grip_click     = false;
    float  grip_value     = 0.0f;
    float  grip_force     = 0.0f;     // Knuckles capacitive force, 0 on Touch
    XrVector2f thumbstick = {0, 0};
    bool   thumb_click    = false;
    bool   thumb_touch    = false;
    XrVector2f trackpad   = {0, 0};   // Knuckles only
    bool   trackpad_touch = false;
    float  trackpad_force = 0.0f;
    bool   a_click        = false;
    bool   b_click        = false;
    bool   system_click   = false;
    bool   menu_click     = false;
    XrPosef aim_pose       = {{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef grip_pose      = {{0, 0, 0, 1}, {0, 0, 0}};
    bool   aim_pose_valid  = false;
    bool   grip_pose_valid = false;
};

struct HandJointSnapshot {
    bool valid = false;
    static constexpr uint32_t kJointCount = 26; // XR_HAND_JOINT_COUNT_EXT
    XrPosef poses[kJointCount];
};

class XrController {
public:
    XrController()  = default;
    ~XrController() { Destroy(); }

    XrController(const XrController&)            = delete;
    XrController& operator=(const XrController&) = delete;

    // Build the action set, suggest bindings for Index + Touch (fallback),
    // attach to the session and create per-hand action spaces. Optionally
    // initialises XR_EXT_hand_tracking when the runtime supports it.
    bool Init(XrInstance instance, XrSession session,
              XrSpace baseSpace, bool enableHandTracking);

    void Destroy();

    // Per-frame: sync actions and refresh mState[]. predictedDisplayTime
    // is the value returned by xrWaitFrame; it is used both to locate the
    // pose actions and (if enabled) to query hand joints.
    void SyncFrame(XrTime predictedDisplayTime);

    const ControllerState&    State(Hand h)     const { return mState[static_cast<uint32_t>(h)];  }
    const HandJointSnapshot&  HandJoints(Hand h) const { return mJoints[static_cast<uint32_t>(h)]; }

    // amplitude in [0..1], duration in nanoseconds (XR_MIN_HAPTIC_DURATION
    // for "as short as possible"), frequency in Hz (0 = unspecified).
    void TriggerHaptic(Hand h, float amplitude, XrDuration durationNs, float frequencyHz = 0.0f);

private:
    bool CreateActionSet();
    bool SuggestIndexBindings();
    bool SuggestTouchFallbackBindings();
    bool InitHandTracking();

    XrPath PathOf(const char* str) const;

    // Owned XR objects.
    XrInstance  mInstance     = XR_NULL_HANDLE;
    XrSession   mSession      = XR_NULL_HANDLE;
    XrSpace     mBaseSpace    = XR_NULL_HANDLE;
    XrActionSet mActionSet    = XR_NULL_HANDLE;

    XrPath mHandPath[2]{XR_NULL_PATH, XR_NULL_PATH};

    // Pose actions + per-hand action spaces.
    XrAction mAimPoseAction   = XR_NULL_HANDLE;
    XrAction mGripPoseAction  = XR_NULL_HANDLE;
    XrSpace  mAimSpace[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace  mGripSpace[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};

    // Digital / analogue input actions.
    XrAction mTriggerClick = XR_NULL_HANDLE;
    XrAction mTriggerValue = XR_NULL_HANDLE;
    XrAction mGripClick    = XR_NULL_HANDLE;
    XrAction mGripValue    = XR_NULL_HANDLE;
    XrAction mGripForce    = XR_NULL_HANDLE;     // Knuckles only
    XrAction mThumbstick   = XR_NULL_HANDLE;
    XrAction mThumbClick   = XR_NULL_HANDLE;
    XrAction mThumbTouch   = XR_NULL_HANDLE;
    XrAction mTrackpad     = XR_NULL_HANDLE;     // Knuckles only
    XrAction mTrackpadTouch = XR_NULL_HANDLE;
    XrAction mTrackpadForce = XR_NULL_HANDLE;
    XrAction mAClick       = XR_NULL_HANDLE;
    XrAction mBClick       = XR_NULL_HANDLE;
    XrAction mSystemClick  = XR_NULL_HANDLE;
    XrAction mMenuClick    = XR_NULL_HANDLE;     // Touch fallback only
    XrAction mHaptic       = XR_NULL_HANDLE;

    // XR_EXT_hand_tracking (optional).
    bool                     mHandTrackingEnabled = false;
    XrHandTrackerEXT         mHandTracker[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    PFN_xrCreateHandTrackerEXT  pfnCreateHandTracker  = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker = nullptr;
    PFN_xrLocateHandJointsEXT   pfnLocateHandJoints   = nullptr;

    // Per-frame state.
    std::array<ControllerState,    2> mState{};
    std::array<HandJointSnapshot,  2> mJoints{};
};

} // namespace vr_pcvr
