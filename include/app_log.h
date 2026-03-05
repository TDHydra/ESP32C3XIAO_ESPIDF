/**
 * @file app_log.h
 * @brief Application-level log ring buffer and web-stream API.
 *
 * Intercepts every ESP_LOGx() call via esp_log_set_vprintf(), parses the
 * ESP-IDF log line format and stores the result in a circular ring buffer.
 * The web-server SSE handler then streams entries to connected browsers.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Log severity levels (aligned with ESP-IDF numeric values). */
typedef enum {
    APP_LOG_VERBOSE  = 0,
    APP_LOG_DEBUG    = 1,
    APP_LOG_INFO     = 2,
    APP_LOG_WARN     = 3,
    APP_LOG_ERROR    = 4,
    APP_LOG_CRITICAL = 5,
} app_log_level_t;

/** One entry stored in the ring buffer. */
typedef struct {
    uint32_t        id;                     /**< Monotonically increasing ID */
    uint32_t        ts_s;                   /**< Seconds since boot */
    app_log_level_t level;
    char            tag[CFG_LOG_TAG_MAX];
    char            msg[CFG_LOG_MSG_MAX];
} app_log_entry_t;

/**
 * @brief Initialise the log subsystem.
 *        Installs the custom vprintf hook; must be called once before any
 *        other subsystem so their log output is captured.
 */
esp_err_t app_log_init(void);

/**
 * @brief Write a formatted message directly into the ring buffer.
 *        Safe to call from any task context (not ISR).
 */
void app_log_write(app_log_level_t level, const char *tag,
                   const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/**
 * @brief Return the ID of the oldest entry currently held in the buffer.
 *        Use this as the starting cursor when a new SSE client connects so it
 *        receives a replay of recent history.
 */
uint32_t app_log_oldest_id(void);

/**
 * @brief Fetch the next unread entry after *cursor_id.
 *
 * @param[in,out] cursor_id  On entry: last consumed ID.
 *                           On success: updated to the returned entry's ID + 1.
 * @param[out]    out        Caller-allocated buffer filled on success.
 * @return true  if an entry was returned, false if there are no new entries.
 */
bool app_log_next(uint32_t *cursor_id, app_log_entry_t *out);

/**
 * @brief Return the total number of entries ever written (monotone counter).
 */
uint32_t app_log_total(void);

#ifdef __cplusplus
}
#endif
