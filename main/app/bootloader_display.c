/**
 * @file bootloader_display.c
 * @brief Minimal LCD display for bootloader status
 * 
 * This module provides a simple status display during OTA firmware updates.
 * It initializes only the minimum hardware needed (I2C, CH422G, LCD) and
 * draws directly to the framebuffer without using LVGL.
 * 
 * The display shows:
 * - A header indicating bootloader mode
 * - Current status (Waiting, Receiving, Writing, etc.)
 * - Progress bar for firmware transfer
 */

#include "bootloader_display.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include "waveshare_lcd.h"
#include "ch422g.h"

static const char *TAG = "bootloader_display";

// Display dimensions
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

// Colors in RGB565 format
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_BLUE      0x001F
#define COLOR_GREEN     0x07E0
#define COLOR_RED       0xF800
#define COLOR_YELLOW    0xFFE0
#define COLOR_DARK_GRAY 0x4208
#define COLOR_ORANGE    0xFD20

// Layout constants
#define HEADER_HEIGHT   60
#define STATUS_Y        200
#define PROGRESS_Y      300
#define PROGRESS_HEIGHT 40
#define PROGRESS_MARGIN 100

// Static handles
static esp_lcd_panel_handle_t s_panel = NULL;
static ch422g_handle_t s_ch422g = NULL;
static uint16_t *s_framebuffer = NULL;
static bool s_initialized = false;

// Simple 8x8 font bitmap (ASCII 32-126)
// Each character is 8 bytes, one per row, MSB = leftmost pixel
static const uint8_t font_8x8[][8] = {
    // Space (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00},
    // " (34)
    {0x6C, 0x6C, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x6C, 0xFE, 0x6C, 0x6C, 0xFE, 0x6C, 0x00, 0x00},
    // $ (36)
    {0x18, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x18, 0x00},
    // % (37)
    {0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00, 0x00},
    // & (38)
    {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00},
    // ' (39)
    {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ( (40)
    {0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00},
    // ) (41)
    {0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00},
    // * (42)
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    // + (43)
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00},
    // , (44)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30},
    // - (45)
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    // . (46)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00},
    // / (47)
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x00, 0x00},
    // 0 (48)
    {0x7C, 0xC6, 0xCE, 0xD6, 0xE6, 0xC6, 0x7C, 0x00},
    // 1 (49)
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    // 2 (50)
    {0x7C, 0xC6, 0x06, 0x1C, 0x70, 0xC6, 0xFE, 0x00},
    // 3 (51)
    {0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00},
    // 4 (52)
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00},
    // 5 (53)
    {0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00},
    // 6 (54)
    {0x38, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00},
    // 7 (55)
    {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},
    // 8 (56)
    {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00},
    // 9 (57)
    {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00},
    // : (58)
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00},
    // ; (59)
    {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30},
    // < (60)
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00},
    // = (61)
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00},
    // > (62)
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00},
    // ? (63)
    {0x7C, 0xC6, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00},
    // @ (64)
    {0x7C, 0xC6, 0xDE, 0xDE, 0xDC, 0xC0, 0x7C, 0x00},
    // A (65)
    {0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0x00},
    // B (66)
    {0xFC, 0xC6, 0xC6, 0xFC, 0xC6, 0xC6, 0xFC, 0x00},
    // C (67)
    {0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00},
    // D (68)
    {0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8, 0x00},
    // E (69)
    {0xFE, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xFE, 0x00},
    // F (70)
    {0xFE, 0xC0, 0xC0, 0xFC, 0xC0, 0xC0, 0xC0, 0x00},
    // G (71)
    {0x7C, 0xC6, 0xC0, 0xCE, 0xC6, 0xC6, 0x7E, 0x00},
    // H (72)
    {0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00},
    // I (73)
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    // J (74)
    {0x1E, 0x06, 0x06, 0x06, 0xC6, 0xC6, 0x7C, 0x00},
    // K (75)
    {0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0xC6, 0x00},
    // L (76)
    {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xFE, 0x00},
    // M (77)
    {0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00},
    // N (78)
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00},
    // O (79)
    {0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},
    // P (80)
    {0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0, 0xC0, 0x00},
    // Q (81)
    {0x7C, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x06},
    // R (82)
    {0xFC, 0xC6, 0xC6, 0xFC, 0xD8, 0xCC, 0xC6, 0x00},
    // S (83)
    {0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0xC6, 0x7C, 0x00},
    // T (84)
    {0xFE, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    // U (85)
    {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},
    // V (86)
    {0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00},
    // W (87)
    {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00},
    // X (88)
    {0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0xC6, 0x00},
    // Y (89)
    {0xC6, 0xC6, 0x6C, 0x38, 0x18, 0x18, 0x18, 0x00},
    // Z (90)
    {0xFE, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0xFE, 0x00},
    // [ (91)
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00},
    // \ (92)
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00},
    // ] (93)
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00},
    // ^ (94)
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00},
    // _ (95)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00},
    // ` (96)
    {0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00},
    // a (97)
    {0x00, 0x00, 0x7C, 0x06, 0x7E, 0xC6, 0x7E, 0x00},
    // b (98)
    {0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xFC, 0x00},
    // c (99)
    {0x00, 0x00, 0x7C, 0xC6, 0xC0, 0xC6, 0x7C, 0x00},
    // d (100)
    {0x06, 0x06, 0x7E, 0xC6, 0xC6, 0xC6, 0x7E, 0x00},
    // e (101)
    {0x00, 0x00, 0x7C, 0xC6, 0xFE, 0xC0, 0x7C, 0x00},
    // f (102)
    {0x1C, 0x36, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x00},
    // g (103)
    {0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x7C},
    // h (104)
    {0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00},
    // i (105)
    {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00},
    // j (106)
    {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78},
    // k (107)
    {0xC0, 0xC0, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0x00},
    // l (108)
    {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00},
    // m (109)
    {0x00, 0x00, 0xCC, 0xFE, 0xD6, 0xD6, 0xD6, 0x00},
    // n (110)
    {0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00},
    // o (111)
    {0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0x00},
    // p (112)
    {0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0},
    // q (113)
    {0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x06},
    // r (114)
    {0x00, 0x00, 0xDC, 0xE6, 0xC0, 0xC0, 0xC0, 0x00},
    // s (115)
    {0x00, 0x00, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x00},
    // t (116)
    {0x30, 0x30, 0x7C, 0x30, 0x30, 0x36, 0x1C, 0x00},
    // u (117)
    {0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0x7E, 0x00},
    // v (118)
    {0x00, 0x00, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00},
    // w (119)
    {0x00, 0x00, 0xC6, 0xD6, 0xD6, 0xFE, 0x6C, 0x00},
    // x (120)
    {0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00},
    // y (121)
    {0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x7C},
    // z (122)
    {0x00, 0x00, 0xFE, 0x0C, 0x38, 0x60, 0xFE, 0x00},
    // { (123)
    {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00},
    // | (124)
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    // } (125)
    {0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00},
    // ~ (126)
    {0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

/**
 * @brief Draw a single character at the specified position
 */
static void draw_char(int x, int y, char c, uint16_t color, int scale)
{
    if (!s_framebuffer || c < 32 || c > 126) return;
    
    int idx = c - 32;
    const uint8_t *glyph = font_8x8[idx];
    
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                // Draw scaled pixel
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT) {
                            s_framebuffer[py * DISPLAY_WIDTH + px] = color;
                        }
                    }
                }
            }
        }
    }
}

/**
 * @brief Draw a string at the specified position
 */
static void draw_string(int x, int y, const char *str, uint16_t color, int scale)
{
    int cx = x;
    while (*str) {
        draw_char(cx, y, *str, color, scale);
        cx += 8 * scale;
        str++;
    }
}

/**
 * @brief Draw a centered string
 */
static void draw_string_centered(int y, const char *str, uint16_t color, int scale)
{
    int len = strlen(str);
    int width = len * 8 * scale;
    int x = (DISPLAY_WIDTH - width) / 2;
    draw_string(x, y, str, color, scale);
}

/**
 * @brief Fill a rectangle with a color
 */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_framebuffer) return;
    
    for (int py = y; py < y + h && py < DISPLAY_HEIGHT; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < DISPLAY_WIDTH; px++) {
            if (px < 0) continue;
            s_framebuffer[py * DISPLAY_WIDTH + px] = color;
        }
    }
}

/**
 * @brief Draw the header bar
 */
static void draw_header(void)
{
    fill_rect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_BLUE);
    draw_string_centered(20, "FIRMWARE UPDATE MODE", COLOR_WHITE, 3);
}

/**
 * @brief Draw a progress bar
 */
static void draw_progress_bar(int progress)
{
    int bar_width = DISPLAY_WIDTH - 2 * PROGRESS_MARGIN;
    int filled_width = (bar_width * progress) / 100;
    
    // Background (dark gray)
    fill_rect(PROGRESS_MARGIN, PROGRESS_Y, bar_width, PROGRESS_HEIGHT, COLOR_DARK_GRAY);
    
    // Filled portion (green)
    if (filled_width > 0) {
        fill_rect(PROGRESS_MARGIN, PROGRESS_Y, filled_width, PROGRESS_HEIGHT, COLOR_GREEN);
    }
    
    // Border
    fill_rect(PROGRESS_MARGIN, PROGRESS_Y, bar_width, 2, COLOR_WHITE);
    fill_rect(PROGRESS_MARGIN, PROGRESS_Y + PROGRESS_HEIGHT - 2, bar_width, 2, COLOR_WHITE);
    fill_rect(PROGRESS_MARGIN, PROGRESS_Y, 2, PROGRESS_HEIGHT, COLOR_WHITE);
    fill_rect(PROGRESS_MARGIN + bar_width - 2, PROGRESS_Y, 2, PROGRESS_HEIGHT, COLOR_WHITE);
    
    // Percentage text
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", progress);
    draw_string_centered(PROGRESS_Y + PROGRESS_HEIGHT + 10, pct_str, COLOR_WHITE, 2);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t bootloader_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing bootloader display...");
    
    esp_err_t ret;
    
    // 1. Initialize I2C (needed for CH422G)
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 8,  // CONFIG_I2C_MASTER_SDA_IO
        .scl_io_num = 9,  // CONFIG_I2C_MASTER_SCL_IO
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    
    ret = i2c_param_config(I2C_NUM_0, &i2c_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 2. Initialize CH422G (needed for backlight)
    ch422g_config_t ch422g_config = {
        .i2c_port = I2C_NUM_0,
        .timeout_ms = 1000,
    };
    ret = ch422g_init(&ch422g_config, &s_ch422g);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize CH422G: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 3. Initialize LCD Panel (minimal config)
    waveshare_lcd_config_t lcd_config = {
        .h_res = DISPLAY_WIDTH,
        .v_res = DISPLAY_HEIGHT,
        .pixel_clock_hz = 16000000,
        .num_fb = 1,  // Single buffer for bootloader
        .bounce_buffer_size_px = DISPLAY_WIDTH * 10,  // Small bounce buffer
        .ch422g_handle = s_ch422g,
    };
    
    ret = waveshare_lcd_init(&lcd_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LCD: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Get the framebuffer pointer
    ret = waveshare_lcd_get_frame_buffer(s_panel, 1, (void**)&s_framebuffer, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get framebuffer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_initialized = true;
    
    // Clear screen to black
    fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, COLOR_BLACK);
    
    // Draw header
    draw_header();
    
    // Initial status
    bootloader_display_update(BOOTLOADER_STATUS_WAITING, 0);
    
    ESP_LOGI(TAG, "Bootloader display initialized");
    return ESP_OK;
}

void bootloader_display_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing bootloader display...");
    
    // The LCD panel and I2C will be reused by the main app,
    // so we don't fully deinit here. Just clear our state.
    s_panel = NULL;
    s_ch422g = NULL;
    s_framebuffer = NULL;
    s_initialized = false;
}

void bootloader_display_update(bootloader_display_status_t status, int progress)
{
    if (!s_initialized || !s_framebuffer) {
        return;
    }
    
    // Clear status area
    fill_rect(0, STATUS_Y - 30, DISPLAY_WIDTH, 150, COLOR_BLACK);
    
    const char *status_text = "";
    uint16_t status_color = COLOR_WHITE;
    
    switch (status) {
        case BOOTLOADER_STATUS_WAITING:
            status_text = "Waiting for firmware...";
            status_color = COLOR_WHITE;
            break;
        case BOOTLOADER_STATUS_RECEIVING:
            status_text = "Receiving firmware";
            status_color = COLOR_YELLOW;
            draw_progress_bar(progress);
            break;
        case BOOTLOADER_STATUS_WRITING:
            status_text = "Writing to flash...";
            status_color = COLOR_ORANGE;
            draw_progress_bar(progress);
            break;
        case BOOTLOADER_STATUS_VERIFYING:
            status_text = "Verifying firmware...";
            status_color = COLOR_YELLOW;
            break;
        case BOOTLOADER_STATUS_SUCCESS:
            status_text = "Update successful!";
            status_color = COLOR_GREEN;
            draw_string_centered(STATUS_Y + 50, "Rebooting...", COLOR_GREEN, 2);
            break;
        case BOOTLOADER_STATUS_ERROR:
            status_text = "Update failed!";
            status_color = COLOR_RED;
            break;
        case BOOTLOADER_STATUS_CHECKSUM_ERR:
            status_text = "Checksum error!";
            status_color = COLOR_RED;
            break;
        case BOOTLOADER_STATUS_FRAME_LOST:
            status_text = "CAN frame lost - retrying";
            status_color = COLOR_ORANGE;
            break;
    }
    
    draw_string_centered(STATUS_Y, status_text, status_color, 3);
}

void bootloader_display_message(const char *line1, const char *line2)
{
    if (!s_initialized || !s_framebuffer) {
        return;
    }
    
    // Clear message area
    fill_rect(0, STATUS_Y - 30, DISPLAY_WIDTH, 150, COLOR_BLACK);
    
    if (line1) {
        draw_string_centered(STATUS_Y, line1, COLOR_WHITE, 3);
    }
    if (line2) {
        draw_string_centered(STATUS_Y + 40, line2, COLOR_WHITE, 2);
    }
}
