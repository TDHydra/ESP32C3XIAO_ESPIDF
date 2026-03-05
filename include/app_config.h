/**
 * @file app_config.h
 * @brief Hardware pin assignments and application-wide tuning constants.
 *
 * Seeed XIAO ESP32-C3 wiring guide
 * ─────────────────────────────────────────────────────────────────────────
 * Component          Board label   ESP32-C3 GPIO   Notes
 * ─────────────────  ──────────    ─────────────   ──────────────────────
 * Battery voltage    A1 / D1       GPIO3           Onboard 2:1 divider
 * HC-SR501 OUT       D0 / A0       GPIO2           3.3 V logic output
 * INMP441 SCK        D4 / SDA      GPIO6           I2S bit-clock
 * INMP441 WS         D5 / SCL      GPIO7           I2S word-select (LRCK)
 * INMP441 SD         D2            GPIO4           I2S serial data
 * INMP441 VDD                      3V3             INMP441 requires 1.8–3.3 V
 * INMP441 GND                      GND
 * INMP441 L/R                      GND             Left channel selection
 * ─────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Battery ADC (ADC1, GPIO3)
 * GPIO3 = ADC1 channel 3 on ESP32-C3
 * The XIAO ESP32-C3 board routes the battery + terminal through a
 * 200 kΩ / 200 kΩ voltage divider to A1/GPIO3 so the ADC never
 * sees more than ~2.1 V at full charge.
 * Adjust CFG_BATTERY_VDIV_RATIO if your board uses a different ratio.
 * ================================================================ */
#define CFG_BATTERY_ADC_UNIT        ADC_UNIT_1
#define CFG_BATTERY_ADC_CHANNEL     ADC_CHANNEL_3   /**< GPIO3 = ADC1_CH3 */
#define CFG_BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12 /**< 0 – ~3.1 V input range */
#define CFG_BATTERY_ADC_SAMPLES     64              /**< Oversamples averaged per reading */
#define CFG_BATTERY_VDIV_RATIO      2.0f            /**< Divide-down ratio (2:1 onboard) — documentation */
#define CFG_BATTERY_VDIV_NUM        2               /**< Integer numerator  of divider ratio */
#define CFG_BATTERY_VDIV_DEN        1               /**< Integer denominator of divider ratio */
#define CFG_BATTERY_CHECK_MS        30000           /**< Measurement period (ms) */

/** Li-ion cell voltage thresholds — identical for 18650 and 21700 */#define CFG_BATTERY_FULL_MV         4200            /**< 100 % */
#define CFG_BATTERY_HIGH_MV         3900            /**< ~75 % */
#define CFG_BATTERY_MED_MV          3700            /**< ~50 % */
#define CFG_BATTERY_LOW_MV          3500            /**< ~25 % — LOW warning */
#define CFG_BATTERY_CRITICAL_MV     3200            /**< ~5 %  — CRITICAL alert */
#define CFG_BATTERY_EMPTY_MV        3000            /**<  0 %  — emergency shutdown */

/* ================================================================
 * HC-SR501 PIR motion sensor (GPIO2)
 * ================================================================ */
#define CFG_PIR_GPIO                GPIO_NUM_2      /**< HC-SR501 signal output */
#define CFG_PIR_DEBOUNCE_MS         2000            /**< Ignore re-triggers within 2 s */

/* ================================================================
 * INMP441 I2S microphone
 * ================================================================ */
#define CFG_I2S_PORT                I2S_NUM_0
#define CFG_I2S_BCK_GPIO            GPIO_NUM_6      /**< Bit-clock  (D4/SDA) */
#define CFG_I2S_WS_GPIO             GPIO_NUM_7      /**< Word-select (D5/SCL) */
#define CFG_I2S_DATA_GPIO           GPIO_NUM_4      /**< Serial data (D2)    */
#define CFG_I2S_SAMPLE_RATE         16000           /**< Hz */
#define CFG_I2S_DMA_BUF_LEN        1024            /**< Samples per DMA buffer */
#define CFG_I2S_DMA_BUF_COUNT      4               /**< Number of DMA buffers */
#define CFG_AUDIO_HIGH_THRESHOLD    8000            /**< RMS above this → LOUD alert */
#define CFG_AUDIO_LOG_INTERVAL_MS   5000            /**< Periodic audio-level log (ms) */

/* ================================================================
 * WiFi — Access Point (no router required)
 * Connect to "ESP32C3-Monitor" then browse to http://192.168.4.1
 * ================================================================ */
#define CFG_WIFI_AP_SSID            "ESP32C3-Monitor"
#define CFG_WIFI_AP_PASS            "monitor123"    /**< ≥ 8 characters */
#define CFG_WIFI_AP_CHANNEL         6
#define CFG_WIFI_AP_MAX_CONN        4

/* ================================================================
 * Web / HTTP server
 * ================================================================ */
#define CFG_WEBSERVER_PORT          80
/** SSE poll granularity — handler wakes every this many ms */
#define CFG_SSE_POLL_MS             100
/** Send status event (doubles as SSE keepalive) after this many idle polls */
#define CFG_SSE_KEEPALIVE_POLLS     50   /* 50 × 100 ms = 5 s */

/* ================================================================
 * Application log ring buffer
 * ================================================================ */
#define CFG_LOG_RING_SIZE           200  /**< Entries retained in memory */
#define CFG_LOG_MSG_MAX             192  /**< Max message bytes (incl. NUL) */
#define CFG_LOG_TAG_MAX             16   /**< Max tag bytes (incl. NUL) */

#ifdef __cplusplus
}
#endif
