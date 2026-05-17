// SPDX-License-Identifier: GPL-3.0-or-later

#include "VrInputBridge.h"

#include "common/param_package.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "common/vr_config.h"
#include "core/frontend/input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace vr_pcvr {

namespace {

constexpr const char* kEngine = "vr";

// Singleton state pointer used by the device adapters. Only one
// VrInputBridge ever exists per process (one VR session), so a raw
// pointer suffices and avoids per-Create lookups through a registry.
VrInputBridge::State* g_state = nullptr;

class VrButtonDevice final : public Input::ButtonDevice {
public:
    explicit VrButtonDevice(int id) : mId(id) {}
    bool GetStatus() const override {
        if (!g_state || mId < 0 || mId >= VrInputBridge::NumVrButtons) return false;
        return g_state->buttons[mId].load(std::memory_order_relaxed);
    }
private:
    int mId;
};

class VrAnalogDevice final : public Input::AnalogDevice {
public:
    explicit VrAnalogDevice(int id) : mId(id) {}
    std::tuple<float, float> GetStatus() const override {
        if (!g_state || mId < 0 || mId >= VrInputBridge::NumVrAnalogs) return {0.0f, 0.0f};
        const auto x = g_state->analog_xy[mId * 2 + 0].load(std::memory_order_relaxed);
        const auto y = g_state->analog_xy[mId * 2 + 1].load(std::memory_order_relaxed);
        return {x, y};
    }
private:
    int mId;
};

class VrButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VrButtonDevice>(params.Get("id", 0));
    }
};

class VrAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VrAnalogDevice>(params.Get("id", 0));
    }
};

// Motion device: reads head-tracking accel + gyro from the shared atomic
// state that VrApp pushes every frame.
class VrMotionDevice final : public Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        if (!g_state) return {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
        const Common::Vec3<float> accel{
            g_state->motion_accel[0].load(std::memory_order_relaxed),
            g_state->motion_accel[1].load(std::memory_order_relaxed),
            g_state->motion_accel[2].load(std::memory_order_relaxed),
        };
        const Common::Vec3<float> gyro{
            g_state->motion_gyro[0].load(std::memory_order_relaxed),
            g_state->motion_gyro[1].load(std::memory_order_relaxed),
            g_state->motion_gyro[2].load(std::memory_order_relaxed),
        };
        return {accel, gyro};
    }
};

class VrMotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<VrMotionDevice>();
    }
};

std::string ButtonParam(int id) {
    Common::ParamPackage p{{"engine", kEngine}, {"id", std::to_string(id)}};
    return p.Serialize();
}

std::string AnalogParam(int id) {
    Common::ParamPackage p{{"engine", kEngine}, {"id", std::to_string(id)}};
    return p.Serialize();
}

float ApplyDeadzone(float v, float dz) {
    if (std::fabs(v) < dz) return 0.0f;
    const float sign  = v < 0.0f ? -1.0f : 1.0f;
    const float scale = (std::fabs(v) - dz) / (1.0f - dz);
    return sign * std::clamp(scale, 0.0f, 1.0f);
}

} // namespace

VrInputBridge::VrInputBridge() {
    g_state = &mState;
    for (auto& b : mState.buttons)   b.store(false);
    for (auto& a : mState.analog_xy) a.store(0.0f);
    // Initialise bindings to defaults; LoadBindings() may override.
    for (int i = 0; i < NumVrButtons; ++i) {
        mBindings[i] = DefaultBinding(static_cast<VrButtonId>(i));
    }
}

VrInputBridge::~VrInputBridge() {
    UnregisterFactories();
    if (g_state == &mState) g_state = nullptr;
}

void VrInputBridge::RegisterFactories() {
    if (mFactoriesRegistered) return;
    Input::RegisterFactory<Input::ButtonDevice>(kEngine, std::make_shared<VrButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>(kEngine, std::make_shared<VrAnalogFactory>());
    // Override the default motion_emu factory with our head-tracking one.
    // InputCommon::Init() registers the default first; we stomp it here so
    // that HID's Input::CreateDevice<MotionDevice>("engine:motion_emu") gets
    // our implementation instead of the mouse-tilt emulator.
    Input::RegisterFactory<Input::MotionDevice>("motion_emu",
                                                std::make_shared<VrMotionFactory>());
    mFactoriesRegistered = true;
}

void VrInputBridge::UnregisterFactories() {
    if (!mFactoriesRegistered) return;
    Input::UnregisterFactory<Input::ButtonDevice>(kEngine);
    Input::UnregisterFactory<Input::AnalogDevice>(kEngine);
    Input::UnregisterFactory<Input::MotionDevice>("motion_emu");
    mFactoriesRegistered = false;
}

void VrInputBridge::OverrideInputProfile() {
    auto& profile = Settings::values.current_input_profile;

    // Map every 3DS native button to a vr engine id (or to the "null"
    // engine for buttons we deliberately don't drive).
    using namespace Settings::NativeButton;
    profile.buttons[A]      = ButtonParam(Btn_A);
    profile.buttons[B]      = ButtonParam(Btn_B);
    profile.buttons[X]      = ButtonParam(Btn_X);
    profile.buttons[Y]      = ButtonParam(Btn_Y);
    profile.buttons[Up]     = ButtonParam(Btn_Up);
    profile.buttons[Down]   = ButtonParam(Btn_Down);
    profile.buttons[Left]   = ButtonParam(Btn_Left);
    profile.buttons[Right]  = ButtonParam(Btn_Right);
    profile.buttons[L]      = ButtonParam(Btn_L);
    profile.buttons[R]      = ButtonParam(Btn_R);
    profile.buttons[Start]  = ButtonParam(Btn_Start);
    profile.buttons[Select] = ButtonParam(Btn_Select);
    profile.buttons[ZL]     = ButtonParam(Btn_ZL);
    profile.buttons[ZR]     = ButtonParam(Btn_ZR);
    profile.buttons[Home]   = ButtonParam(Btn_Home);

    // Debug / Gpio14 / Power are not driven by the VR controllers - leave
    // them as the "null" engine so HID treats them as permanently released.
    profile.buttons[Debug]  = "engine:null";
    profile.buttons[Gpio14] = "engine:null";
    profile.buttons[Power]  = "engine:null";

    using namespace Settings::NativeAnalog;
    profile.analogs[CirclePad] = AnalogParam(Analog_CirclePad);
    profile.analogs[CStick]    = AnalogParam(Analog_CStick);

    // Point the motion device at our head-tracking implementation.
    // The motion_emu engine name is already registered by RegisterFactories().
    profile.motion_device = "engine:motion_emu";
}

void VrInputBridge::UpdateMotion(const float accel[3], const float gyro[3]) {
    for (int i = 0; i < 3; ++i) {
        mState.motion_accel[i].store(accel[i], std::memory_order_relaxed);
        mState.motion_gyro[i].store(gyro[i],   std::memory_order_relaxed);
    }
}

void VrInputBridge::Apply(const XrController& controller, bool menu_open,
                          bool right_touch_active,
                          bool wrist_start_held,
                          bool wrist_select_held) {
    // While the in-VR menu is open, suppress every 3DS HID input. The
    // menu-toggle frame would otherwise leak (left B = 3DS Y, trigger
    // pulls = 3DS L/R, etc.) into the running game even though the core
    // RunLoop is paused, and the leaked press would latch on resume.
    if (menu_open) {
        for (auto& b : mState.buttons)   b.store(false, std::memory_order_relaxed);
        for (auto& a : mState.analog_xy) a.store(0.0f,  std::memory_order_relaxed);
        // Reset latches so the next non-menu frame starts clean.
        mGripLatched[0] = mGripLatched[1] = false;
        mHomePulseQueued = false;
        return;
    }

    const auto& l = controller.State(Hand::Left);
    const auto& r = controller.State(Hand::Right);

    // Resolve a Src to a digital boolean. Triggers/grips use the same
    // thresholds as before; grips additionally honour the latch state.
    auto src_bool = [&](Src s) -> bool {
        switch (s) {
        case Src::None:      return false;
        case Src::L_A:       return l.a_click;
        case Src::L_B:       return l.b_click;
        case Src::R_A:       return r.a_click;
        case Src::R_B:       return r.b_click;
        case Src::L_Trigger: return l.trigger_click || l.trigger_value > 0.6f;
        case Src::R_Trigger: return r.trigger_click || r.trigger_value > 0.6f;
        case Src::L_Grip:    return mGripLatched[0];
        case Src::R_Grip:    return mGripLatched[1];
        case Src::L_Thumb:   return l.thumb_click;
        case Src::R_Thumb:   return r.thumb_click;
        case Src::L_System:  return l.system_click || l.menu_click;
        case Src::R_System:  return r.system_click || r.menu_click;
        case Src::Count:     return false;
        }
        return false;
    };

    // Update grip hysteresis BEFORE we resolve any L_Grip / R_Grip.
    auto grip_apply = [&](int hand, float v) {
        if (mGripLatched[hand]) {
            if (v < kGripRelease) mGripLatched[hand] = false;
        } else {
            if (v > kGripEnter)   mGripLatched[hand] = true;
        }
    };
    grip_apply(0, std::max(l.grip_value, l.grip_force));
    grip_apply(1, std::max(r.grip_value, r.grip_force));

    // Resolve all remappable digital buttons through the bindings table.
    bool btn[NumVrButtons] = {};
    for (int i = 0; i < NumVrButtons; ++i) {
        btn[i] = src_bool(mBindings[i]);
    }

    // Right-trigger gating: when the right trigger is currently driving
    // the bottom-screen touch, suppress whichever 3DS button is bound
    // to R_Trigger so a single trigger pull doesn't both touch and fire.
    if (right_touch_active) {
        for (int i = 0; i < NumVrButtons; ++i) {
            if (mBindings[i] == Src::R_Trigger) btn[i] = false;
        }
    }

    // Wrist Start / Select OR-in (regardless of binding so the wrist
    // buttons always work even if the user has remapped Start/Select).
    if (wrist_start_held)  btn[Btn_Start]  = true;
    if (wrist_select_held) btn[Btn_Select] = true;

    // Home: one-shot pulse from the wrist long-press path.
    if (mHomePulseQueued) {
        btn[Btn_Home]    = true;
        mHomePulseQueued = false;
    }

    // D-pad: prefer right-hand trackpad quadrants (Knuckles); fall back
    // to the left thumbstick if the trackpad isn't being touched.
    bool dp_up = false, dp_down = false, dp_left = false, dp_right = false;
    if (r.trackpad_touch) {
        dp_right = r.trackpad.x >  kTrackpadEdge;
        dp_left  = r.trackpad.x < -kTrackpadEdge;
        dp_up    = r.trackpad.y >  kTrackpadEdge;
        dp_down  = r.trackpad.y < -kTrackpadEdge;
    } else if (std::fabs(l.thumbstick.x) > 0.7f || std::fabs(l.thumbstick.y) > 0.7f) {
        dp_right = l.thumbstick.x >  0.7f;
        dp_left  = l.thumbstick.x < -0.7f;
        dp_up    = l.thumbstick.y >  0.7f;
        dp_down  = l.thumbstick.y < -0.7f;
    }
    btn[Btn_Up]    = dp_up;
    btn[Btn_Down]  = dp_down;
    btn[Btn_Left]  = dp_left;
    btn[Btn_Right] = dp_right;

    for (int i = 0; i < NumVrButtons; ++i) {
        mState.buttons[i].store(btn[i], std::memory_order_relaxed);
    }

    // Analog sticks. Apply a small dead-zone to suppress Knuckles drift.
    const float lx = ApplyDeadzone(l.thumbstick.x, kStickDeadzone);
    const float ly = ApplyDeadzone(l.thumbstick.y, kStickDeadzone);
    const float rx = ApplyDeadzone(r.thumbstick.x, kStickDeadzone);
    const float ry = ApplyDeadzone(r.thumbstick.y, kStickDeadzone);

    mState.analog_xy[Analog_CirclePad * 2 + 0].store(lx, std::memory_order_relaxed);
    mState.analog_xy[Analog_CirclePad * 2 + 1].store(ly, std::memory_order_relaxed);
    mState.analog_xy[Analog_CStick   * 2 + 0].store(rx, std::memory_order_relaxed);
    mState.analog_xy[Analog_CStick   * 2 + 1].store(ry, std::memory_order_relaxed);
}

// ---- Source name table + persistence ----------------------------------------

namespace {
struct SrcEntry { VrInputBridge::Src s; const char* name; };
constexpr SrcEntry kSrcTable[] = {
    {VrInputBridge::Src::None,      "none"},
    {VrInputBridge::Src::L_A,       "left_a"},
    {VrInputBridge::Src::L_B,       "left_b"},
    {VrInputBridge::Src::R_A,       "right_a"},
    {VrInputBridge::Src::R_B,       "right_b"},
    {VrInputBridge::Src::L_Trigger, "left_trigger"},
    {VrInputBridge::Src::R_Trigger, "right_trigger"},
    {VrInputBridge::Src::L_Grip,    "left_grip"},
    {VrInputBridge::Src::R_Grip,    "right_grip"},
    {VrInputBridge::Src::L_Thumb,   "left_thumb"},
    {VrInputBridge::Src::R_Thumb,   "right_thumb"},
    {VrInputBridge::Src::L_System,  "left_system"},
    {VrInputBridge::Src::R_System,  "right_system"},
};
struct BindKey { VrInputBridge::VrButtonId id; const char* key; };
constexpr BindKey kBindKeys[] = {
    {VrInputBridge::Btn_A,      "bind_a"},
    {VrInputBridge::Btn_B,      "bind_b"},
    {VrInputBridge::Btn_X,      "bind_x"},
    {VrInputBridge::Btn_Y,      "bind_y"},
    {VrInputBridge::Btn_L,      "bind_l"},
    {VrInputBridge::Btn_R,      "bind_r"},
    {VrInputBridge::Btn_ZL,     "bind_zl"},
    {VrInputBridge::Btn_ZR,     "bind_zr"},
    {VrInputBridge::Btn_Start,  "bind_start"},
    {VrInputBridge::Btn_Select, "bind_select"},
};
} // namespace

const char* VrInputBridge::SrcName(Src s) {
    for (const auto& e : kSrcTable) if (e.s == s) return e.name;
    return "none";
}

VrInputBridge::Src VrInputBridge::ParseSrc(const char* name) {
    if (!name) return Src::None;
    for (const auto& e : kSrcTable) if (std::strcmp(e.name, name) == 0) return e.s;
    return Src::None;
}

VrInputBridge::Src VrInputBridge::DefaultBinding(VrButtonId id) {
    switch (id) {
    case Btn_A:      return Src::R_A;
    case Btn_B:      return Src::R_B;
    case Btn_X:      return Src::L_A;
    case Btn_Y:      return Src::L_B;
    case Btn_L:      return Src::L_Trigger;
    case Btn_R:      return Src::R_Trigger;
    case Btn_ZL:     return Src::L_Grip;
    case Btn_ZR:     return Src::R_Grip;
    case Btn_Start:  return Src::R_System;
    case Btn_Select: return Src::L_System;
    default:         return Src::None;
    }
}

void VrInputBridge::SetBinding(VrButtonId id, Src src) {
    if (id < 0 || id >= NumVrButtons) return;
    mBindings[id] = src;
}

void VrInputBridge::SaveBindings() const {
    for (const auto& bk : kBindKeys) {
        Common::VRConfig::Set(bk.key, SrcName(mBindings[bk.id]));
    }
}

void VrInputBridge::LoadBindings() {
    for (const auto& bk : kBindKeys) {
        const std::string v = Common::VRConfig::Get(bk.key);
        if (!v.empty()) {
            const Src parsed = ParseSrc(v.c_str());
            mBindings[bk.id] = parsed;
        }
    }
}

} // namespace vr_pcvr
