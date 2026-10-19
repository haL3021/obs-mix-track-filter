#pragma once

#include <obs.h>
#include <obs-frontend-api.h>
#include <media-io/audio-io.h>
#include <obs-source.h>

#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

/**
 * FilterChainSnapshot holds an immutable array of active filter source pointers.
 * This is used for lock-free, zero-allocation access on the real-time audio thread.
 */
struct FilterChainSnapshot {
    std::vector<obs_source_t*> filters;

    FilterChainSnapshot() = default;
    ~FilterChainSnapshot() {
        for (obs_source_t* filter : filters) {
            if (filter) {
                obs_source_release(filter);
            }
        }
    }

    FilterChainSnapshot(const FilterChainSnapshot&) = delete;
    FilterChainSnapshot& operator=(const FilterChainSnapshot&) = delete;
};

enum class DispatchOrderStatus {
    Optimal,   // All active tracks execute first in LIFO loop
    Degraded,  // At least one active track is preceded by another consumer
    Unhooked   // Audio subsystem not available or no tracks hooked
};

struct DispatchOrderInfo {
    DispatchOrderStatus status = DispatchOrderStatus::Unhooked;
    int degradedTrack = -1; // 1-based track index
};

/**
 * AudioProcessor handles the mix track audio processing.
 * - Hooks into OBS mix track audio output (mix 0).
 * - Passes raw audio through an internal dummy obs_source_t filter chain.
 * - Maintains a lock-free snapshot mechanism between UI and real-time audio thread.
 */
class AudioProcessor {
public:
    static AudioProcessor& instance();

    void initialize();
    void terminate();

    // Attach/Detach to OBS mix track audio output
    void hookAudio();
    void unhookAudio();
    void rehookAudio();
    void rehookTrack(size_t mix);
    void syncTrackMask();
    bool isAudioHooked() const { return isHooked.load(std::memory_order_acquire); }

    static constexpr size_t MAX_AUDIO_TRACKS = 6;

    // Real-time audio processing callback (invoked by OBS audio subsystem)
    static void onAudioData(void* param, size_t mix_idx, struct audio_data* data);

    // Dummy source access (trackIdx: 0..5 corresponding to Track 1..6)
    obs_source_t* getDummySource(size_t trackIdx = 0) const;

    // Filter chain snapshot management (UI thread updates snapshot for audio thread)
    void updateSnapshot(size_t trackIdx);
    void updateAllSnapshots();

    // Filter chain duplication helpers (UI thread only)
    void copyFilters(size_t srcTrackIdx, size_t dstTrackIdx);
    void copyFiltersToAppliedTracks(size_t srcTrackIdx);

    // Signal callback for dummy source filter changes (native dialog or dock)
    static void onFiltersStructureChanged(void* param, calldata_t* data);

    // Mix track bypass (per-track bitmask: bits 0..5 = tracks 1..6)
    void setTrackBypass(size_t trackIdx, bool bypass) {
        if (trackIdx >= MAX_AUDIO_TRACKS) return;
        uint8_t current = bypassMask.load(std::memory_order_relaxed);
        uint8_t desired;
        do {
            desired = bypass ? static_cast<uint8_t>(current | (1 << trackIdx))
                             : static_cast<uint8_t>(current & ~(1 << trackIdx));
        } while (!bypassMask.compare_exchange_weak(current, desired,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
    }
    bool isTrackBypassed(size_t trackIdx) const {
        if (trackIdx >= MAX_AUDIO_TRACKS) return false;
        return (bypassMask.load(std::memory_order_acquire) & (1 << trackIdx)) != 0;
    }
    uint8_t getBypassMask() const { return bypassMask.load(std::memory_order_acquire); }
    void setBypassMask(uint8_t mask) { bypassMask.store(mask & 0x3F, std::memory_order_release); }

    // Mix track selection mask (bits 0..5 = tracks 1..6)
    void setTrackMask(uint8_t mask);
    uint8_t getTrackMask() const { return trackMask.load(std::memory_order_acquire); }

    // Serialization helper
    void saveFilters(obs_data_t* save_data);
    void loadFilters(obs_data_t* save_data);

    // Audio dispatch order inspection (called from UI thread only)
    DispatchOrderInfo checkDispatchOrder();

    // Frontend initialization lifecycle
    void setFrontendLoaded(bool loaded) { frontendLoaded.store(loaded, std::memory_order_release); }
    bool isFrontendLoaded() const { return frontendLoaded.load(std::memory_order_acquire); }

    // Batch loading state
    bool isLoadingFiltersActive() const { return isLoadingFilters.load(std::memory_order_acquire); }

private:
    AudioProcessor() = default;
    ~AudioProcessor();

    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor& operator=(const AudioProcessor&) = delete;

    void registerDummySourceType();

    struct TrackSignalContext {
        AudioProcessor* processor = nullptr;
        size_t trackIdx = 0;
    };

    // Hidden source containers for filters (one per track)
    obs_source_t* dummySources[MAX_AUDIO_TRACKS]{};
    TrackSignalContext signalContexts[MAX_AUDIO_TRACKS]{};

    // Batch filter loading guard
    std::atomic<bool> isLoadingFilters{false};

    // Hook state & track selection
    std::mutex hookMutex;
    std::atomic<bool> frontendLoaded{false};
    std::atomic<bool> isHooked{false};
    std::atomic<uint8_t> bypassMask{0x00};
    std::atomic<uint8_t> trackMask{0x01};
    uint8_t hookedMixes{0};

    // Lock-free RCU-style snapshots for the audio thread (one per track)
    std::atomic<FilterChainSnapshot*> activeSnapshots[MAX_AUDIO_TRACKS]{};

    // Storage for retired snapshots to safely delete outside audio thread
    std::mutex retiredMutex;
    std::vector<std::unique_ptr<FilterChainSnapshot>> retiredSnapshots;

    // Cross-fade smoothing state and backup buffer for bypass toggling (audio thread only)
    float fadeGains[6]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float rawBackup[MAX_AV_PLANES][AUDIO_OUTPUT_FRAMES]{};
    static constexpr float FADE_RAMP_SAMPLES = 1024.0f;
};
