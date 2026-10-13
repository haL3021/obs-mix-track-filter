#pragma once

#include <QDialog>
#include <cstdint>

class QCheckBox;
class QLabel;
class QDialogButtonBox;

#define MIX_TRACK_FILTER_VERSION "0.3.10"

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override = default;

    // Singleton launcher / lifecycle helpers
    static void openDialog();
    static void destroyDialog();

    // Configuration file persistence (%APPDATA%/obs-studio/plugin_config/mix-track-filter/settings.json)
    static uint8_t loadTrackMask();
    static void saveTrackMask(uint8_t mask);
    static bool loadAutoRehook();
    static void saveAutoRehook(bool autoRehook);

    // Refresh UI checkboxes to reflect currently active settings
    void loadCurrentSettings();

private slots:
    void onAccepted();
    void onApplyClicked();

private:
    void setupUI();
    void applySettings();
    uint8_t getMaskFromUI() const;
    void setMaskToUI(uint8_t mask);

    QCheckBox* trackBoxes[6]{};
    QLabel* versionLabel = nullptr;
    QDialogButtonBox* buttonBox = nullptr;
};
