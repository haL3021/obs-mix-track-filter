#pragma once

#include <QWidget>
#include <QPointer>
#include <obs.h>
#include <obs-frontend-api.h>

#include <vector>
#include <chrono>

class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QCheckBox;
class QScrollArea;
class QLabel;
class QTimer;
class QResizeEvent;



/**
 * MixTrackFilterDock provides the Qt UI for managing mix track audio filters.
 * Features:
 * - List view of filters with individual enable/disable checkboxes.
 * - Add (+) popup enumerating available audio filters in OBS Studio.
 * - Remove (-), Move Up (▲), and Move Down (▼) controls.
 * - Dynamic Properties View for inspecting and tweaking selected filter settings.
 * - Native OBS filter manager launcher button.
 * - Persistence callback integration with obs_frontend_add_save_callback.
 * - Bidirectional synchronization with OBS native filter dialog via OBS source signals.
 */
class MixTrackFilterDock : public QWidget {
    Q_OBJECT

public:
    explicit MixTrackFilterDock(QWidget* parent = nullptr);
    ~MixTrackFilterDock() override;

    // Registers this dock into OBS Studio's UI via obs_frontend_add_dock_by_id
    static void registerDock();

    // Frontend save/load callback
    static void saveLoadCallback(obs_data_t* save_data, bool saving, void* private_data);

    // Notify dock of settings changes (called from SettingsDialog)
    static void notifySettingsChanged();

public slots:
    void refreshFilterList();

private slots:
    void onAddFilterClicked();
    void onRemoveFilterClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onFilterItemChanged(QListWidgetItem* item);
    void onBypassAllToggled(bool checked);
    void onOpenNativeFilterDialogClicked();
    void onAutoRehookToggled(bool checked);
    void onRehookOrderClicked();
    void updateOrderStatus();

    // Fine-grained synchronization slots invoked on Qt GUI thread
    void onFilterEnabledChanged(const QString& name, bool enabled);
    void onFilterRenamed(const QString& prevName, const QString& newName);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUI();
    void updateOrderLayout();
    void connectSourceSignals();
    void disconnectSourceSignals();
    void connectFilterSignals(obs_source_t* filter);
    void disconnectFilterSignals(obs_source_t* filter);
    void disconnectAllFilterSignals();

    // OBS source signal callbacks
    static void onFilterAddSignal(void* param, calldata_t* data);
    static void onFilterRemoveSignal(void* param, calldata_t* data);
    static void onReorderFiltersSignal(void* param, calldata_t* data);
    static void onFilterEnableSignal(void* param, calldata_t* data);
    static void onFilterRenameSignal(void* param, calldata_t* data);

    // Main layout
    QVBoxLayout* mainLayout = nullptr;

    // Controls
    QCheckBox* chkBypassAll = nullptr;
    QListWidget* filterList = nullptr;
    QPushButton* btnAdd = nullptr;
    QPushButton* btnRemove = nullptr;
    QPushButton* btnMoveUp = nullptr;
    QPushButton* btnMoveDown = nullptr;
    QPushButton* btnOpenNative = nullptr;
    QGridLayout* orderLayout = nullptr;
    QCheckBox* chkAutoRehook = nullptr;
    QLabel* lblOrderStatus = nullptr;
    QPushButton* btnRehookOrder = nullptr;
    QTimer* orderTimer = nullptr;
    bool isOrderSingleLine = false;
    bool isOrderLayoutInitialized = false;

    // Auto-rehook safety state
    std::chrono::steady_clock::time_point lastAutoRehookTime{};
    int autoRehookRetryCount = 0;
    static constexpr int MAX_AUTO_REHOOK_RETRIES = 3;
    static constexpr int AUTO_REHOOK_COOLDOWN_MS = 3000;


    // Synchronization state
    std::vector<obs_source_t*> hookedFilters;
    bool isInternalUpdating = false;
};
