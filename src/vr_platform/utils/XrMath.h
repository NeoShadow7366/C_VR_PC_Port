// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal pose / quaternion helpers used by the VR layer composition code.
// Intentionally header-only and dependency-free so it can be shared with
// the Android port without modification.

#pragma once

#include "../XrPlatformIncludes.h"

#include <cmath>

#ifndef MATH_FLOAT_PI
#  define MATH_FLOAT_PI 3.14159265358979323846f
#endif

namespace XrMath {

struct Quatf {
    static constexpr XrQuaternionf Identity() { return XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f}; }

    static XrQuaternionf FromEuler(float pitchRad, float yawRad, float rollRad) {
        const float cy = std::cos(yawRad   * 0.5f);
        const float sy = std::sin(yawRad   * 0.5f);
        const float cp = std::cos(pitchRad * 0.5f);
        const float sp = std::sin(pitchRad * 0.5f);
        const float cr = std::cos(rollRad  * 0.5f);
        const float sr = std::sin(rollRad  * 0.5f);
        return XrQuaternionf{
            sp * cy * cr - cp * sy * sr,
            cp * sy * cr + sp * cy * sr,
            cp * cy * sr - sp * sy * cr,
            cp * cy * cr + sp * sy * sr};
    }
};

struct Posef {
    static constexpr XrPosef Identity() {
        return XrPosef{Quatf::Identity(), XrVector3f{0.0f, 0.0f, 0.0f}};
    }

    // Transform a point by a pose: result = pose.position + pose.orientation * v.
    static XrVector3f Transform(const XrPosef& pose, const XrVector3f& v) {
        // q * v * q^-1 with q = (x,y,z,w)
        const float x = pose.orientation.x;
        const float y = pose.orientation.y;
        const float z = pose.orientation.z;
        const float w = pose.orientation.w;
        const float ix =  w * v.x + y * v.z - z * v.y;
        const float iy =  w * v.y + z * v.x - x * v.z;
        const float iz =  w * v.z + x * v.y - y * v.x;
        const float iw = -x * v.x - y * v.y - z * v.z;
        return XrVector3f{
            ix * w + iw * -x + iy * -z - iz * -y + pose.position.x,
            iy * w + iw * -y + iz * -x - ix * -z + pose.position.y,
            iz * w + iw * -z + ix * -y - iy * -x + pose.position.z,
        };
    }
};

} // namespace XrMath
