/**
 * @file fade_controller.c
 * @brief Lighting Fade Controller Implementation
 * 
 * Sends lighting scene parameters and transition duration to LED controllers.
 * LED controllers perform local high-fidelity fading. For long fades (>255s),
 * automatically segments into multiple command sets with intermediate targets.
 * 
 * @see docs/ARCHITECTURE.md §6 for Fade Algorithm specification
 */

#include "fade_controller.h"
#include "lcc_node.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fade_ctrl";

/// Maximum duration that can be sent in a single command (255 seconds)
#define MAX_SEGMENT_DURATION_SEC  255

/**
 * @brief Internal fade state
 */
typedef struct {
    bool initialized;
    
    // Fade state machine
    fade_state_t state;
    
    // Original fade request (before segmentation)
    lighting_state_t original_start;    // Starting values when fade began
    lighting_state_t final_target;      // Ultimate target values
    uint32_t total_duration_ms;         // Total fade duration (all segments)
    
    // Current segment
    lighting_state_t segment_target;    // Target for current segment
    uint32_t segment_duration_ms;       // Duration of current segment
    int current_segment;                // 0-based segment index
    int total_segments;                 // Total number of segments
    
    // Timing
    int64_t fade_start_us;              // Timestamp when ENTIRE fade started
    int64_t segment_start_us;           // Timestamp when current segment started
    
    // Tracking what LED controllers are currently showing (for segment starts)
    lighting_state_t current;           // Current/last sent values
    
} fade_state_internal_t;

static fade_state_internal_t s_fade = {0};

/**
 * @brief Get parameter value from lighting_state_t by index
 */
/**
 * @brief Interpolate between two lighting states
 */
static void interpolate_state(const lighting_state_t *start, const lighting_state_t *end,
                               float progress, lighting_state_t *result)
{
    result->red = start->red + (int16_t)(end->red - start->red) * progress;
    result->green = start->green + (int16_t)(end->green - start->green) * progress;
    result->blue = start->blue + (int16_t)(end->blue - start->blue) * progress;
    result->white = start->white + (int16_t)(end->white - start->white) * progress;
    result->brightness = start->brightness + (int16_t)(end->brightness - start->brightness) * progress;
}

/**
 * @brief Send all 6 LCC events (RGBW + Brightness + Duration)
 */
static esp_err_t send_lighting_command(const lighting_state_t *target, uint8_t duration_sec)
{
    esp_err_t ret;
    
    // Send RGBW + Brightness
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_RED, target->red);
    if (ret != ESP_OK) return ret;
    
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_GREEN, target->green);
    if (ret != ESP_OK) return ret;
    
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_BLUE, target->blue);
    if (ret != ESP_OK) return ret;
    
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_WHITE, target->white);
    if (ret != ESP_OK) return ret;
    
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_BRIGHTNESS, target->brightness);
    if (ret != ESP_OK) return ret;
    
    // Duration triggers the fade on receivers
    ret = lcc_node_send_lighting_event(LIGHT_PARAM_DURATION, duration_sec);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGD(TAG, "Sent: R=%d G=%d B=%d W=%d Br=%d Dur=%ds",
             target->red, target->green, target->blue, target->white,
             target->brightness, duration_sec);
    
    return ESP_OK;
}

/**
 * @brief Start the next segment of a multi-segment fade
 * 
 * For fades >255s, we divide into equal-duration segments.
 * This keeps the math simple: each segment covers 1/N of time and 1/N of color change.
 */
static esp_err_t start_next_segment(void)
{
    s_fade.current_segment++;
    
    if (s_fade.current_segment >= s_fade.total_segments) {
        // All segments complete
        s_fade.state = FADE_STATE_COMPLETE;
        ESP_LOGD(TAG, "All segments complete");
        return ESP_OK;
    }
    
    // All segments have equal duration (total / num_segments)
    s_fade.segment_duration_ms = s_fade.total_duration_ms / s_fade.total_segments;
    
    // Progress is simply (segment + 1) / total_segments since all segments are equal
    float segment_end_progress = (float)(s_fade.current_segment + 1) / (float)s_fade.total_segments;
    
    interpolate_state(&s_fade.original_start, &s_fade.final_target,
                      segment_end_progress, &s_fade.segment_target);
    
    uint8_t duration_sec = (uint8_t)(s_fade.segment_duration_ms / 1000);
    
    ESP_LOGD(TAG, "Starting segment %d/%d: %lums to R=%d G=%d B=%d W=%d Br=%d",
             s_fade.current_segment + 1, s_fade.total_segments,
             (unsigned long)s_fade.segment_duration_ms,
             s_fade.segment_target.red, s_fade.segment_target.green,
             s_fade.segment_target.blue, s_fade.segment_target.white,
             s_fade.segment_target.brightness);
    
    s_fade.segment_start_us = esp_timer_get_time();
    
    return send_lighting_command(&s_fade.segment_target, duration_sec);
}

esp_err_t fade_controller_init(void)
{
    if (s_fade.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    memset(&s_fade, 0, sizeof(s_fade));
    s_fade.state = FADE_STATE_IDLE;
    s_fade.initialized = true;
    
    ESP_LOGI(TAG, "Fade controller initialized");
    return ESP_OK;
}

esp_err_t fade_controller_start(const fade_params_t *params)
{
    if (!s_fade.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!params) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Store original start (current LED state) and final target
    s_fade.original_start = s_fade.current;
    s_fade.final_target = params->target;
    s_fade.total_duration_ms = params->duration_ms;
    
    // Calculate number of segments needed
    if (params->duration_ms == 0) {
        s_fade.total_segments = 1;
    } else {
        s_fade.total_segments = (params->duration_ms + (MAX_SEGMENT_DURATION_SEC * 1000 - 1)) /
                                 (MAX_SEGMENT_DURATION_SEC * 1000);
    }
    
    s_fade.current_segment = -1;  // Will be incremented to 0 in start_next_segment
    s_fade.fade_start_us = esp_timer_get_time();
    s_fade.state = FADE_STATE_FADING;
    
    ESP_LOGD(TAG, "Starting fade: %lums (%d segment%s) to R=%d G=%d B=%d W=%d Br=%d",
             (unsigned long)params->duration_ms,
             s_fade.total_segments, s_fade.total_segments > 1 ? "s" : "",
             params->target.red, params->target.green, params->target.blue,
             params->target.white, params->target.brightness);
    
    // Start first segment
    esp_err_t ret = start_next_segment();
    if (ret != ESP_OK) {
        s_fade.state = FADE_STATE_IDLE;
        return ret;
    }
    
    // Update current to target (LED controllers are now fading to this)
    s_fade.current = s_fade.segment_target;
    
    return ESP_OK;
}

esp_err_t fade_controller_apply_immediate(const lighting_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    fade_params_t params = {
        .target = *state,
        .duration_ms = 0
    };
    
    return fade_controller_start(&params);
}

esp_err_t fade_controller_tick(void)
{
    if (!s_fade.initialized) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (s_fade.state == FADE_STATE_IDLE) {
        return ESP_OK;
    }
    
    if (s_fade.state == FADE_STATE_COMPLETE) {
        // Transition to idle
        s_fade.state = FADE_STATE_IDLE;
        return ESP_OK;
    }
    
    // FADING state - check if current segment is complete
    int64_t now_us = esp_timer_get_time();
    int64_t segment_elapsed_us = now_us - s_fade.segment_start_us;
    uint32_t segment_elapsed_ms = (uint32_t)(segment_elapsed_us / 1000);
    
    if (segment_elapsed_ms >= s_fade.segment_duration_ms) {
        // Current segment complete - update current state and start next
        s_fade.current = s_fade.segment_target;
        
        esp_err_t ret = start_next_segment();
        if (ret != ESP_OK && s_fade.state == FADE_STATE_FADING) {
            // Error starting next segment, but not complete - retry next tick
            ESP_LOGW(TAG, "Failed to start next segment: %s", esp_err_to_name(ret));
        }
        
        // Update current for next segment
        if (s_fade.state == FADE_STATE_FADING) {
            s_fade.current = s_fade.segment_target;
        }
    }
    
    return ESP_OK;
}

fade_state_t fade_controller_get_progress(fade_progress_t *progress)
{
    if (!s_fade.initialized) {
        if (progress) {
            memset(progress, 0, sizeof(*progress));
        }
        return FADE_STATE_IDLE;
    }
    
    if (progress) {
        progress->state = s_fade.state;
        progress->current = s_fade.final_target;  // What we're fading to
        progress->total_ms = s_fade.total_duration_ms;
        
        if (s_fade.state == FADE_STATE_FADING) {
            int64_t elapsed_us = esp_timer_get_time() - s_fade.fade_start_us;
            progress->elapsed_ms = (uint32_t)(elapsed_us / 1000);
            if (progress->elapsed_ms > progress->total_ms) {
                progress->elapsed_ms = progress->total_ms;
            }
            
            if (progress->total_ms > 0) {
                progress->progress_percent = (uint8_t)((progress->elapsed_ms * 100) / progress->total_ms);
                if (progress->progress_percent > 100) {
                    progress->progress_percent = 100;
                }
            } else {
                progress->progress_percent = 100;
            }
        } else if (s_fade.state == FADE_STATE_COMPLETE) {
            progress->elapsed_ms = progress->total_ms;
            progress->progress_percent = 100;
        } else {
            progress->elapsed_ms = 0;
            progress->progress_percent = 0;
        }
    }
    
    return s_fade.state;
}

bool fade_controller_is_active(void)
{
    return s_fade.initialized && s_fade.state == FADE_STATE_FADING;
}

void fade_controller_abort(void)
{
    if (!s_fade.initialized) {
        return;
    }
    
    if (s_fade.state == FADE_STATE_FADING) {
        ESP_LOGI(TAG, "Fade aborted");
        // Send immediate apply to stop LED controllers at current interpolated position
        // (They'll calculate their own current position based on elapsed time)
    }
    
    s_fade.state = FADE_STATE_IDLE;
}

esp_err_t fade_controller_get_current(lighting_state_t *state)
{
    if (!s_fade.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *state = s_fade.current;
    return ESP_OK;
}

esp_err_t fade_controller_set_current(const lighting_state_t *state)
{
    if (!s_fade.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_fade.current = *state;
    
    ESP_LOGI(TAG, "Current state set: B=%d R=%d G=%d B=%d W=%d",
             state->brightness, state->red, state->green, state->blue, state->white);
    
    return ESP_OK;
}
