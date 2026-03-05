/**
 * @file microphone.c
 * @brief INMP441 I2S microphone driver (ESP-IDF 5.x new I2S API).
 *
 * The INMP441 outputs 24-bit audio in the MSB of each 32-bit I2S word
 * (Philips / standard left-justified mode).  We run a background task that
 * continuously drains the DMA ring, computes RMS amplitude and logs the
 * level periodically.  Motion-triggered audio events are also annotated.
 *
 * Wiring reminder (L/R pin to GND → left channel):
 *   SCK  → CFG_I2S_BCK_GPIO
 *   WS   → CFG_I2S_WS_GPIO
 *   SD   → CFG_I2S_DATA_GPIO
 *   VDD  → 3V3
 *   GND  → GND
 *   L/R  → GND
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "microphone.h"
#include "pir.h"
#include "app_config.h"
#include "app_log.h"

static const char *TAG = "MIC";

/* ── State ───────────────────────────────────────────────────────────── */
static i2s_chan_handle_t s_rx_handle     = NULL;
static volatile uint32_t s_rms_latest   = 0;

/* ── Audio task ──────────────────────────────────────────────────────── */

static void microphone_task(void *arg)
{
    ESP_LOGI(TAG, "Microphone task started (rate=%d Hz, buf=%d samples)",
             CFG_I2S_SAMPLE_RATE, CFG_I2S_DMA_BUF_LEN);

    /* Allocate DMA read buffer on heap to avoid stack overflow */
    int32_t *samples = (int32_t *)malloc(CFG_I2S_DMA_BUF_LEN * sizeof(int32_t));
    if (!samples) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer — task exiting");
        vTaskDelete(NULL);
        return;
    }

    uint64_t last_log_ms = 0;
    bool     prev_motion = false;

    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_handle, samples,
                                          CFG_I2S_DMA_BUF_LEN * sizeof(int32_t),
                                          &bytes_read, pdMS_TO_TICKS(200));

        if (err != ESP_OK || bytes_read == 0) {
            ESP_LOGD(TAG, "I2S read: no data (err=%d)", err);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int num_samples = (int)(bytes_read / sizeof(int32_t));

        /* Compute RMS.  INMP441 24-bit data is in bits [31:8] of each word. */
        int64_t sum_sq = 0;
        for (int i = 0; i < num_samples; i++) {
            /* Arithmetic right-shift preserves sign */
            int32_t s = samples[i] >> 8;
            sum_sq += (int64_t)s * s;
        }
        uint32_t rms = (uint32_t)sqrt((double)sum_sq / num_samples);
        s_rms_latest = rms;

        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        bool motion = pir_motion_detected();

        /* Log audio level at regular intervals */
        if ((now_ms - last_log_ms) >= CFG_AUDIO_LOG_INTERVAL_MS) {
            app_log_write(APP_LOG_DEBUG, TAG,
                          "Audio RMS=%lu  (motion=%s)",
                          (unsigned long)rms, motion ? "YES" : "no");
            last_log_ms = now_ms;
        }

        /* Warn on loud audio */
        if (rms > CFG_AUDIO_HIGH_THRESHOLD) {
            app_log_write(APP_LOG_WARN, TAG,
                          "LOUD audio detected: RMS=%lu (threshold=%d)",
                          (unsigned long)rms, CFG_AUDIO_HIGH_THRESHOLD);
        }

        /* Annotate audio level when motion starts */
        if (motion && !prev_motion) {
            app_log_write(APP_LOG_INFO, TAG,
                          "Audio at motion event: RMS=%lu", (unsigned long)rms);
        }

        prev_motion = motion;
    }

    free(samples);
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t microphone_init(void)
{
    ESP_LOGI(TAG, "Initialising INMP441 I2S microphone");
    ESP_LOGI(TAG, "  BCK=GPIO%d  WS=GPIO%d  DATA=GPIO%d  Rate=%d Hz",
             CFG_I2S_BCK_GPIO, CFG_I2S_WS_GPIO, CFG_I2S_DATA_GPIO,
             CFG_I2S_SAMPLE_RATE);

    /* 1. Create an RX-only channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CFG_I2S_PORT,
                                                             I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = CFG_I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = CFG_I2S_DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

    /* 2. Configure standard (Philips) mode */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CFG_I2S_SAMPLE_RATE),
        /* 32-bit slot, mono — INMP441 left channel (L/R=GND) */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk  = I2S_GPIO_UNUSED,
            .bclk  = CFG_I2S_BCK_GPIO,
            .ws    = CFG_I2S_WS_GPIO,
            .dout  = I2S_GPIO_UNUSED,
            .din   = CFG_I2S_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    /* 3. Start audio task (large stack for math + heap buffer) */
    BaseType_t ret = xTaskCreate(microphone_task, "microphone",
                                  8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create microphone task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "INMP441 microphone ready");
    return ESP_OK;
}

uint32_t microphone_get_rms(void)
{
    return s_rms_latest;
}
