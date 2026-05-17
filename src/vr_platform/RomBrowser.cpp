// SPDX-License-Identifier: GPL-3.0-or-later

#include "RomBrowser.h"

#include "common/logging/log.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cstdint>
#include <filesystem>
#include <locale>
#include <system_error>

namespace vr_pcvr {

namespace {

bool IsRomExtension(const std::string& ext_lower) {
    return ext_lower == ".3ds"  || ext_lower == ".3dsx" || ext_lower == ".cci" ||
           ext_lower == ".cxi"  || ext_lower == ".app"  || ext_lower == ".elf" ||
           ext_lower == ".axf";
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string PathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Convert a UTF-16 char16_t array (NUL-terminated within bounds) to UTF-8.
std::string Utf16ToUtf8(const char16_t* data, std::size_t max_chars) {
    std::u16string s16;
    s16.reserve(max_chars);
    for (std::size_t i = 0; i < max_chars && data[i] != 0; ++i) {
        s16.push_back(data[i]);
    }
    if (s16.empty()) return {};
    // codecvt is deprecated in C++17 but still functional and avoids
    // pulling in ICU just for short SMDH titles.
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
    try {
        return conv.to_bytes(s16);
    } catch (...) {
        return {};
    }
}

// Try to decode SMDH metadata for the given ROM. Populates long_title,
// program_id and icon_rgba (48x48 RGBA8) when available. All-or-nothing
// per field — caller treats empty fields as "missing".
void TryDecodeSmdh(const std::string& path, RomBrowser::Entry& out) {
    auto loader = Loader::GetLoader(path);
    if (!loader) return;

    std::uint64_t pid = 0;
    if (loader->ReadProgramId(pid) == Loader::ResultStatus::Success) {
        out.program_id = pid;
    }

    std::vector<std::uint8_t> smdh_bytes;
    if (loader->ReadIcon(smdh_bytes) != Loader::ResultStatus::Success) {
        return;
    }
    if (!Loader::IsValidSMDH(smdh_bytes)) return;

    const auto* smdh = reinterpret_cast<const Loader::SMDH*>(smdh_bytes.data());

    // Long title (English).
    const auto long16 = smdh->GetLongTitle(Loader::SMDH::TitleLanguage::English);
    out.long_title = Utf16ToUtf8(long16.data(), long16.size());

    // Decode 48x48 RGB565 icon → RGBA8.
    const std::vector<std::uint16_t> rgb565 = smdh->GetIcon(true);
    constexpr std::uint32_t kSize = 48;
    if (rgb565.size() < kSize * kSize) return;
    out.icon_w = kSize;
    out.icon_h = kSize;
    out.icon_rgba.resize(kSize * kSize * 4);
    for (std::uint32_t i = 0; i < kSize * kSize; ++i) {
        const std::uint16_t px = rgb565[i];
        const std::uint8_t r5 = static_cast<std::uint8_t>((px >> 11) & 0x1F);
        const std::uint8_t g6 = static_cast<std::uint8_t>((px >> 5)  & 0x3F);
        const std::uint8_t b5 = static_cast<std::uint8_t>( px        & 0x1F);
        // Expand to 8-bit with bit replication for full-range output.
        out.icon_rgba[i * 4 + 0] = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
        out.icon_rgba[i * 4 + 1] = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
        out.icon_rgba[i * 4 + 2] = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
        out.icon_rgba[i * 4 + 3] = 0xFF;
    }
}

} // namespace

RomBrowser::~RomBrowser() {
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void RomBrowser::Rescan(std::size_t maxEntries, std::size_t maxDepth) {
    if (mWorker.joinable()) {
        mWorker.join();
    }
    DoScan(maxEntries, maxDepth);
}

void RomBrowser::RescanAsync(std::size_t maxEntries, std::size_t maxDepth) {
    // If a previous worker has finished but not yet been joined, reap it.
    if (mWorker.joinable() && !mScanning.load(std::memory_order_acquire)) {
        mWorker.join();
    }
    // Already running: caller's request is coalesced with the in-flight scan.
    if (mScanning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Capture by value; root snapshot for thread safety.
    mWorker = std::thread([this, maxEntries, maxDepth]() {
        DoScan(maxEntries, maxDepth);
        mScanning.store(false, std::memory_order_release);
    });
}

std::vector<RomBrowser::Entry> RomBrowser::EntriesSnapshot() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return mEntries;
}

std::size_t RomBrowser::EntryCount() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return mEntries.size();
}

void RomBrowser::DoScan(std::size_t maxEntries, std::size_t maxDepth) {
    std::vector<Entry> result;
    if (mRoot.empty()) {
        LOG_INFO(Frontend, "RomBrowser: no root configured, nothing to scan");
    } else {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root_path(reinterpret_cast<const char8_t*>(mRoot.data()),
                                 reinterpret_cast<const char8_t*>(mRoot.data() + mRoot.size()));
        if (!fs::is_directory(root_path, ec)) {
            LOG_WARNING(Frontend, "RomBrowser: root '{}' is not a directory", mRoot);
        } else {
            fs::recursive_directory_iterator it(
                root_path,
                fs::directory_options::skip_permission_denied,
                ec);
            const fs::recursive_directory_iterator end;
            if (ec) {
                LOG_WARNING(Frontend, "RomBrowser: failed to open '{}': {}", mRoot, ec.message());
            } else {
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    if (static_cast<std::size_t>(it.depth()) > maxDepth) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    const auto& entry = *it;
                    if (!entry.is_regular_file(ec)) continue;
                    const auto ext = ToLowerAscii(entry.path().extension().string());
                    if (!IsRomExtension(ext)) continue;
                    Entry e;
                    e.full_path = PathToUtf8(entry.path());
                    std::error_code rel_ec;
                    auto rel = fs::relative(entry.path(), root_path, rel_ec);
                    const fs::path display_path =
                        (rel_ec ? entry.path().filename() : rel).replace_extension();
                    e.display_name = PathToUtf8(display_path);
                    std::replace(e.display_name.begin(), e.display_name.end(), '\\', '/');
                    // Best-effort SMDH decode — failures leave fields empty.
                    TryDecodeSmdh(e.full_path, e);
                    result.push_back(std::move(e));
                    if (result.size() >= maxEntries) {
                        LOG_INFO(Frontend, "RomBrowser: hit {} entry cap, stopping scan",
                                 maxEntries);
                        break;
                    }
                }
            }
        }
    }
    std::sort(result.begin(), result.end(),
              [](const Entry& a, const Entry& b) {
                  return a.display_name < b.display_name;
              });
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mEntries = std::move(result);
    }
    mHasScanned.store(true, std::memory_order_release);
    LOG_INFO(Frontend, "RomBrowser: scanned '{}', {} ROM(s)", mRoot, EntryCount());
    // Slice A diagnostic: report SMDH decode hit-rate so we can confirm
    // the loader path works against the user's library before wiring
    // the icons through to ImGui.
    {
        std::lock_guard<std::mutex> lk(mMutex);
        std::size_t with_icon = 0, with_title = 0;
        for (const auto& e : mEntries) {
            if (!e.icon_rgba.empty())  ++with_icon;
            if (!e.long_title.empty()) ++with_title;
        }
        LOG_INFO(Frontend, "RomBrowser: SMDH decode {} icon, {} title (of {})",
                 with_icon, with_title, mEntries.size());
    }
}

} // namespace vr_pcvr
