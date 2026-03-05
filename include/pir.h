/**
 * @file pir.h
 * @brief HC-SR501 PIR motion sensor API.
 *
 * Installs a GPIO interrupt on the sensor output pin and debounces
 * consecutive triggers.  Motion state is readable at any time via
 * pir_motion_detected().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the PIR GPIO pin and install an edge-triggered interrupt.
 * @return ESP_OK on success.
 */
esp_err_t pir_init(void);

/**
 * @brief Return true if motion was detected within the last
 *        CFG_PIR_DEBOUNCE_MS milliseconds.
 */
bool pir_motion_detected(void);

/**
 * @brief Return the epoch-ms timestamp of the last detected motion event.
 *        Returns 0 if no motion has been seen yet.
 */
uint64_t pir_last_event_ms(void);

#ifdef __cplusplus
}
#endif
