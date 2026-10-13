#include "mix-track-dock.hpp"
#include "audio-processor.hpp"
#include "settings-dialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QCheckBox>
#include <QScrollArea>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QFormLayout>
#include <QTimer>
#include <QFrame>
#include <QResizeEvent>
#include <algorithm>


static MixTrackFilterDock* s_activeDock = nullptr;

MixTrackFilterDock::MixTrackFilterDock(QWidget* parent) : QWidget(parent) {
    s_activeDock = this;
    setupUI();
    connectSourceSignals();
    refreshFilterList();
    updateOrderStatus();
}

MixTrackFilterDock::~MixTrackFilterDock() {
    if (orderTimer) {
        orderTimer->stop();
    }
    disconnectSourceSignals();
    disconnectAllFilterSignals();
    if (s_activeDock == this) {
        s_activeDock = nullptr;
    }
}

void MixTrackFilterDock::registerDock() {
    auto dock = new MixTrackFilterDock();
    obs_frontend_add_dock_by_id("mix_track_filter_dock", "Mix Track Filter", dock);
    obs_frontend_add_save_callback(MixTrackFilterDock::saveLoadCallback, dock);
}

void MixTrackFilterDock::saveLoadCallback(obs_data_t* save_data, bool saving, void* private_data) {
    (void)private_data;
    if (saving) {
        AudioProcessor::instance().saveFilters(save_data);
    } else {
        AudioProcessor::instance().loadFilters(save_data);
        if (s_activeDock) {
            s_activeDock->refreshFilterList();
        }
    }
}

void MixTrackFilterDock::notifySettingsChanged() {
    if (s_activeDock) {
        s_activeDock->updateOrderStatus();
    }
}

void MixTrackFilterDock::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    // Filter List Header
    mainLayout->addWidget(new QLabel("Loaded Filter Chain:", this));

    // Filter List Widget
    filterList = new QListWidget(this);
    filterList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(filterList, &QListWidget::itemChanged, this, &MixTrackFilterDock::onFilterItemChanged);
    mainLayout->addWidget(filterList, 1);

    // Bypass check
    chkBypassAll = new QCheckBox("Bypass All Filters", this);
    chkBypassAll->setChecked(AudioProcessor::instance().isBypassed());
    connect(chkBypassAll, &QCheckBox::toggled, this, &MixTrackFilterDock::onBypassAllToggled);
    mainLayout->addWidget(chkBypassAll);

    // Button Grid: 2 rows x 2 columns
    // Column 0: Add, Remove; Column 1: Up, Down
    auto buttonLayout = new QGridLayout();
    buttonLayout->setSpacing(4);

    btnAdd = new QPushButton("+ Add", this);
    btnRemove = new QPushButton("- Remove", this);
    btnMoveUp = new QPushButton("▲ Up", this);
    btnMoveDown = new QPushButton("▼ Down", this);

    buttonLayout->addWidget(btnAdd, 0, 0);
    buttonLayout->addWidget(btnRemove, 1, 0);
    buttonLayout->addWidget(btnMoveUp, 0, 1);
    buttonLayout->addWidget(btnMoveDown, 1, 1);
    mainLayout->addLayout(buttonLayout);

    connect(btnAdd, &QPushButton::clicked, this, &MixTrackFilterDock::onAddFilterClicked);
    connect(btnRemove, &QPushButton::clicked, this, &MixTrackFilterDock::onRemoveFilterClicked);
    connect(btnMoveUp, &QPushButton::clicked, this, &MixTrackFilterDock::onMoveUpClicked);
    connect(btnMoveDown, &QPushButton::clicked, this, &MixTrackFilterDock::onMoveDownClicked);

    // Native OBS filter window launcher button
    btnOpenNative = new QPushButton("Open Filter Window...", this);
    connect(btnOpenNative, &QPushButton::clicked, this, &MixTrackFilterDock::onOpenNativeFilterDialogClicked);
    mainLayout->addWidget(btnOpenNative);

    // Separator line
    auto* hLine = new QFrame(this);
    hLine->setFrameShape(QFrame::HLine);
    hLine->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(hLine);

    // Audio dispatch order controls in a responsive grid layout
    orderLayout = new QGridLayout();
    orderLayout->setContentsMargins(0, 0, 0, 0);
    orderLayout->setSpacing(4);

    chkAutoRehook = new QCheckBox("Auto-Rehook", this);
    bool initialAutoRehook = SettingsDialog::loadAutoRehook();
    chkAutoRehook->setChecked(initialAutoRehook);
    connect(chkAutoRehook, &QCheckBox::toggled, this, &MixTrackFilterDock::onAutoRehookToggled);

    lblOrderStatus = new QLabel("Status: Checking...", this);
    lblOrderStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    mainLayout->addLayout(orderLayout);
    updateOrderLayout();

    btnRehookOrder = new QPushButton("Rehook Order", this);
    connect(btnRehookOrder, &QPushButton::clicked, this, &MixTrackFilterDock::onRehookOrderClicked);
    mainLayout->addWidget(btnRehookOrder);

    // Initialize timer for continuous periodic order inspection (always running)
    orderTimer = new QTimer(this);
    orderTimer->setInterval(1000);
    connect(orderTimer, &QTimer::timeout, this, &MixTrackFilterDock::updateOrderStatus);
    orderTimer->start();
}

void MixTrackFilterDock::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateOrderLayout();
}

void MixTrackFilterDock::updateOrderLayout() {
    if (!chkAutoRehook || !lblOrderStatus || !orderLayout) return;

    int margins = mainLayout ? (mainLayout->contentsMargins().left() + mainLayout->contentsMargins().right()) : 12;
    int availableWidth = width() - margins;

    int requiredWidth = chkAutoRehook->sizeHint().width() + lblOrderStatus->sizeHint().width() + 16;
    bool fitsInOneLine = (availableWidth >= requiredWidth);

    if (isOrderLayoutInitialized && fitsInOneLine == isOrderSingleLine) {
        return;
    }

    isOrderLayoutInitialized = true;
    isOrderSingleLine = fitsInOneLine;

    orderLayout->removeWidget(chkAutoRehook);
    orderLayout->removeWidget(lblOrderStatus);

    if (isOrderSingleLine) {
        // Single row: Checkbox on left, status on right
        orderLayout->addWidget(chkAutoRehook, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
        orderLayout->addWidget(lblOrderStatus, 0, 1, Qt::AlignRight | Qt::AlignVCenter);
        orderLayout->setColumnStretch(0, 0);
        orderLayout->setColumnStretch(1, 1);
    } else {
        // Two rows: Checkbox on row 0, right-aligned status on row 1
        orderLayout->addWidget(chkAutoRehook, 0, 0, 1, 2, Qt::AlignLeft | Qt::AlignVCenter);
        orderLayout->addWidget(lblOrderStatus, 1, 0, 1, 2, Qt::AlignRight | Qt::AlignVCenter);
        orderLayout->setColumnStretch(0, 1);
        orderLayout->setColumnStretch(1, 0);
    }
}

void MixTrackFilterDock::onAutoRehookToggled(bool checked) {
    SettingsDialog::saveAutoRehook(checked);
    autoRehookRetryCount = 0; // Reset circuit breaker counter
    updateOrderStatus();
    updateOrderLayout();
}

void MixTrackFilterDock::onRehookOrderClicked() {
    autoRehookRetryCount = 0; // Manual rehook clears paused circuit breaker
    AudioProcessor::instance().rehookAudio();
    updateOrderStatus();
}

void MixTrackFilterDock::updateOrderStatus() {
    if (!lblOrderStatus) {
        return;
    }

    DispatchOrderInfo info = AudioProcessor::instance().checkDispatchOrder();

    if (info.status == DispatchOrderStatus::Degraded) {
        if (chkAutoRehook && chkAutoRehook->isChecked()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAutoRehookTime).count();

            if (elapsed >= AUTO_REHOOK_COOLDOWN_MS) {
                if (autoRehookRetryCount < MAX_AUTO_REHOOK_RETRIES) {
                    lastAutoRehookTime = now;
                    autoRehookRetryCount++;
                    blog(LOG_INFO, "[MixTrackFilter] Degraded order detected on track %d. Auto-rehook triggered (attempt %d/%d).",
                         info.degradedTrack, autoRehookRetryCount, MAX_AUTO_REHOOK_RETRIES);
                    if (info.degradedTrack >= 1 && info.degradedTrack <= 6) {
                        AudioProcessor::instance().rehookTrack(static_cast<size_t>(info.degradedTrack - 1));
                    } else {
                        AudioProcessor::instance().rehookAudio();
                    }
                    info = AudioProcessor::instance().checkDispatchOrder();
                } else {
                    lblOrderStatus->setText("Status: Degraded (Auto-Rehook Paused)");
                    lblOrderStatus->setStyleSheet("color: #F44336; font-weight: bold;");
                    updateOrderLayout();
                    return;
                }
            }
        }
    }

    switch (info.status) {
    case DispatchOrderStatus::Optimal:
        autoRehookRetryCount = 0;
        lblOrderStatus->setText("Status: Optimal");
        lblOrderStatus->setStyleSheet("color: #4CAF50; font-weight: bold;");
        break;
    case DispatchOrderStatus::Degraded:
        lblOrderStatus->setText("Status: Degraded");
        lblOrderStatus->setStyleSheet("color: #FF9800; font-weight: bold;");
        break;
    case DispatchOrderStatus::Unhooked:
        lblOrderStatus->setText("Status: Unhooked");
        lblOrderStatus->setStyleSheet("color: #9E9E9E;");
        break;
    }
    updateOrderLayout();
}



void MixTrackFilterDock::onBypassAllToggled(bool checked) {
    AudioProcessor::instance().setBypassAll(checked);
}

void MixTrackFilterDock::onOpenNativeFilterDialogClicked() {
    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (dummySource) {

        obs_frontend_open_source_filters(dummySource);
    }
}

void MixTrackFilterDock::refreshFilterList() {
    if (isInternalUpdating) return;

    // Remember currently selected item name and row to restore selection after refresh
    QString selectedName;
    if (QListWidgetItem* cur = filterList->currentItem()) {
        selectedName = cur->data(Qt::UserRole).toString();
    }
    int selectedRow = filterList->currentRow();

    filterList->blockSignals(true);
    filterList->clear();

    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (dummySource) {
        // Disconnect previous filter hooks before reconnecting updated ones
        disconnectAllFilterSignals();

        struct EnumParam {
            MixTrackFilterDock* dock;
            QListWidget* list;
        } param = {this, filterList};

        obs_source_enum_filters(dummySource, [](obs_source_t*, obs_source_t* filter, void* p) {
            auto ep = static_cast<EnumParam*>(p);
            const char* name = obs_source_get_name(filter);
            bool enabled = obs_source_enabled(filter);

            auto item = new QListWidgetItem(QString::fromUtf8(name), ep->list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, QString::fromUtf8(name));

            // Hook filter-specific signals (enable, rename)
            ep->dock->connectFilterSignals(filter);
        }, &param);
    }

    // Restore selection: match by filter name first, fall back to row
    int newCount = filterList->count();
    if (newCount > 0) {
        int targetRow = -1;
        if (!selectedName.isEmpty()) {
            for (int i = 0; i < newCount; ++i) {
                if (filterList->item(i)->data(Qt::UserRole).toString() == selectedName) {
                    targetRow = i;
                    break;
                }
            }
        }
        if (targetRow == -1 && selectedRow >= 0) {
            targetRow = qMin(selectedRow, newCount - 1);
        }
        if (targetRow >= 0) {
            filterList->setCurrentRow(targetRow);
        }
    }

    filterList->blockSignals(false);
}

void MixTrackFilterDock::connectSourceSignals() {
    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    signal_handler_t* sh = obs_source_get_signal_handler(dummySource);
    if (sh) {
        signal_handler_connect(sh, "filter_add", MixTrackFilterDock::onFilterAddSignal, this);
        signal_handler_connect(sh, "filter_remove", MixTrackFilterDock::onFilterRemoveSignal, this);
        signal_handler_connect(sh, "reorder_filters", MixTrackFilterDock::onReorderFiltersSignal, this);
    }
}

void MixTrackFilterDock::disconnectSourceSignals() {
    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    signal_handler_t* sh = obs_source_get_signal_handler(dummySource);
    if (sh) {
        signal_handler_disconnect(sh, "filter_add", MixTrackFilterDock::onFilterAddSignal, this);
        signal_handler_disconnect(sh, "filter_remove", MixTrackFilterDock::onFilterRemoveSignal, this);
        signal_handler_disconnect(sh, "reorder_filters", MixTrackFilterDock::onReorderFiltersSignal, this);
    }
}

void MixTrackFilterDock::connectFilterSignals(obs_source_t* filter) {
    if (!filter) return;
    auto it = std::find(hookedFilters.begin(), hookedFilters.end(), filter);
    if (it != hookedFilters.end()) return;

    signal_handler_t* sh = obs_source_get_signal_handler(filter);
    if (sh) {
        signal_handler_connect(sh, "enable", MixTrackFilterDock::onFilterEnableSignal, this);
        signal_handler_connect(sh, "rename", MixTrackFilterDock::onFilterRenameSignal, this);
        hookedFilters.push_back(filter);
    }
}

void MixTrackFilterDock::disconnectFilterSignals(obs_source_t* filter) {
    if (!filter) return;
    auto it = std::find(hookedFilters.begin(), hookedFilters.end(), filter);
    if (it == hookedFilters.end()) return;

    signal_handler_t* sh = obs_source_get_signal_handler(filter);
    if (sh) {
        signal_handler_disconnect(sh, "enable", MixTrackFilterDock::onFilterEnableSignal, this);
        signal_handler_disconnect(sh, "rename", MixTrackFilterDock::onFilterRenameSignal, this);
    }
    hookedFilters.erase(it);
}

void MixTrackFilterDock::disconnectAllFilterSignals() {
    for (obs_source_t* filter : hookedFilters) {
        signal_handler_t* sh = obs_source_get_signal_handler(filter);
        if (sh) {
            signal_handler_disconnect(sh, "enable", MixTrackFilterDock::onFilterEnableSignal, this);
            signal_handler_disconnect(sh, "rename", MixTrackFilterDock::onFilterRenameSignal, this);
        }
    }
    hookedFilters.clear();
}

void MixTrackFilterDock::onFilterAddSignal(void* param, calldata_t* data) {
    (void)data;
    auto dock = static_cast<MixTrackFilterDock*>(param);
    if (!dock || AudioProcessor::instance().isLoadingFiltersActive()) return;

    QPointer<MixTrackFilterDock> safeDock(dock);
    QMetaObject::invokeMethod(dock, [safeDock]() {
        if (safeDock) {
            safeDock->refreshFilterList();
        }
    }, Qt::QueuedConnection);
}

void MixTrackFilterDock::onFilterRemoveSignal(void* param, calldata_t* data) {
    (void)data;
    auto dock = static_cast<MixTrackFilterDock*>(param);
    if (!dock || AudioProcessor::instance().isLoadingFiltersActive()) return;

    QPointer<MixTrackFilterDock> safeDock(dock);
    QMetaObject::invokeMethod(dock, [safeDock]() {
        if (safeDock) {
            safeDock->refreshFilterList();
        }
    }, Qt::QueuedConnection);
}

void MixTrackFilterDock::onReorderFiltersSignal(void* param, calldata_t* data) {
    (void)data;
    auto dock = static_cast<MixTrackFilterDock*>(param);
    if (!dock || AudioProcessor::instance().isLoadingFiltersActive()) return;

    QPointer<MixTrackFilterDock> safeDock(dock);
    QMetaObject::invokeMethod(dock, [safeDock]() {
        if (safeDock) {
            safeDock->refreshFilterList();
        }
    }, Qt::QueuedConnection);
}

void MixTrackFilterDock::onFilterEnableSignal(void* param, calldata_t* data) {
    auto dock = static_cast<MixTrackFilterDock*>(param);
    if (!dock) return;

    obs_source_t* filter = static_cast<obs_source_t*>(calldata_ptr(data, "source"));
    bool enabled = calldata_bool(data, "enabled");
    if (!filter) return;

    const char* name = obs_source_get_name(filter);
    QString filterName = QString::fromUtf8(name ? name : "");

    QPointer<MixTrackFilterDock> safeDock(dock);
    QMetaObject::invokeMethod(dock, [safeDock, filterName, enabled]() {
        if (safeDock) {
            safeDock->onFilterEnabledChanged(filterName, enabled);
        }
    }, Qt::QueuedConnection);
}

void MixTrackFilterDock::onFilterRenameSignal(void* param, calldata_t* data) {
    auto dock = static_cast<MixTrackFilterDock*>(param);
    if (!dock) return;

    const char* new_name = calldata_string(data, "new_name");
    const char* prev_name = calldata_string(data, "prev_name");

    QString newNameStr = QString::fromUtf8(new_name ? new_name : "");
    QString prevNameStr = QString::fromUtf8(prev_name ? prev_name : "");

    QPointer<MixTrackFilterDock> safeDock(dock);
    QMetaObject::invokeMethod(dock, [safeDock, prevNameStr, newNameStr]() {
        if (safeDock) {
            safeDock->onFilterRenamed(prevNameStr, newNameStr);
        }
    }, Qt::QueuedConnection);
}

void MixTrackFilterDock::onFilterEnabledChanged(const QString& name, bool enabled) {
    if (isInternalUpdating || name.isEmpty()) return;

    filterList->blockSignals(true);
    for (int i = 0; i < filterList->count(); ++i) {
        QListWidgetItem* item = filterList->item(i);
        if (item->data(Qt::UserRole).toString() == name) {
            item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
            break;
        }
    }
    filterList->blockSignals(false);
}

void MixTrackFilterDock::onFilterRenamed(const QString& prevName, const QString& newName) {
    if (isInternalUpdating || prevName.isEmpty() || newName.isEmpty()) return;

    filterList->blockSignals(true);
    for (int i = 0; i < filterList->count(); ++i) {
        QListWidgetItem* item = filterList->item(i);
        if (item->data(Qt::UserRole).toString() == prevName) {
            item->setText(newName);
            item->setData(Qt::UserRole, newName);
            break;
        }
    }
    filterList->blockSignals(false);
}

void MixTrackFilterDock::onAddFilterClicked() {
    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    QMenu menu(this);

    // Enumerate available filter types in OBS Studio
    size_t idx = 0;
    const char* typeId = nullptr;
    while (obs_enum_filter_types(idx++, &typeId)) {
        uint32_t caps = obs_get_source_output_flags(typeId);
        // Only list audio-capable filters (VST, Gain, Compressor, Limiter, etc.)
        if ((caps & OBS_SOURCE_AUDIO) != 0) {
            const char* displayName = obs_source_get_display_name(typeId);
            QString label = QString::fromUtf8(displayName ? displayName : typeId);
            QAction* action = menu.addAction(label);
            action->setData(QString::fromUtf8(typeId));
        }
    }

    QAction* selected = menu.exec(btnAdd->mapToGlobal(QPoint(0, btnAdd->height())));
    if (!selected) return;

    QString filterId = selected->data().toString();
    QString defaultName = selected->text();

    bool ok = false;
    QString filterName = QInputDialog::getText(this, "Add Mix Track Filter",
                                               "Enter unique filter name:",
                                               QLineEdit::Normal, defaultName, &ok);
    if (!ok || filterName.trimmed().isEmpty()) return;

    std::string idStr = filterId.toUtf8().constData();
    std::string nameStr = filterName.trimmed().toUtf8().constData();

    // Check if name already exists
    obs_source_t* existing = obs_source_get_filter_by_name(dummySource, nameStr.c_str());
    if (existing) {
        obs_source_release(existing);
        QMessageBox::warning(this, "Name Conflict", "A filter with this name already exists.");
        return;
    }

    obs_source_t* newFilter = obs_source_create_private(idStr.c_str(), nameStr.c_str(), nullptr);
    if (!newFilter) {
        QMessageBox::critical(this, "Creation Failed", "Failed to create the selected filter.");
        return;
    }

    // Adding to dummySource will trigger OBS filter_add signal,
    // which automatically updates AudioProcessor snapshot and refreshes dock list.
    obs_source_filter_add(dummySource, newFilter);
    obs_source_release(newFilter);
}

void MixTrackFilterDock::onRemoveFilterClicked() {
    QListWidgetItem* item = filterList->currentItem();
    if (!item) return;

    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    std::string name = item->data(Qt::UserRole).toString().toUtf8().constData();
    obs_source_t* filter = obs_source_get_filter_by_name(dummySource, name.c_str());
    if (filter) {
        // Removing from dummySource triggers filter_remove signal
        obs_source_filter_remove(dummySource, filter);
        obs_source_release(filter);
    }
}

void MixTrackFilterDock::onMoveUpClicked() {
    QListWidgetItem* item = filterList->currentItem();
    if (!item) return;

    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    std::string name = item->data(Qt::UserRole).toString().toUtf8().constData();
    obs_source_t* filter = obs_source_get_filter_by_name(dummySource, name.c_str());
    if (filter) {
        // Setting order triggers reorder_filters signal
        obs_source_filter_set_order(dummySource, filter, OBS_ORDER_MOVE_UP);
        obs_source_release(filter);
    }
}

void MixTrackFilterDock::onMoveDownClicked() {
    QListWidgetItem* item = filterList->currentItem();
    if (!item) return;

    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    std::string name = item->data(Qt::UserRole).toString().toUtf8().constData();
    obs_source_t* filter = obs_source_get_filter_by_name(dummySource, name.c_str());
    if (filter) {
        // Setting order triggers reorder_filters signal
        obs_source_filter_set_order(dummySource, filter, OBS_ORDER_MOVE_DOWN);
        obs_source_release(filter);
    }
}

void MixTrackFilterDock::onFilterItemChanged(QListWidgetItem* item) {
    if (!item || isInternalUpdating) return;

    obs_source_t* dummySource = AudioProcessor::instance().getDummySource();
    if (!dummySource) return;

    std::string name = item->data(Qt::UserRole).toString().toUtf8().constData();
    obs_source_t* filter = obs_source_get_filter_by_name(dummySource, name.c_str());
    if (filter) {
        bool enabled = (item->checkState() == Qt::Checked);
        if (obs_source_enabled(filter) != enabled) {
            isInternalUpdating = true;
            obs_source_set_enabled(filter, enabled);
            isInternalUpdating = false;
        }
        obs_source_release(filter);
    }
}
