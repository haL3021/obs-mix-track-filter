#include "audio-processor.hpp"

#include <cstring>
#include <algorithm>
#include <util/platform.h>

extern "C" struct obs_audio_data *process_filter_audio_c(obs_source_t *filter, struct obs_audio_data *audio);
extern "C" int check_audio_dispatch_order_c(audio_t *audio, size_t mix_idx, audio_output_callback_t callback, void *param);


// Identifier for our internal private dummy source type
static const char* DUMMY_SOURCE_ID = "mix_track_filter_dummy_source";

// Minimal callbacks for the dummy source definition
static void dummy_source_destroy(void* data) {
    (void)data;
}

static void* dummy_source_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;
    (void)source;
    return (void*)1; // Non-null dummy context pointer
}

static const char* dummy_source_get_name(void* type_data) {
    (void)type_data;
    return "Mix Track Filter Dummy Source";
}

AudioProcessor& AudioProcessor::instance() {
    static AudioProcessor instance;
    return instance;
}

AudioProcessor::~AudioProcessor() {
    terminate();
}

void AudioProcessor::registerDummySourceType() {
    static bool registered = false;
    if (registered) return;

    obs_source_info info = {};
    info.id = DUMMY_SOURCE_ID;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = dummy_source_get_name;
    info.create = dummy_source_create;
    info.destroy = dummy_source_destroy;

    obs_register_source(&info);
    registered = true;
}

void AudioProcessor::initialize() {
    if (dummySource) return;

    registerDummySourceType();

    // Create private dummy source to hold mix track filters natively
    dummySource = obs_source_create_private(DUMMY_SOURCE_ID, "Mix Track Filter Chain Container", nullptr);

    // Initial empty snapshot
    auto emptySnap = new FilterChainSnapshot();
    activeSnapshot.store(emptySnap, std::memory_order_release);

    // Connect signals for automatic snapshot updates whenever filters change natively or in dock
    signal_handler_t* sh = obs_source_get_signal_handler(dummySource);
    if (sh) {
        signal_handler_connect(sh, "filter_add", AudioProcessor::onFiltersStructureChanged, this);
        signal_handler_connect(sh, "filter_remove", AudioProcessor::onFiltersStructureChanged, this);
        signal_handler_connect(sh, "reorder_filters", AudioProcessor::onFiltersStructureChanged, this);
    }
}

void AudioProcessor::terminate() {
    frontendLoaded.store(false, std::memory_order_release);
    unhookAudio();

    if (dummySource) {
        signal_handler_t* sh = obs_source_get_signal_handler(dummySource);
        if (sh) {
            signal_handler_disconnect(sh, "filter_add", AudioProcessor::onFiltersStructureChanged, this);
            signal_handler_disconnect(sh, "filter_remove", AudioProcessor::onFiltersStructureChanged, this);
            signal_handler_disconnect(sh, "reorder_filters", AudioProcessor::onFiltersStructureChanged, this);
        }
    }

    // Clear snapshot
    FilterChainSnapshot* oldSnap = activeSnapshot.exchange(nullptr, std::memory_order_acq_rel);
    if (oldSnap) {
        for (obs_source_t* filter : oldSnap->filters) {
            obs_source_release(filter);
        }
        delete oldSnap;
    }

    // Clear retired snapshots
    {
        std::lock_guard<std::mutex> lock(retiredMutex);
        for (auto& snap : retiredSnapshots) {
            if (snap) {
                for (obs_source_t* filter : snap->filters) {
                    obs_source_release(filter);
                }
            }
        }
        retiredSnapshots.clear();
    }

    if (dummySource) {
        obs_source_release(dummySource);
        dummySource = nullptr;
    }
}

void AudioProcessor::hookAudio() {
    std::lock_guard<std::mutex> lock(hookMutex);
    audio_t* audio = obs_get_audio();
    if (!audio) {
        blog(LOG_WARNING, "[MixTrackFilter] Failed to obtain OBS audio subsystem.");
        return;
    }

    uint8_t mask = trackMask.load(std::memory_order_acquire);
    for (size_t mix = 0; mix < 6; ++mix) {
        if ((mask & (1 << mix)) && !(hookedMixes & (1 << mix))) {
            bool connected = audio_output_connect(audio, mix, nullptr, AudioProcessor::onAudioData, this);
            if (connected) {
                hookedMixes |= (1 << mix);
                blog(LOG_INFO, "[MixTrackFilter] Successfully hooked mix track %zu.", mix + 1);
            } else {
                blog(LOG_ERROR, "[MixTrackFilter] Failed to hook mix track %zu.", mix + 1);
            }
        }
    }
    isHooked.store(hookedMixes != 0, std::memory_order_release);
}

void AudioProcessor::unhookAudio() {
    std::lock_guard<std::mutex> lock(hookMutex);
    audio_t* audio = obs_get_audio();
    if (audio) {
        for (size_t mix = 0; mix < 6; ++mix) {
            if (hookedMixes & (1 << mix)) {
                audio_output_disconnect(audio, mix, AudioProcessor::onAudioData, this);
            }
        }
    }
    hookedMixes = 0;
    isHooked.store(false, std::memory_order_release);
    blog(LOG_INFO, "[MixTrackFilter] All mix track audio outputs unhooked.");
}

void AudioProcessor::rehookTrack(size_t mix) {
    if (mix >= 6) return;
    std::lock_guard<std::mutex> lock(hookMutex);
    audio_t* audio = obs_get_audio();
    if (!audio) return;

    uint8_t mask = trackMask.load(std::memory_order_acquire);
    if (!(mask & (1 << mix))) return;

    if (hookedMixes & (1 << mix)) {
        audio_output_disconnect(audio, mix, AudioProcessor::onAudioData, this);
        hookedMixes &= ~(1 << mix);
    }
    bool connected = audio_output_connect(audio, mix, nullptr, AudioProcessor::onAudioData, this);
    if (connected) {
        hookedMixes |= (1 << mix);
        blog(LOG_INFO, "[MixTrackFilter] Rehooked mix track %zu.", mix + 1);
    }
    isHooked.store(hookedMixes != 0, std::memory_order_release);
}

void AudioProcessor::syncTrackMask() {
    std::lock_guard<std::mutex> lock(hookMutex);
    audio_t* audio = obs_get_audio();
    if (!audio) return;

    uint8_t mask = trackMask.load(std::memory_order_acquire);
    for (size_t mix = 0; mix < 6; ++mix) {
        bool shouldBeHooked = (mask & (1 << mix)) != 0;
        bool isCurrentlyHooked = (hookedMixes & (1 << mix)) != 0;

        if (shouldBeHooked && !isCurrentlyHooked) {
            bool connected = audio_output_connect(audio, mix, nullptr, AudioProcessor::onAudioData, this);
            if (connected) {
                hookedMixes |= (1 << mix);
                blog(LOG_INFO, "[MixTrackFilter] Connected mix track %zu.", mix + 1);
            }
        } else if (!shouldBeHooked && isCurrentlyHooked) {
            audio_output_disconnect(audio, mix, AudioProcessor::onAudioData, this);
            hookedMixes &= ~(1 << mix);
            blog(LOG_INFO, "[MixTrackFilter] Disconnected mix track %zu.", mix + 1);
        }
    }
    isHooked.store(hookedMixes != 0, std::memory_order_release);
}

void AudioProcessor::rehookAudio() {
    std::lock_guard<std::mutex> lock(hookMutex);
    audio_t* audio = obs_get_audio();
    if (!audio) {
        blog(LOG_WARNING, "[MixTrackFilter] Failed to obtain OBS audio subsystem for rehook.");
        return;
    }

    // Rehook each track sequentially to minimize disconnection window per track
    uint8_t mask = trackMask.load(std::memory_order_acquire);
    for (size_t mix = 0; mix < 6; ++mix) {
        if (mask & (1 << mix)) {
            if (hookedMixes & (1 << mix)) {
                audio_output_disconnect(audio, mix, AudioProcessor::onAudioData, this);
                hookedMixes &= ~(1 << mix);
            }
            bool connected = audio_output_connect(audio, mix, nullptr, AudioProcessor::onAudioData, this);
            if (connected) {
                hookedMixes |= (1 << mix);
            }
        } else if (hookedMixes & (1 << mix)) {
            audio_output_disconnect(audio, mix, AudioProcessor::onAudioData, this);
            hookedMixes &= ~(1 << mix);
        }
    }
    isHooked.store(hookedMixes != 0, std::memory_order_release);
    blog(LOG_INFO, "[MixTrackFilter] Rehooked mix track audio outputs (mask: 0x%02X).", hookedMixes);
}

void AudioProcessor::setTrackMask(uint8_t mask) {
    mask &= 0x3F; // Limit to 6 tracks
    trackMask.store(mask, std::memory_order_release);

    if (!frontendLoaded.load(std::memory_order_acquire)) {
        return; // OBS frontend is still loading, will hook on FINISHED_LOADING
    }

    // Use differential hook synchronization to avoid disconnecting unaffected active tracks
    syncTrackMask();
}

void AudioProcessor::onAudioData(void* param, size_t mix_idx, struct audio_data* data) {
    auto processor = static_cast<AudioProcessor*>(param);
    if (!processor || !data || mix_idx >= 6) return;

    // Check if this track mix is active
    if (!(processor->trackMask.load(std::memory_order_relaxed) & (1 << mix_idx))) {
        return;
    }

    bool isBypassed = processor->isBypassed();
    float& currentGain = processor->fadeGains[mix_idx];

    // Fast path: fully bypassed and fade-out completed -> zero CPU overhead
    if (isBypassed && currentGain <= 0.0f) {
        return;
    }

    // Fetch the current active filter chain snapshot: lock-free atomic pointer read
    FilterChainSnapshot* snapshot = processor->activeSnapshot.load(std::memory_order_acquire);
    if (!snapshot || snapshot->filters.empty()) {
        return; // No filters to apply
    }

    // Determine if we need to cross-fade (smoothing) between raw and filtered audio
    bool isTransitioning = isBypassed || (currentGain < 1.0f);

    // Save pre-filter raw audio only when transitioning across bypass states
    float rawBackup[MAX_AV_PLANES][AUDIO_OUTPUT_FRAMES];
    if (isTransitioning) {
        for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
            if (data->data[plane]) {
                std::memcpy(rawBackup[plane], data->data[plane], data->frames * sizeof(float));
            }
        }
    }

    // Cast raw audio_data to obs_audio_data (identical memory layout: data planes, frames, timestamp)
    auto audio = reinterpret_cast<struct obs_audio_data*>(data);

    // Sequentially process each active filter in the chain
    for (obs_source_t* filter : snapshot->filters) {
        if (!filter) continue;

        if (obs_source_enabled(filter)) {
            audio = process_filter_audio_c(filter, audio);

            // If a filter silenced or dropped the audio, clear remaining buffers and terminate chain
            if (!audio) {
                constexpr size_t UNCLAMPED_OFFSET_FLOATS = MAX_AUDIO_CHANNELS * AUDIO_OUTPUT_FRAMES;
                for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
                    if (data->data[plane]) {
                        float* normalBuf = reinterpret_cast<float*>(data->data[plane]);
                        float* unclampedBuf = normalBuf + UNCLAMPED_OFFSET_FLOATS;
                        std::memset(normalBuf, 0, data->frames * sizeof(float));
                        std::memset(unclampedBuf, 0, data->frames * sizeof(float));
                    }
                }
                break;
            }
        }
    }

    // Synchronize both the standard clamped buffer (mix->buffer) and
    // the unclamped buffer (mix->buffer_unclamped) with the filtered audio.
    constexpr size_t UNCLAMPED_OFFSET_FLOATS = MAX_AUDIO_CHANNELS * AUDIO_OUTPUT_FRAMES;
    const float gainStep = 1.0f / FADE_RAMP_SAMPLES;

    for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
        if (!data->data[plane]) continue;

        float* normalBuf = reinterpret_cast<float*>(data->data[plane]);
        float* unclampedBuf = normalBuf + UNCLAMPED_OFFSET_FLOATS;
        const float* filteredBuf = (audio && audio != reinterpret_cast<struct obs_audio_data*>(data) && audio->data[plane])
                                       ? reinterpret_cast<const float*>(audio->data[plane])
                                       : normalBuf;

        if (isTransitioning) {
            float gain = currentGain;
            for (size_t s = 0; s < data->frames; ++s) {
                if (isBypassed) {
                    gain = std::max(0.0f, gain - gainStep);
                } else {
                    gain = std::min(1.0f, gain + gainStep);
                }
                float rawSample = rawBackup[plane][s];
                float filteredSample = filteredBuf[s];
                float mixedSample = rawSample * (1.0f - gain) + filteredSample * gain;
                normalBuf[s] = mixedSample;
                unclampedBuf[s] = mixedSample;
            }

            // Update gain state after finishing all audio channels
            if (plane == MAX_AV_PLANES - 1 || !data->data[plane + 1]) {
                currentGain = gain;
            }
        } else {
            // Steady state: fast memcpy directly
            if (filteredBuf != normalBuf) {
                std::memcpy(normalBuf, filteredBuf, data->frames * sizeof(float));
            }
            std::memcpy(unclampedBuf, normalBuf, data->frames * sizeof(float));
        }
    }
}

void AudioProcessor::updateSnapshot() {
    if (!dummySource) return;

    auto newSnapshot = std::make_unique<FilterChainSnapshot>();

    // Enumerate existing filters on dummy source
    obs_source_enum_filters(dummySource, [](obs_source_t*, obs_source_t* filter, void* param) {
        auto snap = static_cast<FilterChainSnapshot*>(param);
        obs_source_get_ref(filter); // Increment refcount for snapshot ownership
        snap->filters.push_back(filter);
    }, newSnapshot.get());

    // Atomically swap the new snapshot into place for the audio thread
    FilterChainSnapshot* rawNew = newSnapshot.release();
    FilterChainSnapshot* oldSnapshot = activeSnapshot.exchange(rawNew, std::memory_order_acq_rel);

    // Safely store old snapshot in retired list to defer destruction
    if (oldSnapshot) {
        std::lock_guard<std::mutex> lock(retiredMutex);
        retiredSnapshots.emplace_back(oldSnapshot);

        // Keep retired history bounded (e.g. keep last 4, destroy older ones)
        while (retiredSnapshots.size() > 4) {
            auto& oldest = retiredSnapshots.front();
            for (obs_source_t* filter : oldest->filters) {
                obs_source_release(filter);
            }
            retiredSnapshots.erase(retiredSnapshots.begin());
        }
    }
}

void AudioProcessor::onFiltersStructureChanged(void* param, calldata_t* data) {
    (void)data;
    auto processor = static_cast<AudioProcessor*>(param);
    if (processor && !processor->isLoadingFilters.load(std::memory_order_acquire)) {
        processor->updateSnapshot();
    }
}

void AudioProcessor::saveFilters(obs_data_t* save_data) {
    if (!dummySource || !save_data) return;

    obs_data_array_t* filterArray = obs_data_array_create();

    obs_source_enum_filters(dummySource, [](obs_source_t*, obs_source_t* filter, void* param) {
        auto array = static_cast<obs_data_array_t*>(param);

        obs_source_save(filter);

        obs_data_t* item = obs_data_create();
        const char* id = obs_source_get_id(filter);
        const char* name = obs_source_get_name(filter);
        bool enabled = obs_source_enabled(filter);
        obs_data_t* settings = obs_source_get_settings(filter);

        obs_data_set_string(item, "id", id);
        obs_data_set_string(item, "name", name);
        obs_data_set_bool(item, "enabled", enabled);
        obs_data_set_obj(item, "settings", settings);

        obs_data_array_push_back(array, item);

        obs_data_release(settings);
        obs_data_release(item);
    }, filterArray);

    obs_data_set_array(save_data, "mix_track_filter_chain", filterArray);
    obs_data_array_release(filterArray);
    blog(LOG_INFO, "[MixTrackFilter] Saved mix track filter configuration.");
}

void AudioProcessor::loadFilters(obs_data_t* save_data) {
    if (!dummySource || !save_data) return;

    obs_data_array_t* filterArray = obs_data_get_array(save_data, "mix_track_filter_chain");
    if (!filterArray) return;

    isLoadingFilters.store(true, std::memory_order_release);

    // Remove existing filters from dummy source
    std::vector<obs_source_t*> existingFilters;
    obs_source_enum_filters(dummySource, [](obs_source_t*, obs_source_t* filter, void* param) {
        auto list = static_cast<std::vector<obs_source_t*>*>(param);
        obs_source_get_ref(filter);
        list->push_back(filter);
    }, &existingFilters);

    for (obs_source_t* filter : existingFilters) {
        obs_source_filter_remove(dummySource, filter);
        obs_source_release(filter);
    }

    // Reconstruct filter chain from saved array
    size_t count = obs_data_array_count(filterArray);
    for (size_t i = 0; i < count; ++i) {
        obs_data_t* item = obs_data_array_item(filterArray, i);
        const char* id = obs_data_get_string(item, "id");
        const char* name = obs_data_get_string(item, "name");
        bool enabled = obs_data_get_bool(item, "enabled");
        obs_data_t* settings = obs_data_get_obj(item, "settings");

        obs_source_t* filter = obs_source_create_private(id, name, settings);
        if (filter) {
            obs_source_set_enabled(filter, enabled);
            obs_source_filter_add(dummySource, filter);
            obs_source_load(filter);
            obs_source_release(filter);
        }

        obs_data_release(settings);
        obs_data_release(item);
    }

    obs_data_array_release(filterArray);

    isLoadingFilters.store(false, std::memory_order_release);

    // Update audio thread snapshot
    updateSnapshot();
    blog(LOG_INFO, "[MixTrackFilter] Restored %zu mix track filters.", count);
}

DispatchOrderInfo AudioProcessor::checkDispatchOrder() {
    DispatchOrderInfo info;
    audio_t* audio = obs_get_audio();
    if (!audio || !isHooked.load(std::memory_order_acquire)) {
        info.status = DispatchOrderStatus::Unhooked;
        return info;
    }

    uint8_t mask = trackMask.load(std::memory_order_acquire);
    if (mask == 0) {
        info.status = DispatchOrderStatus::Unhooked;
        return info;
    }

    bool allOptimal = true;
    for (size_t mix = 0; mix < 6; ++mix) {
        if (mask & (1 << mix)) {
            int order = check_audio_dispatch_order_c(audio, mix, AudioProcessor::onAudioData, this);
            if (order == 0) {
                // Optimal on this track (at tail of array, executes first in LIFO loop)
            } else if (order > 0) {
                allOptimal = false;
                if (info.degradedTrack == -1) {
                    info.degradedTrack = static_cast<int>(mix + 1);
                }
            } else {
                // Not found or error
                allOptimal = false;
                if (info.degradedTrack == -1) {
                    info.degradedTrack = static_cast<int>(mix + 1);
                }
            }
        }
    }

    info.status = allOptimal ? DispatchOrderStatus::Optimal : DispatchOrderStatus::Degraded;
    return info;
}

