// SPDX-License-Identifier: GPL-3.0-or-later
//
// Win32 / Vulkan replacement for src/android/app/src/main/jni/vr/OpenXR.{h,cpp}.
//
// Owns the XrInstance, XrSystemId, XrSession and the standard set of
// reference spaces. Vulkan device creation is deferred to Citra's existing
// RendererVulkan via the XR_KHR_vulkan_enable2 path: we expose the device
// requirements through GetVulkanInstanceExtensions / GetVulkanDeviceExtensions
// so the renderer can satisfy them when it constructs its own instance.

#pragma once

#include "../XrPlatformIncludes.h"

#include <cstdint>
#include <string>
#include <vector>

#define OXR(func) ::vr_pcvr::OXR_CheckErrors(func, #func, true)

namespace vr_pcvr {

void OXR_CheckErrors(XrResult result, const char* function, bool failOnError);

class WinPlatform {
public:
    WinPlatform();
    ~WinPlatform();

    WinPlatform(const WinPlatform&)            = delete;
    WinPlatform& operator=(const WinPlatform&) = delete;

    // Phase 1: create the XrInstance, pick a system, and query the Vulkan
    // requirements. Must be called before the renderer creates its
    // VkInstance / VkDevice.
    bool InitInstance();

    // Phase 2: bind to an existing Vulkan instance/device (typically owned
    // by Citra's RendererVulkan) and create the XrSession.
    bool InitSession(VkInstance       vkInstance,
                     VkPhysicalDevice vkPhysical,
                     VkDevice         vkDevice,
                     uint32_t         queueFamilyIndex,
                     uint32_t         queueIndex);

    void Shutdown();

    // Vulkan extension lists the runtime needs us to enable on the
    // VkInstance / VkDevice. Empty until InitInstance() succeeds.
    const std::vector<std::string>& GetVulkanInstanceExtensions() const { return mVkInstExts;   }
    const std::vector<std::string>& GetVulkanDeviceExtensions()   const { return mVkDeviceExts; }

    // Accessors used by the rest of the VR module.
    // Vulkan handles cached during InitSession - the rest of the VR
    // module (VrFrameSubmitter, GameQuadLayer) needs them to record and
    // submit blit command buffers without having to plumb them through
    // every call.
    VkDevice         VkDevice_()         const { return mVkDevice;          }
    VkPhysicalDevice VkPhysicalDevice_() const { return mVkPhysicalDevice;  }
    VkInstance       VkInstance_()       const { return mVkInstance;        }
    VkQueue          VkQueue_()          const { return mVkQueue;           }
    uint32_t         VkQueueFamily()     const { return mVkQueueFamilyIdx;  }
    /// Queue index inside the graphics family that SteamVR should bind to.
    /// See mVrQueueIndex for details.
    uint32_t         VrQueueIndex()      const { return mVrQueueIndex;     }

    XrInstance Instance() const { return mInstance; }
    XrSystemId System()   const { return mSystemId; }
    XrSession  Session()  const { return mSession;  }

    XrSpace LocalSpace()   const { return mLocalSpace;   }
    XrSpace StageSpace()   const { return mStageSpace;   }
    XrSpace ViewSpace()    const { return mViewSpace;    }
    XrSpace HeadSpace()    const { return mHeadSpace;    }

    const XrViewConfigurationView& ViewConfig(int eye) const { return mViewConfig[eye]; }
    size_t MaxLayerCount() const { return mMaxLayerCount; }

    // Optional capability flags (keep extensions truly optional - SteamVR
    // does not implement most of the FB/Meta extensions used on Quest).
    bool HasHandTracking()      const { return mHasHandTracking; }
    bool HasDisplayRefreshRate() const { return mHasDisplayRefreshRate; }

    // Recreate the head space using the current HMD forward direction as
    // the reference. Should be called once on the first frame after
    // session start, mirroring vr_main.cpp::CreateRuntimeInitatedReferenceSpaces.
    void CreateRuntimeInitiatedSpaces(XrTime predictedDisplayTime);

    // Destroy and re-create mHeadSpace with the given yaw-only orientation
    // and head position (both in LOCAL space). Called from VrApp whenever
    // the menu re-centres so the game screen follows the same direction.
    void RecreateHeadSpace(float yawRad, XrVector3f posInLocal);

    // ---- VkInstance / VkDevice creation hooks --------------------------
    //
    // SteamVR's xrCreateSession requires a VkInstance/VkPhysicalDevice/
    // VkDevice that were produced via xrCreateVulkanInstanceKHR /
    // xrGetVulkanGraphicsDevice2KHR / xrCreateVulkanDeviceKHR. We expose
    // these as plain C function pointers (signature-compatible with
    // Vulkan::VrHooks in vk_vr_hooks.h) so the renderer can route its own
    // VkInstance/VkDevice creation through the runtime without depending
    // on OpenXR types.
    static VkResult HookCreateInstance(const VkInstanceCreateInfo* create_info,
                                       const VkAllocationCallbacks* allocator,
                                       VkInstance* out_instance);
    static VkResult HookGetPhysicalDevice(VkInstance instance,
                                          VkPhysicalDevice* out_physical_device);
    static VkResult HookCreateDevice(VkPhysicalDevice physical_device,
                                     const VkDeviceCreateInfo* create_info,
                                     const VkAllocationCallbacks* allocator,
                                     VkDevice* out_device);

private:
    bool EnumerateExtensions();
    bool CreateInstanceAndSystem();
    bool QueryVulkanRequirements();

    // --- XR state ---
    XrInstance mInstance = XR_NULL_HANDLE;
    XrSystemId mSystemId = XR_NULL_SYSTEM_ID;
    XrSession  mSession  = XR_NULL_HANDLE;

    XrSpace mLocalSpace = XR_NULL_HANDLE;
    XrSpace mStageSpace = XR_NULL_HANDLE;
    XrSpace mViewSpace  = XR_NULL_HANDLE;
    XrSpace mHeadSpace  = XR_NULL_HANDLE;

    XrViewConfigurationView mViewConfig[2] = {};
    size_t                  mMaxLayerCount = 16;

    static constexpr XrViewConfigurationType kViewConfigType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    // --- Capabilities ---
    bool mHasHandTracking       = false;
    bool mHasDisplayRefreshRate = false;

    // --- Vulkan requirements (populated by InitInstance) ---
    std::vector<std::string> mVkInstExts;
    std::vector<std::string> mVkDeviceExts;

    // --- Vulkan handles cached from InitSession ---
    VkDevice         mVkDevice         = VK_NULL_HANDLE;
    VkPhysicalDevice mVkPhysicalDevice = VK_NULL_HANDLE;
    VkInstance       mVkInstance       = VK_NULL_HANDLE;
    VkQueue          mVkQueue          = VK_NULL_HANDLE;
    uint32_t         mVkQueueFamilyIdx = 0;
    // Index of the dedicated VkQueue we asked the runtime to bind in
    // HookCreateDevice. 0 ⇒ Citra and SteamVR share queue 0 (legacy /
    // single-queue physical devices); 1 ⇒ SteamVR owns queueIndex=1
    // independently of Citra's queueIndex=0. Set by HookCreateDevice,
    // read by InitSession's xrCreateSession binding.
    uint32_t         mVrQueueIndex     = 0;

    // --- Function pointers from XR_KHR_vulkan_enable2 ---
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVkGraphicsReq2 = nullptr;
};

} // namespace vr_pcvr
