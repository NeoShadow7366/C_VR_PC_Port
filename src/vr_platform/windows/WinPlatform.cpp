// SPDX-License-Identifier: GPL-3.0-or-later

#include "windows/WinPlatform.h"
#include "utils/LogUtils.h"
#include "utils/XrMath.h"
#include "VrSettings.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vr_pcvr {

void OXR_CheckErrors(XrResult result, const char* function, bool failOnError) {
    if (XR_SUCCEEDED(result)) {
        return;
    }
    char errorBuffer[XR_MAX_RESULT_STRING_SIZE] = {};
    // We don't have an instance handle here in all cases, so just log the code.
    if (failOnError) {
        FAIL("OpenXR error: %s -> %d", function, static_cast<int>(result));
    } else {
        ALOGE("OpenXR error: %s -> %d", function, static_cast<int>(result));
    }
    (void)errorBuffer;
}

namespace {

bool ContainsExt(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// Single global so the static Hook* thunks can find the active platform.
// Citra only ever has one Vulkan::Instance, so one platform pointer is fine.
WinPlatform* g_active_platform = nullptr;

PFN_vkGetInstanceProcAddr GetVulkanLoaderProcAddr() {
    // We link vulkan-1 directly (see CMakeLists.txt), so the symbol is
    // available without dlsym/GetProcAddress.
    return ::vkGetInstanceProcAddr;
}

} // namespace

WinPlatform::WinPlatform()  = default;
WinPlatform::~WinPlatform() { Shutdown(); }

bool WinPlatform::InitInstance() {
    if (!CreateInstanceAndSystem()) {
        return false;
    }
    if (!QueryVulkanRequirements()) {
        return false;
    }
    g_active_platform = this;
    return true;
}

bool WinPlatform::EnumerateExtensions() {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
        ALOGE("xrEnumerateInstanceExtensionProperties failed");
        return false;
    }
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()))) {
        return false;
    }

    // Required: vulkan_enable2.
    if (!ContainsExt(props, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        ALOGE("Runtime does not advertise " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }
    // Optional features.
    mHasHandTracking       = ContainsExt(props, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    mHasDisplayRefreshRate = ContainsExt(props, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    // XR_MSFT_controller_model_extension: lets the runtime hand us a real
    // glTF model for each connected controller. Detected here and logged
    // so we can wire a future glTF render path; today we only ship the
    // proxy / billboard ControllerMarker quads in VrApp.
    {
        constexpr const char* kMsftCtrlModel = "XR_MSFT_controller_model";
        const bool present = ContainsExt(props, kMsftCtrlModel);
        ALOGI("XR_MSFT_controller_model: %s",
              present ? "advertised by runtime (proxy markers still used)" : "not advertised");
    }

    return true;
}

bool WinPlatform::CreateInstanceAndSystem() {
    if (!EnumerateExtensions()) {
        return false;
    }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (mHasHandTracking)       enabled.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (mHasDisplayRefreshRate) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

    XrApplicationInfo appInfo{};
    std::strncpy(appInfo.applicationName, "CitraVR-PC", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(appInfo.engineName,      "custom",     XR_MAX_ENGINE_NAME_SIZE - 1);
    appInfo.applicationVersion = 1;
    appInfo.engineVersion      = 1;
    appInfo.apiVersion         = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.applicationInfo       = appInfo;
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();

    if (XR_FAILED(xrCreateInstance(&ici, &mInstance))) {
        ALOGE("xrCreateInstance failed");
        return false;
    }

    XrInstanceProperties iprops{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(mInstance, &iprops))) {
        ALOGI("OpenXR runtime: %s %u.%u.%u", iprops.runtimeName,
              XR_VERSION_MAJOR(iprops.runtimeVersion),
              XR_VERSION_MINOR(iprops.runtimeVersion),
              XR_VERSION_PATCH(iprops.runtimeVersion));
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(mInstance, &sgi, &mSystemId))) {
        ALOGE("xrGetSystem (HMD) failed - is SteamVR running?");
        return false;
    }

    XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(mInstance, mSystemId, &sysProps))) {
        ALOGI("HMD system: %s (vendor %u)", sysProps.systemName, sysProps.vendorId);
        LOG_INFO(Frontend, "VR HMD system: {} (vendor {})", sysProps.systemName,
                 sysProps.vendorId);
        mMaxLayerCount = sysProps.graphicsProperties.maxLayerCount > 0
                             ? sysProps.graphicsProperties.maxLayerCount
                             : 16;
        // Map runtime systemName -> our HMDType profile so VrApp picks the
        // right default supersample factor (and any future per-HMD tweaks).
        // Priority: existing config value > CITRA_VR_HMD env override > runtime
        // systemName auto-detect. SteamVR's OpenXR runtime hides the actual
        // headset model behind "SteamVR/OpenXR : lighthouse", so the env var
        // is the only reliable way to force a Beyond 2 / Index / Vive profile.
        if (VRSettings::values.hmd_type == VRSettings::HMDType::UNKNOWN) {
            const char* env_hmd = std::getenv("CITRA_VR_HMD");
            VRSettings::HMDType from_env = VRSettings::HMDType::UNKNOWN;
            if (env_hmd && *env_hmd) {
                from_env = VRSettings::ParseHmdTypeId(env_hmd);
            }
            if (from_env != VRSettings::HMDType::UNKNOWN) {
                VRSettings::values.hmd_type = from_env;
                ALOGI("HMD profile from CITRA_VR_HMD='%s': %d", env_hmd,
                      static_cast<int>(VRSettings::values.hmd_type));
                LOG_INFO(Frontend, "VR HMD profile from CITRA_VR_HMD='{}': {}",
                         env_hmd, static_cast<int>(VRSettings::values.hmd_type));
            } else {
                VRSettings::values.hmd_type =
                    VRSettings::DetectFromSystemName(sysProps.systemName);
                ALOGI("HMD profile auto-selected: %d",
                      static_cast<int>(VRSettings::values.hmd_type));
                LOG_INFO(Frontend, "VR HMD profile auto-selected: {}",
                         static_cast<int>(VRSettings::values.hmd_type));
            }
        }
    }

    // View configuration (stereo).
    uint32_t viewCount = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(mInstance, mSystemId, kViewConfigType,
                                                    0, &viewCount, nullptr)) ||
        viewCount != 2) {
        ALOGE("Runtime does not advertise 2-view stereo configuration");
        return false;
    }
    XrViewConfigurationView fill[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_FAILED(xrEnumerateViewConfigurationViews(mInstance, mSystemId, kViewConfigType,
                                                    2, &viewCount, fill))) {
        return false;
    }
    mViewConfig[0] = fill[0];
    mViewConfig[1] = fill[1];
    ALOGI("Recommended per-eye image: %ux%u",
          mViewConfig[0].recommendedImageRectWidth,
          mViewConfig[0].recommendedImageRectHeight);

    return true;
}

bool WinPlatform::QueryVulkanRequirements() {
    OXR(xrGetInstanceProcAddr(mInstance, "xrGetVulkanGraphicsRequirements2KHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetVkGraphicsReq2)));
    if (!pfnGetVkGraphicsReq2) {
        ALOGE("xrGetVulkanGraphicsRequirements2KHR not found");
        return false;
    }
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (XR_FAILED(pfnGetVkGraphicsReq2(mInstance, mSystemId, &req))) {
        ALOGE("xrGetVulkanGraphicsRequirements2KHR returned an error");
        return false;
    }
    ALOGI("Vulkan API min %u.%u.%u, max %u.%u.%u",
          XR_VERSION_MAJOR(req.minApiVersionSupported),
          XR_VERSION_MINOR(req.minApiVersionSupported),
          XR_VERSION_PATCH(req.minApiVersionSupported),
          XR_VERSION_MAJOR(req.maxApiVersionSupported),
          XR_VERSION_MINOR(req.maxApiVersionSupported),
          XR_VERSION_PATCH(req.maxApiVersionSupported));

    // With XR_KHR_vulkan_enable2 the runtime creates the VkInstance/VkDevice
    // for us via xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR. We
    // expose those helpers to the caller via this WinPlatform instance.
    // No fixed extension list is returned here.
    mVkInstExts.clear();
    mVkDeviceExts.clear();
    return true;
}

bool WinPlatform::InitSession(VkInstance       vkInstance,
                              VkPhysicalDevice vkPhysical,
                              VkDevice         vkDevice,
                              uint32_t         queueFamilyIndex,
                              uint32_t         queueIndex) {
    if (mInstance == XR_NULL_HANDLE || mSystemId == XR_NULL_SYSTEM_ID) {
        ALOGE("InitSession called before InitInstance");
        return false;
    }

    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance         = vkInstance;
    gb.physicalDevice   = vkPhysical;
    gb.device           = vkDevice;
    gb.queueFamilyIndex = queueFamilyIndex;
    gb.queueIndex       = queueIndex;

    mVkDevice         = vkDevice;
    mVkPhysicalDevice = vkPhysical;
    mVkInstance       = vkInstance;
    mVkQueueFamilyIdx = queueFamilyIndex;
    vkGetDeviceQueue(vkDevice, queueFamilyIndex, queueIndex, &mVkQueue);

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next     = &gb;
    sci.systemId = mSystemId;
    if (XR_FAILED(xrCreateSession(mInstance, &sci, &mSession))) {
        ALOGE("xrCreateSession failed");
        return false;
    }

    // Always-present reference spaces. The runtime-initiated head space is
    // created later, on the first frame, so we have a real predictedDisplayTime.
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace = XrMath::Posef::Identity();

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    OXR(xrCreateReferenceSpace(mSession, &rsci, &mLocalSpace));

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    // STAGE may be unsupported on some seated-only runtimes - non-fatal.
    if (XR_FAILED(xrCreateReferenceSpace(mSession, &rsci, &mStageSpace))) {
        mStageSpace = XR_NULL_HANDLE;
    }

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    OXR(xrCreateReferenceSpace(mSession, &rsci, &mViewSpace));

    // Optional: ask the runtime for the highest display refresh rate it
    // supports, capped per HMD profile. Skipped silently when the
    // XR_FB_display_refresh_rate extension isn't present (most desktop
    // OpenXR runtimes implement it - SteamVR included).
    if (mHasDisplayRefreshRate) {
        PFN_xrEnumerateDisplayRefreshRatesFB pfnEnum = nullptr;
        PFN_xrRequestDisplayRefreshRateFB    pfnReq  = nullptr;
        xrGetInstanceProcAddr(mInstance, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnEnum));
        xrGetInstanceProcAddr(mInstance, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnReq));
        if (pfnEnum && pfnReq) {
            uint32_t count = 0;
            if (XR_SUCCEEDED(pfnEnum(mSession, 0, &count, nullptr)) && count > 0) {
                std::vector<float> rates(count);
                pfnEnum(mSession, count, &count, rates.data());
                // Cap by HMD profile so we don't push a 90 Hz panel beyond
                // its native rate. Beyond 2 ships at 75/90; Index up to 144.
                float cap = 90.0f;
                switch (VRSettings::values.hmd_type) {
                case VRSettings::HMDType::BIGSCREEN_BEYOND_2: cap = 90.0f;  break;
                case VRSettings::HMDType::VALVE_INDEX:        cap = 120.0f; break;
                case VRSettings::HMDType::VIVE_PRO:           cap = 90.0f;  break;
                default:                                       cap = 90.0f;  break;
                }
                float best = 0.0f;
                for (float r : rates) {
                    if (r <= cap && r > best) best = r;
                }
                if (best > 0.0f) {
                    if (XR_SUCCEEDED(pfnReq(mSession, best))) {
                        ALOGI("Display refresh rate: requested %.1f Hz (cap %.1f)", best, cap);
                        LOG_INFO(Frontend,
                                 "VR display refresh rate: requested {:.1f} Hz (cap {:.1f})",
                                 best, cap);
                    } else {
                        ALOGW("xrRequestDisplayRefreshRateFB(%.1f) failed", best);
                        LOG_WARNING(Frontend, "VR xrRequestDisplayRefreshRateFB({:.1f}) failed",
                                    best);
                    }
                }
            }
        }
    }

    return true;
}

void WinPlatform::CreateRuntimeInitiatedSpaces(XrTime predictedDisplayTime) {
    if (mSession == XR_NULL_HANDLE) return;

    // Locate VIEW in LOCAL to capture the user's actual head position
    // and yaw at startup. Strip pitch and roll so the game panel is
    // perfectly upright, then place it at the head position so it
    // appears mDistance metres in front of the user's initial gaze.
    float       yaw     = 0.0f;
    XrVector3f  headPos = {0.0f, 0.0f, 0.0f};

    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (mViewSpace != XR_NULL_HANDLE && mLocalSpace != XR_NULL_HANDLE
        && XR_SUCCEEDED(xrLocateSpace(mViewSpace, mLocalSpace,
                                      predictedDisplayTime, &loc))
        && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
        && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        headPos = loc.pose.position;

        // Extract yaw-only: project the view forward vector onto the
        // horizontal plane and compute the signed angle around Y.
        // rotate(q, {0,0,-1}) = { -2*(qx*qz+qw*qy),
        //                          2*(qw*qx-qy*qz),   <- Y component, unused
        //                          2*(qx^2+qy^2)-1 }
        const auto& q = loc.pose.orientation;
        float fwdX = -2.0f * (q.x * q.z + q.w * q.y);
        float fwdZ =  2.0f * (q.x * q.x + q.y * q.y) - 1.0f;
        const float len = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
        if (len > 1e-3f) { fwdX /= len; fwdZ /= len; }
        else              { fwdX = 0.0f; fwdZ = -1.0f; }
        // yawQ = (0,sin(yaw/2),0,cos(yaw/2)) is a +Y rotation by `yaw`;
        // rotate(yawQ,{0,0,-1}) = (-sin yaw, 0, -cos yaw). To make that
        // equal the actual head forward (fwdX, 0, fwdZ) we need
        // yaw = atan2(-fwdX, -fwdZ). The previous atan2(fwdX, -fwdZ)
        // produced the opposite rotation, so HeadSpace (and the game
        // quad pinned to it at z=-1.5) ended up mirrored across the
        // user's gaze whenever they recentered while not facing straight.
        yaw = std::atan2(-fwdX, -fwdZ);
    }

    RecreateHeadSpace(yaw, headPos);
}

void WinPlatform::RecreateHeadSpace(float yawRad, XrVector3f posInLocal) {
    if (mSession == XR_NULL_HANDLE) return;
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType              = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace.orientation = {0.0f, std::sin(yawRad * 0.5f),
                                           0.0f, std::cos(yawRad * 0.5f)};
    ci.poseInReferenceSpace.position   = posInLocal;
    if (mHeadSpace != XR_NULL_HANDLE) {
        xrDestroySpace(mHeadSpace);
        mHeadSpace = XR_NULL_HANDLE;
    }
    OXR(xrCreateReferenceSpace(mSession, &ci, &mHeadSpace));
}

void WinPlatform::Shutdown() {
    if (mHeadSpace  != XR_NULL_HANDLE) { xrDestroySpace(mHeadSpace);  mHeadSpace  = XR_NULL_HANDLE; }
    if (mViewSpace  != XR_NULL_HANDLE) { xrDestroySpace(mViewSpace);  mViewSpace  = XR_NULL_HANDLE; }
    if (mStageSpace != XR_NULL_HANDLE) { xrDestroySpace(mStageSpace); mStageSpace = XR_NULL_HANDLE; }
    if (mLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(mLocalSpace); mLocalSpace = XR_NULL_HANDLE; }
    if (mSession  != XR_NULL_HANDLE) { xrDestroySession(mSession);   mSession  = XR_NULL_HANDLE; }
    if (mInstance != XR_NULL_HANDLE) { xrDestroyInstance(mInstance); mInstance = XR_NULL_HANDLE; }
    if (g_active_platform == this) {
        g_active_platform = nullptr;
    }
}

// ---------------- Vulkan creation hooks ----------------------------------

VkResult WinPlatform::HookCreateInstance(const VkInstanceCreateInfo* create_info,
                                         const VkAllocationCallbacks* /*allocator*/,
                                         VkInstance* out_instance) {
    if (!g_active_platform || g_active_platform->mInstance == XR_NULL_HANDLE) {
        ALOGE("HookCreateInstance: no active WinPlatform");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_xrCreateVulkanInstanceKHR pfn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_active_platform->mInstance, "xrCreateVulkanInstanceKHR",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&pfn))) ||
        !pfn) {
        ALOGE("xrCreateVulkanInstanceKHR not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    XrVulkanInstanceCreateInfoKHR xr_ci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_ci.systemId               = g_active_platform->mSystemId;
    xr_ci.createFlags            = 0;
    xr_ci.pfnGetInstanceProcAddr = GetVulkanLoaderProcAddr();
    xr_ci.vulkanCreateInfo       = create_info;
    xr_ci.vulkanAllocator        = nullptr;

    VkResult vk_result   = VK_SUCCESS;
    const XrResult xr_rc = pfn(g_active_platform->mInstance, &xr_ci, out_instance, &vk_result);
    if (XR_FAILED(xr_rc)) {
        ALOGE("xrCreateVulkanInstanceKHR returned XrResult=%d", static_cast<int>(xr_rc));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vk_result;
}

VkResult WinPlatform::HookGetPhysicalDevice(VkInstance instance,
                                            VkPhysicalDevice* out_physical_device) {
    if (!g_active_platform || g_active_platform->mInstance == XR_NULL_HANDLE) {
        ALOGE("HookGetPhysicalDevice: no active WinPlatform");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_xrGetVulkanGraphicsDevice2KHR pfn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_active_platform->mInstance,
                                        "xrGetVulkanGraphicsDevice2KHR",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&pfn))) ||
        !pfn) {
        ALOGE("xrGetVulkanGraphicsDevice2KHR not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    XrVulkanGraphicsDeviceGetInfoKHR get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    get_info.systemId       = g_active_platform->mSystemId;
    get_info.vulkanInstance = instance;

    const XrResult xr_rc = pfn(g_active_platform->mInstance, &get_info, out_physical_device);
    if (XR_FAILED(xr_rc)) {
        ALOGE("xrGetVulkanGraphicsDevice2KHR returned XrResult=%d", static_cast<int>(xr_rc));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult WinPlatform::HookCreateDevice(VkPhysicalDevice physical_device,
                                       const VkDeviceCreateInfo* create_info,
                                       const VkAllocationCallbacks* /*allocator*/,
                                       VkDevice* out_device) {
    if (!g_active_platform || g_active_platform->mInstance == XR_NULL_HANDLE) {
        ALOGE("HookCreateDevice: no active WinPlatform");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_xrCreateVulkanDeviceKHR pfn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_active_platform->mInstance, "xrCreateVulkanDeviceKHR",
                                        reinterpret_cast<PFN_xrVoidFunction*>(&pfn))) ||
        !pfn) {
        ALOGE("xrCreateVulkanDeviceKHR not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Vulkan requires external synchronisation per VkQueue. Citra's render
    // thread submits via MasterSemaphoreFence::SubmitWork while SteamVR's
    // OpenXR runtime spawns its own internal worker threads that submit to
    // the queue we hand it via XrGraphicsBindingVulkanKHR. Sharing one
    // VkQueue between the two ⇒ NVIDIA TDR / VK_ERROR_DEVICE_LOST inside
    // a few seconds.
    //
    // Fix: ask the device for two queues from the graphics family. Citra
    // keeps queueIndex=0; we bind SteamVR to queueIndex=1 in InitSession.
    // Both queues are independent VkQueues, so no external sync is needed.
    std::vector<VkDeviceQueueCreateInfo> patched_queues(
        create_info->pQueueCreateInfos,
        create_info->pQueueCreateInfos + create_info->queueCreateInfoCount);
    static constexpr float kQueuePriorities[2] = {1.0f, 1.0f};
    g_active_platform->mVrQueueIndex = 0; // default: shared (no second queue)
    for (auto& q : patched_queues) {
        // Bump the queue Citra picked (graphics family, count=1) to 2 so
        // SteamVR can own queueIndex=1.
        if (q.queueCount == 1) {
            q.queueCount = 2;
            q.pQueuePriorities = kQueuePriorities;
            g_active_platform->mVrQueueIndex = 1;
            break;
        }
    }
    VkDeviceCreateInfo patched_ci = *create_info;
    patched_ci.queueCreateInfoCount = static_cast<uint32_t>(patched_queues.size());
    patched_ci.pQueueCreateInfos    = patched_queues.data();

    XrVulkanDeviceCreateInfoKHR xr_ci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_ci.systemId               = g_active_platform->mSystemId;
    xr_ci.createFlags            = 0;
    xr_ci.pfnGetInstanceProcAddr = GetVulkanLoaderProcAddr();
    xr_ci.vulkanPhysicalDevice   = physical_device;
    xr_ci.vulkanCreateInfo       = &patched_ci;
    xr_ci.vulkanAllocator        = nullptr;

    VkResult vk_result   = VK_SUCCESS;
    const XrResult xr_rc = pfn(g_active_platform->mInstance, &xr_ci, out_device, &vk_result);
    if (XR_FAILED(xr_rc)) {
        ALOGE("xrCreateVulkanDeviceKHR returned XrResult=%d", static_cast<int>(xr_rc));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return vk_result;
}

} // namespace vr_pcvr
