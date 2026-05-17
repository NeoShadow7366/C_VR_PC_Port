// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCVR equivalent of src/android/app/src/main/jni/vr/vr_settings.h.
// HMDType is extended with PCVR HMDs and the passthrough-related fields
// are dropped (Bigscreen Beyond 2 has no passthrough camera).

#pragma once

#include "XrPlatformIncludes.h"

#include <string>

namespace VRSettings {

enum class HMDType {
    UNKNOWN = 0,
    // PCVR
    BIGSCREEN_BEYOND_2,
    VALVE_INDEX,
    VIVE_PRO,
    GENERIC_PCVR,
};

struct Values {
    HMDType     hmd_type                    = HMDType::UNKNOWN;
    uint32_t    resolution_factor           = 0;     // 0 => auto, else 1..6
    int32_t     vr_immersive_mode           = 0;     // 0 = off, 1 = high, 2 = ultra
    int32_t     vr_factor_3d                = 100;   // stereo separation %
    bool        enable_hand_tracking        = true;  // Knuckles capacitive
    bool        enable_haptics              = true;
    std::string preferred_audio_device;              // empty => system default
    // Virtual screen geometry (adjusted in-headset via Settings sliders).
    float       screen_scale                = 1.0f;  // multiplier on base 1.0m quad width
    float       screen_distance             = 1.5f;  // metres in front of user
    float       eye_offset                  = 0.0325f; // per-eye X offset (half-IPD) in metres
    // Auto-resume: if true, last save slot is reloaded on next launch with same ROM.
    bool        auto_resume                 = false;
    // Wrist-button bar offset (in LEFT-GRIP-LOCAL space). Defaults match
    // the historical hardcoded placement: 2 cm forward along the barrel,
    // 8 cm above the back of the hand, no lateral shift, tilted -45 deg
    // around the grip-X axis so the buttons face the user when the arm
    // hangs naturally.
    float       wrist_offset_x              = 0.00f;
    float       wrist_offset_y              = 0.02f;
    float       wrist_offset_z              = 0.08f;
    float       wrist_tilt_deg              = -45.0f;
};

extern Values values;

// Pick a sensible default supersampling factor for an HMD.
uint32_t DefaultResolutionFactorFor(HMDType type);

// Map an XrSystemProperties::systemName string to an HMDType.
HMDType DetectFromSystemName(const std::string& systemName);

// Map a short identifier ("beyond2", "index", "vive", "generic") to an
// HMDType, case-insensitive. Returns UNKNOWN for empty / unrecognised input.
// Used for the CITRA_VR_HMD environment variable override.
HMDType ParseHmdTypeId(const std::string& id);

} // namespace VRSettings
