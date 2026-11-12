/* Voice session supervisor. Hardware and transport have independent owners.
 * The supervisor is the only writer of runtime state; callbacks publish data.
 */
#include "voice_service.h"
#include "voice_audio.h"
#include "voice_output.h"
#include "voice_state.h"
#include "voice_transport.h"
#include "voice_wake_ack.h"
#include "voice_preroll.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "voice_frontend.h"
#endif
#include <math.h>
#include <stdatomic.h>
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "task_priorities.h"
#include "wifi_service.h"

#define VOICE_LISTEN_TIMEOUT_MS 20000
#define VOICE_RESULT_TIMEOUT_MS 45000
#define VOICE_PLAYBACK_TIMEOUT_MS 10000
#define VOICE_COOLDOWN_MS 1500
#if CONFIG_IDF_TARGET_ESP32S3
#define VOICE_TASK_STACK 8192
#else
#define VOICE_TASK_STACK 6144
#endif

static const char *TAG = "voice";
static atomic_int s_state = VOICE_STARTING;
static atomic_bool s_manual_trigger;
#if CONFIG_CMCAR_VOICE_CODEC_BOARD
static atomic_bool s_gain_probe;
#endif

static bool set_state(voice_state_t next)
{
    voice_state_t previous = atomic_load(&s_state);
    if (!voice_state_transition_allowed(previous, next)) {
        ESP_LOGE(TAG, "Rejected state transition %s -> %s",
                 voice_state_name(previous), voice_state_name(next));
        return false;
    }
    atomic_store(&s_state, next);
    voice_audio_set_active(next != VOICE_IDLE);
    ESP_LOGI(TAG, "state %s -> %s", voice_state_name(previous), voice_state_name(next));
    return true;
}

static esp_err_t on_server_audio(const int16_t *pcm, size_t frames,
                                 int sample_rate, int channels, uint32_t epoch, void *arg)
{
    (void)arg;
    return voice_output_write_epoch(pcm, frames, sample_rate, channels, epoch);
}

/* "我在" is played locally before any network operation so a wake still gets
 * a response when WiFi or pyserver is down. The tail is flushed afterwards:
 * the microphone hears the prompt and must not stream it as a user turn. */
static void play_local_ack(void)
{
    uint32_t duration_ms = 0;
    TickType_t started = xTaskGetTickCount();
    if (voice_wake_ack_play(&duration_ms) != ESP_OK) return;
    esp_err_t drained = voice_output_wait_drained(3000);
    if (drained != ESP_OK) {
        ESP_LOGW(TAG, "wake ack did not drain: %s", esp_err_to_name(drained));
    }
    /* wait_drained only proves the queue reached the DMA, not that the speaker
     * finished. Hold off the microphone for the rest of the prompt. */
    uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - started) * portTICK_PERIOD_MS);
    if (elapsed < duration_ms + 50) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms + 50 - elapsed));
    }
    voice_audio_flush();
}

static esp_err_t run_session(void)
{
    if (!set_state(VOICE_CONNECTING)) return ESP_ERR_INVALID_STATE;
    play_local_ack(); /* leave wake detection before the local speaker starts */
    if (!wifi_service_is_connected()) return ESP_ERR_INVALID_STATE;
    const bool continuous = CONFIG_CMCAR_VOICE_CONTINUOUS || CONFIG_CMCAR_VOICE_WAKE_ENABLED;
    voice_transport_t *transport = NULL;
    esp_err_t result = voice_transport_open(&transport, continuous, CONFIG_CMCAR_VOICE_WAKE_ENABLED,
                                            VOICE_SESSION_COMMAND);
    if (result != ESP_OK) return result;
    voice_transport_set_audio_callback(transport, on_server_audio, NULL);
    (void)set_state(VOICE_LISTENING);
    if (continuous) {
        voice_audio_flush(); /* Never replay audio from a previous connection. */
        if (CONFIG_CMCAR_VOICE_WAKE_ENABLED) {
            /* WakeNet fires at the end of the phrase, but the AFE output still
             * contains its tail. The server is playing the ready cue now, so
             * discard that tail instead of letting one-shot ASR treat the
             * wake word itself as the command and close before the user speaks. */
            vTaskDelay(pdMS_TO_TICKS(250));
            voice_audio_flush();
        }
    }
    voice_sequence_t sequence = {0};
    voice_frame_t frame;
    size_t samples = 0;
    const size_t target = CONFIG_CMCAR_VOICE_RECORD_SECONDS * VOICE_SAMPLE_RATE;
    const TickType_t started = xTaskGetTickCount();
    while (continuous || samples < target) {
        uint32_t events = voice_transport_events(transport);
        if (!(events & VOICE_PEER_CONNECTED) || (events & VOICE_PEER_FAILED)) {
            result = ESP_FAIL;
            goto close;
        }
        if (CONFIG_CMCAR_VOICE_WAKE_ENABLED &&
            (events & (VOICE_PEER_RECOGNIZING | VOICE_PEER_FINAL))) break;
        if (CONFIG_CMCAR_VOICE_WAKE_ENABLED &&
            xTaskGetTickCount() - started > pdMS_TO_TICKS(VOICE_LISTEN_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "No utterance within listen deadline");
            result = ESP_ERR_TIMEOUT;
            goto abort;
        }
        if (!voice_audio_receive(&frame, 500)) {
            ESP_LOGE(TAG, "Audio producer stalled");
            result = ESP_ERR_TIMEOUT;
            goto abort;
        }
        if (!frame.frames || frame.frames > VOICE_CHUNK_FRAMES ||
            !voice_sequence_accept(&sequence, frame.sequence)) {
            ESP_LOGE(TAG, "Audio stream discontinuity; discard unfinished utterance");
            result = ESP_ERR_INVALID_STATE;
            goto abort;
        }
        size_t count = frame.frames;
        if (!continuous && count > target - samples) count = target - samples;
        result = voice_transport_send_pcm(transport, frame.pcm, count);
        if (result != ESP_OK) goto close;
        samples += count;
        if (!continuous && xTaskGetTickCount() - started >
            pdMS_TO_TICKS((CONFIG_CMCAR_VOICE_RECORD_SECONDS + 3) * 1000)) {
            result = ESP_ERR_TIMEOUT;
            goto abort;
        }
    }
    (void)set_state(VOICE_PROCESSING);
    result = voice_transport_finish(transport, VOICE_RESULT_TIMEOUT_MS);
    if (result == ESP_OK && voice_output_enabled()) {
        esp_err_t drained = voice_output_wait_drained(VOICE_PLAYBACK_TIMEOUT_MS);
        if (drained != ESP_OK) {
            ESP_LOGW(TAG, "Playback did not drain: %s", esp_err_to_name(drained));
        }
    }
    goto close;
abort:
    voice_transport_abort(transport);
close:
    voice_transport_close(transport);
    return result;
}

static void perform_session(void)
{
    esp_err_t result = run_session();
    if (result != ESP_OK) ESP_LOGW(TAG, "Session ended: %s", esp_err_to_name(result));
    (void)set_state(VOICE_COOLDOWN);
    vTaskDelay(pdMS_TO_TICKS(VOICE_COOLDOWN_MS));
    voice_audio_flush();
    voice_audio_reset_wake();
    atomic_store(&s_manual_trigger, false);
    (void)voice_audio_take_energy_trigger();
    (void)set_state(VOICE_IDLE);
}

#if CONFIG_CMCAR_VOICE_MODE_CONVERSATION
static esp_err_t run_conversation(void)
{
    if (!set_state(VOICE_CONNECTING)) return ESP_ERR_INVALID_STATE;
    play_local_ack();
    if (!wifi_service_is_connected()) return ESP_ERR_INVALID_STATE;
    voice_transport_t *transport = NULL;
    esp_err_t result = voice_transport_open(&transport, true, false,
                                            VOICE_SESSION_CONVERSATION);
    if (result != ESP_OK) return result;
    voice_transport_set_audio_callback(transport, on_server_audio, NULL);
    (void)set_state(VOICE_LISTENING);
    voice_audio_flush();
    /* Drop the tail of the wake phrase before the first user turn. */
    vTaskDelay(pdMS_TO_TICKS(250));
    voice_audio_flush();

    voice_sequence_t sequence = {0};
    voice_frame_t frame;
    TickType_t last_audio = xTaskGetTickCount();
    bool dropping_echo = false;
    bool barge_capture = false;
    /* 600ms (~19KB PSRAM), including the 200ms DTD confirmation. Only AFE
     * residual is retained, never the raw microphone/reference channels. */
    voice_preroll_t preroll = {.capacity = 9600};
    if (voice_transport_barge_audio_enabled(transport)) {
        preroll.pcm = heap_caps_malloc(preroll.capacity * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!preroll.pcm) { voice_transport_close(transport); return ESP_ERR_NO_MEM; }
    }
    while (true) {
        if (voice_transport_poll(transport) != ESP_OK) { result = ESP_FAIL; break; }
        uint32_t events = voice_transport_events(transport);
        /* A clean server end can race the socket close; end wins. */
        if (events & VOICE_PEER_END) { result = ESP_OK; break; }
        if (events & VOICE_PEER_FAILED) { result = ESP_FAIL; break; }
        if (events & VOICE_PEER_DISCONNECTED) { result = ESP_FAIL; break; }
        if (events & VOICE_PEER_STOP) {
            voice_transport_clear_events(transport, VOICE_PEER_STOP);
            if (!barge_capture) {
                voice_audio_flush(); /* legacy: discard the trigger utterance */
                sequence = (voice_sequence_t){0};
            }
            dropping_echo = false;
            ESP_LOGI(TAG, "server stop: playback flushed");
        }
        if (!voice_audio_receive(&frame, 100)) {
            if (xTaskGetTickCount() - last_audio > pdMS_TO_TICKS(5000)) {
                ESP_LOGE(TAG, "Conversation audio producer stalled");
                result = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        last_audio = xTaskGetTickCount();
        if (barge_capture && voice_transport_is_speaking(transport)) {
            /* A later reply owns the speaker now. Start a fresh window. */
            barge_capture = false;
            voice_preroll_reset(&preroll);
        }
        if (!frame.frames || frame.frames > VOICE_CHUNK_FRAMES) {
            ESP_LOGE(TAG, "Conversation audio discontinuity");
            result = ESP_ERR_INVALID_STATE;
            voice_transport_abort(transport);
            break;
        }
        /* A playback transition is an intentional stream boundary, including
         * any queue replacements that happened before this iteration. Apply
         * it BEFORE sequence validation of a stale frame already in hand. */
        if (dropping_echo && !voice_transport_is_speaking(transport) &&
            !voice_output_active()) {
            dropping_echo = false;
            voice_audio_flush();
            sequence = (voice_sequence_t){0};
            continue;
        }
        if (!voice_sequence_accept(&sequence, frame.sequence)) {
            /* While the assistant speaks the network path is shared with TTS;
             * a replaced capture frame must not kill the session or the user
             * loses the ability to interrupt. Resync and keep the newest
             * audio. */
            if (voice_transport_is_speaking(transport) || voice_output_active()) {
                ESP_LOGW(TAG, "audio gap during playback; resyncing");
                voice_sequence_sync(&sequence, frame.sequence);
            } else {
                ESP_LOGE(TAG, "Conversation audio discontinuity");
                result = ESP_ERR_INVALID_STATE;
                voice_transport_abort(transport);
                break;
            }
        }
#if CONFIG_CMCAR_VOICE_AEC
        if (preroll.pcm && voice_transport_is_speaking(transport))
            (void)voice_preroll_push(&preroll, frame.pcm, frame.frames, frame.sequence);
        voice_frontend_set_playback(voice_transport_barge_enabled(transport) &&
            (voice_transport_is_speaking(transport) || voice_output_active()));
        if (voice_frontend_take_barge_in()) {
            esp_err_t interrupted = voice_transport_barge_in(transport);
            if (interrupted != ESP_OK && interrupted != ESP_ERR_INVALID_STATE) {
                result = ESP_FAIL;
                break;
            }
            if (interrupted == ESP_OK) {
                ESP_LOGI(TAG, "local barge-in from fresh AFE residual/VAD windows");
                if (preroll.pcm) {
                    barge_capture = true;
                    dropping_echo = false;
                    while (preroll.used) {
                        const int16_t *saved;
                        size_t n = voice_preroll_peek(&preroll, &saved, VOICE_CHUNK_FRAMES);
                        result = voice_transport_send_pcm(transport, saved, n);
                        if (result != ESP_OK) break;
                        voice_preroll_consume(&preroll, n);
                    }
                    if (result != ESP_OK) break;
                    continue; /* this frame was already included in pre-roll */
                }
            }
        }
#endif
        if (!barge_capture && (voice_transport_is_speaking(transport) || voice_output_active())) {
            if (!dropping_echo) {
                dropping_echo = true;
                ESP_LOGI(TAG, "assistant speaking; microphone frames dropped");
            }
            continue;
        }
        if (dropping_echo) {
            dropping_echo = false;
            voice_audio_flush();
            sequence = (voice_sequence_t){0};
            ESP_LOGI(TAG, "assistant done; microphone streaming resumed");
            continue; /* the frame already in hand still belongs to playback */
        }
        result = voice_transport_send_pcm(transport, frame.pcm, frame.frames);
        if (result != ESP_OK) {
            /* The server may close immediately after {"type":"end"}; a send
             * already in flight then fails, but the session ended cleanly. */
            if (voice_transport_events(transport) & VOICE_PEER_END) {
                result = ESP_OK;
            }
            break;
        }
    }
#if CONFIG_CMCAR_VOICE_AEC
    voice_frontend_set_playback(false);
#endif
    heap_caps_free(preroll.pcm);
    voice_transport_close(transport);
    return result;
}

static void perform_conversation(void)
{
    esp_err_t result = run_conversation();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Conversation ended: %s", esp_err_to_name(result));
    }
    (void)set_state(VOICE_COOLDOWN);
    vTaskDelay(pdMS_TO_TICKS(VOICE_COOLDOWN_MS));
    voice_audio_flush();
    voice_audio_reset_wake();
    atomic_store(&s_manual_trigger, false);
    (void)voice_audio_take_energy_trigger();
    (void)set_state(VOICE_IDLE);
}
#endif

static void voice_task(void *arg)
{
    (void)arg;
    (void)set_state(VOICE_IDLE);
#if CONFIG_CMCAR_VOICE_WAKE_ENABLED
    voice_frame_t frame;
    unsigned received = 0;
#if CONFIG_IDF_TARGET_ESP32S3
    const char *wake_label = CONFIG_CMCAR_VOICE_S3_WAKE_LABEL;
#else
    const char *wake_label = "你好小智";
#endif
#if CONFIG_CMCAR_VOICE_MODE_CONVERSATION
    ESP_LOGI(TAG, "Say %s, then speak naturally; pause 700 ms to end a turn", wake_label);
#else
    ESP_LOGI(TAG, "Say %s; wait for the local tone before speaking", wake_label);
#endif
    while (true) {
        if (!voice_audio_receive(&frame, 500)) continue;
#if CONFIG_CMCAR_VOICE_CODEC_BOARD
        if (atomic_exchange(&s_gain_probe, false)) {
            voice_audio_set_active(true); /* suppress wake during temporary gain steps */
            esp_err_t probe = voice_codec_board_probe_gains(voice_audio_codec());
            ESP_LOGI(TAG, "gain probe completed: %s", esp_err_to_name(probe));
            voice_audio_flush();
            voice_audio_set_active(false);
            continue;
        }
#endif
        bool wake = voice_audio_detect_wake(&frame);
        bool manual = atomic_exchange(&s_manual_trigger, false);
        if (++received % 500 == 0)
            ESP_LOGI(TAG, "idle internal_free=%u psram_free=%u stack_watermark=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (wake || manual) {
#if CONFIG_CMCAR_VOICE_MODE_CONVERSATION
            perform_conversation();
#else
            perform_session();
#endif
        }
    }
#elif CONFIG_CMCAR_VOICE_CONTINUOUS
    while (true) {
        if (wifi_service_is_connected()) perform_session();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#elif CONFIG_CMCAR_VOICE_AUTO_LOOP
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        perform_session();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CMCAR_VOICE_LOOP_DELAY_SEC * 1000));
    }
#else
    while (true) {
        if (atomic_exchange(&s_manual_trigger, false) || voice_audio_take_energy_trigger())
            perform_session();
        else vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}

#if CONFIG_CMCAR_VOICE_AEC
/* Diagnostic-only: a deterministic noise burst lets AEC/ERLE be measured
 * without a server or a human speaker. Triggered by 'a' on the console. */
static void play_aec_test_noise(void)
{
    int16_t pcm[VOICE_CHUNK_FRAMES];
    uint32_t seed = 0x12345678u;
    for (int w = 0; w < 250; ++w) { /* 5 s at 20 ms per chunk */
        for (int i = 0; i < VOICE_CHUNK_FRAMES; ++i) {
            seed = seed * 1664525u + 1013904223u;
            pcm[i] = (int16_t)((int32_t)(seed >> 16) % 2400 - 1200);
        }
        if (voice_output_write(pcm, VOICE_CHUNK_FRAMES, VOICE_SAMPLE_RATE, 1) != ESP_OK) {
            break;
        }
    }
    (void)voice_output_wait_drained(6000);
    ESP_LOGI(TAG, "AEC test noise done");
}

/* Diagnostic-only 1 kHz tone: a periodic reference makes alignment visible
 * even when the acoustic delay is unknown. Triggered by 't'. */
static void play_aec_test_tone(void)
{
    int16_t pcm[VOICE_CHUNK_FRAMES];
    double phase = 0;
    for (int w = 0; w < 250; ++w) {
        for (int i = 0; i < VOICE_CHUNK_FRAMES; ++i) {
            pcm[i] = (int16_t)(2400.0 * sin(2.0 * M_PI * 1000.0 * phase / VOICE_SAMPLE_RATE));
            phase += 1.0;
        }
        if (voice_output_write(pcm, VOICE_CHUNK_FRAMES, VOICE_SAMPLE_RATE, 1) != ESP_OK) {
            break;
        }
    }
    (void)voice_output_wait_drained(6000);
    ESP_LOGI(TAG, "AEC test tone done");
}
#endif

static void console_task(void *arg)
{
    (void)arg;
    if (!uart_is_driver_installed(UART_NUM_0) &&
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Console unavailable; voice remains operational");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Serial s is a diagnostic wake bypass");
    while (true) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) > 0) {
            if ((c == 's' || c == 'S') && atomic_load(&s_state) == VOICE_IDLE) {
                atomic_store(&s_manual_trigger, true);
            }
#if CONFIG_CMCAR_VOICE_AEC
            if ((c == 'g' || c == 'G') && atomic_load(&s_state) == VOICE_IDLE)
                atomic_store(&s_gain_probe, true);
            if ((c == 'a' || c == 'A') && atomic_load(&s_state) == VOICE_IDLE) {
                play_aec_test_noise();
            }
            if ((c == 't' || c == 'T') && atomic_load(&s_state) == VOICE_IDLE) {
                play_aec_test_tone();
            }
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t voice_service_start(void)
{
    if (atomic_load(&s_state) != VOICE_STARTING) return ESP_ERR_INVALID_STATE;
    esp_err_t err = voice_audio_init();
    if (err != ESP_OK) goto disabled;
    err = voice_output_init(voice_audio_codec());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Playback unavailable (%s); ASR/control continue",
                 esp_err_to_name(err));
    }
    TaskHandle_t supervisor = NULL;
    if (xTaskCreate(voice_task, "voice_session", VOICE_TASK_STACK, NULL,
                    TASK_PRIO_VOICE_NETWORK, &supervisor) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        voice_output_destroy();
        voice_audio_destroy();
        goto disabled;
    }
    err = voice_audio_start();
    if (err != ESP_OK) {
        vTaskDelete(supervisor);
        voice_output_destroy();
        voice_audio_destroy();
        goto disabled;
    }
    if (xTaskCreate(console_task, "voice_console", 4096, NULL,
                    TASK_PRIO_VOICE_CONSOLE, NULL) != pdPASS)
        ESP_LOGW(TAG, "Serial bypass unavailable; voice remains operational");
    return ESP_OK;
disabled:
    atomic_store(&s_state, VOICE_DISABLED);
    ESP_LOGE(TAG, "Voice disabled (%s); control bridge remains available", esp_err_to_name(err));
    return err;
}
