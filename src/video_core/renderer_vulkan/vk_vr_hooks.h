// SPDX-License-Identifier: GPL-3.0-or-later
//
// Optional VR integration hooks for the Vulkan renderer.
//
// When running under SteamVR/OpenXR, the runtime requires that the VkInstance,
// VkPhysicalDevice and VkDevice handed to xrCreateSession be created via
// xrCreateVulkanInstanceKHR / xrGetVulkanGraphicsDevice2KHR /
// xrCreateVulkanDeviceKHR (or equivalent). Otherwise xrCreateSession fails
// with XR_ERROR_GRAPHICS_DEVICE_INVALID.
//
// The vr_platform layer installs callbacks here before Citra's RendererVulkan
// is constructed. vk_platform.cpp / vk_instance.cpp consult them and route the
// VkInstance / VkPhysicalDevice / VkDevice creation through the runtime when
// installed. When no callbacks are installed (the normal non-VR build) the
// renderer falls back to vkCreateInstance / vkCreateDevice unchanged.
//
// Lifetime: callers populate VrHooks via SetVrHooks() before the Renderer is
// constructed and clear them with SetVrHooks(nullptr) on shutdown. The
// renderer never copies the struct - it must remain valid for the lifetime of
// the Vulkan::Instance.

#pragma once

#include <mutex>

#include <vulkan/vulkan.h>

namespace Vulkan {

struct VrHooks {
    /// Substitute for vkCreateInstance. The implementation is responsible for
    /// adding any extensions the OpenXR runtime requires before calling
    /// vkCreateInstance internally.
    VkResult (*create_instance)(const VkInstanceCreateInfo* create_info,
                                const VkAllocationCallbacks* allocator,
                                VkInstance* out_instance) = nullptr;

    /// Returns the VkPhysicalDevice the OpenXR runtime expects us to use.
    /// If non-null this overrides Citra's normal physical-device selection.
    VkResult (*get_physical_device)(VkInstance instance,
                                    VkPhysicalDevice* out_physical_device) = nullptr;

    /// Substitute for vkCreateDevice. The implementation may augment the
    /// supplied VkDeviceCreateInfo's extension list with runtime-required
    /// extensions before calling vkCreateDevice internally.
    VkResult (*create_device)(VkPhysicalDevice physical_device,
                              const VkDeviceCreateInfo* create_info,
                              const VkAllocationCallbacks* allocator,
                              VkDevice* out_device) = nullptr;
};

/// Returns the currently-installed hooks, or nullptr if the renderer should
/// use the normal Vulkan creation path.
const VrHooks* GetVrHooks();

/// Install (or clear when nullptr) the VR integration hooks. Must be called
/// before constructing Vulkan::Instance.
void SetVrHooks(const VrHooks* hooks);

/// Returns the global mutex that serialises every vkQueueSubmit /
/// vkQueuePresentKHR on Citra's graphics queue with the OpenXR runtime's
/// internal submissions on the same queue (xrAcquireSwapchainImage,
/// xrWaitSwapchainImage, xrReleaseSwapchainImage, xrEndFrame). Vulkan
/// requires external synchronization per VkQueue; without this lock the
/// Citra render thread and the VR thread race on the queue and the driver
/// reports VK_ERROR_DEVICE_LOST. The mutex is always present (an
/// uncontended lock costs ~10ns), so non-VR builds may also lock it
/// without functional impact.
std::mutex& GetVrQueueMutex();

/// Held by the emulator core while it is mutating renderer-owned
/// resources that the VR thread reads each frame (currently: save-state
/// load/save, which destroys and recreates the off-screen render target
/// images). The VR composite thread must `try_lock` this mutex before
/// touching `EmuWindow_VR_Win::TryGetVrImage()`; if it can't acquire the
/// lock it should skip the blit for that frame and reuse the last
/// composited image. Like `GetVrQueueMutex()`, this mutex is always
/// present so non-VR builds can lock it cheaply.
std::mutex& GetVrCoreBusyMutex();

/// Optional callback installed by vr_platform that flushes any in-flight
/// GPU work the VR composite thread has submitted (typically a
/// vkDeviceWaitIdle on the VR-owned VkDevice). Core invokes this while
/// holding GetVrCoreBusyMutex() before destroying renderer-owned
/// resources (e.g. during save-state load), to guarantee the GPU is no
/// longer dereferencing them.
using VrWaitIdleFn = void (*)();
void SetVrWaitIdleFn(VrWaitIdleFn fn);
VrWaitIdleFn GetVrWaitIdleFn();

} // namespace Vulkan
