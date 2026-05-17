// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny key=value persistent config used by the SteamVR/Windows VR
// frontend (citra_vr.exe) and the Qt configuration dialog.
//
// Stored at `<UserDir>/vr_config.txt`. Format: one `key=value` per line,
// '#' starts a comment, unknown keys are ignored. Designed to be read /
// written without locking - both the in-VR menu and citra-qt can update
// it; readers tolerate partial writes by silently dropping malformed
// lines.

#pragma once

#include <string>

namespace Common::VRConfig {

/// Absolute path of the on-disk config file.
std::string Path();

/// Returns the value for `key`, or empty string if missing.
std::string Get(const std::string& key);

/// Writes (or replaces) the value for `key`.
void Set(const std::string& key, const std::string& value);

/// Convenience: integer parse with default.
int GetInt(const std::string& key, int default_value);
void SetInt(const std::string& key, int value);

/// Convenience: bool parse ("1"/"true"/"yes" => true).
bool GetBool(const std::string& key, bool default_value);
void SetBool(const std::string& key, bool value);

} // namespace Common::VRConfig
