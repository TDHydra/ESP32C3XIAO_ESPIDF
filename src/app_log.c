/**
 * @file app_log.c
 * @brief Ring-buffer log implementation and ESP-IDF vprintf hook.
 *
 * Every call to ESP_LOGx() passes through our custom vprintf handler.
 * We forward the raw text to UART (so serial monitors still work), then
 * parse the ESP-IDF log format "L (T) TAG: message\n" and store a
 * structured entry in the ring buffer.
 *
 * SSE handlers use app_log_next() with a per-connection cursor to replay
 * history and then stream live entries.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_log.h"
#include "app_config.h"

static const char *TAG = "APP_LOG";

/* ── Ring buffer state ──────────────────────────────────────────────── */
static app_log_entry_t s_ring[CFG_LOG_RING_SIZE];
static uint32_t        s_write_pos   = 0;   /* index of next write slot  */
static uint32_t        s_total       = 0;   /* entries ever written      */
static SemaphoreHandle_t s_mutex     = NULL;

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** Map a single ESP-IDF log level character to our enum. */
static app_log_level_t char_to_level(char c)
{
    switch (c) {
        case 'V': return APP_LOG_VERBOSE;
        case 'D': return APP_LOG_DEBUG;
        case 'I': return APP_LOG_INFO;
        case 'W': return APP_LOG_WARN;
        case 'E': return APP_LOG_ERROR;
        default:  return APP_LOG_INFO;
    }
}

/**
 * Parse an ESP-IDF formatted log line.
 * Format: "L (TICK) TAG: message\n"
 * e.g.   "I (1234) WIFI: Station connected\n"
 *
 * On parse failure the raw line is stored verbatim in msg with tag="SYS".
 */
static void parse_idf_line(const char *raw,
                            app_log_level_t *level,
                            char *tag,  size_t tag_len,
                            char *msg,  size_t msg_len)
{
    /* Default values */
    *level = APP_LOG_INFO;
    strncpy(tag, "SYS", tag_len - 1);
    tag[tag_len - 1] = '\0';

    /* Strip trailing newline for storage */
    strncpy(msg, raw, msg_len - 1);
    msg[msg_len - 1] = '\0';
    size_t raw_len = strlen(msg);
    if (raw_len > 0 && msg[raw_len - 1] == '\n') msg[raw_len - 1] = '\0';

    /* Expect at least "L (" */
    if (strlen(raw) < 4 || raw[1] != ' ' || raw[2] != '(') return;

    *level = char_to_level(raw[0]);

    /* Find closing ')' */
    const char *p = strchr(raw + 3, ')');
    if (!p) return;
    p += 2; /* skip ') ' */

    /* Find ':' separating tag from message */
    const char *colon = strchr(p, ':');
    if (!colon) return;

    size_t tag_copy = (size_t)(colon - p);
    if (tag_copy >= tag_len) tag_copy = tag_len - 1;
    strncpy(tag, p, tag_copy);
    tag[tag_copy] = '\0';

    /* Message follows ': ' */
    const char *msg_start = colon + 2;
    strncpy(msg, msg_start, msg_len - 1);
    msg[msg_len - 1] = '\0';
    size_t mlen = strlen(msg);
    if (mlen > 0 && msg[mlen - 1] == '\n') msg[mlen - 1] = '\0';
}

/** Store a pre-parsed entry in the ring buffer (called with mutex held). */
static void ring_push(app_log_level_t level, const char *tag, const char *msg)
{
    app_log_entry_t *e = &s_ring[s_write_pos];
    e->id    = s_total;
    e->ts_s  = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    e->level = level;
    strncpy(e->tag, tag, CFG_LOG_TAG_MAX - 1);
    e->tag[CFG_LOG_TAG_MAX - 1] = '\0';
    strncpy(e->msg, msg, CFG_LOG_MSG_MAX - 1);
    e->msg[CFG_LOG_MSG_MAX - 1] = '\0';

    s_write_pos = (s_write_pos + 1) % CFG_LOG_RING_SIZE;
    s_total++;
}

/* ── Custom vprintf hook ─────────────────────────────────────────────── */

static int app_log_vprintf(const char *fmt, va_list args)
{
    /* 1. Keep writing to UART so serial monitors still work */
    va_list args_uart;
    va_copy(args_uart, args);
    int ret = vprintf(fmt, args_uart);
    va_end(args_uart);

    /* 2. Under mutex: render into a static buffer then parse and store.
     *    Using a static buffer avoids a large on-stack allocation for every
     *    log call.  The mutex ensures the buffer is safe across tasks. */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        static char raw[CFG_LOG_MSG_MAX * 2]; /* protected by mutex */
        vsnprintf(raw, sizeof(raw), fmt, args);

        app_log_level_t level;
        char tag[CFG_LOG_TAG_MAX];
        char msg[CFG_LOG_MSG_MAX];
        parse_idf_line(raw, &level, tag, sizeof(tag), msg, sizeof(msg));
        ring_push(level, tag, msg);
        xSemaphoreGive(s_mutex);
    }

    return ret;
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t app_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Install hook — all subsequent ESP_LOGx calls go through us */
    esp_log_set_vprintf(app_log_vprintf);

    ESP_LOGI(TAG, "Log ring buffer initialised (%d entries)", CFG_LOG_RING_SIZE);
    return ESP_OK;
}

void app_log_write(app_log_level_t level, const char *tag,
                   const char *fmt, ...)
{
    char msg[CFG_LOG_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Also emit through the normal ESP-IDF log so UART captures it */
    switch (level) {
        case APP_LOG_VERBOSE:  ESP_LOGV(tag, "%s", msg); break;
        case APP_LOG_DEBUG:    ESP_LOGD(tag, "%s", msg); break;
        case APP_LOG_WARN:     ESP_LOGW(tag, "%s", msg); break;
        case APP_LOG_ERROR:    ESP_LOGE(tag, "%s", msg); break;
        case APP_LOG_CRITICAL: ESP_LOGE(tag, "CRITICAL: %s", msg); break;
        default:               ESP_LOGI(tag, "%s", msg); break;
    }
    /* The hook above will capture this; no double-insert needed */
}

uint32_t app_log_oldest_id(void)
{
    uint32_t oldest = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        oldest = (s_total > CFG_LOG_RING_SIZE) ? (s_total - CFG_LOG_RING_SIZE) : 0;
        xSemaphoreGive(s_mutex);
    }
    return oldest;
}

bool app_log_next(uint32_t *cursor_id, app_log_entry_t *out)
{
    if (!s_mutex || !cursor_id || !out) return false;

    bool found = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint32_t oldest = (s_total > CFG_LOG_RING_SIZE)
                          ? (s_total - CFG_LOG_RING_SIZE) : 0;

        /* If cursor has fallen behind the oldest retained entry, catch up */
        if (*cursor_id < oldest) *cursor_id = oldest;

        if (*cursor_id < s_total) {
            /* The entry with ID=cursor lives at ring index cursor%RING_SIZE */
            uint32_t idx = (*cursor_id) % CFG_LOG_RING_SIZE;
            *out = s_ring[idx];
            (*cursor_id)++;
            found = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return found;
}

uint32_t app_log_total(void)
{
    uint32_t t = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        t = s_total;
        xSemaphoreGive(s_mutex);
    }
    return t;
}
