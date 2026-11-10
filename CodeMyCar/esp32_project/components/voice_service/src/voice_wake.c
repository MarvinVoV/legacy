#include "voice_wake.h"
#include "sdkconfig.h"
#if CONFIG_CMCAR_VOICE_WAKE_ENABLED
#include <stdlib.h>
#include <string.h>
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "voice_wake";
static srmodel_list_t *models;
static const esp_wn_iface_t *engine;
static model_iface_data_t *instance;
static int16_t *window;
static size_t window_size, used;
static uint32_t expected;
static bool have_sequence, has_inference;
static char *model_name;
static unsigned calls;
static int64_t total_us, max_us;

void voice_wake_destroy(void)
{
    if (instance) engine->destroy(instance);
    instance = NULL;
    free(window); window = NULL;
    if (models) esp_srmodel_deinit(models);
    models = NULL; engine = NULL;
    model_name = NULL;
    used = window_size = 0;
    have_sequence = has_inference = false;
    calls = 0; total_us = max_us = 0;
}

void voice_wake_reset(void)
{
    used = 0;
    have_sequence = false;
    /* C6 ESP-SR 2.6.1 model_clean dereferences uninitialised queues.
     * Never call it; recreate only after inference has actually run. */
    if (instance && has_inference) {
        engine->destroy(instance);
        instance = engine->create(model_name, DET_MODE_90);
        if (!instance) ESP_LOGE(TAG, "model reset failed; wake detection disabled");
    }
    has_inference = false;
}

bool voice_wake_init(void)
{
    const size_t before = esp_get_free_heap_size();
    models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, "wn9s_", "nihaoxiaozhi") : NULL;
    if (!name || models->num != 1) goto failure;
    model_name = name;
    engine = esp_wn_handle_from_name(name);
    if (!engine) goto failure;
    instance = engine->create(name, DET_MODE_90);
    if (!instance || engine->get_samp_rate(instance) != 16000 ||
        engine->get_channel_num(instance) != 1) goto failure;
    int n = engine->get_samp_chunksize(instance);
    if (n <= 0 || n > 4096) goto failure;
    window_size = n;
    window = malloc(window_size * sizeof(int16_t));
    if (!window) goto failure;
    voice_wake_reset();
    ESP_LOGI(TAG, "ready model=%s word=nihaoxiaozhi frame_samples=%u heap_before=%u heap_after=%u largest=%u",
             name, (unsigned)window_size, (unsigned)before, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
failure:
    ESP_LOGE(TAG, "wake init failed; voice disabled, no open-mic fallback");
    voice_wake_destroy();
    return false;
}

bool voice_wake_feed(const int16_t *pcm, size_t samples, uint32_t sequence)
{
    if (!instance || !pcm) return false;
    if (have_sequence && sequence != expected) {
        ESP_LOGW(TAG, "wake audio gap; reset model history");
        voice_wake_reset();
        if (!instance) return false;
    }
    have_sequence = true; expected = sequence + 1;
    while (samples) {
        size_t n = window_size - used;
        if (n > samples) n = samples;
        memcpy(window + used, pcm, n * sizeof(int16_t));
        pcm += n; used += n; samples -= n;
        if (used != window_size) continue;
        int64_t started = esp_timer_get_time();
        has_inference = true;
        int result = engine->detect(instance, window);
        int64_t elapsed = esp_timer_get_time() - started;
        total_us += elapsed;
        if (elapsed > max_us) max_us = elapsed;
        used = 0;
        if (++calls % 300 == 0) {
            ESP_LOGI(TAG, "inference avg_us=%lld max_us=%lld frame_us=%u heap=%u",
                     (long long)(total_us / calls), (long long)max_us,
                     (unsigned)(window_size * 1000000u / 16000u), (unsigned)esp_get_free_heap_size());
        }
        if (result == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "DETECTED nihaoxiaozhi");
            voice_wake_reset();
            return true;
        }
    }
    return false;
}
#else
bool voice_wake_init(void) { return true; }
bool voice_wake_feed(const int16_t *pcm, size_t samples, uint32_t sequence)
{ (void)pcm; (void)samples; (void)sequence; return false; }
void voice_wake_reset(void) {}
void voice_wake_destroy(void) {}
#endif
