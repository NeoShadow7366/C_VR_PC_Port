// SPDX-License-Identifier: GPL-3.0-or-later

#include "XrController.h"
#include "windows/WinPlatform.h"   // for OXR()
#include "utils/LogUtils.h"

#include <cstring>
#include <vector>

namespace vr_pcvr {

namespace {

XrAction MakeAction(XrActionSet set, XrActionType type, const char* name, const char* localised,
                    uint32_t subactionCount = 0, const XrPath* subactionPaths = nullptr) {
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType            = type;
    aci.countSubactionPaths   = subactionCount;
    aci.subactionPaths        = const_cast<XrPath*>(subactionPaths);
    std::strncpy(aci.actionName,         name,       XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aci.localizedActionName, localised, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction a = XR_NULL_HANDLE;
    OXR(xrCreateAction(set, &aci, &a));
    return a;
}

XrActionSuggestedBinding Bind(XrInstance inst, XrAction action, const char* path) {
    XrActionSuggestedBinding asb{};
    asb.action = action;
    XrPath p = XR_NULL_PATH;
    OXR(xrStringToPath(inst, path, &p));
    asb.binding = p;
    return asb;
}

template <typename T>
T ReadState(XrSession session, XrAction action, XrPath subactionPath) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action        = action;
    gi.subactionPath = subactionPath;
    T s{};
    if constexpr (std::is_same_v<T, XrActionStateBoolean>) {
        s.type = XR_TYPE_ACTION_STATE_BOOLEAN;
        xrGetActionStateBoolean(session, &gi, &s);
    } else if constexpr (std::is_same_v<T, XrActionStateFloat>) {
        s.type = XR_TYPE_ACTION_STATE_FLOAT;
        xrGetActionStateFloat(session, &gi, &s);
    } else if constexpr (std::is_same_v<T, XrActionStateVector2f>) {
        s.type = XR_TYPE_ACTION_STATE_VECTOR2F;
        xrGetActionStateVector2f(session, &gi, &s);
    }
    return s;
}

} // namespace

XrPath XrController::PathOf(const char* s) const {
    XrPath p = XR_NULL_PATH;
    if (mInstance) OXR(xrStringToPath(mInstance, s, &p));
    return p;
}

bool XrController::Init(XrInstance instance, XrSession session,
                        XrSpace baseSpace, bool enableHandTracking) {
    mInstance  = instance;
    mSession   = session;
    mBaseSpace = baseSpace;

    mHandPath[0] = PathOf("/user/hand/left");
    mHandPath[1] = PathOf("/user/hand/right");

    if (!CreateActionSet())          return false;
    if (!SuggestIndexBindings())     return false;
    SuggestTouchFallbackBindings(); // best-effort

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets      = &mActionSet;
    OXR(xrAttachSessionActionSets(mSession, &attach));

    // Per-hand action spaces.
    for (uint32_t i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci.subactionPath              = mHandPath[i];
        asci.poseInActionSpace.orientation.w = 1.0f;

        asci.action = mAimPoseAction;
        OXR(xrCreateActionSpace(mSession, &asci, &mAimSpace[i]));

        asci.action = mGripPoseAction;
        OXR(xrCreateActionSpace(mSession, &asci, &mGripSpace[i]));
    }

    if (enableHandTracking) {
        mHandTrackingEnabled = InitHandTracking();
    }
    return true;
}

bool XrController::CreateActionSet() {
    XrActionSetCreateInfo aci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(aci.actionSetName,          "citra_controls", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(aci.localizedActionSetName, "Citra Controls", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    aci.priority = 2;
    OXR(xrCreateActionSet(mInstance, &aci, &mActionSet));

    const XrPath subPaths[2] = {mHandPath[0], mHandPath[1]};

    mAimPoseAction  = MakeAction(mActionSet, XR_ACTION_TYPE_POSE_INPUT,    "aim_pose",   "Aim Pose",  2, subPaths);
    mGripPoseAction = MakeAction(mActionSet, XR_ACTION_TYPE_POSE_INPUT,    "grip_pose",  "Grip Pose", 2, subPaths);

    mTriggerClick   = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "trig_click", "Trigger Click", 2, subPaths);
    mTriggerValue   = MakeAction(mActionSet, XR_ACTION_TYPE_FLOAT_INPUT,   "trig_value", "Trigger",       2, subPaths);
    mGripClick      = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "grip_click", "Grip Click",    2, subPaths);
    mGripValue      = MakeAction(mActionSet, XR_ACTION_TYPE_FLOAT_INPUT,   "grip_value", "Grip",          2, subPaths);
    mGripForce      = MakeAction(mActionSet, XR_ACTION_TYPE_FLOAT_INPUT,   "grip_force", "Grip Force",    2, subPaths);
    mThumbstick     = MakeAction(mActionSet, XR_ACTION_TYPE_VECTOR2F_INPUT,"thumbstick", "Thumbstick",    2, subPaths);
    mThumbClick     = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_click","Thumb Click",   2, subPaths);
    mThumbTouch     = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_touch","Thumb Touch",   2, subPaths);
    mTrackpad       = MakeAction(mActionSet, XR_ACTION_TYPE_VECTOR2F_INPUT,"trackpad",   "Trackpad",      2, subPaths);
    mTrackpadTouch  = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "tp_touch",   "Trackpad Touch",2, subPaths);
    mTrackpadForce  = MakeAction(mActionSet, XR_ACTION_TYPE_FLOAT_INPUT,   "tp_force",   "Trackpad Force",2, subPaths);
    mAClick         = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "a_click",    "A",             2, subPaths);
    mBClick         = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "b_click",    "B",             2, subPaths);
    mSystemClick    = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "sys_click",  "System",        2, subPaths);
    mMenuClick      = MakeAction(mActionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_click", "Menu",          2, subPaths);
    mHaptic         = MakeAction(mActionSet, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic",         2, subPaths);
    return true;
}

bool XrController::SuggestIndexBindings() {
    XrPath profile = PathOf("/interaction_profiles/valve/index_controller");
    if (profile == XR_NULL_PATH) return false;

    std::vector<XrActionSuggestedBinding> b;
    auto add = [&](XrAction a, const char* p) { b.push_back(Bind(mInstance, a, p)); };

    // Pose
    add(mAimPoseAction,  "/user/hand/left/input/aim/pose");
    add(mAimPoseAction,  "/user/hand/right/input/aim/pose");
    add(mGripPoseAction, "/user/hand/left/input/grip/pose");
    add(mGripPoseAction, "/user/hand/right/input/grip/pose");

    // Trigger
    add(mTriggerClick, "/user/hand/left/input/trigger/click");
    add(mTriggerClick, "/user/hand/right/input/trigger/click");
    add(mTriggerValue, "/user/hand/left/input/trigger/value");
    add(mTriggerValue, "/user/hand/right/input/trigger/value");

    // Squeeze (Knuckles capacitive grip)
    add(mGripValue, "/user/hand/left/input/squeeze/value");
    add(mGripValue, "/user/hand/right/input/squeeze/value");
    add(mGripForce, "/user/hand/left/input/squeeze/force");
    add(mGripForce, "/user/hand/right/input/squeeze/force");

    // Thumbstick
    add(mThumbstick,  "/user/hand/left/input/thumbstick");
    add(mThumbstick,  "/user/hand/right/input/thumbstick");
    add(mThumbClick,  "/user/hand/left/input/thumbstick/click");
    add(mThumbClick,  "/user/hand/right/input/thumbstick/click");
    add(mThumbTouch,  "/user/hand/left/input/thumbstick/touch");
    add(mThumbTouch,  "/user/hand/right/input/thumbstick/touch");

    // Trackpad
    add(mTrackpad,      "/user/hand/left/input/trackpad");
    add(mTrackpad,      "/user/hand/right/input/trackpad");
    add(mTrackpadTouch, "/user/hand/left/input/trackpad/touch");
    add(mTrackpadTouch, "/user/hand/right/input/trackpad/touch");
    add(mTrackpadForce, "/user/hand/left/input/trackpad/force");
    add(mTrackpadForce, "/user/hand/right/input/trackpad/force");

    // A / B / System
    add(mAClick,      "/user/hand/left/input/a/click");
    add(mAClick,      "/user/hand/right/input/a/click");
    add(mBClick,      "/user/hand/left/input/b/click");
    add(mBClick,      "/user/hand/right/input/b/click");
    add(mSystemClick, "/user/hand/left/input/system/click");
    add(mSystemClick, "/user/hand/right/input/system/click");

    // Haptics
    add(mHaptic, "/user/hand/left/output/haptic");
    add(mHaptic, "/user/hand/right/output/haptic");

    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile     = profile;
    sb.suggestedBindings      = b.data();
    sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
    OXR(xrSuggestInteractionProfileBindings(mInstance, &sb));
    return true;
}

bool XrController::SuggestTouchFallbackBindings() {
    XrPath profile = PathOf("/interaction_profiles/oculus/touch_controller");
    if (profile == XR_NULL_PATH) return false;

    std::vector<XrActionSuggestedBinding> b;
    auto add = [&](XrAction a, const char* p) { b.push_back(Bind(mInstance, a, p)); };

    add(mAimPoseAction,  "/user/hand/left/input/aim/pose");
    add(mAimPoseAction,  "/user/hand/right/input/aim/pose");
    add(mGripPoseAction, "/user/hand/left/input/grip/pose");
    add(mGripPoseAction, "/user/hand/right/input/grip/pose");

    add(mTriggerValue, "/user/hand/left/input/trigger/value");
    add(mTriggerValue, "/user/hand/right/input/trigger/value");
    add(mGripValue,    "/user/hand/left/input/squeeze/value");
    add(mGripValue,    "/user/hand/right/input/squeeze/value");

    add(mThumbstick,  "/user/hand/left/input/thumbstick");
    add(mThumbstick,  "/user/hand/right/input/thumbstick");
    add(mThumbClick,  "/user/hand/left/input/thumbstick/click");
    add(mThumbClick,  "/user/hand/right/input/thumbstick/click");

    add(mAClick,    "/user/hand/right/input/a/click");
    add(mBClick,    "/user/hand/right/input/b/click");
    add(mMenuClick, "/user/hand/left/input/menu/click");

    add(mHaptic, "/user/hand/left/output/haptic");
    add(mHaptic, "/user/hand/right/output/haptic");

    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile     = profile;
    sb.suggestedBindings      = b.data();
    sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
    OXR(xrSuggestInteractionProfileBindings(mInstance, &sb));
    return true;
}

bool XrController::InitHandTracking() {
    OXR(xrGetInstanceProcAddr(mInstance, "xrCreateHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateHandTracker)));
    OXR(xrGetInstanceProcAddr(mInstance, "xrDestroyHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyHandTracker)));
    OXR(xrGetInstanceProcAddr(mInstance, "xrLocateHandJointsEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnLocateHandJoints)));
    if (!pfnCreateHandTracker || !pfnLocateHandJoints) {
        ALOGW("XR_EXT_hand_tracking entry points missing - hand tracking disabled");
        return false;
    }

    for (uint32_t i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand         = (i == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (XR_FAILED(pfnCreateHandTracker(mSession, &ci, &mHandTracker[i]))) {
            ALOGW("xrCreateHandTrackerEXT(%s) failed", i ? "right" : "left");
            mHandTracker[i] = XR_NULL_HANDLE;
        }
    }
    return mHandTracker[0] != XR_NULL_HANDLE || mHandTracker[1] != XR_NULL_HANDLE;
}

void XrController::SyncFrame(XrTime predictedDisplayTime) {
    if (mActionSet == XR_NULL_HANDLE) return;

    XrActiveActionSet active{};
    active.actionSet     = mActionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &active;
    if (XR_FAILED(xrSyncActions(mSession, &sync))) {
        return;
    }

    for (uint32_t i = 0; i < 2; ++i) {
        ControllerState& s = mState[i];
        const XrPath sp = mHandPath[i];

        s.trigger_click   = ReadState<XrActionStateBoolean>(mSession, mTriggerClick,  sp).currentState;
        s.trigger_value   = ReadState<XrActionStateFloat>  (mSession, mTriggerValue,  sp).currentState;
        s.grip_click      = ReadState<XrActionStateBoolean>(mSession, mGripClick,     sp).currentState;
        s.grip_value      = ReadState<XrActionStateFloat>  (mSession, mGripValue,     sp).currentState;
        s.grip_force      = ReadState<XrActionStateFloat>  (mSession, mGripForce,     sp).currentState;
        const auto stick  = ReadState<XrActionStateVector2f>(mSession, mThumbstick,   sp).currentState;
        s.thumbstick      = stick;
        s.thumb_click     = ReadState<XrActionStateBoolean>(mSession, mThumbClick,    sp).currentState;
        s.thumb_touch     = ReadState<XrActionStateBoolean>(mSession, mThumbTouch,    sp).currentState;
        const auto pad    = ReadState<XrActionStateVector2f>(mSession, mTrackpad,     sp).currentState;
        s.trackpad        = pad;
        s.trackpad_touch  = ReadState<XrActionStateBoolean>(mSession, mTrackpadTouch, sp).currentState;
        s.trackpad_force  = ReadState<XrActionStateFloat>  (mSession, mTrackpadForce, sp).currentState;
        s.a_click         = ReadState<XrActionStateBoolean>(mSession, mAClick,        sp).currentState;
        s.b_click         = ReadState<XrActionStateBoolean>(mSession, mBClick,        sp).currentState;
        s.system_click    = ReadState<XrActionStateBoolean>(mSession, mSystemClick,   sp).currentState;
        s.menu_click      = ReadState<XrActionStateBoolean>(mSession, mMenuClick,     sp).currentState;

        // Locate aim/grip poses.
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(mAimSpace[i], mBaseSpace, predictedDisplayTime, &aim)) &&
            (aim.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (aim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            s.aim_pose = aim.pose;
            s.aim_pose_valid = true;
        } else {
            s.aim_pose_valid = false;
        }
        XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(mGripSpace[i], mBaseSpace, predictedDisplayTime, &grip)) &&
            (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (grip.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            s.grip_pose       = grip.pose;
            s.grip_pose_valid = true;
        } else {
            s.grip_pose_valid = false;
        }

        // Hand joints.
        HandJointSnapshot& js = mJoints[i];
        js.valid = false;
        if (mHandTrackingEnabled && mHandTracker[i] != XR_NULL_HANDLE && pfnLocateHandJoints) {
            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            li.baseSpace = mBaseSpace;
            li.time      = predictedDisplayTime;

            XrHandJointLocationEXT locs[XR_HAND_JOINT_COUNT_EXT] = {};
            XrHandJointLocationsEXT out{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            out.jointCount     = XR_HAND_JOINT_COUNT_EXT;
            out.jointLocations = locs;
            if (XR_SUCCEEDED(pfnLocateHandJoints(mHandTracker[i], &li, &out)) && out.isActive) {
                js.valid = true;
                for (uint32_t j = 0; j < HandJointSnapshot::kJointCount; ++j) {
                    js.poses[j] = locs[j].pose;
                }
            }
        }
    }
}

void XrController::TriggerHaptic(Hand h, float amplitude, XrDuration durationNs, float frequencyHz) {
    if (mHaptic == XR_NULL_HANDLE) return;
    const uint32_t i = static_cast<uint32_t>(h);

    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration  = durationNs;
    v.frequency = frequencyHz > 0.0f ? frequencyHz : XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO};
    ai.action        = mHaptic;
    ai.subactionPath = mHandPath[i];

    xrApplyHapticFeedback(mSession, &ai, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

void XrController::Destroy() {
    if (pfnDestroyHandTracker) {
        for (auto& t : mHandTracker) {
            if (t != XR_NULL_HANDLE) { pfnDestroyHandTracker(t); t = XR_NULL_HANDLE; }
        }
    }
    for (auto& s : mAimSpace)  { if (s != XR_NULL_HANDLE) { xrDestroySpace(s); s = XR_NULL_HANDLE; } }
    for (auto& s : mGripSpace) { if (s != XR_NULL_HANDLE) { xrDestroySpace(s); s = XR_NULL_HANDLE; } }
    if (mActionSet != XR_NULL_HANDLE) { xrDestroyActionSet(mActionSet); mActionSet = XR_NULL_HANDLE; }
}

} // namespace vr_pcvr
