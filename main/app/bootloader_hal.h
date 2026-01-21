/**
 * @file bootloader_hal.h
 * @brief LCC Firmware Upgrade Bootloader HAL Interface
 * 
 * Provides the C interface for the ESP32 bootloader HAL that enables
 * over-the-air firmware updates via the LCC Memory Configuration Protocol.
 * 
 * The bootloader runs as an alternate code path in the application that:
 * 1. Initializes minimal CAN communication
 * 2. Receives firmware data via LCC datagrams/streams
 * 3. Writes to the alternate OTA partition using ESP-IDF OTA APIs
 * 4. Reboots into the new firmware
 * 
 * @see docs/SPEC.md for firmware update requirements
 * @see docs/ARCHITECTURE.md §6 for bootloader design
 */

#ifndef BOOTLOADER_HAL_H_
#define BOOTLOADER_HAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize bootloader state
 * 
 * Must be called early in app_main() after determining reset reason.
 * Initializes RTC memory flag on power-on reset.
 * 
 * @param reset_reason ESP32 reset reason code
 */
void bootloader_hal_init(uint8_t reset_reason);

/**
 * @brief Check if bootloader mode was requested
 * 
 * @return true if bootloader mode should run instead of normal app
 */
bool bootloader_hal_should_enter(void);

/**
 * @brief Run the bootloader
 * 
 * Enters bootloader mode to receive firmware updates via LCC.
 * This function does not return - it reboots after completion.
 * 
 * @param node_id LCC Node ID for the bootloader
 * @param twai_rx_gpio GPIO pin for TWAI RX
 * @param twai_tx_gpio GPIO pin for TWAI TX
 */
void bootloader_hal_run(uint64_t node_id, int twai_rx_gpio, int twai_tx_gpio);

/**
 * @brief Request reboot into bootloader mode
 * 
 * Sets the RTC flag to enter bootloader on next boot and restarts.
 * Called when an LCC "enter bootloader" command is received.
 */
void bootloader_hal_request_reboot(void);

#ifdef __cplusplus
}
#endif

#endif // BOOTLOADER_HAL_H_
