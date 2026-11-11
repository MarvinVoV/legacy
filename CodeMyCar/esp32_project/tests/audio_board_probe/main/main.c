#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_SDA 8
#define I2C_SCL 9
#define I2S_MCK 15
#define I2S_BCK 16
#define I2S_WS 17
#define I2S_DOUT 18
#define I2S_DIN 21
#define RATE 16000
#define FRAMES 320
#define TONE_HZ 1000
#define TONE_AMP 300
#define REC_SECONDS 3

static const char *TAG = "loopback";
static esp_codec_dev_handle_t s_out;
static esp_codec_dev_handle_t s_in;
static int16_t s_tone[FRAMES];

static void tone_task(void *arg)
{
    (void)arg;
    const int frames = REC_SECONDS * RATE / FRAMES;
    for (int n = 0; n < frames; ++n) {
        int ret = esp_codec_dev_write(s_out, s_tone, sizeof(s_tone));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "tone write failed: %d", ret);
            break;
        }
    }
    ESP_LOGI(TAG, "tone finished");
    vTaskDelete(NULL);
}

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* One I2S controller in full-duplex mode: TX STD to ES8311, RX TDM from
     * ES7210. The previous crash was app_main stack overflow from a 2.5 KB
     * local buffer, not the duplex controller itself. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    i2s_chan_handle_t tx = NULL, rx = NULL;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK, .bclk = I2S_BCK, .ws = I2S_WS,
            .dout = I2S_DOUT, .din = I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = I2S_MCK, .bclk = I2S_BCK, .ws = I2S_WS,
            .dout = I2S_DOUT, .din = I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };
    tdm_cfg.slot_cfg.total_slot = 4;
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx, &tdm_cfg));
    /* Enabling TX starts the paired RX in full-duplex; enabling both handles
     * separately is what the IDF 6.1 driver warns about. */
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx,
        .clk_src = I2S_CLK_SRC_DEFAULT,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) { ESP_LOGE(TAG, "data_if fail"); return; }

    audio_codec_i2c_cfg_t out_i2c = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus, .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&out_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t out_cfg = {
        .ctrl_if = out_ctrl, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, .master_mode = false, .use_mclk = true,
        .no_dac_ref = true, .mclk_div = 256,
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&out_cfg);
    esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec, .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&out_dev_cfg);

    audio_codec_i2c_cfg_t in_i2c = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR + 0x02,
        .bus_handle = bus, .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&in_i2c);
    es7210_codec_cfg_t in_cfg = {
        .ctrl_if = in_ctrl, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_div = 256,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&in_cfg);
    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec, .data_if = data_if,
    };
    s_in = esp_codec_dev_new(&in_dev_cfg);
    if (!s_out || !s_in) { ESP_LOGE(TAG, "dev new fail"); return; }

    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16, .channel = 2, .channel_mask = 0,
        .sample_rate = RATE, .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(s_out, &out_fs);
    ESP_LOGI(TAG, "out open=%d", ret);
    if (ret != ESP_CODEC_DEV_OK) return;
    ret = esp_codec_dev_set_out_vol(s_out, 10);
    ESP_LOGI(TAG, "out volume ret=%d", ret);

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16, .channel = 4, .channel_mask = 0,
        .sample_rate = RATE, .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(s_in, &in_fs);
    ESP_LOGI(TAG, "in open=%d", ret);
    if (ret != ESP_CODEC_DEV_OK) return;
    ESP_LOGI(TAG, "in gain ret=%d", esp_codec_dev_set_in_gain(s_in, 30.0f));

    for (int n = 0; n < FRAMES; ++n) {
        s_tone[n] = (int16_t)(TONE_AMP * sin(2.0 * M_PI * TONE_HZ * n / RATE));
    }
    BaseType_t ok = xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    if (ok != pdPASS) { ESP_LOGE(TAG, "tone task fail"); return; }

    static int16_t buf[FRAMES * 4]; /* keep 2.5 KB off the 3.5 KB app_main stack */
    double sums[4] = {0};
    double best_corr[4] = {-1, -1, -1, -1};
    int best_lag[4] = {0, 0, 0, 0};
    double tone_energy = 0;
    for (int n = 0; n < FRAMES; ++n) tone_energy += (double)s_tone[n] * s_tone[n];
    int frames = 0;
    int target_frames = 24; /* 0.48 s is enough for correlation and avoids WDT */
    while (frames < target_frames) {
        ret = esp_codec_dev_read(s_in, buf, sizeof(buf));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "in read fail %d", ret);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int ch = 0; ch < 4; ++ch) {
            for (int n = 0; n < FRAMES; ++n) {
                sums[ch] += (double)buf[n * 4 + ch] * buf[n * 4 + ch];
            }
            for (int lag = -32; lag <= 32; ++lag) {
                double dot = 0, energy = 0;
                for (int n = 0; n < FRAMES; ++n) {
                    int j = n + lag;
                    if (j < 0 || j >= FRAMES) continue;
                    double v = buf[n * 4 + ch];
                    dot += v * s_tone[j];
                    energy += v * v;
                }
                if (energy > 0) {
                    double c = fabs(dot / sqrt(energy * tone_energy));
                    if (c > best_corr[ch]) {
                        best_corr[ch] = c;
                        best_lag[ch] = lag;
                    }
                }
            }
        }
        ++frames;
    }
    for (int ch = 0; ch < 4; ++ch) {
        ESP_LOGI(TAG, "ch%d rms=%.1f corr=%.3f lag=%d", ch,
                 sqrt(sums[ch] / (frames * FRAMES)), best_corr[ch], best_lag[ch]);
    }
    ESP_LOGI(TAG, "loopback done frames=%d", frames);
}
