#include "audio-processor.hpp"

#include <cstring>
#include <algorithm>
#include <util/platform.h>

extern "C" struct obs_audio_data *process_filter_audio_c(obs_source_t *filter, struct obs_audio_data *audio);
extern "C" int check_audio_dispatch_order_mask_c(audio_t *audio, uint8_t track_mask,
                                                 audio_output_callback_t callback, void *param,
                                                 int *out_degraded_track);


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

obs_source_t* AudioProcessor::getDummySource(size_t trackIdx) const {
    if (trackIdx >= MAX_AUDIO_TRACKS) {
        return nullptr;
    }
    return dummySources[trackIdx];
}

void AudioProcessor::initialize() {
    if (dummySources[0]) return;

    registerDummySourceType();

    for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "Mix Track %zu", i + 1);

        dummySources[i] = obs_source_create_private(DUMMY_SOURCE_ID, name, nullptr);

        signalContexts[i] = {this, i};

        auto emptySnap = new FilterChainSnapshot();
        activeSnapshots[i].store(emptySnap, std::memory_order_release);

        signal_handler_t* sh = obs_source_get_signal_handler(dummySources[i]);
        if (sh) {
            signal_handler_connect(sh, "filter_add", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
            signal_handler_connect(sh, "filter_remove", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
            signal_handler_connect(sh, "reorder_filters", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
        }
    }
}

void AudioProcessor::terminate() {
    frontendLoaded.store(false, std::memory_order_release);
    unhookAudio();

    for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
        if (dummySources[i]) {
            signal_handler_t* sh = obs_source_get_signal_handler(dummySources[i]);
            if (sh) {
                signal_handler_disconnect(sh, "filter_add", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
                signal_handler_disconnect(sh, "filter_remove", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
                signal_handler_disconnect(sh, "reorder_filters", AudioProcessor::onFiltersStructureChanged, &signalContexts[i]);
            }
        }

        FilterChainSnapshot* oldSnap = activeSnapshots[i].exchange(nullptr, std::memory_order_acq_rel);
        delete oldSnap;

        if (dummySources[i]) {
            obs_source_release(dummySources[i]);
            dummySources[i] = nullptr;
        }
    }

    // Clear retired snapshots
    {
        std::lock_guard<std::mutex> lock(retiredMutex);
        retiredSnapshots.clear();
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

    bool isBypassed = (processor->bypassMask.load(std::memory_order_relaxed) & (1 << mix_idx)) != 0;
    float& currentGain = processor->fadeGains[mix_idx];

    // Fast path: fully bypassed and fade-out completed -> zero CPU overhead
    if (isBypassed && currentGain <= 0.0f) {
        return;
    }

    // Fetch the current active filter chain snapshot: lock-free atomic pointer read
    FilterChainSnapshot* snapshot = processor->activeSnapshots[mix_idx].load(std::memory_order_acquire);
    if (!snapshot || snapshot->filters.empty()) {
        return; // No filters to apply
    }

    // Check if at least one filter is enabled before touching any audio buffers
    bool hasEnabledFilter = false;
    for (obs_source_t* filter : snapshot->filters) {
        if (filter && obs_source_enabled(filter)) {
            hasEnabledFilter = true;
            break;
        }
    }
    if (!hasEnabledFilter) {
        const float frameStep = static_cast<float>(data->frames) / FADE_RAMP_SAMPLES;
        if (isBypassed) {
            currentGain = std::max(0.0f, currentGain - frameStep);
        } else if (currentGain < 1.0f) {
            currentGain = std::min(1.0f, currentGain + frameStep);
        }
        return;
    }

    // Determine if we need to cross-fade (smoothing) between raw and filtered audio
    bool isTransitioning = isBypassed || (currentGain < 1.0f);

    // Save pre-filter raw audio only when transitioning across bypass states
    if (isTransitioning) {
        for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
            if (data->data[plane]) {
                std::memcpy(processor->rawBackup[plane], data->data[plane], data->frames * sizeof(float));
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
    const float signedStep = isBypassed ? (-1.0f / FADE_RAMP_SAMPLES) : (1.0f / FADE_RAMP_SAMPLES);
    float nextGain = currentGain;

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
                gain = std::clamp(gain + signedStep, 0.0f, 1.0f);
                float rawSample = processor->rawBackup[plane][s];
                float filteredSample = filteredBuf[s];
                float mixedSample = rawSample * (1.0f - gain) + filteredSample * gain;
                normalBuf[s] = mixedSample;
                unclampedBuf[s] = mixedSample;
            }
            nextGain = gain;
        } else {
            // Steady state: fast memcpy directly
            if (filteredBuf != normalBuf) {
                std::memcpy(normalBuf, filteredBuf, data->frames * sizeof(float));
            }
            std::memcpy(unclampedBuf, normalBuf, data->frames * sizeof(float));
        }
    }

    if (isTransitioning) {
        currentGain = nextGain;
    }
}

static void clearSourceFilters(obs_source_t* source) {
    if (!source) return;

    std::vector<obs_source_t*> existingFilters;
    obs_source_enum_filters(source, [](obs_source_t*, obs_source_t* filter, void* param) {
        auto list = static_cast<std::vector<obs_source_t*>*>(param);
        obs_source_get_ref(filter);
        list->push_back(filter);
    }, &existingFilters);

    for (obs_source_t* filter : existingFilters) {
        obs_source_filter_remove(source, filter);
        obs_source_release(filter);
    }
}

static obs_data_array_t* serializeSourceFilters(obs_source_t* source) {
    if (!source) return nullptr;

    obs_data_array_t* filterArray = obs_data_array_create();

    obs_source_enum_filters(source, [](obs_source_t*, obs_source_t* filter, void* param) {
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

    return filterArray;
}

static size_t deserializeSourceFilters(obs_source_t* source, obs_data_array_t* filterArray) {
    if (!source) return 0;

    clearSourceFilters(source);

    if (!filterArray) return 0;

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
            obs_source_filter_add(source, filter);
            obs_source_load(filter);
            obs_source_release(filter);
        }

        obs_data_release(settings);
        obs_data_release(item);
    }
    return count;
}

void AudioProcessor::updateSnapshot(size_t trackIdx) {
    if (trackIdx >= MAX_AUDIO_TRACKS || !dummySources[trackIdx]) return;

    auto newSnapshot = std::make_unique<FilterChainSnapshot>();

    // Enumerate existing filters on track's dummy source
    obs_source_enum_filters(dummySources[trackIdx], [](obs_source_t*, obs_source_t* filter, void* param) {
        auto snap = static_cast<FilterChainSnapshot*>(param);
        obs_source_get_ref(filter); // Increment refcount for snapshot ownership
        snap->filters.push_back(filter);
    }, newSnapshot.get());

    // Atomically swap the new snapshot into place for the audio thread
    FilterChainSnapshot* rawNew = newSnapshot.release();
    FilterChainSnapshot* oldSnapshot = activeSnapshots[trackIdx].exchange(rawNew, std::memory_order_acq_rel);

    // Safely store old snapshot in retired list to defer destruction
    if (oldSnapshot) {
        std::lock_guard<std::mutex> lock(retiredMutex);
        retiredSnapshots.emplace_back(oldSnapshot);

        // Keep retired history bounded (keep last 24 = 4 * MAX_AUDIO_TRACKS, destroy older ones)
        while (retiredSnapshots.size() > (4 * MAX_AUDIO_TRACKS)) {
            retiredSnapshots.erase(retiredSnapshots.begin());
        }
    }
}

void AudioProcessor::updateAllSnapshots() {
    for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
        updateSnapshot(i);
    }
}

void AudioProcessor::onFiltersStructureChanged(void* param, calldata_t* data) {
    (void)data;
    auto ctx = static_cast<TrackSignalContext*>(param);
    if (ctx && ctx->processor && !ctx->processor->isLoadingFilters.load(std::memory_order_acquire)) {
        ctx->processor->updateSnapshot(ctx->trackIdx);
    }
}

void AudioProcessor::copyFilters(size_t srcTrackIdx, size_t dstTrackIdx) {
    if (srcTrackIdx >= MAX_AUDIO_TRACKS || dstTrackIdx >= MAX_AUDIO_TRACKS || srcTrackIdx == dstTrackIdx) {
        return;
    }
    if (!dummySources[srcTrackIdx] || !dummySources[dstTrackIdx]) {
        return;
    }

    isLoadingFilters.store(true, std::memory_order_release);

    obs_data_array_t* serialized = serializeSourceFilters(dummySources[srcTrackIdx]);
    if (serialized) {
        deserializeSourceFilters(dummySources[dstTrackIdx], serialized);
        obs_data_array_release(serialized);
    }

    isLoadingFilters.store(false, std::memory_order_release);

    updateSnapshot(dstTrackIdx);
    blog(LOG_INFO, "[MixTrackFilter] Copied filters from track %zu to track %zu.", srcTrackIdx + 1, dstTrackIdx + 1);
}

void AudioProcessor::copyFiltersToAppliedTracks(size_t srcTrackIdx) {
    if (srcTrackIdx >= MAX_AUDIO_TRACKS || !dummySources[srcTrackIdx]) {
        return;
    }

    uint8_t mask = trackMask.load(std::memory_order_acquire);
    isLoadingFilters.store(true, std::memory_order_release);

    obs_data_array_t* serialized = serializeSourceFilters(dummySources[srcTrackIdx]);
    if (serialized) {
        for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
            if (i != srcTrackIdx && (mask & (1 << i)) && dummySources[i]) {
                deserializeSourceFilters(dummySources[i], serialized);
                updateSnapshot(i);
                blog(LOG_INFO, "[MixTrackFilter] Copied filters from track %zu to track %zu.", srcTrackIdx + 1, i + 1);
            }
        }
        obs_data_array_release(serialized);
    }

    isLoadingFilters.store(false, std::memory_order_release);
}

void AudioProcessor::saveFilters(obs_data_t* save_data) {
    if (!save_data) return;

    obs_data_array_t* chainsArray = obs_data_array_create();

    for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
        obs_data_t* trackObj = obs_data_create();
        obs_data_array_t* filterArray = serializeSourceFilters(dummySources[i]);

        obs_data_set_int(trackObj, "track", static_cast<long long>(i + 1));
        obs_data_set_array(trackObj, "filters", filterArray);

        obs_data_array_push_back(chainsArray, trackObj);

        obs_data_array_release(filterArray);
        obs_data_release(trackObj);
    }

    obs_data_set_array(save_data, "mix_track_filter_chains", chainsArray);
    obs_data_array_release(chainsArray);
    blog(LOG_INFO, "[MixTrackFilter] Saved mix track filter configuration for all tracks.");
}

void AudioProcessor::loadFilters(obs_data_t* save_data) {
    if (!save_data) return;

    obs_data_array_t* chainsArray = obs_data_get_array(save_data, "mix_track_filter_chains");
    obs_data_array_t* legacyArray = obs_data_get_array(save_data, "mix_track_filter_chain");

    if (!chainsArray && !legacyArray) return;

    isLoadingFilters.store(true, std::memory_order_release);

    if (chainsArray) {
        size_t count = obs_data_array_count(chainsArray);
        for (size_t i = 0; i < count && i < MAX_AUDIO_TRACKS; ++i) {
            obs_data_t* trackObj = obs_data_array_item(chainsArray, i);
            obs_data_array_t* filterArray = obs_data_get_array(trackObj, "filters");

            deserializeSourceFilters(dummySources[i], filterArray);

            obs_data_array_release(filterArray);
            obs_data_release(trackObj);
        }
        obs_data_array_release(chainsArray);
        blog(LOG_INFO, "[MixTrackFilter] Restored mix track filters from modern multi-track format.");
    } else if (legacyArray) {
        // Migration from legacy single filter chain:
        // Restore onto active tracks (or Track 1 if all tracks are currently unselected)
        uint8_t mask = trackMask.load(std::memory_order_acquire);
        if (mask == 0) {
            mask = 0x01; // Data loss prevention fallback to Track 1
        }

        for (size_t i = 0; i < MAX_AUDIO_TRACKS; ++i) {
            if (mask & (1 << i)) {
                deserializeSourceFilters(dummySources[i], legacyArray);
                blog(LOG_INFO, "[MixTrackFilter] Migrated legacy filter chain to active track %zu.", i + 1);
            } else {
                clearSourceFilters(dummySources[i]);
            }
        }
        obs_data_array_release(legacyArray);
    }

    isLoadingFilters.store(false, std::memory_order_release);

    updateAllSnapshots();
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

    int degradedTrack = -1;
    int res = check_audio_dispatch_order_mask_c(audio, mask, AudioProcessor::onAudioData, this, &degradedTrack);
    if (res == 0) {
        info.status = DispatchOrderStatus::Optimal;
    } else if (res > 0) {
        info.status = DispatchOrderStatus::Degraded;
        info.degradedTrack = degradedTrack;
    } else {
        info.status = DispatchOrderStatus::Unhooked;
    }
    return info;
}

