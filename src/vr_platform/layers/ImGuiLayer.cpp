// SPDX-License-Identifier: GPL-3.0-or-later

#include "layers/ImGuiLayer.h"
#include "windows/VrFrameSubmitter.h"
#include "utils/LogUtils.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vr_pcvr {

namespace {

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

// Apply CitraVR's in-VR menu theme: Steam-ish dark slate with a teal
// accent that matches the wrist Menu button. Tuned for SRGB output and
// for being read at ~1m through a 0.6m quad in a Vulkan compositor.
void ApplyVrTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 10.0f;
    s.FrameRounding     = 8.0f;
    s.GrabRounding      = 6.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding       = 6.0f;
    s.ChildRounding     = 8.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(28, 24);
    s.FramePadding      = ImVec2(20, 16);
    s.ItemSpacing       = ImVec2(14, 14);
    s.ItemInnerSpacing  = ImVec2(10, 8);
    s.ScrollbarSize     = 22.0f;
    s.GrabMinSize       = 24.0f;
    s.SeparatorTextBorderSize = 3.0f;
    s.SeparatorTextPadding    = ImVec2(20, 6);
    s.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);

    // Palette.
    constexpr ImVec4 kBgDeep    = ImVec4(0.07f, 0.09f, 0.12f, 0.96f);
    constexpr ImVec4 kBgPanel   = ImVec4(0.11f, 0.14f, 0.18f, 1.00f);
    constexpr ImVec4 kBgFrame   = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
    constexpr ImVec4 kBgFrameHo = ImVec4(0.21f, 0.27f, 0.34f, 1.00f);
    constexpr ImVec4 kBgFrameAc = ImVec4(0.25f, 0.33f, 0.42f, 1.00f);
    constexpr ImVec4 kAccent    = ImVec4(0.30f, 0.72f, 0.95f, 1.00f); // teal
    constexpr ImVec4 kAccentHo  = ImVec4(0.45f, 0.85f, 1.00f, 1.00f);
    constexpr ImVec4 kAccentAc  = ImVec4(0.55f, 0.92f, 1.00f, 1.00f);
    constexpr ImVec4 kText      = ImVec4(0.93f, 0.95f, 0.97f, 1.00f);
    constexpr ImVec4 kTextDim   = ImVec4(0.63f, 0.69f, 0.76f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = kBgDeep;
    c[ImGuiCol_ChildBg]             = kBgPanel;
    c[ImGuiCol_PopupBg]             = kBgPanel;
    c[ImGuiCol_Border]              = ImVec4(0.20f, 0.26f, 0.33f, 0.50f);
    c[ImGuiCol_FrameBg]             = kBgFrame;
    c[ImGuiCol_FrameBgHovered]      = kBgFrameHo;
    c[ImGuiCol_FrameBgActive]       = kBgFrameAc;
    c[ImGuiCol_TitleBg]             = kBgPanel;
    c[ImGuiCol_TitleBgActive]       = kBgPanel;
    c[ImGuiCol_TitleBgCollapsed]    = kBgPanel;
    c[ImGuiCol_MenuBarBg]           = kBgPanel;
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.05f, 0.07f, 0.10f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]       = kBgFrameHo;
    c[ImGuiCol_ScrollbarGrabHovered]= kBgFrameAc;
    c[ImGuiCol_ScrollbarGrabActive] = kAccent;
    c[ImGuiCol_CheckMark]           = kAccent;
    c[ImGuiCol_SliderGrab]          = kAccent;
    c[ImGuiCol_SliderGrabActive]    = kAccentHo;
    c[ImGuiCol_Button]              = kBgFrame;
    c[ImGuiCol_ButtonHovered]       = kBgFrameHo;
    c[ImGuiCol_ButtonActive]        = kBgFrameAc;
    c[ImGuiCol_Header]              = ImVec4(0.18f, 0.36f, 0.50f, 0.55f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.22f, 0.45f, 0.62f, 0.75f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.26f, 0.55f, 0.75f, 0.90f);
    c[ImGuiCol_Separator]           = ImVec4(0.20f, 0.26f, 0.33f, 0.60f);
    c[ImGuiCol_SeparatorHovered]    = kAccent;
    c[ImGuiCol_SeparatorActive]     = kAccentHo;
    c[ImGuiCol_ResizeGrip]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab]                 = kBgFrame;
    c[ImGuiCol_TabHovered]          = kAccent;
    c[ImGuiCol_TabActive]           = kBgFrameAc;
    c[ImGuiCol_TabUnfocused]        = kBgFrame;
    c[ImGuiCol_TabUnfocusedActive]  = kBgFrameAc;
    c[ImGuiCol_PlotLines]           = kAccent;
    c[ImGuiCol_PlotLinesHovered]    = kAccentHo;
    c[ImGuiCol_PlotHistogram]       = kAccent;
    c[ImGuiCol_PlotHistogramHovered]= kAccentHo;
    c[ImGuiCol_TextSelectedBg]      = ImVec4(0.30f, 0.72f, 0.95f, 0.35f);
    c[ImGuiCol_NavHighlight]        = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.30f, 0.72f, 0.95f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.07f, 0.09f, 0.12f, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.07f, 0.09f, 0.12f, 0.65f);
    c[ImGuiCol_Text]                = kText;
    c[ImGuiCol_TextDisabled]        = kTextDim;
}

// Locate the bundled Roboto-Medium TTF. Prefers <exe_dir>/Roboto-Medium.ttf
// (shipped via POST_BUILD copy) and falls back to the absolute source-tree
// path baked in at compile time. Returns empty string if neither exists.
std::string LocateMenuFont() {
#if defined(_WIN32)
    wchar_t exeW[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exeW, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        // Strip the file name; keep trailing backslash.
        for (DWORD i = n; i > 0; --i) {
            if (exeW[i - 1] == L'\\' || exeW[i - 1] == L'/') {
                exeW[i] = 0;
                break;
            }
        }
        wchar_t fontW[MAX_PATH];
        std::swprintf(fontW, MAX_PATH, L"%lsRoboto-Medium.ttf", exeW);
        if (GetFileAttributesW(fontW) != INVALID_FILE_ATTRIBUTES) {
            // Convert wide -> UTF-8 for ImGui's AddFontFromFileTTF.
            char utf8[MAX_PATH * 4] = {};
            int len = WideCharToMultiByte(CP_UTF8, 0, fontW, -1,
                                          utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 0) return std::string(utf8);
        }
    }
#endif
#if defined(CITRA_VR_DEV_FONT_PATH)
    {
        const char* dev = CITRA_VR_DEV_FONT_PATH;
        if (dev && dev[0]) {
            FILE* f = std::fopen(dev, "rb");
            if (f) { std::fclose(f); return std::string(dev); }
        }
    }
#endif
    return std::string();
}

} // namespace

bool ImGuiLayer::Init(XrSession        session,
                      VkInstance       instance,
                      VkPhysicalDevice physical,
                      VkDevice         device,
                      uint32_t         queueFamilyIndex,
                      VkQueue          queue,
                      VrFrameSubmitter& submitter) {
    if (IsInitialised()) return true;

    mInstance       = instance;
    mPhysicalDevice = physical;
    mDevice         = device;
    mQueue          = queue;
    mQueueFamilyIdx = queueFamilyIndex;

    if (!mSwapchain.Create(session, kPixelW, kPixelH,
                           static_cast<int64_t>(kColorFormat),
                           /*arraySize=*/1, /*sampleCount=*/1)) {
        ALOGE("ImGuiLayer: VkXrSwapchain::Create failed");
        return false;
    }

    // ---- Render pass: single colour attachment, clear -> COLOR_ATTACHMENT_OPTIMAL.
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = kColorFormat;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &colorAtt;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;

    if (vkCreateRenderPass(device, &rpci, nullptr, &mRenderPass) != VK_SUCCESS) {
        ALOGE("ImGuiLayer: vkCreateRenderPass failed");
        Shutdown();
        return false;
    }

    // ---- Descriptor pool sized for ImGui's font texture + a per-ROM
    // icon descriptor set added by IconCache via ImGui_ImplVulkan_AddTexture.
    // Pool size caps the visible library at ~500 ROMs which matches the
    // RomBrowser entry cap.
    constexpr uint32_t kMaxIconDescs = 512;
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxIconDescs + 4},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets       = kMaxIconDescs + 4;
    dpci.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    dpci.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(device, &dpci, nullptr, &mDescriptorPool) != VK_SUCCESS) {
        ALOGE("ImGuiLayer: vkCreateDescriptorPool failed");
        Shutdown();
        return false;
    }

    // ---- ImGui context + Vulkan backend.
    IMGUI_CHECKVERSION();
    mImGuiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;       // No on-disk imgui.ini for VR.
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(kPixelW), static_cast<float>(kPixelH));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ApplyVrTheme();

    // ---- Font atlas: bake three sizes of Roboto Medium for typographic
    // hierarchy in the menu (caption / body / display). Body is installed
    // as the default so unannotated text renders at body size everywhere.
    // If the TTF is unavailable, ImGui's bundled ProggyClean is used.
    {
        const std::string fontPath = LocateMenuFont();
        if (!fontPath.empty()) {
            ImFontConfig cfg{};
            cfg.OversampleH = 3;
            cfg.OversampleV = 2;
            cfg.PixelSnapH  = false;
            mFontBody    = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 28.0f, &cfg);
            mFontCaption = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 22.0f, &cfg);
            // Display: render with slightly heavier oversampling for sharp
            // edges on the hero banner; this is the most prominent glyph
            // pass through the quad's bilinear filter.
            ImFontConfig cfgD = cfg;
            cfgD.OversampleH = 4;
            mFontDisplay = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 56.0f, &cfgD);
            if (mFontBody) {
                io.FontDefault = mFontBody;
                ALOGI("ImGuiLayer: loaded Roboto-Medium from %s", fontPath.c_str());
            }
        }
        if (!mFontBody) {
            ALOGI("ImGuiLayer: Roboto TTF not found; using ProggyClean fallback");
        }
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance        = instance;
    info.PhysicalDevice  = physical;
    info.Device          = device;
    info.QueueFamily     = queueFamilyIndex;
    info.Queue           = queue;
    info.PipelineCache   = VK_NULL_HANDLE;
    info.DescriptorPool  = mDescriptorPool;
    info.RenderPass      = mRenderPass;
    info.Subpass         = 0;
    info.MinImageCount   = 2;
    info.ImageCount      = std::max(2u, static_cast<uint32_t>(mSwapchain.Images().size()));
    info.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    info.Allocator       = nullptr;
    info.CheckVkResultFn = nullptr;

    if (!ImGui_ImplVulkan_Init(&info)) {
        ALOGE("ImGuiLayer: ImGui_ImplVulkan_Init failed");
        Shutdown();
        return false;
    }

    // Upload font texture using a one-shot command buffer routed through
    // VrFrameSubmitter so it lands on the dedicated VR queue.
    if (!submitter.IsInitialised()) {
        ALOGE("ImGuiLayer: submitter not ready for font upload");
        Shutdown();
        return false;
    }
    VkCommandBuffer cmd = submitter.Begin();
    if (cmd == VK_NULL_HANDLE) {
        ALOGE("ImGuiLayer: submitter.Begin() failed for font upload");
        Shutdown();
        return false;
    }
    ImGui_ImplVulkan_CreateFontsTexture();
    if (!submitter.Submit()) {
        ALOGE("ImGuiLayer: submitter.Submit() failed for font upload");
        Shutdown();
        return false;
    }
    // Wait for the upload to complete before returning - keeps the
    // first user frame deterministic.
    vkQueueWaitIdle(queue);
    ImGui_ImplVulkan_DestroyFontsTexture();   // Drop staging.

    // Default pose: 0.6 m wide quad floating in front of the user, slightly
    // above eye level. Caller can override via SetPose.
    mPose.orientation = XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
    mPose.position    = XrVector3f{0.0f, 0.2f, -1.2f};

    ALOGI("ImGuiLayer initialised (%ux%u R8G8B8A8_SRGB, %u swapchain images)",
          kPixelW, kPixelH, static_cast<unsigned>(mSwapchain.Images().size()));

    // ROM-card icon cache shares the same VkDevice / queue. Failure here
    // is non-fatal: GetOrUpload() returns nullptr so the UI just falls
    // back to text-only cards.
    if (!mIcons.Init(mDevice, mPhysicalDevice, mQueue, mQueueFamilyIdx)) {
        ALOGE("ImGuiLayer: IconCache init failed (cards will be text-only)");
    }
    return true;
}

void ImGuiLayer::DestroyVulkanObjects() {
    if (mDevice == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(mDevice);
    for (auto& kv : mFramebuffers) {
        if (kv.second != VK_NULL_HANDLE) vkDestroyFramebuffer(mDevice, kv.second, nullptr);
    }
    mFramebuffers.clear();
    for (auto& kv : mImageViews) {
        if (kv.second != VK_NULL_HANDLE) vkDestroyImageView(mDevice, kv.second, nullptr);
    }
    mImageViews.clear();
    if (mRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mRenderPass, nullptr);
        mRenderPass = VK_NULL_HANDLE;
    }
    if (mDescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(mDevice, mDescriptorPool, nullptr);
        mDescriptorPool = VK_NULL_HANDLE;
    }
}

void ImGuiLayer::Shutdown() {
    // Free icon descriptor sets BEFORE the ImGui Vulkan backend is
    // destroyed (RemoveTexture talks to the backend's descriptor pool).
    mIcons.Shutdown();
    if (mImGuiContext != nullptr) {
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(static_cast<ImGuiContext*>(mImGuiContext));
        mImGuiContext = nullptr;
    }
    DestroyVulkanObjects();
    mSwapchain.Destroy();
    mDevice         = VK_NULL_HANDLE;
    mPhysicalDevice = VK_NULL_HANDLE;
    mInstance       = VK_NULL_HANDLE;
    mQueue          = VK_NULL_HANDLE;
    mFrameOpen      = false;
    mAcquired       = false;
    mVisible        = false;
}

VkFramebuffer ImGuiLayer::GetOrCreateFramebuffer(VkImage image) {
    auto it = mFramebuffers.find(image);
    if (it != mFramebuffers.end()) return it->second;

    // Need an image view first.
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image                           = image;
    ivci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format                          = kColorFormat;
    ivci.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel   = 0;
    ivci.subresourceRange.levelCount     = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount     = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(mDevice, &ivci, nullptr, &view) != VK_SUCCESS) {
        ALOGE("ImGuiLayer: vkCreateImageView failed");
        return VK_NULL_HANDLE;
    }
    mImageViews[image] = view;

    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass      = mRenderPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments    = &view;
    fbci.width           = kPixelW;
    fbci.height          = kPixelH;
    fbci.layers          = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(mDevice, &fbci, nullptr, &fb) != VK_SUCCESS) {
        ALOGE("ImGuiLayer: vkCreateFramebuffer failed");
        return VK_NULL_HANDLE;
    }
    mFramebuffers[image] = fb;
    return fb;
}

void ImGuiLayer::SetMouse(int px, int py, bool pressed) {
    mMouseX    = static_cast<float>(px);
    mMouseY    = static_cast<float>(py);
    mMouseDown = pressed;
}

bool ImGuiLayer::BeginFrame() {
    // Visually visible covers both fully open AND mid-fade. We must
    // keep emitting frames during the close-fade so the alpha ramp
    // actually shows on the headset. Also run when a toast is active
    // so the notification renders even when the full menu is closed.
    if (!IsInitialised() || (mAlpha <= 0.001f && mToastTimer <= 0.0f && !mPerfActive)) {
        mFrameOpen = false;
        return false;
    }
    if (mImGuiContext != nullptr) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(mImGuiContext));
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize             = ImVec2(static_cast<float>(kPixelW), static_cast<float>(kPixelH));
    io.DeltaTime               = 1.0f / 90.0f;            // good-enough placeholder
    io.MousePos                = ImVec2(mMouseX, mMouseY);
    io.MouseDown[0]            = mMouseDown && mAlpha > 0.5f; // ignore clicks during fade
    // Inject accumulated thumbstick scroll. Consumed by ImGui::NewFrame()
    // below, then auto-zeroed by ImGui internally.
    io.MouseWheel             += mScrollDeltaY;
    mScrollDeltaY              = 0.0f;
    // Apply the open/close fade by modulating ImGui's global alpha.
    // Squared for a slightly more pleasing ease-in.
    ImGui::GetStyle().Alpha = mAlpha * mAlpha;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    mFrameOpen = true;
    return true;
}

void ImGuiLayer::Tick(float dt) {
    const float target = mVisible ? 1.0f : 0.0f;
    if (mAlpha == target) return;
    const float step = (kFadeSeconds > 0.0f) ? (dt / kFadeSeconds) : 1.0f;
    if (mAlpha < target) {
        mAlpha = (mAlpha + step >= target) ? target : (mAlpha + step);
    } else {
        mAlpha = (mAlpha - step <= target) ? target : (mAlpha - step);
    }
    // Tick down the toast timer.
    if (mToastTimer > 0.0f) {
        mToastTimer = std::max(0.0f, mToastTimer - dt);
    }
}

void ImGuiLayer::EndFrameAndRender(VrFrameSubmitter& submitter) {
    if (!mFrameOpen) return;
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    mFrameOpen = false;

    if (!IsInitialised() || drawData == nullptr || drawData->CmdListsCount <= 0) {
        return;
    }

    const uint32_t idx = mSwapchain.AcquireAndWait();
    if (idx >= mSwapchain.Images().size()) {
        ALOGE("ImGuiLayer: swapchain Acquire failed");
        return;
    }
    VkImage image = mSwapchain.Images()[idx];
    VkFramebuffer fb = GetOrCreateFramebuffer(image);
    if (fb == VK_NULL_HANDLE) {
        mSwapchain.Release();
        return;
    }

    VkCommandBuffer cmd = submitter.Begin();
    if (cmd == VK_NULL_HANDLE) {
        mSwapchain.Release();
        return;
    }

    // Render pass: clear to a slightly transparent dark blue. We use
    // alpha=0.85 so the layer passes through a touch of the world
    // behind it (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    // below). The background alpha is modulated by the fade so the
    // panel chrome fades in/out together with its widgets.
    const float fade = mAlpha * mAlpha;
    VkClearValue clear{};
    clear.color.float32[0] = 0.05f * fade;
    clear.color.float32[1] = 0.07f * fade;
    clear.color.float32[2] = 0.10f * fade;
    clear.color.float32[3] = 0.85f * fade;

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass        = mRenderPass;
    rpbi.framebuffer       = fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = {kPixelW, kPixelH};
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;

    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    vkCmdEndRenderPass(cmd);

    if (!submitter.Submit()) {
        ALOGE("ImGuiLayer: submitter.Submit failed");
    }
    mSwapchain.Release();
}

bool ImGuiLayer::BuildLayer(XrSpace space, XrCompositionLayerQuad& outLayer) const {
    if (!IsInitialised() || (!IsVisuallyVisible() && mToastTimer <= 0.0f)) return false;
    outLayer = {};
    outLayer.type                     = XR_TYPE_COMPOSITION_LAYER_QUAD;
    outLayer.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    outLayer.space                    = space;
    outLayer.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
    outLayer.subImage.swapchain       = mSwapchain.Handle();
    outLayer.subImage.imageRect       = {{0, 0},
                                         {static_cast<int32_t>(mSwapchain.Width()),
                                          static_cast<int32_t>(mSwapchain.Height())}};
    outLayer.subImage.imageArrayIndex = 0;
    outLayer.pose                     = mPose;
    outLayer.size                     = XrExtent2Df{kQuadW, kQuadH};
    return true;
}

void ImGuiLayer::ShowToast(std::string text, float duration) {
    ShowToast(std::move(text), ToastKind::Info, duration);
}

void ImGuiLayer::ShowToast(std::string text, ToastKind kind, float duration) {
    mToastText     = std::move(text);
    mToastDuration = duration;
    mToastTimer    = duration;
    mToastKind     = kind;
}

void ImGuiLayer::SetPerfOverlay(bool active, std::string line) {
    mPerfActive = active;
    mPerfText   = std::move(line);
}

void ImGuiLayer::DrawPerfOverlay() {
    if (!mPerfActive || !mFrameOpen || mPerfText.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.95f, 0.55f, 1.0f));
    ImGui::Begin("##vr_perf", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    if (mFontCaption) ImGui::PushFont(mFontCaption);
    ImGui::TextUnformatted(mPerfText.c_str());
    if (mFontCaption) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void ImGuiLayer::DrawToast() {
    if (mToastTimer <= 0.0f || !mFrameOpen) return;

    // Compute a smooth [0..1] fade envelope: fade-in over first 200 ms,
    // hold, then fade-out over the last 300 ms. The slide envelope reuses
    // the fade-in curve (eased) so the toast animates upward as it
    // becomes visible and slips back down as it dismisses.
    float fade = 1.0f;
    float slideT = 1.0f;       // 0 = below screen, 1 = settled
    if (mToastDuration > 0.0f) {
        const float elapsed = mToastDuration - mToastTimer;
        const float fadeIn  = std::min(1.0f, elapsed / 0.20f);
        const float fadeOut = std::min(1.0f, mToastTimer / 0.30f);
        fade   = std::min(fadeIn, fadeOut);
        // Ease-out cubic on the entrance for a soft landing.
        const float t = std::min(fadeIn, fadeOut);
        slideT = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    }

    if (fade < 0.01f) return;

    // Per-kind palette: stripe colour, soft icon-bubble fill, glyph.
    struct KindStyle { ImVec4 stripe; ImVec4 bubble; const char* glyph; };
    KindStyle ks{};
    switch (mToastKind) {
    case ToastKind::Success:
        ks = {ImVec4(0.30f, 0.85f, 0.50f, 1.0f),
              ImVec4(0.18f, 0.45f, 0.27f, 1.0f), "OK"};
        break;
    case ToastKind::Warning:
        ks = {ImVec4(0.95f, 0.72f, 0.20f, 1.0f),
              ImVec4(0.50f, 0.38f, 0.10f, 1.0f), "!"};
        break;
    case ToastKind::Error:
        ks = {ImVec4(0.95f, 0.36f, 0.36f, 1.0f),
              ImVec4(0.50f, 0.18f, 0.18f, 1.0f), "X"};
        break;
    case ToastKind::Info:
    default:
        ks = {ImVec4(0.30f, 0.72f, 0.95f, 1.0f),
              ImVec4(0.16f, 0.36f, 0.50f, 1.0f), "i"};
        break;
    }

    constexpr float kW      = static_cast<float>(kPixelW);
    constexpr float kH      = static_cast<float>(kPixelH);
    constexpr float kToastW = kW * 0.62f;
    constexpr float kToastH = 96.0f;
    const float posX  = (kW - kToastW) * 0.5f;
    const float posYR = kH - kToastH - 56.0f;     // resting Y
    // Slide up ~36 px during the entrance.
    const float posY  = posYR + (1.0f - slideT) * 36.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kToastW, kToastH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f * fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImVec4 borderCol = ks.stripe; borderCol.w = 0.55f * fade;
    ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
    ImGui::PushStyleColor(ImGuiCol_Text,   ImVec4(0.96f, 0.97f, 0.99f, fade));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.10f, 0.13f, 1.0f));
    ImGui::Begin("##vr_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoNav    |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    const ImVec2 wMin = ImGui::GetWindowPos();
    const ImVec2 wMax = ImVec2(wMin.x + kToastW, wMin.y + kToastH);
    ImDrawList* dl    = ImGui::GetWindowDrawList();

    // Left accent stripe (8 px) — reads as a category badge.
    ImVec4 stripeCol = ks.stripe; stripeCol.w *= fade;
    dl->AddRectFilled(wMin, ImVec2(wMin.x + 8.0f, wMax.y),
                      ImGui::GetColorU32(stripeCol), 14.0f,
                      ImDrawFlags_RoundCornersLeft);

    // Icon bubble: rounded square with the glyph centred. Sits 22 px
    // from the stripe, vertically centred.
    constexpr float kBubbleSz = 56.0f;
    const ImVec2 bMin(wMin.x + 22.0f, wMin.y + (kToastH - kBubbleSz) * 0.5f);
    const ImVec2 bMax(bMin.x + kBubbleSz, bMin.y + kBubbleSz);
    ImVec4 bubbleCol = ks.bubble; bubbleCol.w *= fade;
    dl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(bubbleCol), 12.0f);
    if (mFontDisplay) ImGui::PushFont(mFontDisplay);
    const ImVec2 gSz = ImGui::CalcTextSize(ks.glyph);
    dl->AddText(ImVec2(bMin.x + (kBubbleSz - gSz.x) * 0.5f,
                       bMin.y + (kBubbleSz - gSz.y) * 0.5f),
                ImGui::GetColorU32(ImVec4(0.96f, 0.98f, 1.0f, fade)),
                ks.glyph);
    if (mFontDisplay) ImGui::PopFont();

    // Message text: vertically centred to the right of the bubble.
    if (mFontBody) ImGui::PushFont(mFontBody);
    const ImVec2 tSz = ImGui::CalcTextSize(mToastText.c_str());
    const float textX = bMax.x + 18.0f;
    const float textY = wMin.y + (kToastH - tSz.y) * 0.5f;
    dl->AddText(ImVec2(textX, textY),
                ImGui::GetColorU32(ImVec4(0.96f, 0.97f, 0.99f, fade)),
                mToastText.c_str());
    if (mFontBody) ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

} // namespace vr_pcvr
