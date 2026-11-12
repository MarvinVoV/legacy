/* ES7210 + ES8311 + NS4150B board codec.
 *
 * Topology (verified on the target board, 2026-11-12):
 *   - one full-duplex I2S controller: TX STD -> ES8311 DAC, RX TDM 4 slots
 *     <- ES7210 ADC;
 *   - ES7210 channel mapping: ch0/ch2 microphones, ch1 pre-amplifier AEC
 *     reference, ch3 empty;
 *   - the 4x16-bit TDM receive frame forces the paired TX to 32-bit slots.
 *     ES8311 must therefore be opened for 32-bit data or it plays at 2x rate
 *     (a 1 kHz tone measured as 2 kHz). Do not change this to 16-bit output
 *     without re-testing the playback pitch.
 *
 * The voice pipeline stays mono 16 kHz: read() picks one microphone channel
 * and write() expands mono PCM to the 32-bit stereo frame the DAC expects.
 */

#include "voice_codec.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"

#define BOARD_RATE 16000
#define BOARD_FRAMES 320
#define BOARD_IN_CHANNELS 4
#define BOARD_OUT_CHANNELS 2
#define BOARD_I2S_PORT I2S_NUM_0
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_HZ 100000

#define PIN_SDA CONFIG_CMCAR_VOICE_BOARD_I2C_SDA
#define PIN_SCL CONFIG_CMCAR_VOICE_BOARD_I2C_SCL
#define PIN_MCK CONFIG_CMCAR_VOICE_BOARD_I2S_MCK
#define PIN_BCLK CONFIG_CMCAR_VOICE_BOARD_I2S_BCLK
#define PIN_WS CONFIG_CMCAR_VOICE_BOARD_I2S_WS
#define PIN_DOUT CONFIG_CMCAR_VOICE_BOARD_I2S_DOUT
#define PIN_DIN CONFIG_CMCAR_VOICE_BOARD_I2S_DIN
#define PIN_PA CONFIG_CMCAR_VOICE_BOARD_PA_GPIO

static const char *TAG = "voice_board";

typedef struct {
    i2c_master_bus_handle_t bus;
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    esp_codec_dev_handle_t out;
    esp_codec_dev_handle_t in;
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *out_ctrl;
    const audio_codec_ctrl_if_t *in_ctrl;
    const audio_codec_if_t *out_codec;
    const audio_codec_if_t *in_codec;
    int16_t *in_buf;
    int32_t *out_buf;
    int volume;
    bool pa_ready;
} board_impl_t;

static esp_err_t board_read(voice_codec_t *codec, int16_t *dst, size_t frames)
{
    if (codec == NULL || dst == NULL || frames == 0 || frames > BOARD_FRAMES) {
        return ESP_ERR_INVALID_ARG;
    }
    board_impl_t *impl = codec->impl;
    const size_t want = BOARD_FRAMES * BOARD_IN_CHANNELS * sizeof(int16_t);
    int ret = esp_codec_dev_read(impl->in, impl->in_buf, want);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES7210 read failed: %d", ret);
        return ESP_FAIL;
    }
    const int ch = CONFIG_CMCAR_VOICE_BOARD_MIC_CHANNEL;
    for (size_t i = 0; i < frames; ++i) {
        dst[i] = impl->in_buf[i * BOARD_IN_CHANNELS + ch];
    }
    return ESP_OK;
}

static esp_err_t board_write(voice_codec_t *codec, const int16_t *src,
                             size_t frames, int sample_rate)
{
    if (codec == NULL || src == NULL || frames == 0) return ESP_ERR_INVALID_ARG;
    if (sample_rate != BOARD_RATE) {
        ESP_LOGE(TAG, "unsupported playback rate %d", sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    board_impl_t *impl = codec->impl;
    while (frames > 0) {
        size_t n = frames > BOARD_FRAMES ? BOARD_FRAMES : frames;
        for (size_t i = 0; i < n; ++i) {
            int32_t v = (int32_t)src[i] << 16;
            impl->out_buf[2 * i] = v;
            impl->out_buf[2 * i + 1] = v;
        }
        int ret = esp_codec_dev_write(impl->out, impl->out_buf,
                                      n * BOARD_OUT_CHANNELS * sizeof(int32_t));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 write failed: %d", ret);
            return ESP_FAIL;
        }
        src += n;
        frames -= n;
    }
    return ESP_OK;
}

static void board_set_volume(voice_codec_t *codec, int volume)
{
    if (codec == NULL) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    board_impl_t *impl = codec->impl;
    impl->volume = volume;
    if (impl->out != NULL) {
        (void)esp_codec_dev_set_out_vol(impl->out, volume);
    }
}

static void board_pa(bool on, board_impl_t *impl)
{
#if PIN_PA >= 0
    if (impl->pa_ready) gpio_set_level(PIN_PA, on ? 1 : 0);
#else
    (void)on;
    (void)impl;
#endif
}

static void board_destroy(voice_codec_t *codec)
{
    if (codec == NULL) return;
    board_impl_t *impl = codec->impl;
    if (impl != NULL) {
        board_pa(false, impl);
        if (impl->out != NULL) {
            (void)esp_codec_dev_set_out_mute(impl->out, true);
            esp_codec_dev_delete(impl->out);
        }
        if (impl->in != NULL) {
            esp_codec_dev_delete(impl->in);
        }
        /* The codec/ctrl/data interface objects are static singletons owned by
         * esp_codec_dev; the API exposes no destructor for them. Only the
         * I2S channels, the I2C bus and this allocation belong to us. */
        if (impl->rx != NULL) {
            (void)i2s_channel_disable(impl->rx);
            (void)i2s_del_channel(impl->rx);
        }
        if (impl->tx != NULL) {
            (void)i2s_channel_disable(impl->tx);
            (void)i2s_del_channel(impl->tx);
        }
        if (impl->bus != NULL) (void)i2c_del_master_bus(impl->bus);
        heap_caps_free(impl->in_buf);
        heap_caps_free(impl->out_buf);
        free(impl);
    }
    free(codec);
}

static const voice_codec_ops_t s_board_ops = {
    .read = board_read,
    .write = board_write,
    .set_volume = board_set_volume,
    .destroy = board_destroy,
};

static bool board_pins_valid(void)
{
    const int pins[] = {PIN_SDA, PIN_SCL, PIN_MCK, PIN_BCLK, PIN_WS,
                        PIN_DOUT, PIN_DIN};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) return false;
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (pins[i] == pins[j]) return false;
        }
        if (pins[i] == CONFIG_CMCAR_UART_TX_PIN ||
            pins[i] == CONFIG_CMCAR_UART_RX_PIN) {
            return false;
        }
        if (pins[i] >= 26 && pins[i] <= 37) return false; /* PSRAM/flash */
    }
#if PIN_PA >= 0
    if (!GPIO_IS_VALID_OUTPUT_GPIO(PIN_PA)) return false;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (PIN_PA == pins[i]) return false;
    }
#endif
    return true;
}

static esp_err_t board_init_i2s(board_impl_t *impl)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &impl->tx, &impl->rx);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = PIN_DIN,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(impl->tx, &std_cfg);
    if (err != ESP_OK) return err;

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(BOARD_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = PIN_MCK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = PIN_DIN,
            .invert_flags = {false, false, false},
        },
    };
    tdm_cfg.slot_cfg.total_slot = 4;
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_tdm_mode(impl->rx, &tdm_cfg);
    if (err != ESP_OK) return err;

    /* Enabling TX starts the paired RX in full-duplex; enabling both handles
     * separately is unsupported and logs an error. */
    return i2s_channel_enable(impl->tx);
}

static esp_err_t board_init_codecs(board_impl_t *impl)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_I2S_PORT,
        .rx_handle = impl->rx,
        .tx_handle = impl->tx,
        .clk_src = I2S_CLK_SRC_DEFAULT,
    };
    impl->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (impl->data_if == NULL) return ESP_ERR_NO_MEM;

    audio_codec_i2c_cfg_t out_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = impl->bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    impl->out_ctrl = audio_codec_new_i2c_ctrl(&out_i2c);
    if (impl->out_ctrl == NULL) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t out_cfg = {
        .ctrl_if = impl->out_ctrl,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = true,
        .no_dac_ref = true,
        .mclk_div = 256,
    };
    impl->out_codec = es8311_codec_new(&out_cfg);
    if (impl->out_codec == NULL) return ESP_FAIL;
    esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = impl->out_codec,
        .data_if = impl->data_if,
    };
    impl->out = esp_codec_dev_new(&out_dev_cfg);
    if (impl->out == NULL) return ESP_ERR_NO_MEM;

    audio_codec_i2c_cfg_t in_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR + 0x02,
        .bus_handle = impl->bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    impl->in_ctrl = audio_codec_new_i2c_ctrl(&in_i2c);
    if (impl->in_ctrl == NULL) return ESP_ERR_NO_MEM;
    es7210_codec_cfg_t in_cfg = {
        .ctrl_if = impl->in_ctrl,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_div = 256,
    };
    impl->in_codec = es7210_codec_new(&in_cfg);
    if (impl->in_codec == NULL) return ESP_FAIL;
    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = impl->in_codec,
        .data_if = impl->data_if,
    };
    impl->in = esp_codec_dev_new(&in_dev_cfg);
    if (impl->in == NULL) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t board_open_streams(board_impl_t *impl)
{
    /* 32-bit slots are mandatory: the TDM RX frame is 64 bits and forces the
     * paired TX to 32-bit slots; ES8311 must agree or it plays at 2x rate. */
    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 32,
        .channel = BOARD_OUT_CHANNELS,
        .channel_mask = 0,
        .sample_rate = BOARD_RATE,
        .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(impl->out, &out_fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 open failed: %d", ret);
        return ESP_FAIL;
    }
    (void)esp_codec_dev_set_out_mute(impl->out, false);
    esp_err_t err = esp_codec_dev_set_out_vol(impl->out,
                                              CONFIG_CMCAR_VOICE_BOARD_VOLUME) ==
                            ESP_CODEC_DEV_OK
                        ? ESP_OK
                        : ESP_FAIL;
    impl->volume = CONFIG_CMCAR_VOICE_BOARD_VOLUME;

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = BOARD_IN_CHANNELS,
        .channel_mask = 0,
        .sample_rate = BOARD_RATE,
        .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(impl->in, &in_fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES7210 open failed: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_set_in_gain(impl->in,
                                    (float)CONFIG_CMCAR_VOICE_BOARD_INPUT_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES7210 gain failed: %d", ret);
    }
    return err;
}

esp_err_t voice_codec_board_init(voice_codec_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!board_pins_valid()) {
        ESP_LOGE(TAG, "Invalid board pins or collision with control UART");
        return ESP_ERR_INVALID_ARG;
    }

    voice_codec_t *codec = calloc(1, sizeof(*codec));
    board_impl_t *impl = calloc(1, sizeof(*impl));
    if (codec == NULL || impl == NULL) {
        free(codec);
        free(impl);
        return ESP_ERR_NO_MEM;
    }
    codec->ops = &s_board_ops;
    codec->impl = impl;

    esp_err_t err;
#if PIN_PA >= 0
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PIN_PA,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&pa_cfg);
    if (err != ESP_OK) goto fail;
    gpio_set_level(PIN_PA, 0);
    impl->pa_ready = true;
#endif

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &impl->bus);
    if (err != ESP_OK) goto fail;

    err = board_init_i2s(impl);
    if (err != ESP_OK) goto fail;

    err = board_init_codecs(impl);
    if (err != ESP_OK) goto fail;

    impl->in_buf = heap_caps_malloc(
        BOARD_FRAMES * BOARD_IN_CHANNELS * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    impl->out_buf = heap_caps_malloc(
        BOARD_FRAMES * BOARD_OUT_CHANNELS * sizeof(int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (impl->in_buf == NULL || impl->out_buf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = board_open_streams(impl);
    if (err != ESP_OK) goto fail;

    /* The amplifier is enabled only after the codec is configured, so a reset
     * or a failed init cannot drive a floating DAC into the speaker. */
    board_pa(true, impl);

    codec->caps = (voice_codec_caps_t){
        .input_sample_rate = BOARD_RATE,
        .output_sample_rate = BOARD_RATE,
        .input_channels = 1,
        .output_channels = BOARD_OUT_CHANNELS,
        .input_reference = true,
    };
    ESP_LOGI(TAG,
             "board codec ready: I2C %d/%d, I2S %d/%d/%d, out=%d in=%d gain=%ddB "
             "mic_ch=%d vol=%d pa=%s",
             PIN_SDA, PIN_SCL, PIN_MCK, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN,
             CONFIG_CMCAR_VOICE_BOARD_INPUT_GAIN_DB,
             CONFIG_CMCAR_VOICE_BOARD_MIC_CHANNEL,
             CONFIG_CMCAR_VOICE_BOARD_VOLUME,
             CONFIG_CMCAR_VOICE_BOARD_PA_GPIO >= 0 ? "gpio" : "hardwired");
    *out = codec;
    return ESP_OK;

fail:
    board_destroy(codec);
    return err;
}

#else /* !ESP32S3 */

esp_err_t voice_codec_board_init(voice_codec_t **out)
{
    if (out != NULL) *out = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
