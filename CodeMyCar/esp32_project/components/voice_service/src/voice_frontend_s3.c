#include "voice_frontend.h"
#include "voice_dtd.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "voice_afe";
static srmodel_list_t *models;
static const esp_afe_sr_iface_t *iface;
static esp_afe_sr_data_t *afe;
static int16_t *feed_buffer;
static size_t feed_samples, feed_used;
static int feed_nch = 1;
static atomic_int s_in_rms;
static atomic_int s_ref_rms;
static atomic_int s_out_rms;
static atomic_int s_mic_peak, s_ref_peak;
static atomic_uint s_mic_clips, s_ref_clips;
static unsigned s_fetch_count;
static bool wake_enabled;
static atomic_bool s_playback, s_barge, s_discontinuous;
static voice_dtd_t s_dtd;
#if CONFIG_CMCAR_VOICE_DENOISE
#define DENOISE_ENABLED true
#else
#define DENOISE_ENABLED false
#endif

void voice_frontend_destroy(void)
{
    atomic_store(&s_playback, false);
    atomic_store(&s_barge, false);
    atomic_store(&s_discontinuous, false);
    atomic_store(&s_out_rms, 0);
    atomic_store(&s_mic_clips, 0);
    atomic_store(&s_ref_clips, 0);
    s_dtd = (voice_dtd_t){0};
    s_fetch_count = 0;
    if (afe) iface->destroy(afe);
    afe = NULL;
    iface = NULL;
    heap_caps_free(feed_buffer);
    feed_buffer = NULL;
    feed_samples = feed_used = 0;
    feed_nch = 1;
    if (models) esp_srmodel_deinit(models);
    models = NULL;
}

bool voice_frontend_init(void)
{
    if (afe) return true;
    models = esp_srmodel_init("model");
    /* Match the configured pretrained phrase exactly. Never fall back to an
     * unrelated model if the model partition and application disagree. */
    char *wake = models ? esp_srmodel_filter(models, "wn9l_", CONFIG_CMCAR_VOICE_S3_WAKE_MODEL) : NULL;
    if (!wake && models) wake = esp_srmodel_filter(models, "wn9_", CONFIG_CMCAR_VOICE_S3_WAKE_MODEL);
    if (!wake) goto fail;
#if CONFIG_CMCAR_VOICE_AEC
    const char *input_format = "MR"; /* microphone, playback reference */
#if CONFIG_CMCAR_VOICE_AEC_HIGH_PERF
    afe_mode_t mode = AFE_MODE_HIGH_PERF;
    aec_mode_t aec_mode = AEC_MODE_FD_HIGH_PERF;
#else
    afe_mode_t mode = AFE_MODE_LOW_COST;
    aec_mode_t aec_mode = AEC_MODE_FD_LOW_COST;
#endif
#else
    const char *input_format = "M";
    afe_mode_t mode = AFE_MODE_LOW_COST;
#endif
    /* Full-duplex AFE: AEC NLP (residual echo suppression) is only effective
     * in the FD AEC modes; SR modes leave NLP_OFF and the echo keeps
     * false-triggering VAD/wake during playback. */
    afe_config_t *cfg = afe_config_init(input_format, models, AFE_TYPE_FD, mode);
    if (!cfg) goto fail;
#if CONFIG_CMCAR_VOICE_AEC
    /* ch1 is the measured pre-amplifier reference. Keep the profile's filter
     * budget; the library's filter_length is not a duration in milliseconds. */
    cfg->aec_init = true;
    cfg->aec_mode = aec_mode;
    cfg->aec_filter_length = CONFIG_CMCAR_VOICE_AEC_FILTER_LENGTH;
    /* Without NLP the residual echo keeps VAD/wake false-positive during
     * playback; the AFE default ended up as NLP_OFF in the pipeline dump. */
    cfg->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
#else
    cfg->aec_init = false; /* no synchronised reference in the legacy mic path */
#endif
    cfg->se_init = false;  /* single microphone */
    cfg->agc_init = false;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_2;
    cfg->vad_model_name = esp_srmodel_filter(models, "vadnet", NULL);
    cfg->vad_min_speech_ms = 96;
    cfg->vad_min_noise_ms = 600;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = wake;
    cfg->wakenet_model_name_2 = NULL;
    cfg->wakenet_mode = DET_MODE_90;
    cfg->ns_init = DENOISE_ENABLED;
    cfg->ns_model_name = esp_srmodel_filter(models, "nsnet", NULL);
    cfg->afe_ns_mode = cfg->ns_model_name ? AFE_NS_MODE_NET : AFE_NS_MODE_WEBRTC;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 3;
    iface = esp_afe_handle_from_config(cfg);
    if (iface) afe = iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (!afe || iface->get_samp_rate(afe) != 16000) goto fail;
    int nch = iface->get_feed_channel_num(afe);
#if CONFIG_CMCAR_VOICE_AEC
    if (nch != 2) goto fail;
#else
    if (nch != 1) goto fail;
#endif
    feed_nch = nch;
    int count = iface->get_feed_chunksize(afe);
    if (count <= 0 || count > 4096) goto fail;
    feed_samples = (size_t)count;
    feed_buffer = heap_caps_malloc(feed_samples * (size_t)feed_nch * sizeof(int16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!feed_buffer) goto fail;
    /* NS is bypassed while waiting for wake; enable only for the utterance. */
    if (DENOISE_ENABLED) iface->disable_ns(afe);
    wake_enabled = true;
#if CONFIG_CMCAR_VOICE_WAKE_THRESHOLD > 0
    if (iface->set_wakenet_threshold != NULL) {
        const float threshold = (float)CONFIG_CMCAR_VOICE_WAKE_THRESHOLD / 1000.0f;
        int set = iface->set_wakenet_threshold(afe, 1, threshold);
        ESP_LOGI(TAG, "wake threshold %.3f set=%d", threshold, set);
    }
#endif
    iface->print_pipeline(afe);
    ESP_LOGI(TAG, "ready 16kHz wake=%s feed=%u x %dch AEC=%s; workspace prefers PSRAM",
             wake, (unsigned)feed_samples, feed_nch,
             CONFIG_CMCAR_VOICE_AEC ? "on" : "off");
    return true;
fail:
    ESP_LOGE(TAG, "AFE init failed: voice closed, control bridge remains available");
    voice_frontend_destroy();
    return false;
}

bool voice_frontend_feed(const int16_t *pcm, size_t samples)
{
    if (!afe || !pcm) return false;
    const size_t chunk = feed_samples * (size_t)feed_nch;
    while (samples) {
        size_t n = chunk - feed_used;
        if (n > samples) n = samples;
        memcpy(feed_buffer + feed_used, pcm, n * sizeof(int16_t));
        feed_used += n; pcm += n; samples -= n;
        if (feed_used == chunk) {
            /* Input diagnostics only. Do not mix these feed-time statistics
             * with delayed AFE output to make a double-talk decision. */
            int64_t sum_mic = 0, sum_ref = 0;
            int mic_peak = 0, ref_peak = 0;
            unsigned mic_clips = 0, ref_clips = 0;
            for (size_t i = 0; i < feed_samples; ++i) {
                int32_t m = feed_buffer[i * feed_nch];
                int32_t v = feed_nch > 1 ? feed_buffer[i * feed_nch + 1] : 0;
                sum_mic += (int64_t)m * m;
                sum_ref += (int64_t)v * v;
                int ma = m < 0 ? -m : m, ra = v < 0 ? -v : v;
                if (ma > mic_peak) mic_peak = ma;
                if (ra > ref_peak) ref_peak = ra;
                mic_clips += ma >= 32760;
                ref_clips += ra >= 32760;
            }
            int mic_rms = (int)sqrt((double)sum_mic / (double)feed_samples);
            int ref_rms = (int)sqrt((double)sum_ref / (double)feed_samples);
            atomic_store(&s_in_rms, mic_rms);
            atomic_store(&s_ref_rms, ref_rms);
            atomic_store(&s_mic_peak, mic_peak);
            atomic_store(&s_ref_peak, ref_peak);
            atomic_fetch_add(&s_mic_clips, mic_clips);
            atomic_fetch_add(&s_ref_clips, ref_clips);

            int result = iface->feed(afe, feed_buffer);
            feed_used = 0;
            if (result < 0) { voice_frontend_discontinuity(); return false; }
        }
    }
    return true;
}

int voice_frontend_out_rms(void) { return atomic_load(&s_out_rms); }
int voice_frontend_ref_rms(void) { return atomic_load(&s_ref_rms); }

void voice_frontend_discontinuity(void)
{
    feed_used = 0;
    atomic_store(&s_discontinuous, true);
}

bool voice_frontend_fetch(const int16_t **pcm, size_t *samples,
                          bool *wake, bool *speech, bool allow_wake)
{
    *pcm = NULL; *samples = 0; *wake = *speech = false;
    if (!afe) return false;
    /* Only this fetch owner applies algorithm controls. No model clean(),
     * no destroy/recreate on every wake as in the old C6 standalone adapter. */
    if (allow_wake != wake_enabled) {
        if (allow_wake) {
            iface->enable_wakenet(afe);
            if (DENOISE_ENABLED) iface->disable_ns(afe);
        } else {
            iface->disable_wakenet(afe);
            if (DENOISE_ENABLED) iface->enable_ns(afe);
        }
        wake_enabled = allow_wake;
    }
    afe_fetch_result_t *r = iface->fetch_with_delay(afe, pdMS_TO_TICKS(100));
    bool gap = atomic_exchange(&s_discontinuous, false);
    if (gap || !r || r->ret_value < 0 || !r->data || r->data_size <= 0 || r->data_size % 2) {
        s_dtd.held = s_dtd.gap = 0;
        s_dtd.playing = false;
        atomic_store(&s_barge, false);
        return false;
    }
    *pcm = r->data;
    *samples = (size_t)r->data_size / sizeof(int16_t);
    *wake = allow_wake && r->wakeup_state == WAKENET_DETECTED;
    *speech = r->vad_state == VAD_SPEECH;
#if CONFIG_CMCAR_VOICE_AEC
    double sum = 0, square = 0;
    for (size_t i = 0; i < *samples; ++i) {
        double v = r->data[i]; sum += v; square += v * v;
    }
    double mean = sum / *samples;
    int out = (int)sqrt(fmax(0, square / *samples - mean * mean));
    atomic_store(&s_out_rms, out);
    if (voice_dtd_update(&s_dtd, atomic_load(&s_playback), *speech, out, *samples))
        atomic_store(&s_barge, true);
    if ((++s_fetch_count % 25u) == 0u)
        ESP_LOGI(TAG, "afe out_ac=%d vad=%d; raw mic/ref rms=%d/%d peak=%d/%d clips=%u/%u (unaligned diagnostics)",
                 out, *speech, atomic_load(&s_in_rms), atomic_load(&s_ref_rms),
                 atomic_load(&s_mic_peak), atomic_load(&s_ref_peak),
                 atomic_load(&s_mic_clips), atomic_load(&s_ref_clips));
#endif

    return true;
}

void voice_frontend_set_playback(bool enabled)
{
    atomic_store(&s_playback, enabled);
    if (!enabled) atomic_store(&s_barge, false);
}
bool voice_frontend_take_barge_in(void) { return atomic_exchange(&s_barge, false); }
