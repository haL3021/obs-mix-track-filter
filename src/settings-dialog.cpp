#include "settings-dialog.hpp"
#include "audio-processor.hpp"
#include "mix-track-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QMainWindow>
#include <QPointer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>

static QPointer<SettingsDialog> s_settingsDialog = nullptr;

void SettingsDialog::openDialog() {
    if (!s_settingsDialog) {
        QMainWindow* mainWindow = static_cast<QMainWindow*>(obs_frontend_get_main_window());
        s_settingsDialog = new SettingsDialog(mainWindow);
    }
    s_settingsDialog->loadCurrentSettings();
    s_settingsDialog->show();
    s_settingsDialog->raise();
    s_settingsDialog->activateWindow();
}

void SettingsDialog::destroyDialog() {
    if (s_settingsDialog) {
        delete s_settingsDialog;
        s_settingsDialog = nullptr;
    }
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Mix Track Filter Settings");
    setupUI();
    loadCurrentSettings();
}

void SettingsDialog::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(14, 12, 14, 12);

    // 1. Top row: Version text aligned to the top right
    auto* topLayout = new QHBoxLayout();
    topLayout->addStretch();
    versionLabel = new QLabel(QString("Ver. %1").arg(MIX_TRACK_FILTER_VERSION), this);
    topLayout->addWidget(versionLabel);
    mainLayout->addLayout(topLayout);

    // 2. Track selection group box: Applied Tracks
    auto* trackGroup = new QGroupBox("Applied Tracks", this);
    auto* trackLayout = new QHBoxLayout(trackGroup);
    trackLayout->setSpacing(10);

    auto* trackLabel = new QLabel("Track:", trackGroup);
    trackLayout->addWidget(trackLabel);

    for (int i = 0; i < 6; ++i) {
        trackBoxes[i] = new QCheckBox(QString::number(i + 1), trackGroup);
        trackLayout->addWidget(trackBoxes[i]);
    }
    trackLayout->addStretch();
    mainLayout->addWidget(trackGroup);

    // 3. Dialog Button Box (OK / Cancel / Apply)
    buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::onApplyClicked);

    mainLayout->addWidget(buttonBox);
}

uint8_t SettingsDialog::getMaskFromUI() const {
    uint8_t mask = 0;
    for (int i = 0; i < 6; ++i) {
        if (trackBoxes[i] && trackBoxes[i]->isChecked()) {
            mask |= static_cast<uint8_t>(1 << i);
        }
    }
    return mask;
}

void SettingsDialog::setMaskToUI(uint8_t mask) {
    for (int i = 0; i < 6; ++i) {
        if (trackBoxes[i]) {
            trackBoxes[i]->setChecked((mask & (1 << i)) != 0);
        }
    }
}

void SettingsDialog::loadCurrentSettings() {
    uint8_t currentMask = AudioProcessor::instance().getTrackMask();
    setMaskToUI(currentMask);
}

void SettingsDialog::applySettings() {
    uint8_t newMask = getMaskFromUI();
    AudioProcessor::instance().setTrackMask(newMask);
    saveTrackMask(newMask);
    MixTrackFilterDock::notifySettingsChanged();
}

void SettingsDialog::onAccepted() {
    applySettings();
    accept();
}

void SettingsDialog::onApplyClicked() {
    applySettings();
}

static obs_data_t* openOrCreateSettingsData(char** out_path) {
    char* path = obs_module_config_path("settings.json");
    if (!path) return nullptr;

    QFileInfo fileInfo(QString::fromUtf8(path));
    fileInfo.dir().mkpath(".");

    obs_data_t* data = obs_data_create_from_json_file(path);
    if (!data) {
        data = obs_data_create();
    }

    // Ensure all keys are populated with defaults if missing
    if (!obs_data_has_user_value(data, "track_mask")) {
        obs_data_set_int(data, "track_mask", 0x01); // Default: Track 1
    }
    if (!obs_data_has_user_value(data, "auto_rehook")) {
        obs_data_set_bool(data, "auto_rehook", true); // Default: true
    }
    obs_data_set_string(data, "version", MIX_TRACK_FILTER_VERSION);

    if (out_path) {
        *out_path = path;
    } else {
        bfree(path);
    }
    return data;
}

static void commitSettingsData(obs_data_t* data, char* path) {
    if (!data || !path) return;

    bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
    if (ok) {
        blog(LOG_INFO, "[MixTrackFilter] Saved settings: version=%s, track_mask=0x%02llX, auto_rehook=%s",
             MIX_TRACK_FILTER_VERSION,
             static_cast<long long>(obs_data_get_int(data, "track_mask")),
             obs_data_get_bool(data, "auto_rehook") ? "true" : "false");
    } else {
        blog(LOG_ERROR, "[MixTrackFilter] Failed to save settings to %s", path);
    }

    obs_data_release(data);
    bfree(path);
}

uint8_t SettingsDialog::loadTrackMask() {
    obs_data_t* data = openOrCreateSettingsData(nullptr);
    if (!data) return 0x01;

    uint8_t mask = static_cast<uint8_t>(obs_data_get_int(data, "track_mask") & 0x3F);
    obs_data_release(data);
    return mask;
}

void SettingsDialog::saveTrackMask(uint8_t mask) {
    char* path = nullptr;
    obs_data_t* data = openOrCreateSettingsData(&path);
    if (!data || !path) return;

    obs_data_set_int(data, "track_mask", mask);
    commitSettingsData(data, path);
}

bool SettingsDialog::loadAutoRehook() {
    obs_data_t* data = openOrCreateSettingsData(nullptr);
    if (!data) return true;

    bool autoRehook = obs_data_get_bool(data, "auto_rehook");
    obs_data_release(data);
    return autoRehook;
}

void SettingsDialog::saveAutoRehook(bool autoRehook) {
    char* path = nullptr;
    obs_data_t* data = openOrCreateSettingsData(&path);
    if (!data || !path) return;

    obs_data_set_bool(data, "auto_rehook", autoRehook);
    commitSettingsData(data, path);
}


