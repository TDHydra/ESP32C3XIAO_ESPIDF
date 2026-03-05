/**
 * @file webserver.h
 * @brief HTTP server with embedded dashboard and SSE live-log stream.
 *
 * Endpoints:
 *   GET /          — HTML monitoring dashboard (auto-updates via SSE)
 *   GET /events    — Server-Sent Events stream (log + status events)
 *   GET /status    — JSON snapshot of current system state
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP server.
 *        Must be called after wifi_init_ap().
 * @return ESP_OK on success.
 */
esp_err_t webserver_start(void);

#ifdef __cplusplus
}
#endif
