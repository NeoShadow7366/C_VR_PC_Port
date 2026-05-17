// SPDX-FileCopyrightText: 2026 CitraVR / Sheikah Protocol authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// citra_vr_launcher.exe
// ---------------------
// Tiny shim invoked by Steam (or directly) for the "Sheikah Protocol VR" entry.
//
//   1. If SteamVR's vrserver.exe is already running, skip ahead.
//   2. Otherwise launch "steam://run/250820" (SteamVR's Steam AppID) so Steam
//      starts SteamVR (which also makes SteamVR the active OpenXR runtime).
//   3. Poll up to LAUNCHER_TIMEOUT_S seconds for vrserver.exe to appear.
//   4. CreateProcessW citra_vr.exe (next to this launcher) with all argv
//      forwarded verbatim.
//
// Subsystem WINDOWS so no console flashes. All errors surfaced via MessageBox.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr int kLauncherTimeoutSeconds = 60;
constexpr wchar_t kSteamVrAppUri[] = L"steam://run/250820";
constexpr wchar_t kVrServerExe[]   = L"vrserver.exe";
constexpr wchar_t kCitraVrExe[]    = L"citra_vr.exe";

void ShowError(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"Sheikah Protocol VR Launcher",
                MB_OK | MB_ICONERROR);
}

bool IsProcessRunning(const wchar_t* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L"";
    std::wstring path(buf, n);
    auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash);
}

// Build a single command-line string from raw argv, quoting each properly.
// We skip argv[0] (launcher path) and prepend the citra_vr.exe path.
std::wstring BuildChildCmdLine(const std::wstring& exe, int argc, wchar_t** argv) {
    auto quote = [](const std::wstring& s) -> std::wstring {
        if (s.empty()) return L"\"\"";
        bool needs_quote = s.find_first_of(L" \t\"") != std::wstring::npos;
        if (!needs_quote) return s;
        std::wstring r = L"\"";
        for (wchar_t c : s) {
            if (c == L'"') r += L"\\\"";
            else r += c;
        }
        r += L"\"";
        return r;
    };
    std::wstring cmd = quote(exe);
    for (int i = 1; i < argc; ++i) {
        cmd += L" ";
        cmd += quote(argv[i]);
    }
    return cmd;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        ShowError(L"Failed to parse command line.");
        return 1;
    }

    const std::wstring exe_dir = GetExeDir();
    if (exe_dir.empty()) {
        ShowError(L"Failed to resolve launcher directory.");
        LocalFree(argv);
        return 1;
    }
    const std::wstring citra_vr = exe_dir + L"\\" + kCitraVrExe;
    if (GetFileAttributesW(citra_vr.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowError(L"citra_vr.exe not found next to launcher:\n" + citra_vr);
        LocalFree(argv);
        return 1;
    }

    // Phase 1: ensure SteamVR is up.
    if (!IsProcessRunning(kVrServerExe)) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", kSteamVrAppUri,
                                    nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            ShowError(L"Failed to launch SteamVR via Steam. "
                      L"Is Steam installed and running?");
            LocalFree(argv);
            return 1;
        }
        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start)
                   .count() < kLauncherTimeoutSeconds) {
            if (IsProcessRunning(kVrServerExe)) { ready = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!ready) {
            ShowError(L"SteamVR did not start within "
                      + std::to_wstring(kLauncherTimeoutSeconds)
                      + L" seconds. Aborting.");
            LocalFree(argv);
            return 1;
        }
        // Give SteamVR a beat to fully come online before OpenXR queries it.
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // Phase 2: launch citra_vr.exe with forwarded args.
    std::wstring child_cmd = BuildChildCmdLine(citra_vr, argc, argv);
    LocalFree(argv);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd_mutable = child_cmd; // CreateProcess can modify the buffer
    BOOL ok = CreateProcessW(
        citra_vr.c_str(),
        cmd_mutable.data(),
        nullptr, nullptr, FALSE,
        0,
        nullptr,
        exe_dir.c_str(),
        &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        ShowError(L"Failed to launch citra_vr.exe (error " + std::to_wstring(err) + L").");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
