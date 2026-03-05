/**
 * @file pir.c
 * @brief HC-SR501 PIR motion sensor driver.
 *
 * The HC-SR501 pulls its output HIGH for 3–5 s whenever motion is detected
 * (adjustable via the onboard trim pots).  We install a rising-edge GPIO
 * interrupt and record the timestamp.  A debounce window prevents repeated
 * log spam during sustained motion.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "pir.h"
#include "app_config.h"
#include "app_log.h"

static const char *TAG = "PIR";

/* ── State ───────────────────────────────────────────────────────────── */
static volatile uint64_t s_last_event_ms = 0;  /* ms since boot of last trigger */
static volatile uint64_t s_last_logged_ms = 0; /* ms since boot of last log line */

/* ── ISR ─────────────────────────────────────────────────────────────── */

static void IRAM_ATTR pir_isr_handler(void *arg)
{
    (void)arg;
    s_last_event_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
}

/* ── Monitor task ────────────────────────────────────────────────────── */

static void pir_task(void *arg)
{
    ESP_LOGI(TAG, "PIR monitor task started (GPIO%d, debounce=%d ms)",
             CFG_PIR_GPIO, CFG_PIR_DEBOUNCE_MS);

    bool prev_motion = false;

    while (true) {
        bool motion = pir_motion_detected();

        if (motion && !prev_motion) {
            /* Rising edge of motion window */
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            if ((now_ms - s_last_logged_ms) >= CFG_PIR_DEBOUNCE_MS) {
                app_log_write(APP_LOG_INFO, TAG,
                              "Motion DETECTED at T+%llu ms", now_ms);
                s_last_logged_ms = now_ms;
            }
        } else if (!motion && prev_motion) {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            app_log_write(APP_LOG_DEBUG, TAG, "Motion CLEARED at T+%llu ms", now_ms);
        }

        prev_motion = motion;
        vTaskDelay(pdMS_TO_TICKS(200)); /* 200 ms polling interval */
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t pir_init(void)
{
    ESP_LOGI(TAG, "Initialising PIR sensor on GPIO%d", CFG_PIR_GPIO);

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CFG_PIR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  /* keep LOW when idle */
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    /* Install ISR service (if not already installed) */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(CFG_PIR_GPIO, pir_isr_handler, NULL));

    /* Start monitoring task */
    BaseType_t task_ret = xTaskCreate(pir_task, "pir", 3072, NULL, 4, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PIR task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PIR sensor ready — waiting for motion...");
    return ESP_OK;
}

bool pir_motion_detected(void)
{
    if (s_last_event_ms == 0) return false;
    uint64_t now_ms    = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uint64_t elapsed   = now_ms - s_last_event_ms;
    return elapsed < (uint64_t)CFG_PIR_DEBOUNCE_MS;
}

uint64_t pir_last_event_ms(void)
{
    return s_last_event_ms;
}
