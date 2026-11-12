/* Legacy ICS43434 I2S microphone codec.
 *
 * Input only: 16 kHz, 32-bit slots, 24-bit data in the selected slot, mono
 * 16-bit PCM delivered to the voice pipeline. Playback belongs to the future
 * board codec; this implementation intentionally reports no output. */

#include "voice_codec.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define CODEC_BCLK_GPIO CONFIG_CMCAR_VOICE_I2S_BCLK
#define CODEC_WS_GPIO CONFIG_CMCAR_VOICE_I2S_WS
#define CODEC_DIN_GPIO CONFIG_CMCAR_VOICE_I2S_DIN
#if CONFIG_CMCAR_VOICE_I2S_RIGHT_CHANNEL
#define CODEC_INPUT_SLOT 1
#else
#define CODEC_INPUT_SLOT 0
#endif
#define CODEC_RATE 16000
#define CODEC_FRAMES 320

static const char *TAG = "voice_codec";

typedef struct {
    i2s_chan_handle_t rx;
    int32_t *raw;
} i2s_codec_impl_t;

static esp_err_t i2s_read(voice_codec_t *codec, int16_t *dst, size_t frames)
{
    if (codec == NULL || dst == NULL || frames == 0) return ESP_ERR_INVALID_ARG;
    i2s_codec_impl_t *impl = codec->impl;
    const size_t want = frames * 2 * sizeof(int32_t);
    size_t got = 0;
    esp_err_t err = i2s_channel_read(impl->rx, impl->raw, want, &got, portMAX_DELAY);
    if (err != ESP_OK) return err;
    if (got != want) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < frames; ++i) {
        dst[i] = (int16_t)(impl->raw[2 * i + CODEC_INPUT_SLOT] >> 16);
    }
    return ESP_OK;
}

static esp_err_t i2s_write(voice_codec_t *codec, const int16_t *src, size_t frames,
                           int sample_rate)
{
    (void)codec;
    (void)src;
    (void)frames;
    (void)sample_rate;
    return ESP_ERR_NOT_SUPPORTED;
}

static void i2s_set_volume(voice_codec_t *codec, int volume)
{
    (void)codec;
    (void)volume;
}

static void i2s_destroy(voice_codec_t *codec)
{
    if (codec == NULL) return;
    i2s_codec_impl_t *impl = codec->impl;
    if (impl != NULL) {
        if (impl->rx != NULL) {
            (void)i2s_channel_disable(impl->rx);
            (void)i2s_del_channel(impl->rx);
        }
        heap_caps_free(impl->raw);
        free(impl);
    }
    free(codec);
}

static const voice_codec_ops_t s_i2s_ops = {
    .read = i2s_read,
    .write = i2s_write,
    .set_volume = i2s_set_volume,
    .destroy = i2s_destroy,
};

static bool pins_valid(void)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CODEC_BCLK_GPIO) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(CODEC_WS_GPIO) ||
        !GPIO_IS_VALID_GPIO(CODEC_DIN_GPIO) ||
        CODEC_BCLK_GPIO == CODEC_WS_GPIO || CODEC_BCLK_GPIO == CODEC_DIN_GPIO ||
        CODEC_WS_GPIO == CODEC_DIN_GPIO ||
        CODEC_BCLK_GPIO == CONFIG_CMCAR_UART_TX_PIN ||
        CODEC_BCLK_GPIO == CONFIG_CMCAR_UART_RX_PIN ||
        CODEC_WS_GPIO == CONFIG_CMCAR_UART_TX_PIN ||
        CODEC_WS_GPIO == CONFIG_CMCAR_UART_RX_PIN ||
        CODEC_DIN_GPIO == CONFIG_CMCAR_UART_TX_PIN ||
        CODEC_DIN_GPIO == CONFIG_CMCAR_UART_RX_PIN) {
        return false;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    const int pins[] = {CODEC_BCLK_GPIO, CODEC_WS_GPIO, CODEC_DIN_GPIO};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] >= 26 && pins[i] <= 37) return false;
    }
#endif
    return true;
}

esp_err_t voice_codec_i2s_init(voice_codec_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!pins_valid()) {
        ESP_LOGE(TAG, "Invalid I2S pins or collision with control UART");
        return ESP_ERR_INVALID_ARG;
    }

    voice_codec_t *codec = calloc(1, sizeof(*codec));
    i2s_codec_impl_t *impl = calloc(1, sizeof(*impl));
    if (codec == NULL || impl == NULL) {
        free(codec);
        free(impl);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &impl->rx);
    if (err != ESP_OK) {
        free(impl);
        free(codec);
        return err;
    }

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CODEC_BCLK_GPIO,
            .ws = CODEC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CODEC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    err = i2s_channel_init_std_mode(impl->rx, &cfg);
    if (err != ESP_OK) {
        i2s_channel_disable(impl->rx);
        i2s_del_channel(impl->rx);
        free(impl);
        free(codec);
        return err;
    }
    /* ICS43434 releases SD in the unused slot. Keep an unplugged input quiet
     * instead of feeding floating-pin noise into the wake model. */
    gpio_set_pull_mode(CODEC_DIN_GPIO, GPIO_PULLDOWN_ONLY);

    impl->raw = heap_caps_malloc(sizeof(int32_t) * CODEC_FRAMES * 2,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (impl->raw == NULL) {
        i2s_channel_disable(impl->rx);
        i2s_del_channel(impl->rx);
        free(impl);
        free(codec);
        return ESP_ERR_NO_MEM;
    }

    codec->ops = &s_i2s_ops;
    codec->impl = impl;
    codec->caps = (voice_codec_caps_t){
        .input_sample_rate = CODEC_RATE,
        .output_sample_rate = 0,
        .input_channels = 1,
        .output_channels = 0,
        .input_reference = false,
    };

    err = i2s_channel_enable(impl->rx);
    if (err != ESP_OK) {
        i2s_destroy(codec);
        return err;
    }

    ESP_LOGI(TAG, "I2S codec BCLK=%d WS=%d DIN=%d slot=%s",
             CODEC_BCLK_GPIO, CODEC_WS_GPIO, CODEC_DIN_GPIO,
             CODEC_INPUT_SLOT ? "right" : "left");
    *out = codec;
    return ESP_OK;
}
