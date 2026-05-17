// SPDX-License-Identifier: GPL-3.0-or-later
//
// AimBeamLayer - thin laser-pointer beam quads drawn from each controller's
// aim origin to its cursor endpoint. Gives the "embodied pointer" feel that
// VR apps use to show where the controller is pointing.
//
// Each hand gets one elongated XrCompositionLayerQuad: 4 mm wide, length
// = distance from controller to cursor, tinted in the hand's accent colour
// (left = teal, right = amber) at 50% alpha so it doesn't block the scene.
// The swapchain is a single-texel gradient cleared once at init; no GPU
// work per frame beyond position/orientation updates.

#pragma once

#include "../XrPlatformIncludes.h"
#include "../windows/VkXrSwapchain.h"

#include <array>
#include <cstdint>

namespace vr_pcvr {

class VrFrameSubmitter;

class AimBeamLayer {
public:
    struct BeamSpec {
        XrVector3f origin{};    // controller aim-pose position
        XrVector3f endpoint{};  // cursor/hit position in LocalSpace
        bool       visible = false;
    };

    AimBeamLayer()  = default;
    ~AimBeamLayer() { Shutdown(); }

    AimBeamLayer(const AimBeamLayer&)            = delete;
    AimBeamLayer& operator=(const AimBeamLayer&) = delete;

    bool Init(XrSession session, VrFrameSubmitter& submitter,
              VkPhysicalDevice physical);
    void Shutdown();
    bool IsInitialised() const { return mSwapchains[0].Handle() != XR_NULL_HANDLE; }

    void SetBeams(const BeamSpec& left, const BeamSpec& right) {
        mBeams[0] = left;
        mBeams[1] = right;
    }

    // Populate outLayers (capacity 2) with one quad per visible beam.
    // Returns the number of layers written (0..2).
    uint32_t BuildLayers(XrSpace space,
                         XrCompositionLayerQuad outLayers[2]) const;

private:
    std::array<VkXrSwapchain, 2> mSwapchains{};
    std::array<BeamSpec, 2>      mBeams{};
};

} // namespace vr_pcvr
