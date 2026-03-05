/**
 * @file microphone.h
 * @brief INMP441 I2S microphone API.
 *
 * Configures I2S in standard (Philips) mode to receive 32-bit frames from
 * the INMP441 at 16 kHz.  A background task reads DMA buffers, computes RMS
 * amplitude and logs the result periodically.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the I2S peripheral and start the audio sampling task.
 * @return ESP_OK on success.
 */
esp_err_t microphone_init(void);

/**
 * @brief Return the most recent RMS amplitude (0 – 32767 scale).
 *        Updated every CFG_I2S_DMA_BUF_LEN samples by the audio task.
 */
uint32_t microphone_get_rms(void);

#ifdef __cplusplus
}
#endif
