// SPDX-License-Identifier: GPL-3.0-or-later

#include "VrSettings.h"

#include <algorithm>
#include <cctype>

namespace VRSettings {

Values values;

uint32_t DefaultResolutionFactorFor(HMDType type) {
    switch (type) {
    case HMDType::BIGSCREEN_BEYOND_2:
        // Beyond 2 panels are extremely dense; 4x covers 1:1 quad sampling
        // at the default 1.0 m wide x 1.5 m distance configuration.
        return 4;
    case HMDType::VALVE_INDEX:
    case HMDType::VIVE_PRO:
        return 3;
    case HMDType::GENERIC_PCVR:
    case HMDType::UNKNOWN:
    default:
        return 2;
    }
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

HMDType DetectFromSystemName(const std::string& systemName) {
    const auto name = ToLower(systemName);
    if (name.find("beyond") != std::string::npos)         return HMDType::BIGSCREEN_BEYOND_2;
    if (name.find("index")  != std::string::npos)         return HMDType::VALVE_INDEX;
    if (name.find("vive")   != std::string::npos)         return HMDType::VIVE_PRO;
    if (!name.empty())                                    return HMDType::GENERIC_PCVR;
    return HMDType::UNKNOWN;
}

HMDType ParseHmdTypeId(const std::string& id) {
    const auto v = ToLower(id);
    if (v == "beyond2" || v == "beyond" || v == "bigscreen")  return HMDType::BIGSCREEN_BEYOND_2;
    if (v == "index"   || v == "valve"  || v == "knuckles")   return HMDType::VALVE_INDEX;
    if (v == "vive"    || v == "vivepro" || v == "vive_pro") return HMDType::VIVE_PRO;
    if (v == "generic" || v == "pcvr"   || v == "other")     return HMDType::GENERIC_PCVR;
    return HMDType::UNKNOWN;
}

} // namespace VRSettings
