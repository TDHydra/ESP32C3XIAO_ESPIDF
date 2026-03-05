/**
 * @file main.c
 * @brief Application entry point — ESP32-C3 XIAO environmental monitor.
 *
 * Subsystem initialisation order
 * ──────────────────────────────
 *  1. app_log      — ring buffer + vprintf hook (captures all ESP_LOGx output)
 *  2. wifi_init_ap — NVS + netif + softAP
 *  3. webserver    — HTTP server + SSE log stream
 *  4. battery      — ADC voltage monitor (30 s period)
 *  5. pir          — HC-SR501 GPIO interrupt + debounce task
 *  6. microphone   — INMP441 I2S receiver + RMS task
 *
 * Dashboard:  http://192.168.4.1  (connect to "ESP32C3-Monitor" AP first)
 * Serial log: 115200 baud
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "app_log.h"
#include "wifi.h"
#include "webserver.h"
#include "battery.h"
#include "pir.h"
#include "microphone.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* ── 1. Log subsystem (must be first) ─────────────────────────── */
    ESP_ERROR_CHECK(app_log_init());

    app_log_write(APP_LOG_INFO, TAG, "=== ESP32-C3 XIAO Monitor booting ===");
    app_log_write(APP_LOG_INFO, TAG,
        "Firmware built: " __DATE__ " " __TIME__);

    /* ── 2. WiFi SoftAP ───────────────────────────────────────────── */
    app_log_write(APP_LOG_INFO, TAG, "Starting WiFi SoftAP...");
    ESP_ERROR_CHECK(wifi_init_ap());

    /* ── 3. HTTP web server ───────────────────────────────────────── */
    app_log_write(APP_LOG_INFO, TAG, "Starting web server...");
    ESP_ERROR_CHECK(webserver_start());

    /* ── 4. Battery monitor ───────────────────────────────────────── */
    app_log_write(APP_LOG_INFO, TAG, "Starting battery monitor...");
    esp_err_t batt_err = battery_init();
    if (batt_err != ESP_OK) {
        app_log_write(APP_LOG_WARN, TAG,
            "Battery init failed (%d) — continuing without monitoring", batt_err);
    }

    /* ── 5. PIR motion sensor ─────────────────────────────────────── */
    app_log_write(APP_LOG_INFO, TAG, "Starting PIR sensor...");
    esp_err_t pir_err = pir_init();
    if (pir_err != ESP_OK) {
        app_log_write(APP_LOG_WARN, TAG,
            "PIR init failed (%d) — continuing without motion detection", pir_err);
    }

    /* ── 6. INMP441 microphone ────────────────────────────────────── */
    app_log_write(APP_LOG_INFO, TAG, "Starting INMP441 microphone...");
    esp_err_t mic_err = microphone_init();
    if (mic_err != ESP_OK) {
        app_log_write(APP_LOG_WARN, TAG,
            "Microphone init failed (%d) — continuing without audio", mic_err);
    }

    app_log_write(APP_LOG_INFO, TAG,
        "All subsystems ready.  Open http://192.168.4.1 in a browser.");

    /* ── Idle loop: periodic heartbeat to web log ─────────────────── */
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000)); /* 60-second heartbeat */
        tick++;
        app_log_write(APP_LOG_DEBUG, TAG,
            "Heartbeat #%lu — heap free: %lu B",
            (unsigned long)tick,
            (unsigned long)esp_get_free_heap_size());
    }
}