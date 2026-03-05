/**
 * @file battery.c
 * @brief Li-ion battery voltage monitoring via ADC.
 *
 * Uses ESP-IDF 5.x ADC oneshot driver with curve-fitting calibration
 * (falls back to line-fitting if eFuse data is unavailable).
 *
 * The onboard 2:1 voltage divider means the ADC pin sees ≤ 2.1 V even
 * at full charge — well within the DB_12 attenuation range of ~3.1 V.
 *
 * State machine:
 *   FULL → HIGH → MEDIUM → LOW (warn) → CRITICAL (alert) → EMPTY (shutdown)
 */
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "battery.h"
#include "app_config.h"
#include "app_log.h"

static const char *TAG = "BATTERY";

/* ── Module state ────────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t          s_cali_handle = NULL;
static bool                       s_cali_valid  = false;
static battery_info_t             s_info        = { .state = BATTERY_STATE_UNKNOWN,
                                                      .state_str = "UNKNOWN" };

/* ── Helpers ─────────────────────────────────────────────────────────── */

static const char *state_to_str(battery_state_t s)
{
    switch (s) {
        case BATTERY_STATE_FULL:     return "FULL";
        case BATTERY_STATE_HIGH:     return "HIGH";
        case BATTERY_STATE_MEDIUM:   return "MEDIUM";
        case BATTERY_STATE_LOW:      return "LOW";
        case BATTERY_STATE_CRITICAL: return "CRITICAL";
        case BATTERY_STATE_EMPTY:    return "EMPTY";
        default:                     return "UNKNOWN";
    }
}

static battery_state_t mv_to_state(uint32_t mv)
{
    if (mv >= CFG_BATTERY_FULL_MV)     return BATTERY_STATE_FULL;
    if (mv >= CFG_BATTERY_HIGH_MV)     return BATTERY_STATE_HIGH;
    if (mv >= CFG_BATTERY_MED_MV)      return BATTERY_STATE_MEDIUM;
    if (mv >= CFG_BATTERY_LOW_MV)      return BATTERY_STATE_LOW;
    if (mv >= CFG_BATTERY_CRITICAL_MV) return BATTERY_STATE_CRITICAL;
    return BATTERY_STATE_EMPTY;
}

/** Linear interpolation: map voltage to 0-100% SOC. */
static uint8_t mv_to_percent(uint32_t mv)
{
    if (mv >= CFG_BATTERY_FULL_MV)  return 100;
    if (mv <= CFG_BATTERY_EMPTY_MV) return 0;
    uint32_t range = CFG_BATTERY_FULL_MV - CFG_BATTERY_EMPTY_MV;
    uint32_t above = mv - CFG_BATTERY_EMPTY_MV;
    return (uint8_t)((above * 100) / range);
}

/** Read CFG_BATTERY_ADC_SAMPLES raw values and return averaged millivolts. */
static uint32_t read_battery_mv(void)
{
    if (!s_adc_handle) return 0;

    int64_t sum = 0;
    int     valid = 0;
    for (int i = 0; i < CFG_BATTERY_ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, CFG_BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            valid++;
        }
    }
    if (valid == 0) return 0;

    int raw_avg = (int)(sum / valid);
    int voltage_mv = 0;

    if (s_cali_valid) {
        adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &voltage_mv);
    } else {
        /* Fallback: linear approximation (12-bit, ~3100 mV full scale at DB_12) */
        voltage_mv = (int)((raw_avg * 3100) / 4095);
    }

    /* Undo the onboard voltage divider using integer arithmetic.
     * CFG_BATTERY_VDIV_NUM/DEN encode the ratio (e.g. 2/1 for a 2:1 divider). */
    return (uint32_t)((uint32_t)voltage_mv * CFG_BATTERY_VDIV_NUM
                      / CFG_BATTERY_VDIV_DEN);
}

/* ── Monitor task ────────────────────────────────────────────────────── */

static void battery_task(void *arg)
{
    ESP_LOGI(TAG, "Battery monitor task started (period=%d ms)", CFG_BATTERY_CHECK_MS);

    battery_state_t prev_state = BATTERY_STATE_UNKNOWN;

    while (true) {
        uint32_t mv = read_battery_mv();

        if (mv > 0) {
            battery_state_t state   = mv_to_state(mv);
            uint8_t         percent = mv_to_percent(mv);

            s_info.voltage_mv = mv;
            s_info.percent    = percent;
            s_info.state      = state;
            s_info.state_str  = state_to_str(state);

            ESP_LOGI(TAG, "Battery: %lu mV  %u%%  [%s]",
                     (unsigned long)mv, percent, state_to_str(state));

            /* Log transitions and alerts */
            if (state != prev_state) {
                if (state == BATTERY_STATE_LOW) {
                    app_log_write(APP_LOG_WARN, TAG,
                        "LOW BATTERY: %lu mV (%u%%) — please recharge soon",
                        (unsigned long)mv, percent);
                } else if (state == BATTERY_STATE_CRITICAL) {
                    app_log_write(APP_LOG_CRITICAL, TAG,
                        "CRITICAL BATTERY: %lu mV (%u%%) — recharge NOW",
                        (unsigned long)mv, percent);
                } else if (state == BATTERY_STATE_EMPTY) {
                    app_log_write(APP_LOG_CRITICAL, TAG,
                        "BATTERY EMPTY: %lu mV — system may shutdown",
                        (unsigned long)mv);
                }
                prev_state = state;
            }
        } else {
            ESP_LOGW(TAG, "ADC read failed — battery voltage unavailable");
        }

        vTaskDelay(pdMS_TO_TICKS(CFG_BATTERY_CHECK_MS));
    }
}

/* ── Calibration setup ───────────────────────────────────────────────── */

static void calibration_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = CFG_BATTERY_ADC_UNIT,
        .chan     = CFG_BATTERY_ADC_CHANNEL,
        .atten   = CFG_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_valid = true;
        ESP_LOGI(TAG, "ADC calibration: curve-fitting scheme");
        return;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id   = CFG_BATTERY_ADC_UNIT,
        .atten     = CFG_BATTERY_ADC_ATTEN,
        .bitwidth  = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_valid = true;
        ESP_LOGI(TAG, "ADC calibration: line-fitting scheme");
        return;
    }
#endif

    ESP_LOGW(TAG, "ADC calibration not available — using raw approximation");
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t battery_init(void)
{
    ESP_LOGI(TAG, "Initialising battery monitor");
    ESP_LOGI(TAG, "  ADC unit=%d  channel=%d  atten=%d",
             CFG_BATTERY_ADC_UNIT, CFG_BATTERY_ADC_CHANNEL, CFG_BATTERY_ADC_ATTEN);
    ESP_LOGI(TAG, "  Voltage divider ratio: %d:%d",
             CFG_BATTERY_VDIV_NUM, CFG_BATTERY_VDIV_DEN);
    ESP_LOGI(TAG, "  Thresholds: LOW=%d mV  CRIT=%d mV",
             CFG_BATTERY_LOW_MV, CFG_BATTERY_CRITICAL_MV);

    /* Configure ADC unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = CFG_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    /* Configure ADC channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = CFG_BATTERY_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle,
                                               CFG_BATTERY_ADC_CHANNEL,
                                               &chan_cfg));

    calibration_init();

    /* Take an immediate reading so s_info is valid before the task runs */
    uint32_t mv = read_battery_mv();
    if (mv > 0) {
        s_info.voltage_mv = mv;
        s_info.percent    = mv_to_percent(mv);
        s_info.state      = mv_to_state(mv);
        s_info.state_str  = state_to_str(s_info.state);
        ESP_LOGI(TAG, "Initial reading: %lu mV  %u%%  [%s]",
                 (unsigned long)mv, s_info.percent, s_info.state_str);
    }

    /* Start periodic monitor task */
    BaseType_t ret = xTaskCreate(battery_task, "battery",
                                  4096, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

battery_info_t battery_get_info(void)
{
    return s_info;
}
