// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-VR ROM browser helper. Walks a configured root directory for 3DS
// game files and exposes a simple cached entry list so the in-VR ImGui
// menu can present them as clickable buttons.
//
// Lifetime: owned by VrApp. Most methods run on the XR thread.
// RescanAsync() spawns a worker thread; destructor joins it.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vr_pcvr {

class RomBrowser {
public:
    struct Entry {
        // What to show on the button (relative-to-root, forward slashes).
        std::string display_name;
        // Absolute path passed back to the load-ROM callback.
        std::string full_path;

        // SMDH-derived metadata (decoded off-thread inside DoScan).
        // long_title is the English long title (UTF-8). Empty when the
        // ROM has no SMDH (e.g. .elf, .3dsx).
        std::string              long_title;
        // 48x48 RGBA8 icon bytes (9 KiB). Empty when no SMDH icon was
        // available; the UI should fall back to a colored placeholder.
        std::vector<std::uint8_t> icon_rgba;
        std::uint32_t            icon_w     = 0;
        std::uint32_t            icon_h     = 0;
        std::uint64_t            program_id = 0;
    };

    RomBrowser() = default;
    ~RomBrowser();
    RomBrowser(const RomBrowser&) = delete;
    RomBrowser& operator=(const RomBrowser&) = delete;

    // Set the directory to scan. Does not trigger a rescan; call Rescan().
    void SetRoot(std::string path) { mRoot = std::move(path); }
    const std::string& Root() const { return mRoot; }

    // Optional: tell the browser which ROM is currently loaded so the
    // UI can show a "Currently loaded:" header. Stored verbatim.
    void SetCurrentRom(std::string path) { mCurrentRom = std::move(path); }
    const std::string& CurrentRom() const { return mCurrentRom; }

    // Synchronous scan (blocks the caller). Used at startup.
    void Rescan(std::size_t maxEntries = 500, std::size_t maxDepth = 4);

    // Spawn a background worker that scans without blocking the caller.
    // Subsequent calls while a scan is in flight are ignored.
    void RescanAsync(std::size_t maxEntries = 500, std::size_t maxDepth = 4);

    // True when an async scan is currently running.
    bool IsScanning() const { return mScanning.load(std::memory_order_acquire); }

    // Snapshot the current entry list under the lock. Safe to call while
    // a scan is in progress (will return the previous results).
    std::vector<Entry> EntriesSnapshot() const;
    std::size_t        EntryCount() const;

    bool HasScanned() const { return mHasScanned.load(std::memory_order_acquire); }

private:
    void DoScan(std::size_t maxEntries, std::size_t maxDepth);

    std::string                mRoot;
    std::string                mCurrentRom;

    mutable std::mutex         mMutex;     // guards mEntries
    std::vector<Entry>         mEntries;

    std::atomic<bool>          mHasScanned{false};
    std::atomic<bool>          mScanning{false};
    std::thread                mWorker;
};

} // namespace vr_pcvr
