/**
 * @file battery.h
 * @brief Battery voltage monitoring API.
 *
 * Reads the Li-ion cell voltage via an onboard ADC voltage divider and
 * classifies it into named states.  Works with 18650 and 21700 cells —
 * both share the same 3.0 – 4.2 V window.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Named battery charge states. */
typedef enum {
    BATTERY_STATE_FULL     = 0, /**< ≥ 4.20 V  (~100 %)  */
    BATTERY_STATE_HIGH     = 1, /**< ≥ 3.90 V  (~75 %)   */
    BATTERY_STATE_MEDIUM   = 2, /**< ≥ 3.70 V  (~50 %)   */
    BATTERY_STATE_LOW      = 3, /**< ≥ 3.50 V  (~25 %) ⚠ */
    BATTERY_STATE_CRITICAL = 4, /**< ≥ 3.20 V  (~5 %)  ✘ */
    BATTERY_STATE_EMPTY    = 5, /**< < 3.20 V  (0 %)   ☠ */
    BATTERY_STATE_UNKNOWN  = 6, /**< ADC not yet read      */
} battery_state_t;

/** Snapshot returned by battery_get_info(). */
typedef struct {
    uint32_t       voltage_mv;   /**< Measured cell voltage in mV */
    uint8_t        percent;      /**< Estimated SOC 0–100 %       */
    battery_state_t state;
    const char    *state_str;    /**< Human-readable state label  */
} battery_info_t;

/**
 * @brief Initialise ADC unit, configure channel and calibration.
 *        Starts a periodic monitoring task that logs warnings when the
 *        battery enters LOW or CRITICAL state.
 * @return ESP_OK on success.
 */
esp_err_t battery_init(void);

/**
 * @brief Return the most recent battery reading (updated every
 *        CFG_BATTERY_CHECK_MS milliseconds by the monitor task).
 */
battery_info_t battery_get_info(void);

#ifdef __cplusplus
}
#endif
