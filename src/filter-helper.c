#include <obs.h>
#include <obs-internal.h>
#include <media-io/audio-io.h>
#include <util/darray.h>
#include <util/threading.h>

struct obs_audio_data *process_filter_audio_c(obs_source_t *filter, struct obs_audio_data *audio)
{
    if (filter && filter->info.filter_audio) {
        return filter->info.filter_audio(filter->context.data, audio);
    }
    return audio;
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif

struct audio_input_internal {
    struct audio_convert_info conversion;
    void *resampler;
    audio_output_callback_t callback;
    void *param;
};

struct audio_mix_internal {
    DARRAY(struct audio_input_internal) inputs;
    float buffer[MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
    float buffer_unclamped[MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
};

struct audio_output_internal {
    struct audio_output_info info;
    size_t block_size;
    size_t channels;
    size_t planes;

    pthread_t thread;
    os_event_t *stop_event;

    bool initialized;

    audio_input_callback_t input_cb;
    void *input_param;
    pthread_mutex_t input_mutex;
    struct audio_mix_internal mixes[MAX_AUDIO_MIXES];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif


int check_audio_dispatch_order_c(audio_t *audio, size_t mix_idx, audio_output_callback_t callback, void *param)
{
    if (!audio || mix_idx >= MAX_AUDIO_MIXES) {
        return -1;
    }

    struct audio_output_internal *ao = (struct audio_output_internal *)audio;
    pthread_mutex_lock(&ao->input_mutex);

    struct audio_mix_internal *mix = &ao->mixes[mix_idx];
    size_t count = mix->inputs.num;
    int order = -1;

    for (size_t i = 0; i < count; i++) {
        struct audio_input_internal *input = &mix->inputs.array[i];
        if (input->callback == callback && input->param == param) {
            order = (int)(count - 1 - i);
            break;
        }
    }

    pthread_mutex_unlock(&ao->input_mutex);
    return order;
}

