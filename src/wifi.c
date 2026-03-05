/**
 * @file wifi.c
 * @brief WiFi SoftAP initialisation.
 *
 * Creates an Access Point named CFG_WIFI_AP_SSID so the user can connect
 * directly (no external router needed) and browse to http://192.168.4.1
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "wifi.h"
#include "app_config.h"
#include "app_log.h"

static const char *TAG = "WIFI";

/* ── Event handler ───────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    switch (id) {
    case WIFI_EVENT_AP_START:
        app_log_write(APP_LOG_INFO, TAG,
            "SoftAP started: SSID=\"%s\"  CH=%d  max_conn=%d",
            CFG_WIFI_AP_SSID, CFG_WIFI_AP_CHANNEL, CFG_WIFI_AP_MAX_CONN);
        app_log_write(APP_LOG_INFO, TAG,
            "Dashboard URL: http://192.168.4.1");
        break;

    case WIFI_EVENT_AP_STOP:
        app_log_write(APP_LOG_WARN, TAG, "SoftAP stopped");
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        app_log_write(APP_LOG_INFO, TAG,
            "Client connected: MAC=" MACSTR "  AID=%d",
            MAC2STR(ev->mac), ev->aid);
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *ev =
            (wifi_event_ap_stadisconnected_t *)data;
        app_log_write(APP_LOG_INFO, TAG,
            "Client disconnected: MAC=" MACSTR "  AID=%d",
            MAC2STR(ev->mac), ev->aid);
        break;
    }

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t wifi_init_ap(void)
{
    ESP_LOGI(TAG, "Initialising WiFi SoftAP");

    /* NVS is required by the WiFi driver */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash issue (%d) — erasing and re-initialising", nvs_ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "Failed to create default AP netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    /* Build AP config */
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.ap.ssid,
            CFG_WIFI_AP_SSID,
            sizeof(wifi_cfg.ap.ssid) - 1);
    strncpy((char *)wifi_cfg.ap.password,
            CFG_WIFI_AP_PASS,
            sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.ssid_len      = (uint8_t)strlen(CFG_WIFI_AP_SSID);
    wifi_cfg.ap.channel       = CFG_WIFI_AP_CHANNEL;
    wifi_cfg.ap.max_connection = CFG_WIFI_AP_MAX_CONN;
    wifi_cfg.ap.authmode      = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP ready — SSID: \"%s\"  Pass: \"%s\"  IP: 192.168.4.1",
             CFG_WIFI_AP_SSID, CFG_WIFI_AP_PASS);
    return ESP_OK;
}
