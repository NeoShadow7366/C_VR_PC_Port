// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/vr_config.h"

#include <cstdlib>
#include <fstream>
#include <vector>

#include "common/file_util.h"
#include "common/logging/log.h"

namespace Common::VRConfig {

std::string Path() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "vr_config.txt";
}

std::string Get(const std::string& key) {
    const std::string path = Path();
    if (!FileUtil::Exists(path)) {
        return {};
    }
    std::ifstream in;
    OpenFStream(in, path, std::ios::in);
    if (!in.is_open()) {
        return {};
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (line.compare(0, eq, key) == 0) {
            return line.substr(eq + 1);
        }
    }
    return {};
}

void Set(const std::string& key, const std::string& value) {
    const std::string path = Path();
    FileUtil::CreateFullPath(path);
    std::vector<std::string> lines;
    if (FileUtil::Exists(path)) {
        std::ifstream in;
        OpenFStream(in, path, std::ios::in);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(std::move(line));
            }
        }
    }
    bool replaced = false;
    for (auto& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (line.compare(0, eq, key) == 0) {
            line = key + "=" + value;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        lines.push_back(key + "=" + value);
    }
    std::ofstream out;
    OpenFStream(out, path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARNING(Frontend, "Failed to write VR config at {}", path);
        return;
    }
    for (const auto& line : lines) {
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.put('\n');
    }
}

int GetInt(const std::string& key, int default_value) {
    const std::string s = Get(key);
    if (s.empty()) {
        return default_value;
    }
    try {
        return std::stoi(s);
    } catch (...) {
        return default_value;
    }
}

void SetInt(const std::string& key, int value) {
    Set(key, std::to_string(value));
}

bool GetBool(const std::string& key, bool default_value) {
    const std::string s = Get(key);
    if (s.empty()) {
        return default_value;
    }
    return s == "1" || s == "true" || s == "True" || s == "yes" || s == "YES";
}

void SetBool(const std::string& key, bool value) {
    Set(key, value ? "1" : "0");
}

} // namespace Common::VRConfig
