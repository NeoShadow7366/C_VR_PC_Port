// SPDX-License-Identifier: GPL-3.0-or-later
//
// ImGuiLayer — renders Dear ImGui to a dedicated XR swapchain and
// exposes it as an XrCompositionLayerQuad. Used by the in-VR menu /
// settings UI on the Windows/SteamVR backend.
//
// Pipeline:
//   1. Init() creates a fixed-size colour swapchain, a 1-attachment
//      render pass, a small descriptor pool, and runs ImGui_ImplVulkan
//      bring-up (font upload via VrFrameSubmitter).
//   2. BeginFrame() / EndFrameAndRender() bracket the user's ImGui::*
//      calls each XR frame. EndFrameAndRender acquires a swapchain
//      image, renders the draw data into it, and submits.
//   3. BuildLayer() fills out an XrCompositionLayerQuad pointing at
//      the released swapchain image.
//
// Threading: BeginFrame / EndFrameAndRender / BuildLayer all run on
// the XR thread (same as VrApp::Frame). ImGui's global context is owned
// here; nobody else should touch it.

#pragma once

#include "Swapchain.h"
#include "XrPlatformIncludes.h"
#include "layers/IconCache.h"
#include "windows/VkXrSwapchain.h"

#include <cstdint>
#include <string>
#include <unordered_map>

// Forward-declare ImGui's font handle in the global namespace so the
// FontXxx() accessors below return the same type ImGui's API expects.
struct ImFont;

namespace vr_pcvr {

class VrFrameSubmitter;

class ImGuiLayer {
public:
    ImGuiLayer()  = default;
    ~ImGuiLayer() { Shutdown(); }

    ImGuiLayer(const ImGuiLayer&)            = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Pixel size of the menu surface. 2048×1536 at 2.0×1.2 m gives
    // ~1.0 mm/texel at 1.2 m — comfortable for a VR panel at typical
    // head-to-menu distance. Wider format gives room for the card grid.
    static constexpr uint32_t kPixelW = 2048;
    static constexpr uint32_t kPixelH = 1536;

    // World-space size of the quad in metres. 2.0 × 1.2 m at 1.2 m
    // distance subtends ~90° × 54° — wider to match the 2048 px surface
    // and fill the sweet-spot FOV on typical PCVR headsets.
    static constexpr float kQuadW = 2.0f;
    static constexpr float kQuadH = 1.2f;

    bool Init(XrSession        session,
              VkInstance       instance,
              VkPhysicalDevice physical,
              VkDevice         device,
              uint32_t         queueFamilyIndex,
              VkQueue          queue,
              VrFrameSubmitter& submitter);
    void Shutdown();
    bool IsInitialised() const { return mSwapchain.Handle() != XR_NULL_HANDLE; }

    // Show / hide the menu. While hidden, the layer is omitted from
    // BuildLayer() and BeginFrame becomes a no-op (returns false).
    void SetVisible(bool v) { mVisible = v; }
    bool IsVisible() const  { return mVisible; }

    // True while the menu has any non-zero on-screen presence (i.e.
    // mid open- or close-fade, or fully open). Use this in the layer
    // list / dim-layer logic so the close fade is actually visible.
    bool IsVisuallyVisible() const { return mAlpha > 0.001f; }

    // Advance the open/close fade by `dt` seconds. Cheap; safe to
    // call every frame even when the menu is hidden.
    void Tick(float dt);

    // Current fade value [0..1]. 1 = fully open. Used by sibling
    // layers (e.g. DimLayer) that want to ramp in lockstep with the
    // menu.
    float Alpha() const { return mAlpha; }

    // Pose: positioned in head/local space at z=-kDistance, slightly
    // above the game quad. Caller may override by setting `pose`.
    void SetPose(const XrPosef& pose) { mPose = pose; }
    const XrPosef& Pose() const       { return mPose; }

    // Pixel hit point for the menu (e.g. driven by left-hand raycast).
    // (px, py) is in [0, kPixelW) x [0, kPixelH); pressed = trigger.
    void SetMouse(int px, int py, bool pressed);

    // Accumulate a scroll delta (in ImGui wheel units) that will be
    // injected into io.MouseWheel on the next BeginFrame call.
    // Positive = scroll down, negative = scroll up (matches ImGui sign).
    // Typical source: right-hand thumbstick Y for hands-free scrolling.
    void AddScroll(float dy) { mScrollDeltaY += dy; }

    // Bracketing helpers around the user's ImGui::* draw calls.
    // Returns false if the layer is hidden or not initialised.
    bool BeginFrame();
    void EndFrameAndRender(VrFrameSubmitter& submitter);

    // Build the composition-layer header. Returns true if a layer
    // should be added to xrEndFrame. `space` is the reference space to
    // attach the quad to (typically HeadSpace).
    bool BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const;

    const VkXrSwapchain& Swapchain() const { return mSwapchain; }

    // Toast notification: show a transient message at the bottom of the
    // menu quad surface for `duration` seconds. Appears even when the
    // full menu is closed (BeginFrame still runs, clear is transparent).
    enum class ToastKind { Info, Success, Warning, Error };
    void ShowToast(std::string text, float duration = 1.5f);
    void ShowToast(std::string text, ToastKind kind, float duration = 1.5f);
    bool HasToast() const { return mToastTimer > 0.0f; }
    // Draw the active toast ImGui window. Called by VrApp between
    // BeginFrame and EndFrameAndRender. No-op when no toast is active.
    void DrawToast();

    // Persistent perf overlay (FPS / frame ms / dropped blits). When
    // `active` is true the overlay is drawn every frame regardless of
    // menu visibility, and BeginFrame is forced to return true so the
    // ImGui pass runs even with the main panel hidden.
    void SetPerfOverlay(bool active, std::string line);
    bool IsPerfOverlayActive() const { return mPerfActive; }
    void DrawPerfOverlay();

    // ROM-card icon cache for thumbnails. Lifetime tied to ImGuiLayer
    // (uses the same VkDevice / queue / descriptor pool indirectly via
    // the ImGui Vulkan backend).
    IconCache& Icons() { return mIcons; }

    // Pre-baked TTF fonts for typographic hierarchy in the in-VR menu.
    // FontBody is also installed as ImGui's default font (so plain text
    // calls render at body size). Push/pop FontDisplay or FontCaption
    // around regions that need a different scale. May be nullptr if the
    // TTF could not be loaded (then ImGui's default ProggyClean is used).
    ImFont* FontBody()    const { return mFontBody; }
    ImFont* FontCaption() const { return mFontCaption; }
    ImFont* FontDisplay() const { return mFontDisplay; }

private:
    void DestroyVulkanObjects();
    VkFramebuffer GetOrCreateFramebuffer(VkImage image);

    VkXrSwapchain    mSwapchain;
    VkDevice         mDevice          = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice  = VK_NULL_HANDLE;
    VkInstance       mInstance        = VK_NULL_HANDLE;
    VkQueue          mQueue           = VK_NULL_HANDLE;
    uint32_t         mQueueFamilyIdx  = 0;

    VkRenderPass     mRenderPass      = VK_NULL_HANDLE;
    VkDescriptorPool mDescriptorPool  = VK_NULL_HANDLE;

    // Lazily created framebuffer per swapchain image (and image view).
    std::unordered_map<VkImage, VkFramebuffer> mFramebuffers;
    std::unordered_map<VkImage, VkImageView>   mImageViews;

    bool             mVisible         = false;
    bool             mFrameOpen       = false;
    bool             mAcquired        = false;
    uint32_t         mAcquiredIdx     = 0;

    // Open/close fade. Target is 1.0 when mVisible, 0.0 otherwise;
    // Tick() ramps mAlpha towards it over kFadeSeconds. Multiplied
    // into ImGui's style.Alpha during the menu's BeginFrame so the
    // entire panel fades smoothly without any per-widget changes.
    static constexpr float kFadeSeconds = 0.16f;
    float            mAlpha           = 0.0f;

    // Mouse state set externally each frame.
    float            mMouseX          = 0.0f;
    float            mMouseY          = 0.0f;
    bool             mMouseDown       = false;
    // Accumulated scroll delta (ImGui wheel units). Drained into
    // io.MouseWheel at the start of each BeginFrame / ImGui::NewFrame.
    float            mScrollDeltaY    = 0.0f;

    XrPosef          mPose{};

    // ImGui owns its own context; we just make sure to set it active
    // before calling into it (in case the user creates auxiliary
    // contexts later).
    void* /*ImGuiContext*/ mImGuiContext = nullptr;

    // Toast state.
    std::string      mToastText;
    float            mToastTimer    = 0.0f;  // seconds remaining
    float            mToastDuration = 0.0f;  // original duration (for fade calc)
    ToastKind        mToastKind     = ToastKind::Info;

    // Persistent perf overlay (always-on text widget).
    std::string      mPerfText;
    bool             mPerfActive    = false;

    // ROM-card icon cache (Vulkan textures + ImGui descriptor sets).
    IconCache        mIcons;

    // Pre-baked TTF fonts (owned by ImGui::GetIO().Fonts).
    ImFont*          mFontBody     = nullptr;
    ImFont*          mFontCaption  = nullptr;
    ImFont*          mFontDisplay  = nullptr;
};

} // namespace vr_pcvr
