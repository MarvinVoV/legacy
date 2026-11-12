#include "voice_wake_ack.h"

#include <string.h>

#include "esp_log.h"
#include "voice_output.h"

#define ACK_RATE 16000
#define ACK_CHUNK_FRAMES 320

extern const uint8_t wake_ack_pcm_start[] asm("_binary_wake_ack_pcm_start");
extern const uint8_t wake_ack_pcm_end[] asm("_binary_wake_ack_pcm_end");

static const char *TAG = "wake_ack";

bool voice_wake_ack_available(void)
{
    return (size_t)(wake_ack_pcm_end - wake_ack_pcm_start) >= 2 * ACK_CHUNK_FRAMES;
}

esp_err_t voice_wake_ack_play(uint32_t *duration_ms)
{
    if (duration_ms != NULL) *duration_ms = 0;
    if (!voice_output_enabled() || !voice_wake_ack_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t *cursor = wake_ack_pcm_start;
    const size_t total_frames = (size_t)(wake_ack_pcm_end - wake_ack_pcm_start) / 2;
    int16_t pcm[ACK_CHUNK_FRAMES];
    size_t remaining = total_frames;
    while (remaining > 0) {
        size_t n = remaining > ACK_CHUNK_FRAMES ? ACK_CHUNK_FRAMES : remaining;
        /* Embedded data has byte alignment; never hand the codec an unaligned
         * int16_t pointer. */
        memcpy(pcm, cursor, n * sizeof(int16_t));
        esp_err_t err = voice_output_write(pcm, n, ACK_RATE, 1);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wake ack playback failed: %s", esp_err_to_name(err));
            return err;
        }
        cursor += n * sizeof(int16_t);
        remaining -= n;
    }
    if (duration_ms != NULL) {
        *duration_ms = (uint32_t)(total_frames * 1000 / ACK_RATE);
    }
    ESP_LOGI(TAG, "local wake ack queued (%u frames, %u ms)",
             (unsigned)total_frames,
             (unsigned)(total_frames * 1000 / ACK_RATE));
    return ESP_OK;
}
