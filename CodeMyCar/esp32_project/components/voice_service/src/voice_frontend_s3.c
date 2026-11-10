#include "voice_frontend.h"
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
static bool wake_enabled;
#if CONFIG_CMCAR_VOICE_DENOISE
#define DENOISE_ENABLED true
#else
#define DENOISE_ENABLED false
#endif

void voice_frontend_destroy(void)
{
    if (afe) iface->destroy(afe);
    afe = NULL;
    iface = NULL;
    heap_caps_free(feed_buffer);
    feed_buffer = NULL;
    feed_samples = feed_used = 0;
    if (models) esp_srmodel_deinit(models);
    models = NULL;
}

bool voice_frontend_init(void)
{
    if (afe) return true;
    models = esp_srmodel_init("model");
    /* WakeNet9l is the fast-speech variant (tts3); WakeNet9 often misses a
     * quickly spoken "你好小智". Keep the fallback so a stale sdkconfig fails
     * loudly here instead of silently loading an unrelated model. */
    char *wake = models ? esp_srmodel_filter(models, "wn9l_", "nihaoxiaozhi") : NULL;
    if (!wake && models) wake = esp_srmodel_filter(models, "wn9_", "nihaoxiaozhi");
    if (!wake) goto fail;
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (!cfg) goto fail;
    cfg->aec_init = false; /* Mac playback has no synchronised reference. */
    cfg->se_init = false;  /* Current hardware has ONE microphone. */
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
    if (!afe || iface->get_feed_channel_num(afe) != 1 ||
        iface->get_samp_rate(afe) != 16000) goto fail;
    int count = iface->get_feed_chunksize(afe);
    if (count <= 0 || count > 4096) goto fail;
    feed_samples = (size_t)count;
    feed_buffer = heap_caps_malloc(feed_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!feed_buffer) goto fail;
    /* NS is bypassed while waiting for wake; enable only for the utterance. */
    if (DENOISE_ENABLED) iface->disable_ns(afe);
    wake_enabled = true;
    iface->print_pipeline(afe);
    ESP_LOGI(TAG, "ready mono 16kHz wake=%s feed_samples=%u AEC=off; workspace prefers PSRAM", wake, (unsigned)feed_samples);
    return true;
fail:
    ESP_LOGE(TAG, "AFE init failed: voice closed, control bridge remains available");
    voice_frontend_destroy();
    return false;
}

bool voice_frontend_feed(const int16_t *pcm, size_t samples)
{
    if (!afe || !pcm) return false;
    while (samples) {
        size_t n = feed_samples - feed_used;
        if (n > samples) n = samples;
        memcpy(feed_buffer + feed_used, pcm, n * sizeof(int16_t));
        feed_used += n; pcm += n; samples -= n;
        if (feed_used == feed_samples) {
            int result = iface->feed(afe, feed_buffer);
            feed_used = 0;
            if (result < 0) return false;
        }
    }
    return true;
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
    if (!r || r->ret_value < 0 || !r->data || r->data_size <= 0 || r->data_size % 2) return false;
    *pcm = r->data;
    *samples = (size_t)r->data_size / sizeof(int16_t);
    *wake = allow_wake && r->wakeup_state == WAKENET_DETECTED;
    *speech = r->vad_state == VAD_SPEECH;
    return true;
}
