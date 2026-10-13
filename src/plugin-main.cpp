#include "plugin-main.h"
#include "audio-processor.hpp"
#include "mix-track-dock.hpp"
#include "settings-dialog.hpp"

#include <QAction>

// Bypass strict version check (master branch libobs is 33.0, but user is on 32.2.2)
#undef LIBOBS_API_VER
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(32, 0, 0)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mix-track-filter-plugin", "en-US")

MODULE_EXPORT const char* obs_module_description(void) {
    return "Mix Track Audio Filter Plugin for OBS Studio with Qt Dock UI.";
}

static void on_frontend_event(enum obs_frontend_event event, void* private_data) {
    (void)private_data;
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        AudioProcessor::instance().setFrontendLoaded(true);
        AudioProcessor::instance().hookAudio();
    } else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED ||
               event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
               event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED ||
               event == OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED) {
        // Encoders are appended to the audio input array when output starts.
        // Re-hook AudioProcessor so it is placed at the end of the array,
        // ensuring it runs FIRST in OBS's backwards (LIFO) audio dispatch loop.
        AudioProcessor::instance().rehookAudio();
    }
}

bool obs_module_load(void) {
    blog(LOG_INFO, "[MixTrackFilter] Initializing Mix Track Audio Filter Plugin (version %s)...",
         MIX_TRACK_FILTER_VERSION);

    // Load persisted track mask configuration before hooking audio
    uint8_t savedMask = SettingsDialog::loadTrackMask();
    AudioProcessor::instance().setTrackMask(savedMask);

    // Initialize backend audio processor and dummy filter container
    AudioProcessor::instance().initialize();

    // Register dock widget before OBS restores frontend window state
    MixTrackFilterDock::registerDock();

    // Register "Tools" menu item to open Settings Dialog
    auto* settingsAction = static_cast<QAction*>(
        obs_frontend_add_tools_menu_qaction("Mix Track Filter Settings"));
    if (settingsAction) {
        QObject::connect(settingsAction, &QAction::triggered, []() {
            SettingsDialog::openDialog();
        });
    }

    // Register callback for OBS UI initialization
    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[MixTrackFilter] Unloading Mix Track Audio Filter Plugin...");

    obs_frontend_remove_event_callback(on_frontend_event, nullptr);

    // Destroy settings dialog if instantiated
    SettingsDialog::destroyDialog();

    // Unhook mix track audio output and cleanup resources
    AudioProcessor::instance().unhookAudio();
    AudioProcessor::instance().terminate();
}
