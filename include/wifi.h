/**
 * @file wifi.h
 * @brief WiFi Access Point initialisation.
 *
 * Creates a softAP named CFG_WIFI_AP_SSID.  No router required —
 * connect directly from a phone or laptop then browse to http://192.168.4.1
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise NVS, TCP/IP stack, event loop and WiFi softAP.
 * @return ESP_OK on success.
 */
esp_err_t wifi_init_ap(void);

#ifdef __cplusplus
}
#endif
