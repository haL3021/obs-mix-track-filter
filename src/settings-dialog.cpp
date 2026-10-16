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

static bool s_settingsCached = false;
static uint8_t s_cachedTrackMask = 0x01;
static bool s_cachedAutoRehook = true;

void SettingsDialog::applySettings() {
    uint8_t newMask = getMaskFromUI();
    if (newMask == AudioProcessor::instance().getTrackMask()) {
        return;
    }
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

static void ensureSettingsLoaded() {
    if (s_settingsCached) return;

    char* path = obs_module_config_path("settings.json");
    if (!path) return;

    obs_data_t* data = obs_data_create_from_json_file(path);
    bfree(path);

    if (data) {
        if (obs_data_has_user_value(data, "track_mask")) {
            s_cachedTrackMask = static_cast<uint8_t>(obs_data_get_int(data, "track_mask") & 0x3F);
        }
        if (obs_data_has_user_value(data, "auto_rehook")) {
            s_cachedAutoRehook = obs_data_get_bool(data, "auto_rehook");
        }
        obs_data_release(data);
    }
    s_settingsCached = true;
}

static void saveSettingsToFile() {
    char* path = obs_module_config_path("settings.json");
    if (!path) return;

    QFileInfo fileInfo(QString::fromUtf8(path));
    fileInfo.dir().mkpath(".");

    obs_data_t* data = obs_data_create_from_json_file(path);
    if (!data) {
        data = obs_data_create();
    }

    obs_data_set_int(data, "track_mask", s_cachedTrackMask);
    obs_data_set_bool(data, "auto_rehook", s_cachedAutoRehook);
    obs_data_set_string(data, "version", MIX_TRACK_FILTER_VERSION);

    bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
    if (ok) {
        blog(LOG_INFO, "[MixTrackFilter] Saved settings: version=%s, track_mask=0x%02X, auto_rehook=%s",
             MIX_TRACK_FILTER_VERSION,
             static_cast<unsigned int>(s_cachedTrackMask),
             s_cachedAutoRehook ? "true" : "false");
    } else {
        blog(LOG_ERROR, "[MixTrackFilter] Failed to save settings to %s", path);
    }

    obs_data_release(data);
    bfree(path);
}

uint8_t SettingsDialog::loadTrackMask() {
    ensureSettingsLoaded();
    return s_cachedTrackMask;
}

void SettingsDialog::saveTrackMask(uint8_t mask) {
    ensureSettingsLoaded();
    s_cachedTrackMask = mask & 0x3F;
    saveSettingsToFile();
}

bool SettingsDialog::loadAutoRehook() {
    ensureSettingsLoaded();
    return s_cachedAutoRehook;
}

void SettingsDialog::saveAutoRehook(bool autoRehook) {
    ensureSettingsLoaded();
    s_cachedAutoRehook = autoRehook;
    saveSettingsToFile();
}


