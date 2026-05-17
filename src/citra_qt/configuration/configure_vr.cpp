// SPDX-License-Identifier: GPL-3.0-or-later

#include "citra_qt/configuration/configure_vr.h"

#include "common/vr_config.h"
#include "ui_configure_vr.h"

namespace {

// Combo index <-> hmd_type id used in vr_config.txt and parsed by
// VRSettings::ParseHmdTypeId. Index 0 = auto-detect (empty value).
constexpr const char* kHmdIds[] = {"", "beyond2", "index", "vive", "generic"};
constexpr int         kHmdCount = static_cast<int>(sizeof(kHmdIds) / sizeof(kHmdIds[0]));

int HmdIdToIndex(const std::string& id) {
    for (int i = 0; i < kHmdCount; ++i) {
        if (id == kHmdIds[i]) {
            return i;
        }
    }
    return 0; // auto
}

} // namespace

ConfigureVR::ConfigureVR(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureVR>()) {
    ui->setupUi(this);
    SetConfiguration();
}

ConfigureVR::~ConfigureVR() = default;

void ConfigureVR::SetConfiguration() {
    ui->combo_hmd->setCurrentIndex(HmdIdToIndex(Common::VRConfig::Get("hmd_type")));

    // resolution_factor: 0 = auto (per-HMD default), 1..6 explicit.
    int rf = Common::VRConfig::GetInt("resolution_factor", 0);
    if (rf < 0 || rf > 6) {
        rf = 0;
    }
    ui->combo_resolution->setCurrentIndex(rf);

    int imm = Common::VRConfig::GetInt("vr_immersive_mode", 0);
    if (imm < 0 || imm > 2) {
        imm = 0;
    }
    ui->combo_immersive->setCurrentIndex(imm);

    ui->spin_factor3d->setValue(Common::VRConfig::GetInt("vr_factor_3d", 100));
    ui->check_hand_tracking->setChecked(Common::VRConfig::GetBool("enable_hand_tracking", true));
    ui->check_haptics->setChecked(Common::VRConfig::GetBool("enable_haptics", true));
}

void ConfigureVR::ApplyConfiguration() {
    const int hmdIdx = ui->combo_hmd->currentIndex();
    if (hmdIdx > 0 && hmdIdx < kHmdCount) {
        Common::VRConfig::Set("hmd_type", kHmdIds[hmdIdx]);
    } else {
        Common::VRConfig::Set("hmd_type", "");
    }

    Common::VRConfig::SetInt("resolution_factor", ui->combo_resolution->currentIndex());
    Common::VRConfig::SetInt("vr_immersive_mode", ui->combo_immersive->currentIndex());
    Common::VRConfig::SetInt("vr_factor_3d", ui->spin_factor3d->value());
    Common::VRConfig::SetBool("enable_hand_tracking", ui->check_hand_tracking->isChecked());
    Common::VRConfig::SetBool("enable_haptics", ui->check_haptics->isChecked());
}

void ConfigureVR::RetranslateUI() {
    ui->retranslateUi(this);
}
