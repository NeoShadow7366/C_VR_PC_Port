// SPDX-License-Identifier: GPL-3.0-or-later
//
// VrInputBridge - translates per-frame XrController state into Citra HID
// inputs by registering a dedicated `vr` input engine in InputCommon and
// rewriting the active InputProfile so 3DS buttons / circle-pad / c-stick
// are read straight from the OpenXR action set.
//
// The mapping follows plan §2.1.5:
//
//   3DS A          <- right A click
//   3DS B          <- right B click
//   3DS X          <- left  A click
//   3DS Y          <- left  B click
//   3DS L          <- left  trigger click
//   3DS R          <- right trigger click
//   3DS ZL         <- left  squeeze value > kGripThreshold
//   3DS ZR         <- right squeeze value > kGripThreshold
//   3DS Start      <- right system click
//   3DS Select     <- left  system click
//   3DS Up/Down/   <- right (or left) trackpad quadrants when touched
//      Left/Right
//   Circle pad     <- left  thumbstick
//   C-stick        <- right thumbstick

#pragma once

#include "XrController.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace vr_pcvr {

class VrInputBridge {
public:
    // Distinct virtual id per 3DS button we drive. Kept in step with the
    // engine-level "id" parameter we encode into the InputProfile.
    enum VrButtonId : int {
        Btn_A = 0,
        Btn_B,
        Btn_X,
        Btn_Y,
        Btn_Up,
        Btn_Down,
        Btn_Left,
        Btn_Right,
        Btn_L,
        Btn_R,
        Btn_Start,
        Btn_Select,
        Btn_ZL,
        Btn_ZR,
        Btn_Home,
        NumVrButtons,
    };

    enum VrAnalogId : int {
        Analog_CirclePad = 0,
        Analog_CStick,
        NumVrAnalogs,
    };

    static constexpr float kGripThreshold = 0.5f;
    // Grip-with-hysteresis: latch on at kGripEnter, release at kGripRelease.
    static constexpr float kGripEnter     = 0.55f;
    static constexpr float kGripRelease   = 0.30f;
    static constexpr float kStickDeadzone = 0.15f;
    static constexpr float kTrackpadEdge  = 0.4f;
    // Right system-click hold time to synthesise 3DS Home (nanoseconds).
    static constexpr long long kHomeHoldNs = 600'000'000LL; // 600 ms

    VrInputBridge();
    ~VrInputBridge();

    VrInputBridge(const VrInputBridge&)            = delete;
    VrInputBridge& operator=(const VrInputBridge&) = delete;

    // Register the "vr" engine factories in InputCommon. Must be called
    // after InputCommon::Init() (which runs implicitly from
    // Settings::Apply).
    void RegisterFactories();
    void UnregisterFactories();

    // Rewrite Settings::values.current_input_profile so its 3DS button /
    // analog bindings point at the "vr" engine ids above. Must be called
    // before Core::System::Load so HID picks up the new bindings.
    void OverrideInputProfile();

    // Per-frame: read both controllers and update the atomic state the
    // ButtonDevice / AnalogDevice instances expose to HID. Safe to call
    // from any thread.
    //
    //  menu_open          - the in-VR menu is up; zero ALL inputs so the
    //                       toggle / interaction frames don't leak into
    //                       3DS HID (e.g. left B = 3DS Y was firing on
    //                       every menu open/close).
    //  right_touch_active - the right trigger is currently driving the
    //                       bottom-screen touch raycast; suppress 3DS R
    //                       so a single trigger pull doesn't both touch
    //                       and fire R.
    // Physical controller source for a remappable 3DS button. Only the
    // digital face / shoulder / system buttons are remappable; dpad and
    // analog sticks keep their hard-coded mapping (right trackpad +
    // left thumbstick respectively).
    enum class Src : uint8_t {
        None = 0,
        L_A, L_B, R_A, R_B,
        L_Trigger, R_Trigger,
        L_Grip,    R_Grip,
        L_Thumb,   R_Thumb,
        L_System,  R_System,
        Count
    };
    static const char* SrcName(Src s);
    static Src         ParseSrc(const char* name);
    // Default source for each VrButtonId (matches plan §2.1.5).
    static Src         DefaultBinding(VrButtonId id);

    void SetBinding(VrButtonId id, Src src);
    Src  GetBinding(VrButtonId id) const { return mBindings[id]; }

    // Persist the current binding table to vr_config.txt under keys
    // "bind_a", "bind_b", ... using SrcName(). LoadBindings reads the
    // same keys back; both default to the hard-coded mapping when the
    // key is missing or unrecognised.
    void SaveBindings() const;
    void LoadBindings();

    void Apply(const XrController& controller,
               bool menu_open          = false,
               bool right_touch_active = false,
               bool wrist_start_held   = false,
               bool wrist_select_held  = false);

    // Queue a single-frame 3DS Home press. Cleared by Apply() at end
    // of the next call. Safe to call from the XR thread.
    void FireHome() { mHomePulseQueued = true; }

    // Backing state - public so the small Device adapter classes in the
    // .cpp can take a stable reference into this array without friending.
    struct State {
        std::array<std::atomic<bool>,                     NumVrButtons>  buttons{};
        std::array<std::atomic<float>,                    NumVrAnalogs * 2> analog_xy{};
        // Head-tracking motion: accelerometer (g) and gyroscope (deg/sec)
        // in the 3DS body frame. Updated every frame by VrApp.
        std::atomic<float> motion_accel[3] = {0.0f, -1.0f, 0.0f}; // gravity at rest
        std::atomic<float> motion_gyro[3]  = {0.0f,  0.0f, 0.0f};
    };
    State& GetState() { return mState; }

    // Update the motion state from VrApp. accel is in g-units, gyro in deg/sec.
    // Thread-safe; called from the XR render thread each frame.
    void UpdateMotion(const float accel[3], const float gyro[3]);

private:
    State mState;
    bool  mFactoriesRegistered = false;

    // Grip hysteresis state (per hand). Once a grip exceeds kGripEnter
    // it latches ZL/ZR "on" until it falls below kGripRelease, which
    // stops Knuckles capacitive-grip chatter.
    bool mGripLatched[2] = {false, false};

    // Home is fired by VrApp (wrist-button long-press); Apply() raises
    // Btn_Home for exactly one frame after FireHome() is called.
    bool mHomePulseQueued = false;

    // Per-button physical source. Defaults populated in ctor.
    Src  mBindings[NumVrButtons]{};
};

} // namespace vr_pcvr
