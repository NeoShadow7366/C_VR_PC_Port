// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureVR;
}

/// VR-frontend preferences. Backed by `<UserDir>/vr_config.txt`
/// (`Common::VRConfig`); citra_vr.exe consumes these on startup.
class ConfigureVR : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureVR(QWidget* parent = nullptr);
    ~ConfigureVR() override;

    void ApplyConfiguration();
    void RetranslateUI();
    void SetConfiguration();

private:
    std::unique_ptr<Ui::ConfigureVR> ui;
};
