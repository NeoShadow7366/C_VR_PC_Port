// SPDX-License-Identifier: GPL-3.0-or-later
//
// citra_vr - native SteamVR/OpenXR entry point for the Citra emulator.
//
// This binary mirrors the layout of `src/citra/citra.cpp` (the SDL2
// frontend), but instead of opening a Win32 window with SDL it:
//
//   1. Brings up `vr_pcvr::WinPlatform` to discover SteamVR's OpenXR
//      runtime and pick an HMD system.
//   2. Constructs an off-screen `vr_pcvr::EmuWindow_VR_Win` frontend so
//      Citra's `RendererVulkan` produces dual-screen frames into a
//      VkImage instead of presenting to a window surface.
//   3. Loads the chosen ROM through `Core::System::Load`, which in turn
//      builds the Vulkan renderer.
//   4. Hands the renderer's VkInstance / VkDevice / queue family to
//      `WinPlatform::InitSession` so the OpenXR session can bind to the
//      same Vulkan device the emulator already owns.
//   5. Wires the renderer's headless presentation path into the
//      EmuWindow's publish slot (see `EmuWindow_VR_Win::BindToRenderer`).
//   6. Spawns a dedicated XR thread that pumps the OpenXR event queue
//      and runs the per-frame `xrWaitFrame -> ... -> xrEndFrame` cycle
//      via `vr_pcvr::VrApp::Frame`. Meanwhile the main thread runs
//      Citra's normal `system.RunLoop()` until the runtime asks us to
//      exit.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/vr_config.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/hle/service/service.h"
#include "input_common/main.h"
#include "network/network.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "video_core/renderer_vulkan/vk_vr_hooks.h"
#include "vr_platform/VrApp.h"
#include "vr_platform/VrInputBridge.h"
#include "vr_platform/VrSettings.h"
#include "vr_platform/windows/EmuWindow_VR_Win.h"
#include "vr_platform/windows/WinPlatform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <filesystem>
#include <time.h>

extern "C" {
// Tell Nvidia drivers to use the dedicated GPU on hybrid laptops.
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
}
#endif

namespace {

#ifdef _WIN32
// True when the process was started from Explorer / Steam (no parent
// console). In that case any std::cout output and any LOG_CRITICAL
// messages would only appear in the briefly-flashing console window,
// so we additionally surface them via MessageBoxW so the user can read
// what went wrong.
bool ProcessHasOwnConsole() {
    HWND console = ::GetConsoleWindow();
    if (!console) return false;
    DWORD console_pid = 0;
    ::GetWindowThreadProcessId(console, &console_pid);
    return console_pid == ::GetCurrentProcessId();
}

void ShowFatalMessage(const std::string& msg) {
    if (!ProcessHasOwnConsole()) {
        const std::wstring wmsg(msg.begin(), msg.end());
        ::MessageBoxW(nullptr, wmsg.c_str(), L"citra_vr - error",
                      MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
    std::cerr << msg << std::endl;
}

// Write a MiniDump for the current process to %APPDATA%\Citra\dumps\.
// Returns the full path written, or empty string on failure. Safe to
// call from an SEH handler (only stack + small allocs, no C++ unwind).
std::string WriteMiniDump(EXCEPTION_POINTERS* ep) {
    char user_dir[MAX_PATH] = {};
    DWORD len = ::GetEnvironmentVariableA("APPDATA", user_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};

    std::string dump_dir = std::string(user_dir) + "\\Citra\\dumps";
    ::CreateDirectoryA((std::string(user_dir) + "\\Citra").c_str(), nullptr);
    ::CreateDirectoryA(dump_dir.c_str(), nullptr);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char filename[MAX_PATH];
    std::snprintf(filename, sizeof(filename),
                  "%s\\citra_vr_%04u%02u%02u_%02u%02u%02u.dmp",
                  dump_dir.c_str(), st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);

    HANDLE file = ::CreateFileA(filename, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    // MiniDumpWithDataSegs+IndirectlyReferencedMemory gives a useful
    // walkable stack + locals without ballooning to a full memory dump.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

    BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                  ::GetCurrentProcessId(), file, type,
                                  ep ? &mei : nullptr, nullptr, nullptr);
    ::CloseHandle(file);
    return ok ? std::string(filename) : std::string();
}
#else
void ShowFatalMessage(const std::string& msg) { std::cerr << msg << std::endl; }
#endif

// Scan `root` (recursively, depth-limited) for the first 3DS ROM file.
// Returns the absolute path as a UTF-8 string, or empty if none found.
// Used as a fallback when no ROM is on the command line and no last_rom
// is persisted, so the app can still start VR and open the in-VR picker.
static std::string FindFirstRomInDir(const std::string& root) {
    if (root.empty()) return {};
    namespace fs = std::filesystem;
    static constexpr int kMaxDepth = 4;
    // Extensions to recognise as 3DS ROMs (lower-case).
    auto is_rom_ext = [](const std::string& ext) {
        return ext == ".3ds" || ext == ".cci" || ext == ".cxi" || ext == ".3dsx";
    };
    try {
        fs::recursive_directory_iterator it(
            fs::path(root),
            fs::directory_options::skip_permission_denied);
        for (auto& entry : it) {
            if (it.depth() > kMaxDepth) { it.disable_recursion_pending(); continue; }
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (is_rom_ext(ext)) return entry.path().string();
        }
    } catch (...) {}
    return {};
}

void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [options] <filename>\n"
                 "  -h, --help        Show this help and exit\n"
                 "  -v, --version     Show version and exit\n"
                 "  -l, --loadslot N  Load save-state slot N after ROM loads\n"
                 "      --vk-debug    Enable Vulkan validation layers\n"
                 "                    (also via CITRA_VR_VK_DEBUG=1; off by default\n"
                 "                    because SteamVR's D3D11/Vulkan interop trips\n"
                 "                    validation-layer assertions and crashes)\n"
                 "      --selftest    Headless smoke-test: load ROM, run N frames,\n"
                 "                    exit 0 on success. No SteamVR / HMD required.\n"
                 "      --frames N    Number of RunLoop frames for --selftest (default 30)\n"
                 "Runs Citra inside SteamVR via OpenXR. Requires a running\n"
                 "SteamVR session and a Vulkan-capable GPU.\n";
}

void PrintVersion() {
    std::cout << "citra_vr " << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;
}

std::string PickRomFromArgs(int argc, char** argv) {
    std::string filepath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            PrintHelp(argv[0]);
            std::exit(0);
        }
        if (a == "-v" || a == "--version") {
            PrintVersion();
            std::exit(0);
        }
        if (!a.empty() && a[0] != '-') {
            filepath = a;
        }
    }
    return filepath;
}

[[maybe_unused]] static void* keep_pick_rom_referenced = reinterpret_cast<void*>(&PickRomFromArgs);

// vr_config.txt persistence helpers live in common/vr_config.{h,cpp};
// see Common::VRConfig::{Get,Set,GetInt,SetInt,GetBool,SetBool}.

} // namespace

int main(int argc, char** argv) {
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    // Make sure the file backend flushes even when we abort early.
    SCOPE_EXIT({ Common::Log::Stop(); });

#ifdef _WIN32
    // Catch unhandled SEH (e.g. cubeb's __fastfail / stack-cookie checks
    // that bypass normal C++ unwind) so the log file is flushed and we
    // know what stopped us.
    ::SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        const auto code = ep ? ep->ExceptionRecord->ExceptionCode : 0u;
        const auto addr = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        LOG_CRITICAL(Frontend, "Unhandled SEH 0x{:08x} at {}", code, addr);
        const std::string dump = WriteMiniDump(ep);
        if (!dump.empty()) {
            LOG_CRITICAL(Frontend, "Wrote crash dump: {}", dump);
        } else {
            LOG_CRITICAL(Frontend, "MiniDumpWriteDump failed (err {})",
                         ::GetLastError());
        }
        Common::Log::Stop();
        return EXCEPTION_EXECUTE_HANDLER;
    });
    // abort() / std::terminate() / pure-virtual / invalid-parameter
    // also bypass scope guards. Hook them for the same reason.
    std::set_terminate([] {
        LOG_CRITICAL(Frontend, "std::terminate called");
        const std::string dump = WriteMiniDump(nullptr);
        if (!dump.empty()) {
            LOG_CRITICAL(Frontend, "Wrote crash dump: {}", dump);
        }
        Common::Log::Stop();
        std::abort();
    });
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // First-chance C++ exception tracer: capture a stack trace at the throw
    // site so we can map it back to a source line via the PDB. Citra's Load()
    // is throwing std::out_of_range from somewhere we cannot see post-unwind.
    ::AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
        constexpr DWORD kCppExceptionCode = 0xE06D7363; // 'msc'
        if (!ep || ep->ExceptionRecord->ExceptionCode != kCppExceptionCode) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        void* frames[32]{};
        const USHORT n = ::CaptureStackBackTrace(0, 32, frames, nullptr);
        std::string trace;
        for (USHORT i = 0; i < n; ++i) {
            HMODULE mod = nullptr;
            ::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(frames[i]), &mod);
            char name[MAX_PATH] = "?";
            if (mod) {
                ::GetModuleFileNameA(mod, name, MAX_PATH);
            }
            const auto base = reinterpret_cast<uintptr_t>(mod);
            const auto ip = reinterpret_cast<uintptr_t>(frames[i]);
            const char* leaf = std::strrchr(name, '\\');
            leaf = leaf ? leaf + 1 : name;
            char line[256];
            std::snprintf(line, sizeof(line), "  %s+0x%llx\n", leaf,
                          static_cast<unsigned long long>(ip - base));
            trace += line;
        }
        LOG_CRITICAL(Frontend, "C++ throw stack:\n{}", trace);
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif

    Common::DetachedTasks detached_tasks;

#ifdef _WIN32
    // cubeb's WASAPI backend asserts CO_E_NOTINITIALIZED if COM has not
    // been brought up on the thread that creates the audio context.
    // The SDL/Qt frontends pull this in transparently; we have to do it
    // ourselves. STA matches what Qt does and works for both WASAPI and
    // shell APIs we may call later (e.g. file dialogs).
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SCOPE_EXIT({
        if (SUCCEEDED(com_hr)) {
            ::CoUninitialize();
        }
    });
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        LOG_WARNING(Frontend, "CoInitializeEx failed (hr=0x{:08x})",
                    static_cast<unsigned>(com_hr));
    }
#endif

#ifdef _WIN32
    int argc_w = 0;
    auto argv_w = ::CommandLineToArgvW(::GetCommandLineW(), &argc_w);
    std::string filepath;
    int  load_slot  = -1;  // -1 means "do not load a save state"
    bool vk_debug   = false;
    bool selftest   = false;
    int  selftest_frames = 30;
    for (int i = 1; i < argc_w; ++i) {
        const std::string a = Common::UTF16ToUTF8(argv_w[i]);
        if (a == "-h" || a == "--help") {
            PrintHelp(argc > 0 ? argv[0] : "citra_vr");
            ::LocalFree(argv_w);
            return 0;
        }
        if (a == "-v" || a == "--version") {
            PrintVersion();
            ::LocalFree(argv_w);
            return 0;
        }
        if ((a == "--loadslot" || a == "-l") && i + 1 < argc_w) {
            load_slot = std::atoi(Common::UTF16ToUTF8(argv_w[++i]).c_str());
            continue;
        }
        if (a == "--vk-debug") {
            vk_debug = true;
            continue;
        }
        if (a == "--selftest") {
            selftest = true;
            continue;
        }
        if (a == "--frames" && i + 1 < argc_w) {
            const int n = std::atoi(Common::UTF16ToUTF8(argv_w[++i]).c_str());
            if (n > 0) selftest_frames = n;
            continue;
        }
        if (!a.empty() && a[0] != '-') {
            filepath = a;
        }
    }
    if (argv_w) {
        ::LocalFree(argv_w);
    }
#else
    std::string filepath = PickRomFromArgs(argc, argv);
    int  load_slot       = -1;
    bool vk_debug        = false;
    bool selftest        = false;
    int  selftest_frames = 30;
#endif

    // In launcher mode (no ROM on command line), auto-select a ROM so VR can
    // start, then immediately open the in-VR Library so the user can pick
    // their actual game without ever leaving the headset.
    bool launcher_mode = false;

    if (filepath.empty()) {
        if (selftest) {
            LOG_CRITICAL(Frontend, "No ROM specified - pass the path on the command line");
            PrintHelp(argc > 0 ? argv[0] : "citra_vr");
            ShowFatalMessage("Usage: citra_vr.exe --selftest [--frames N] <rom.3dsx>\n"
                             "No ROM path was provided for --selftest.");
            return -1;
        }

        // 1. Resume last-played ROM (persisted in vr_config.txt on every load).
        filepath = Common::VRConfig::Get("last_rom");
        if (!filepath.empty() && !FileUtil::Exists(filepath)) {
            LOG_INFO(Frontend, "citra_vr: last_rom '{}' no longer exists, clearing", filepath);
            filepath.clear();
        }

        // 2. Scan rom_dir for any available ROM.
        if (filepath.empty()) {
            std::string rom_dir;
            if (const char* env = std::getenv("CITRA_VR_ROM_DIR"); env && *env)
                rom_dir = env;
            else
                rom_dir = Common::VRConfig::Get("rom_dir");
            if (!rom_dir.empty()) {
                LOG_INFO(Frontend, "citra_vr: no last_rom - scanning '{}' for any ROM", rom_dir);
                filepath = FindFirstRomInDir(rom_dir);
            }
        }

        if (filepath.empty()) {
            ShowFatalMessage(
                "CitraVR: No ROM to launch.\n\n"
                "To use the in-VR game picker:\n"
                "  Set CITRA_VR_ROM_DIR in run_citra_vr.bat to your ROMs folder.\n\n"
                "Or pass a ROM directly:\n"
                "  citra_vr.exe \"C:\\path\\to\\game.3ds\"");
            return -1;
        }

        LOG_INFO(Frontend, "citra_vr: launcher mode - booting '{}', in-VR picker will open",
                 filepath);
        launcher_mode = true;
    }

    MicroProfileOnThreadCreate("EmuThread");
    SCOPE_EXIT({ MicroProfileShutdown(); });

    // Force the Vulkan backend - the SteamVR composition path requires it.
    Settings::values.graphics_api = Settings::GraphicsAPI::Vulkan;

    // The VR layer composes its own stereo: each eye gets its own
    // composition layer. Force the renderer into mono so it doesn't
    // try to side-by-side stereo INTO the off-screen framebuffer
    // (which would pillarbox the actual game content into a narrow
    // strip and leave the rest black).
    Settings::values.render_3d = Settings::StereoRenderOption::Off;
    Settings::values.mono_render_option = Settings::MonoRenderOption::LeftEye;

    // Vulkan validation is OPT-IN: SteamVR's compositor uses D3D11/Vulkan
    // interop (BlankEyeBuffer with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT)
    // that trips internal validation-layer assertions and crashes the
    // process. Enable via --vk-debug or CITRA_VR_VK_DEBUG=1 when diagnosing.
    if (vk_debug || std::getenv("CITRA_VR_VK_DEBUG") != nullptr) {
        Settings::values.renderer_debug = true;
        LOG_INFO(Frontend,
                 "citra_vr: Vulkan validation enabled ({})",
                 vk_debug ? "--vk-debug" : "CITRA_VR_VK_DEBUG");
    }

    auto& system = Core::System::GetInstance();
    system.ApplySettings();

    Frontend::RegisterDefaultApplets(system);

    // Pull persisted VR-frontend preferences out of vr_config.txt so the
    // user's choice from the citra-qt VR tab (or in-VR menu) sticks
    // across restarts. Env vars (CITRA_VR_HMD) still take precedence
    // and are applied later by WinPlatform::CreateInstanceAndSystem.
    {
        const std::string hmd_id = Common::VRConfig::Get("hmd_type");
        if (!hmd_id.empty()) {
            VRSettings::values.hmd_type = VRSettings::ParseHmdTypeId(hmd_id);
        }
        VRSettings::values.resolution_factor =
            static_cast<uint32_t>(Common::VRConfig::GetInt(
                "resolution_factor",
                static_cast<int>(VRSettings::values.resolution_factor)));
        VRSettings::values.vr_immersive_mode = Common::VRConfig::GetInt(
            "vr_immersive_mode", VRSettings::values.vr_immersive_mode);
        VRSettings::values.vr_factor_3d = Common::VRConfig::GetInt(
            "vr_factor_3d", VRSettings::values.vr_factor_3d);
        VRSettings::values.enable_hand_tracking = Common::VRConfig::GetBool(
            "enable_hand_tracking", VRSettings::values.enable_hand_tracking);
        VRSettings::values.enable_haptics = Common::VRConfig::GetBool(
            "enable_haptics", VRSettings::values.enable_haptics);
        // Virtual screen geometry (screen_scale and screen_distance).
        // Stored as plain floats in vr_config.txt.
        {
            const std::string sv = Common::VRConfig::Get("screen_scale");
            if (!sv.empty()) {
                try { VRSettings::values.screen_scale = std::stof(sv); } catch (...) {}
            }
            const std::string dv = Common::VRConfig::Get("screen_distance");
            if (!dv.empty()) {
                try { VRSettings::values.screen_distance = std::stof(dv); } catch (...) {}
            }
            const std::string ev = Common::VRConfig::Get("eye_offset");
            if (!ev.empty()) {
                try { VRSettings::values.eye_offset = std::stof(ev); } catch (...) {}
            }
            // Wrist bar position (left-grip-local). Persisted by the
            // in-VR "Reposition wrist bar" drag mode.
            const std::string wox = Common::VRConfig::Get("wrist_offset_x");
            if (!wox.empty()) {
                try { VRSettings::values.wrist_offset_x = std::stof(wox); } catch (...) {}
            }
            const std::string woy = Common::VRConfig::Get("wrist_offset_y");
            if (!woy.empty()) {
                try { VRSettings::values.wrist_offset_y = std::stof(woy); } catch (...) {}
            }
            const std::string woz = Common::VRConfig::Get("wrist_offset_z");
            if (!woz.empty()) {
                try { VRSettings::values.wrist_offset_z = std::stof(woz); } catch (...) {}
            }
            const std::string wtd = Common::VRConfig::Get("wrist_tilt_deg");
            if (!wtd.empty()) {
                try { VRSettings::values.wrist_tilt_deg = std::stof(wtd); } catch (...) {}
            }
        }
        VRSettings::values.auto_resume = Common::VRConfig::GetBool(
            "auto_resume", VRSettings::values.auto_resume);
        LOG_INFO(Frontend,
                 "vr_config.txt loaded: hmd_type='{}'({}) resolution_factor={} "
                 "vr_immersive_mode={} vr_factor_3d={} enable_hand_tracking={} enable_haptics={}",
                 hmd_id, static_cast<int>(VRSettings::values.hmd_type),
                 VRSettings::values.resolution_factor,
                 VRSettings::values.vr_immersive_mode, VRSettings::values.vr_factor_3d,
                 VRSettings::values.enable_hand_tracking, VRSettings::values.enable_haptics);
        // Push VR config values into the Citra Settings so HID / rasterizer
        // pick them up without needing a separate code path.
        Settings::values.vr_immersive_mode =
            static_cast<u32>(VRSettings::values.vr_immersive_mode);
        Settings::values.factor_3d =
            static_cast<u32>(VRSettings::values.vr_factor_3d);
    }

    // Bring up the input bridge BEFORE Core::System::Load so the HID
    // service binds the active input profile to our "vr" engine.
    auto input_bridge = std::make_unique<vr_pcvr::VrInputBridge>();
    InputCommon::Init();
    // Network must be initialised before Core::System::LoadState, which
    // dereferences Network::GetRoomMember() to check multiplayer state.
    // Without this, save-state load crashes with a null shared_ptr deref.
    Network::Init();
    input_bridge->RegisterFactories();
    input_bridge->OverrideInputProfile();
    // Restore any user-customised button bindings from vr_config.txt.
    input_bridge->LoadBindings();

    // D5: Auto-resume — if no explicit --loadslot and auto_resume is set,
    // check whether the last-saved ROM matches this session and restore the slot.
    if (load_slot < 0 && VRSettings::values.auto_resume) {
        const std::string last_rom  = Common::VRConfig::Get("last_rom");
        const int         last_slot = Common::VRConfig::GetInt("last_slot", -1);
        if (!last_rom.empty() && last_rom == filepath && last_slot >= 0) {
            load_slot = last_slot;
            LOG_INFO(Frontend, "citra_vr: auto-resume: applying last_slot={} for '{}'",
                     load_slot, filepath);
        }
    }

    // ---- Phase 1: bring up OpenXR (instance + system) --------------------
    // In --selftest mode we skip the OpenXR instance creation entirely so
    // that the smoke-test can run without SteamVR / an HMD present.
    auto platform = std::make_unique<vr_pcvr::WinPlatform>();
    Vulkan::VrHooks vr_hooks{};
    SCOPE_EXIT({ Vulkan::SetVrHooks(nullptr); });
    if (!selftest) {
        if (!platform->InitInstance()) {
            LOG_CRITICAL(Frontend,
                         "Failed to create OpenXR instance. Is SteamVR running and an HMD connected?");
            ShowFatalMessage(
                "Failed to create an OpenXR instance.\n\n"
                "Checklist:\n"
                "  1. SteamVR is running and showing the headset (small status panel).\n"
                "  2. SteamVR is the active OpenXR runtime\n"
                "     (SteamVR -> Settings -> Developer -> 'Set SteamVR as OpenXR runtime').\n"
                "  3. Headset is on, awake, and detected by SteamVR.\n\n"
                "See %APPDATA%\\Citra\\log\\citra_log.txt for the OpenXR error code.");
            return -1;
        }

        // Install Vulkan creation hooks so Citra's RendererVulkan routes its
        // VkInstance / VkPhysicalDevice / VkDevice through xrCreateVulkanInstanceKHR
        // / xrGetVulkanGraphicsDevice2KHR / xrCreateVulkanDeviceKHR. Without this
        // SteamVR rejects xrCreateSession with XR_ERROR_GRAPHICS_DEVICE_INVALID.
        vr_hooks.create_instance     = &vr_pcvr::WinPlatform::HookCreateInstance;
        vr_hooks.get_physical_device = &vr_pcvr::WinPlatform::HookGetPhysicalDevice;
        vr_hooks.create_device       = &vr_pcvr::WinPlatform::HookCreateDevice;
        Vulkan::SetVrHooks(&vr_hooks);

        // Pick a sensible default supersampling factor based on the detected HMD.
        if (VRSettings::values.resolution_factor == 0) {
            VRSettings::values.resolution_factor =
                VRSettings::DefaultResolutionFactorFor(VRSettings::values.hmd_type);
        }
    } else {
        LOG_INFO(Frontend, "citra_vr --selftest: skipping OpenXR instance (headless mode)");
        // Use resolution_factor=1 in selftest to keep resource usage minimal.
        if (VRSettings::values.resolution_factor == 0) {
            VRSettings::values.resolution_factor = 1;
        }
    }

    // ---- Phase 2: create the headless EmuWindow + load the ROM ----------
    auto emu_window = std::make_unique<vr_pcvr::EmuWindow_VR_Win>(
        system, *platform, VRSettings::values.resolution_factor);

    // Other frontends (citra SDL, Qt, Android) populate this map from their
    // config files; without it Service::Init throws std::out_of_range from
    // Settings::values.lle_modules.at(name). Default every module to HLE.
    if (Settings::values.lle_modules.empty()) {
        for (const auto& service_module : Service::service_module_map) {
            Settings::values.lle_modules.emplace(service_module.name, false);
        }
    }

    // The HID service binds its touch device from current_input_profile.touch_device.
    // Other frontends populate this from their config; without it HID gets a null
    // device and our EmuWindow::TouchPressed updates are silently ignored.
    if (Settings::values.current_input_profile.touch_device.empty()) {
        Settings::values.current_input_profile.touch_device = "engine:emu_window";
    }

    LOG_INFO(Frontend, "citra_vr: loading ROM '{}'", filepath);
    Core::System::ResultStatus load_result;
    try {
        load_result = system.Load(*emu_window, filepath, /*secondary_window=*/nullptr);
    } catch (const std::exception& ex) {
        LOG_CRITICAL(Frontend, "system.Load threw std::exception: {}", ex.what());
        Common::Log::Stop();
        ShowFatalMessage(std::string("system.Load threw an exception:\n  ") + ex.what());
        return -1;
    } catch (...) {
        LOG_CRITICAL(Frontend, "system.Load threw an unknown exception");
        Common::Log::Stop();
        ShowFatalMessage("system.Load threw an unknown exception (see log).");
        return -1;
    }
    LOG_INFO(Frontend, "citra_vr: system.Load returned {}", static_cast<int>(load_result));
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Failed to load ROM ({}): {}",
                     static_cast<int>(load_result), system.GetStatusDetails());
        ShowFatalMessage(
            "Failed to load ROM:\n  " + filepath +
            "\n\nResult code: " + std::to_string(static_cast<int>(load_result)) +
            "\nDetails: " + system.GetStatusDetails() +
            "\n\nSee %APPDATA%\\Citra\\log\\citra_log.txt for full diagnostics.");
        return -1;
    }

    // D5: Persist the successfully-loaded ROM path so auto-resume can match it.
    Common::VRConfig::Set("last_rom", filepath);

    // If the user requested a save-state load via --loadslot, apply it
    // NOW - before the XR session is bound to the renderer. LoadState
    // calls System::Shutdown(true) + Init() which destroys & recreates
    // the Vulkan renderer; doing this after InitSession would dangle the
    // XR session's VkDevice handle. The signal-based path is the same
    // one Save uses, so all the renderer-cache rebuild logic runs
    // exactly as it does in non-VR Citra.
    if (load_slot >= 0) {
        LOG_INFO(Frontend, "citra_vr: applying pre-XR LoadState slot {}", load_slot);
        // We just freshly Init()-ed the System, so the renderer/services
        // are pristine. Tell serialize to skip Shutdown(true)+Init() and
        // deserialise straight into the existing subsystems. This avoids
        // destroying the brand-new VR-Vulkan renderer (which crashes its
        // own dtor in this configuration).
        system.skip_shutdown_on_load = true;
        system.SendSignal(Core::System::Signal::Load, static_cast<u32>(load_slot));
        // Drive one RunLoop iteration so the signal is consumed.
        try {
            const auto rs = system.RunLoop();
            LOG_INFO(Frontend, "citra_vr: post-LoadState RunLoop returned {}",
                     static_cast<int>(rs));
            if (rs != Core::System::ResultStatus::Success &&
                rs != Core::System::ResultStatus::ShutdownRequested) {
                LOG_ERROR(Frontend,
                          "citra_vr: LoadState slot {} did not succeed (rs={}): {}",
                          load_slot, static_cast<int>(rs), system.GetStatusDetails());
            }
        } catch (const std::exception& ex) {
            LOG_CRITICAL(Frontend, "LoadState threw: {}", ex.what());
            ShowFatalMessage(std::string("LoadState threw an exception:\n  ") + ex.what());
            return -1;
        } catch (...) {
            LOG_CRITICAL(Frontend, "LoadState threw an unknown exception");
            ShowFatalMessage("LoadState threw an unknown exception (see log).");
            return -1;
        }
        // Done — re-enable the standard rebuild path for any future
        // in-process LoadStates (currently we never do these from VR
        // anyway, but keep the flag scoped to this one operation).
        system.skip_shutdown_on_load = false;
    }

    // ---- Selftest (--selftest): headless smoke-test, no OpenXR required ---
    // Run `selftest_frames` RunLoop iterations then exit cleanly. The test
    // covers: logging, settings, VR config parse, input init, service init,
    // ROM load + Vulkan renderer creation, and N frames of 3DS emulation.
    // It does NOT test the OpenXR session, XR swapchain, or controller input.
    // Exit 0 = pass, non-zero = fail.
    if (selftest) {
        LOG_INFO(Frontend, "citra_vr --selftest: running {} frames (headless)", selftest_frames);
        std::atomic<bool> st_stop{false};
        auto* renderer_base = &system.GPU().Renderer();
        auto* renderer = dynamic_cast<Vulkan::RendererVulkan*>(renderer_base);
        if (renderer) {
            renderer->Rasterizer()->LoadDiskResources(
                st_stop,
                [](VideoCore::LoadCallbackStage, std::size_t, std::size_t) {});
        }
        int frames_ok = 0;
        for (int i = 0; i < selftest_frames; ++i) {
            Core::System::ResultStatus rs;
            try {
                rs = system.RunLoop();
            } catch (const std::exception& ex) {
                LOG_CRITICAL(Frontend, "citra_vr --selftest: RunLoop threw at frame {}: {}",
                             i, ex.what());
                return 1;
            } catch (...) {
                LOG_CRITICAL(Frontend, "citra_vr --selftest: RunLoop threw unknown at frame {}", i);
                return 1;
            }
            if (rs == Core::System::ResultStatus::ShutdownRequested) {
                LOG_INFO(Frontend, "citra_vr --selftest: ShutdownRequested at frame {}", i);
                break;
            }
            if (rs != Core::System::ResultStatus::Success) {
                LOG_ERROR(Frontend, "citra_vr --selftest: RunLoop error {} at frame {}: {}",
                          static_cast<int>(rs), i, system.GetStatusDetails());
                return 1;
            }
            ++frames_ok;
        }
        LOG_INFO(Frontend, "citra_vr --selftest: PASS — {} frames completed", frames_ok);
        std::cout << "SELFTEST PASS: " << frames_ok << " frames\n";

        // Minimal clean shutdown for the selftest path.
        Network::Shutdown();
        InputCommon::Shutdown();
        emu_window.reset();
        system.Shutdown();
        detached_tasks.WaitForAllTasks();
        return 0;
    }

    // ---- Phase 3: bind OpenXR session to the renderer's Vulkan device ---
    LOG_INFO(Frontend, "citra_vr: querying renderer");
    auto* renderer_base = &system.GPU().Renderer();
    LOG_INFO(Frontend, "citra_vr: renderer base = {}",
             static_cast<const void*>(renderer_base));
    auto* renderer = dynamic_cast<Vulkan::RendererVulkan*>(renderer_base);
    LOG_INFO(Frontend, "citra_vr: RendererVulkan dyn-cast = {}",
             static_cast<const void*>(renderer));
    if (renderer == nullptr) {
        LOG_CRITICAL(Frontend,
                     "RendererVulkan was not created - VR backend requires graphics_api=Vulkan");
        ShowFatalMessage(
            "Citra did not create a Vulkan renderer.\n\n"
            "The VR backend requires graphics_api = Vulkan. Check\n"
            "%APPDATA%\\Citra\\config\\sdl2-config.ini and ensure\n"
            "  [Renderer]\n  graphics_api = 1\n"
            "or simply delete that file and let citra_vr regenerate it.");
        return -1;
    }

    auto& vk_instance = renderer->GetVulkanInstance();
    LOG_INFO(Frontend, "citra_vr: calling InitSession (vk_inst={}, dev={}, queue_family={}, vr_queue_index={})",
             static_cast<const void*>(vk_instance.GetInstance()),
             static_cast<const void*>(vk_instance.GetDevice()),
             vk_instance.GetGraphicsQueueFamilyIndex(),
             platform->VrQueueIndex());
    if (!platform->InitSession(static_cast<VkInstance>(vk_instance.GetInstance()),
                               static_cast<VkPhysicalDevice>(vk_instance.GetPhysicalDevice()),
                               static_cast<VkDevice>(vk_instance.GetDevice()),
                               vk_instance.GetGraphicsQueueFamilyIndex(),
                               platform->VrQueueIndex())) {
        LOG_CRITICAL(Frontend, "xrCreateSession failed - cannot continue");
        ShowFatalMessage(
            "xrCreateSession failed.\n\n"
            "This usually means SteamVR rejected the Vulkan device Citra picked\n"
            "(wrong physical device or missing required extension).\n\n"
            "See %APPDATA%\\Citra\\log\\citra_log.txt for the OpenXR error code.");
        return -1;
    }

    // Hook the headless present path into the EmuWindow so finished
    // dual-screen frames land in the publish slot the XR thread reads.
    emu_window->BindToRenderer(renderer->GetMainPresentWindow());

    // Force the renderer to apply our overridden bg_color (we set it
    // before the renderer existed, so the initial ApplySettings was
    // a no-op for the gpu side).
    renderer->Settings().bg_color_update_requested = true;

    // ---- Phase 4: build the platform-agnostic VR app + XR thread --------
    auto vr_app = std::make_unique<vr_pcvr::VrApp>(*platform);
    vr_app->SetEmuWindow(emu_window.get());
    if (!vr_app->Start()) {
        LOG_CRITICAL(Frontend, "VrApp::Start failed");
        ShowFatalMessage(
            "VrApp::Start failed (XR swapchain or input setup).\n\n"
            "See %APPDATA%\\Citra\\log\\citra_log.txt for details.");
        return -1;
    }

    // Configure the in-VR ROM browser. Root precedence:
    //   1. CITRA_VR_ROM_DIR env var (explicit user preference).
    //   2. Persisted vr_config.txt rom_dir= (set last time the user
    //      picked a ROM through the in-VR browser).
    //   3. Directory of the ROM that was just loaded (likely siblings).
    {
        std::string rom_dir;
        if (const char* env = std::getenv("CITRA_VR_ROM_DIR"); env != nullptr && *env) {
            rom_dir = env;
        } else if (auto cfg = Common::VRConfig::Get("rom_dir"); !cfg.empty()) {
            rom_dir = std::move(cfg);
        } else {
            const auto last_slash = filepath.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                rom_dir = filepath.substr(0, last_slash);
            }
        }
        vr_app->Browser().SetRoot(std::move(rom_dir));
        vr_app->Browser().SetCurrentRom(filepath);
    }

    LOG_INFO(Frontend, "citra_vr: {} | {}-{}", Common::g_build_fullname, Common::g_scm_branch,
             Common::g_scm_desc);
    Settings::LogSettings();

    std::atomic<bool> exit_requested{false};

    // Wire the in-VR menu's "load this ROM" action. We don't try to swap
    // ROMs in-process (LoadState already trips a renderer-cache rebuild
    // crash on this codebase, see /memories/repo/vr_load_state_crash.md);
    // instead we write a sentinel file the launcher .bat picks up and
    // restart with the chosen ROM.
    vr_app->SetOnLoadRom([&exit_requested](const std::string& rom_path) {
        const std::string user_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
        const std::string sentinel = user_dir + "next_rom.txt";
        FileUtil::CreateFullPath(sentinel);
        std::ofstream out;
        OpenFStream(out, sentinel, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out.write(rom_path.data(), static_cast<std::streamsize>(rom_path.size()));
            out.close();
            LOG_INFO(Frontend, "Wrote next-rom sentinel: {} -> {}", sentinel, rom_path);
        } else {
            LOG_ERROR(Frontend, "Failed to write next-rom sentinel at {}", sentinel);
        }
        // Persist the chosen ROM's directory as the next-session default
        // browser root, so users don't need to set CITRA_VR_ROM_DIR.
        const auto last_slash = rom_path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            Common::VRConfig::Set("rom_dir", rom_path.substr(0, last_slash));
        }
        exit_requested.store(true);
    });

    // Save-state Load: write both the current ROM path (so the launcher
    // re-launches the same game) and the requested slot. citra_vr will
    // see the slot via --loadslot N and apply LoadState before binding
    // the XR session.
    vr_app->SetOnLoadState([&exit_requested, &filepath](int slot) {
        const std::string user_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
        const std::string rom_sentinel  = user_dir + "next_rom.txt";
        const std::string slot_sentinel = user_dir + "next_load_slot.txt";
        FileUtil::CreateFullPath(rom_sentinel);
        {
            std::ofstream out;
            OpenFStream(out, rom_sentinel, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(filepath.data(), static_cast<std::streamsize>(filepath.size()));
            }
        }
        {
            std::ofstream out;
            OpenFStream(out, slot_sentinel, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                const std::string s = std::to_string(slot);
                out.write(s.data(), static_cast<std::streamsize>(s.size()));
            }
        }
        LOG_INFO(Frontend, "Wrote load-state sentinel: slot {} for ROM {}", slot, filepath);
        exit_requested.store(true);
    });

    // Wrist-button long-press -> 3DS Home pulse via the input bridge.
    vr_app->SetOnHome([&input_bridge]() {
        if (input_bridge) input_bridge->FireHome();
    });

    // Hand the in-VR menu a pointer to the bridge so the Controls pane
    // can read/write the remappable button bindings directly.
    vr_app->SetInputBridge(input_bridge.get());

    // Show the first-launch welcome card if the user has never seen it.
    if (!Common::VRConfig::GetBool("seen_welcome", false)) {
        vr_app->SetShowWelcome(true);
    }

    // Launcher mode: open the in-VR ROM browser immediately so the user
    // can pick their game without leaving the headset.
    if (launcher_mode) {
        vr_app->SetLauncherMode(true);
    }

    std::thread xr_thread([&] {
        MicroProfileOnThreadCreate("XrThread");
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
        bool ever_running = false;
        try {
            while (!exit_requested.load(std::memory_order_relaxed)) {
                if (!vr_app->PollEvents()) {
                    exit_requested.store(true);
                    break;
                }
                if (vr_app->ExitRequested()) {
                    LOG_INFO(Frontend, "VrApp requested exit (menu Quit) - shutting down");
                    exit_requested.store(true);
                    break;
                }
                if (vr_app->IsRunning()) {
                    if (!ever_running) {
                        ever_running = true;
                        LOG_INFO(Frontend, "XR session reached running state - starting frame loop");
                    }
                    vr_app->Frame(/*renderCitraToImage=*/nullptr, /*userdata=*/nullptr);
                    input_bridge->Apply(vr_app->Input(),
                                        vr_app->IsMenuOpen(),
                                        vr_app->IsRightTouchActive(),
                                        vr_app->IsWristStartHeld(),
                                        vr_app->IsWristSelectHeld());
                } else {
                    // Session not yet READY (or stopping) - throttle the spin.
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        } catch (const std::exception& ex) {
            LOG_CRITICAL(Frontend, "XR thread threw std::exception: {}", ex.what());
            Common::Log::Stop();
            exit_requested.store(true);
        } catch (...) {
            LOG_CRITICAL(Frontend, "XR thread threw an unknown exception");
            Common::Log::Stop();
            exit_requested.store(true);
        }
    });

    // ---- Phase 5: run Citra's emulator core on the main thread ---------
    std::atomic<bool> stop_run{false};
    renderer->Rasterizer()->LoadDiskResources(
        stop_run,
        [](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            LOG_DEBUG(Frontend, "Loading stage {} progress {}/{}", static_cast<u32>(stage), value,
                      total);
        });

    while (!exit_requested.load(std::memory_order_relaxed)) {
        // Pause-on-menu: when the in-VR menu is open, skip RunLoop so the
        // emulated CPU/GPU stop advancing. Sleep briefly so the main
        // thread doesn't busy-spin.
        if (vr_app && vr_app->Menu().IsVisible()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        // C6: Auto-pause when the HMD is not on-head (session VISIBLE without
        // FOCUSED). Emulation continues in the XR thread but the CPU core
        // is throttled so battery / thermals are not wasted off-head.
        if (vr_app && !vr_app->IsHmdOnHead()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        Core::System::ResultStatus result;
        try {
            result = system.RunLoop();
        } catch (const std::exception& ex) {
            LOG_CRITICAL(Frontend, "system.RunLoop threw std::exception: {}", ex.what());
            Common::Log::Stop();
            exit_requested.store(true);
            break;
        } catch (...) {
            LOG_CRITICAL(Frontend, "system.RunLoop threw an unknown exception");
            Common::Log::Stop();
            exit_requested.store(true);
            break;
        }
        switch (result) {
        case Core::System::ResultStatus::ShutdownRequested:
            exit_requested.store(true);
            break;
        case Core::System::ResultStatus::Success:
            break;
        default:
            LOG_ERROR(Frontend, "Error in main run loop: {} ({})", static_cast<int>(result),
                      system.GetStatusDetails());
            break;
        }
    }

    // ---- Phase 6a: stop XR thread (release the OpenXR session) ----------
    exit_requested.store(true);
    if (xr_thread.joinable()) {
        xr_thread.join();
    }

    // ---- Phase 7: sentinel-driven self-relaunch -------------------------
    // Done BEFORE the destructive shutdown below: system.Shutdown() on this
    // branch can crash (see /memories/repo/vr_load_state_crash.md), and a
    // crash there would prevent the relaunch. The XR session is already
    // released above so the freshly-spawned child can grab it cleanly.
    // ---------------------------------------------------------------------
    // The in-VR menu's "Load this ROM" / "Load save state N" actions write
    // sentinel files in the user dir then request exit. Historically the
    // .bat launcher consumed these and re-spawned us with the right args.
    // Doing the relaunch here too means it works regardless of how we
    // were started (run_citra_vr.bat, citra-qt --vr, double-click, etc.).
#ifdef _WIN32
    {
        const std::string user_dir   = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
        const std::string rom_sent   = user_dir + "next_rom.txt";
        const std::string slot_sent  = user_dir + "next_load_slot.txt";
        if (FileUtil::Exists(rom_sent)) {
            std::string next_rom;
            FileUtil::ReadFileToString(true, rom_sent, next_rom);
            // Trim trailing whitespace / newlines.
            while (!next_rom.empty() &&
                   (next_rom.back() == '\n' || next_rom.back() == '\r' ||
                    next_rom.back() == ' '  || next_rom.back() == '\t')) {
                next_rom.pop_back();
            }
            FileUtil::Delete(rom_sent);

            std::string next_slot;
            if (FileUtil::Exists(slot_sent)) {
                FileUtil::ReadFileToString(true, slot_sent, next_slot);
                while (!next_slot.empty() &&
                       (next_slot.back() == '\n' || next_slot.back() == '\r' ||
                        next_slot.back() == ' '  || next_slot.back() == '\t')) {
                    next_slot.pop_back();
                }
                FileUtil::Delete(slot_sent);
            }

            if (!next_rom.empty() && FileUtil::Exists(next_rom)) {
                // Build the command line: "<exe>" [--loadslot N] "<rom>"
                wchar_t exe_path[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
                const std::wstring rom_w  = Common::UTF8ToUTF16W(next_rom);
                std::wstring cmdline = L"\"";
                cmdline += exe_path;
                cmdline += L"\"";
                if (!next_slot.empty()) {
                    cmdline += L" --loadslot ";
                    cmdline += Common::UTF8ToUTF16W(next_slot);
                }
                cmdline += L" \"";
                cmdline += rom_w;
                cmdline += L"\"";

                LOG_INFO(Frontend, "Self-relaunch: rom='{}' slot='{}'", next_rom, next_slot);
                Common::Log::Stop(); // flush before spawning

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
                cmd_buf.push_back(L'\0');
                if (CreateProcessW(exe_path, cmd_buf.data(), nullptr, nullptr, FALSE,
                                   0, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    // Skip the destructive shutdown below: the child is
                    // already running and Phase 6b's system.Shutdown() can
                    // crash on this branch, which would also kill us mid-
                    // exit and pollute the log. ExitProcess gets us out
                    // cleanly without running C++ destructors.
                    ::ExitProcess(0);
                } // failure leaves the user back at their previous shell
            }
        }
    }
#endif

    // ---- Phase 6b: destructive shutdown (may crash on this branch) ------
    // Anything that *must* happen before exit (relaunch, log flush) has
    // already happened above, so a crash here is harmless to the user
    // experience.
    Network::Shutdown();
    InputCommon::Shutdown();

    vr_app.reset();
    input_bridge.reset();
    emu_window.reset();
    platform.reset();

    system.Shutdown();
    detached_tasks.WaitForAllTasks();

    return 0;
}
