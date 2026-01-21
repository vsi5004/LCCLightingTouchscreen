/**
 * @file bootloader_display.h
 * @brief Minimal LCD display for bootloader status
 * 
 * Provides simple status display during firmware updates without
 * requiring the full LVGL stack. Uses direct framebuffer rendering.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bootloader display status states
 */
typedef enum {
    BOOTLOADER_STATUS_WAITING,      ///< Waiting for firmware data
    BOOTLOADER_STATUS_RECEIVING,    ///< Receiving firmware
    BOOTLOADER_STATUS_WRITING,      ///< Writing to flash
    BOOTLOADER_STATUS_VERIFYING,    ///< Verifying firmware
    BOOTLOADER_STATUS_SUCCESS,      ///< Update successful
    BOOTLOADER_STATUS_ERROR,        ///< Error occurred
    BOOTLOADER_STATUS_CHECKSUM_ERR, ///< Checksum error
    BOOTLOADER_STATUS_FRAME_LOST,   ///< CAN frame lost
} bootloader_display_status_t;

/**
 * @brief Initialize the bootloader display
 * 
 * Sets up minimal LCD hardware for status display.
 * Only I2C, CH422G, and LCD panel are initialized.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t bootloader_display_init(void);

/**
 * @brief Deinitialize the bootloader display
 * 
 * Releases LCD resources before continuing to bootloader.
 */
void bootloader_display_deinit(void);

/**
 * @brief Update the bootloader status display
 * 
 * @param status Current bootloader status
 * @param progress Progress percentage (0-100) for RECEIVING/WRITING states
 */
void bootloader_display_update(bootloader_display_status_t status, int progress);

/**
 * @brief Show a custom message on the bootloader display
 * 
 * @param line1 First line of text (max 40 chars)
 * @param line2 Second line of text (max 40 chars), or NULL
 */
void bootloader_display_message(const char *line1, const char *line2);

#ifdef __cplusplus
}
#endif
