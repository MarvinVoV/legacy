/* ES7210 + ES8311 + NS4150B probe: tone playback + 4-channel RMS time series.
 *
 * A7 discriminator: the same firmware is run once with the microphones open
 * and once with them covered. A channel whose RMS does not drop when the mics
 * are covered is an electrical tap, not acoustic pickup. RMS (not a single
 * correlation value) is the measurement that decides it.
 */

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
#define FRAMES 320          /* 20 ms per read at 16 kHz */
#define TONE_HZ 1000
#define TONE_AMP 300
#define TONE_SECONDS 40

/* Microphone mapping (A6) needs a broadband, clearly audible source so that
 * covering one mic produces an unmistakable RMS drop on its channel. Set to
 * 0 to get the pure 1 kHz reference/tone test back. */
#define TEST_NOISE 1
#define NOISE_AMP 2000

#define WINDOW_READS 25     /* 25 * 20 ms = 0.5 s */
#define WINDOWS 30          /* 15 s time series */

static const char *TAG = "loopback";
static esp_codec_dev_handle_t s_out;
static esp_codec_dev_handle_t s_in;
static int16_t s_ref[FRAMES];           /* mono reference used by correlation */
static int32_t s_tone[FRAMES * 2];      /* stereo 32-bit slots */
static int32_t s_silence[FRAMES * 2];
static volatile bool s_tone_on = true;

static double tone_amp_at(const int16_t *buf, int ch, double f)
{
    double re = 0, im = 0, energy = 0;
    for (int k = 0; k < FRAMES; ++k) {
        double ph = 2.0 * M_PI * f * k / RATE;
        double v = buf[k * 4 + ch];
        re += v * cos(ph);
        im += v * sin(ph);
        energy += v * v;
    }
    if (energy <= 0) return 0;
    /* Fraction of the channel's energy explained by a sinusoid at f. */
    double cre = 2.0 * re / FRAMES, cim = 2.0 * im / FRAMES;
    return (cre * cre + cim * cim) * FRAMES / (2.0 * energy);
}

static void scan_frequency(const int16_t *buf, int w)
{
    double best_f[4] = {0}, best_fit[4] = {0};
    for (int ch = 0; ch < 4; ++ch) {
        for (int f = 200; f <= 4000; f += 50) {
            double fit = tone_amp_at(buf, ch, f);
            if (fit > best_fit[ch]) { best_fit[ch] = fit; best_f[ch] = f; }
        }
    }
    ESP_LOGI(TAG, "scan w=%02d f0=%4.0f/%1.2f f1=%4.0f/%1.2f f2=%4.0f/%1.2f f3=%4.0f/%1.2f",
             w, best_f[0], best_fit[0], best_f[1], best_fit[1],
             best_f[2], best_fit[2], best_f[3], best_fit[3]);
}


static void tone_task(void *arg)
{
    (void)arg;
    const int writes = TONE_SECONDS * RATE / FRAMES;
    for (int n = 0; n < writes; ++n) {
        const int32_t *buf = s_tone_on ? s_tone : s_silence;
        int ret = esp_codec_dev_write(s_out, (void *)buf, sizeof(s_tone));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "tone write failed: %d", ret);
            break;
        }
    }
    /* Keep the TX DMA fed with silence so the I2S clock (and paired RX) keeps
     * running while the capture window finishes. */
    for (int n = 0; n < 200; ++n) {
        esp_codec_dev_write(s_out, s_silence, sizeof(s_silence));
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
     * ES7210. A 2.5 KB local buffer used to overflow the app_main stack, and
     * enabling both handles separately is unsupported; see README. */
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
        .bits_per_sample = 32, .channel = 2, .channel_mask = 0,
        .sample_rate = RATE, .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(s_out, &out_fs);
    ESP_LOGI(TAG, "out open=%d", ret);
    if (ret != ESP_CODEC_DEV_OK) return;
    ret = esp_codec_dev_set_out_vol(s_out, TEST_NOISE ? 40 : 40);
    ESP_LOGI(TAG, "out volume ret=%d", ret);

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16, .channel = 4, .channel_mask = 0,
        .sample_rate = RATE, .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(s_in, &in_fs);
    ESP_LOGI(TAG, "in open=%d", ret);
    if (ret != ESP_CODEC_DEV_OK) return;
    ESP_LOGI(TAG, "in gain ret=%d", esp_codec_dev_set_in_gain(s_in, 37.5f));

    uint32_t seed = 0x12345678u;
    for (int n = 0; n < FRAMES; ++n) {
#if TEST_NOISE
        seed = seed * 1664525u + 1013904223u;
        s_ref[n] = (int16_t)((int32_t)(seed >> 16) % (2 * NOISE_AMP) - NOISE_AMP);
#else
        s_ref[n] = (int16_t)(TONE_AMP * sin(2.0 * M_PI * TONE_HZ * n / RATE));
#endif
        s_tone[2 * n] = s_tone[2 * n + 1] = (int32_t)s_ref[n] << 16;
    }
    BaseType_t ok = xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    if (ok != pdPASS) { ESP_LOGE(TAG, "tone task fail"); return; }

    static int16_t buf[FRAMES * 4]; /* keep 2.5 KB off the 3.5 KB app_main stack */
    ESP_LOGI(TAG, "PROBE START windows=%d window_ms=%d src=%s vol=%d gain=37.5dB",
             WINDOWS, WINDOW_READS * 20, TEST_NOISE ? "noise" : "1000Hz",
             TEST_NOISE ? 40 : 40);

    for (int w = 0; w < WINDOWS; ++w) {
        if (w == WINDOWS / 2) {
            /* Second half is the silence baseline: same microphone, no tone. */
            s_tone_on = false;
            ESP_LOGI(TAG, "TONE OFF");
        }
        if (w == 8) {
            /* Middle third: tone data keeps streaming but the DAC is muted.
             * A channel that loses the 1 kHz here follows the analog output;
             * one that keeps it is a digital tap. */
            esp_codec_dev_set_out_mute(s_out, true);
            ESP_LOGI(TAG, "DAC MUTED");
        }
        double sum[4] = {0}, sumsq[4] = {0};
        int peak[4] = {0};
        int reads = 0;
        while (reads < WINDOW_READS) {
            ret = esp_codec_dev_read(s_in, buf, sizeof(buf));
            if (ret != ESP_CODEC_DEV_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            for (int ch = 0; ch < 4; ++ch) {
                for (int n = 0; n < FRAMES; ++n) {
                    int v = buf[n * 4 + ch];
                    sum[ch] += v;
                    sumsq[ch] += (double)v * v;
                    int a = v < 0 ? -v : v;
                    if (a > peak[ch]) peak[ch] = a;
                }
            }
            ++reads;
        }
        const double n = (double)reads * FRAMES;
        if (!TEST_NOISE && (w == 2 || w == 6)) {
            scan_frequency(buf, w);
        }
        /* Steady-state correlation on the last 20 ms block. A hardware
         * reference should be a clean, high-correlation copy of the tone. */
        double tone_energy = 0;
        for (int k = 0; k < FRAMES; ++k) {
            tone_energy += (double)s_ref[k] * s_ref[k];
        }
        double best[4] = {0, 0, 0, 0};
        int best_lag[4] = {0, 0, 0, 0};
        for (int ch = 0; ch < 4; ++ch) {
            for (int lag = -64; lag <= 64; ++lag) {
                double dot = 0, energy = 0;
                for (int k = 0; k < FRAMES; ++k) {
                    int j = k + lag;
                    if (j < 0 || j >= FRAMES) continue;
                    double v = buf[k * 4 + ch];
                    dot += v * s_ref[j];
                    energy += v * v;
                }
                if (energy > 0) {
                    double c = fabs(dot / sqrt(energy * tone_energy));
                    if (c > best[ch]) { best[ch] = c; best_lag[ch] = lag; }
                }
            }
        }
        ESP_LOGI(TAG, "w=%02d t=%.1fs ch0=%4.0f/%4d ch1=%4.0f/%4d ch2=%4.0f/%4d ch3=%4.0f/%4d c=%1.2f,%1.2f,%1.2f,%1.2f",
                 w, (w + 1) * 0.5,
                 sqrt(sumsq[0] / n - (sum[0] / n) * (sum[0] / n)), peak[0],
                 sqrt(sumsq[1] / n - (sum[1] / n) * (sum[1] / n)), peak[1],
                 sqrt(sumsq[2] / n - (sum[2] / n) * (sum[2] / n)), peak[2],
                 sqrt(sumsq[3] / n - (sum[3] / n) * (sum[3] / n)), peak[3],
                 best[0], best[1], best[2], best[3]);
    }
    ESP_LOGI(TAG, "PROBE DONE");
}
