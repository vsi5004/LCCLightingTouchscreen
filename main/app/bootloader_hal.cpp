/**
 * @file bootloader_hal.cpp
 * @brief LCC Firmware Upgrade Bootloader HAL Implementation
 * 
 * Implements the ESP32 bootloader HAL for over-the-air firmware updates
 * via the LCC Memory Configuration Protocol (Memory Space 0xEF).
 * 
 * This module wraps OpenMRN's Esp32BootloaderHal.hxx with application-specific
 * display callbacks for visual feedback during updates.
 * 
 * The bootloader uses ESP-IDF's OTA APIs to write firmware to the alternate
 * partition, enabling safe updates with automatic rollback on failure.
 * 
 * @see docs/SPEC.md FR-060 to FR-064 for firmware update requirements
 * @see docs/ARCHITECTURE.md §8 for bootloader integration
 */

#include "bootloader_hal.h"
#include "bootloader_display.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"

// For reset reason detection
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/rtc.h"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#include "esp32/rom/rtc.h"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#include "esp32s2/rom/rtc.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#include "esp32c3/rom/rtc.h"
#endif

static const char *TAG = "bootloader_hal";

// Track display state for LED callbacks
static bool s_display_initialized = false;
static int s_write_progress = 0;

// ============================================================================
// Include OpenMRN Bootloader headers to get BootloaderLed enum
// ============================================================================

// Include OpenMRN bootloader interface (defines BootloaderLed enum)
#include "openlcb/bootloader_hal.h"

// ============================================================================
// Bootloader LED Callbacks (required by Esp32BootloaderHal.hxx)
// ============================================================================

/**
 * @brief Control bootloader status indicators
 * 
 * Instead of LEDs (which this board doesn't have), we update the LCD
 * display to show the current bootloader status.
 * 
 * @param led LED indicator to control (from BootloaderLed enum)
 * @param value true = on, false = off
 */
void bootloader_led(enum BootloaderLed led, bool value)
{
    // Log state changes for serial debugging
    if (value) {
        switch (led)
        {
            case LED_ACTIVE:
                ESP_LOGD(TAG, "[Status] Bootloader active");
                if (s_display_initialized) {
                    bootloader_display_update(BOOTLOADER_STATUS_RECEIVING, s_write_progress);
                }
                break;
            case LED_WRITING:
                ESP_LOGI(TAG, "[Status] Writing flash...");
                if (s_display_initialized) {
                    // Estimate progress - increment by 1% each write call
                    s_write_progress += 1;
                    if (s_write_progress > 99) s_write_progress = 99;
                    bootloader_display_update(BOOTLOADER_STATUS_WRITING, s_write_progress);
                }
                break;
            case LED_IDENT:
                ESP_LOGD(TAG, "[Status] Identify");
                break;
            case LED_CSUM_ERROR:
                ESP_LOGW(TAG, "[Status] Checksum error!");
                if (s_display_initialized) {
                    bootloader_display_update(BOOTLOADER_STATUS_CHECKSUM_ERR, 0);
                }
                break;
            case LED_REQUEST:
                ESP_LOGD(TAG, "[Status] Request received");
                break;
            case LED_FRAME_LOST:
                ESP_LOGW(TAG, "[Status] Frame lost!");
                if (s_display_initialized) {
                    bootloader_display_update(BOOTLOADER_STATUS_FRAME_LOST, s_write_progress);
                }
                break;
            default:
                break;
        }
    }
}

// ============================================================================
// Required HAL Functions (called by OpenMRN bootloader)
// ============================================================================

/**
 * @brief Set hardware to a safe state
 * 
 * Called by the bootloader before starting. This function should disable
 * interrupts and set all outputs to safe states.
 * 
 * On the Waveshare board, we leave the LCD and touch as-is since
 * we're not using them during bootloader mode.
 */
void bootloader_hw_set_to_safe(void)
{
    ESP_LOGD(TAG, "Setting hardware to safe state");
    // The ESP32 OTA bootloader doesn't need to disable much -
    // the TWAI peripheral will be initialized by bootloader_hw_init()
    // and we don't need to manipulate any other GPIOs during update.
}

/**
 * @brief Check if hardware requests bootloader entry
 * 
 * This would typically check a GPIO pin (bootloader switch).
 * On this board, we only enter bootloader via the RTC flag set
 * by an LCC command, so this always returns false.
 * 
 * @return Always false (no hardware bootloader switch)
 */
bool request_bootloader(void)
{
    // We don't have a physical bootloader button on this board.
    // Bootloader entry is controlled only via the RTC memory flag
    // set by lcc_node_request_bootloader().
    return false;
}

// ============================================================================
// Include the OpenMRN ESP32 Bootloader HAL (after callbacks are defined)
// ============================================================================

#include "freertos_drivers/esp32/Esp32BootloaderHal.hxx"

// ============================================================================
// C Interface Implementation
// ============================================================================

extern "C" {

void bootloader_hal_init(uint8_t reset_reason)
{
    ESP_LOGI(TAG, "Initializing bootloader HAL (reset_reason=%d)", reset_reason);
    esp32_bootloader_init(reset_reason);
}

bool bootloader_hal_should_enter(void)
{
    // Check the RTC memory flag set by bootloader_hal_request_reboot()
    // The flag is defined in Esp32BootloaderHal.hxx
    extern uint32_t bootloader_request;
    const uint32_t RTC_BOOL_TRUE = 0x92e01a42;
    
    bool should_enter = (bootloader_request == RTC_BOOL_TRUE);
    if (should_enter) {
        ESP_LOGI(TAG, "Bootloader mode requested via RTC flag");
    }
    return should_enter;
}

void bootloader_hal_run(uint64_t node_id, int twai_rx_gpio, int twai_tx_gpio)
{
    ESP_LOGI(TAG, "Entering bootloader mode");
    ESP_LOGI(TAG, "  Node ID: %012llx", (unsigned long long)node_id);
    ESP_LOGI(TAG, "  TWAI RX: GPIO%d, TX: GPIO%d", twai_rx_gpio, twai_tx_gpio);
    
    // Initialize the display for visual feedback
    if (bootloader_display_init() == ESP_OK) {
        s_display_initialized = true;
        s_write_progress = 0;
        ESP_LOGI(TAG, "Bootloader display initialized");
    } else {
        ESP_LOGW(TAG, "Failed to initialize bootloader display, continuing without visual feedback");
        s_display_initialized = false;
    }
    
    // Log OTA partition info
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    
    if (running) {
        ESP_LOGI(TAG, "  Running from: %s", running->label);
    }
    if (next) {
        ESP_LOGI(TAG, "  Will update: %s", next->label);
    }
    
    // Show waiting status on display
    if (s_display_initialized) {
        bootloader_display_update(BOOTLOADER_STATUS_WAITING, 0);
    }
    
    // Run the bootloader (does not return - reboots after completion)
    esp32_bootloader_run(
        node_id,
        static_cast<gpio_num_t>(twai_rx_gpio),
        static_cast<gpio_num_t>(twai_tx_gpio),
        true  // reboot_on_exit
    );
    
    // Should never reach here, but just in case
    ESP_LOGE(TAG, "Bootloader returned unexpectedly, restarting...");
    esp_restart();
}

void bootloader_hal_request_reboot(void)
{
    ESP_LOGI(TAG, "Requesting reboot into bootloader mode...");
    
    // Set the RTC memory flag
    extern uint32_t bootloader_request;
    const uint32_t RTC_BOOL_TRUE = 0x92e01a42;
    bootloader_request = RTC_BOOL_TRUE;
    
    // Give time for any pending operations
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Restart into bootloader
    ESP_LOGI(TAG, "Restarting...");
    esp_restart();
}

} // extern "C"
