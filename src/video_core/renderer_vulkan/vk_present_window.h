// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderpassCache;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Semaphore vr_handoff;   // headless: signaled on graphics_queue after
                                // render_ready is consumed; VR thread waits
                                // on this from its own queue.
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
};

class PresentWindow final {
public:
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns the last used render frame.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return swapchain.GetImageCount();
    }

    // ----- Headless / OpenXR composition support --------------------
    //
    // When the PresentWindow is constructed against an EmuWindow whose
    // WindowSystemType is `Headless`, no Win32/X11/Wayland surface is
    // created and no `vk::SwapchainKHR` is built. Instead, finished
    // frames are handed to a registered callback (typically the SteamVR
    // backend's `EmuWindow_VR_Win`) which composites them into the XR
    // swapchain on a separate thread.
    //
    // Synchronisation is a 1-deep producer/consumer handshake:
    //   * The renderer thread blocks inside CopyToSwapchain until the
    //     consumer has called NotifyFrameConsumed(), guaranteeing the
    //     consumer's GPU work that reads the published `VkImage` has
    //     been submitted.
    //   * The consumer must call NotifyFrameConsumed() exactly once per
    //     published frame, after queueing its blit command buffer.

    struct PublishedFrame {
        VkImage     image           = VK_NULL_HANDLE;
        VkSemaphore render_complete = VK_NULL_HANDLE;
        VkFence     present_done    = VK_NULL_HANDLE;
        u32         width           = 0;
        u32         height          = 0;
    };
    using FramePublishCallback = std::function<void(const PublishedFrame&)>;

    void SetFramePublishCallback(FramePublishCallback callback);
    void NotifyFrameConsumed();
    bool IsHeadless() const { return is_headless; }

private:
    void PresentThread(std::stop_token token);

    void CopyToSwapchain(Frame* frame);

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    Swapchain swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool vsync_enabled{};
    bool blit_supported;
    bool use_present_thread{true};
    bool is_headless{false};
    void* last_render_surface{};

    // Headless publish handshake.
    FramePublishCallback frame_publish_callback;
    std::mutex publish_mutex;
    std::condition_variable publish_cv;
    bool publish_pending{false};
};

} // namespace Vulkan
