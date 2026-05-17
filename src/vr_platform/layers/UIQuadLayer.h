// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCVR ImGui / Qt overlay quad layer. Stub.

#pragma once

#include "../XrPlatformIncludes.h"

namespace vr_pcvr {

class UIQuadLayer {
public:
    bool Init(XrSession /*session*/) { return true; }
    void Shutdown() {}
};

} // namespace vr_pcvr
