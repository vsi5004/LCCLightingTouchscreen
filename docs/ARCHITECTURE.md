# Architecture

## 1. Project Structure

```
LCCLightingTouchscreen/
├── CMakeLists.txt
├── sdkconfig.defaults
├── lv_conf.h                 # LVGL configuration (root level)
├── components/
│   ├── OpenMRN/              # Git submodule
│   └── board_drivers/        # Hardware abstraction
│       ├── ch422g.c/.h       # I2C expander driver
│       ├── waveshare_lcd.c/.h
│       ├── waveshare_touch.c/.h
│       └── waveshare_sd.c/.h
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     # LVGL 8.x, esp_lcd_touch, esp_jpeg
│   ├── Kconfig.projbuild
│   ├── main.c                # Entry point, hardware init, SD error screen
│   ├── lv_conf.h             # LVGL configuration (main level)
│   ├── app/                  # Application logic
│   │   ├── lcc_node.cpp/.h   # OpenMRN integration, event production
│   │   ├── scene_manager.c/.h
│   │   ├── fade_controller.c/.h  # ✓ Implemented: state machine, interpolation
│   │   └── screen_timeout.c/.h   # ✓ Implemented: backlight power saving
│   └── ui/                   # LVGL screens
│       ├── ui_common.c/.h    # LVGL init, mutex, flush callbacks
│       ├── ui_main.c/.h      # Main tabview container
│       ├── ui_manual.c/.h    # Manual RGBW sliders, Apply button
│       └── ui_scenes.c/.h    # Card carousel, progress bar, Apply button
└── docs/
```

---

## 2. Task Model

### Tasks

| Task | Priority | Stack | Core | Responsibility |
|------|----------|-------|------|----------------|
| lvgl_task | 2 | 6KB | CPU1 | LVGL rendering via `lv_timer_handler()` |
| openmrn_task | 5 | 8KB | Any | OpenMRN executor loop |
| lighting_task | 4 | 4KB | Any | Fade controller tick (10ms interval) |
| main_task | 1 | 4KB | CPU1 | Hardware init, app orchestration |

**CPU Affinity Strategy:**
- **CPU0**: Dedicated to RGB LCD DMA ISRs (bounce buffer transfers)
- **CPU1**: Main task + LVGL rendering (avoids contention with display DMA)
- This separation reduces visual artifacts (banding) during UI animations

### Task Implementation Notes
- **lvgl_task**: Created by `ui_init()`, runs continuously calling `lv_timer_handler()`
- **openmrn_task**: Created by `lcc_node_init()`, runs OpenMRN's internal executor
- **lighting_task**: Created in `app_main()`, calls `fade_controller_tick()` every 10ms

---

## 3. Inter-Task Communication

- **UI → Lighting**: FreeRTOS queue (commands: UPDATE, APPLY_SCENE, CANCEL)
- **Lighting → LCC**: Direct OpenMRN event producer API
- **SD Worker → UI**: FreeRTOS queue (notifications: SCENE_LOADED, SAVE_COMPLETE)
- **LVGL mutex**: Required for all LVGL API access from non-UI tasks

---

## 4. State Machines

### Application State

```
BOOT → SPLASH → LCC_INIT → MAIN_UI
         ↓          ↓
     (timeout)  (timeout)
         ↓          ↓
      MAIN_UI ← MAIN_UI (degraded)
```

### Lighting State (Fade Controller)

```
IDLE ←──────────────────┐
  │                     │
  ├─ start() ──→ FADING │
  │                ↓    │
  │            COMPLETE─┘
  │
  └─ apply_immediate() ─→ (sends all 6 params with Duration=0, stays IDLE)
```

**Implemented in `fade_controller.c`:**
- `fade_controller_start()`: Sends target + duration, tracks segment progress
- `fade_controller_apply_immediate()`: Sends all 6 params with Duration=0 (instant)
- `fade_controller_tick()`: Called periodically, advances to next segment when needed
- `fade_controller_get_progress()`: Returns 0.0-1.0 for progress bar updates
- `fade_controller_abort()`: Cancels active fade, resets to IDLE

**Protocol:** Duration-triggered (6 events per scene change)
**Fading:** Performed locally by LED controllers at ~60fps

---

## 5. OpenMRN Integration

### Build Configuration

OpenMRN requires ESP-IDF 5.1.6 (GCC 12.2.0) due to newlib/libstdc++ incompatibility 
in GCC 13.x/14.x. The component CMakeLists.txt excludes `EspIdfWiFi.cxx` (uses 
ESP-IDF 5.3+ APIs) since this project uses CAN, not WiFi.

Key compile options:
- C++ Standard: C++14
- `-fno-strict-aliasing` — Required for OpenMRN compatibility
- `-D_GLIBCXX_USE_C99` — C99 compatibility defines
- `-Wno-volatile` — Suppress deprecated volatile warnings

### LVGL Performance Tuning

The following settings optimize scroll and animation performance:

| Setting | Value | Source | Purpose |
|---------|-------|--------|--------|
| `LV_DISP_DEF_REFR_PERIOD` | 10ms | sdkconfig | 100Hz refresh for smooth animations |
| `LV_INDEV_DEF_READ_PERIOD` | 10ms | lv_conf.h | Fast touch polling |
| `LV_INDEV_DEF_SCROLL_THROW` | 5 | lv_conf.h | Reduced scroll momentum |
| `LV_INDEV_DEF_SCROLL_LIMIT` | 30 | lv_conf.h | Lower scroll sensitivity |
| `LV_MEM_CUSTOM` | 1 | sdkconfig | Use stdlib malloc (PSRAM-aware) |
| `LV_MEMCPY_MEMSET_STD` | 1 | sdkconfig | Use optimized libc memory functions |
| `LV_ATTRIBUTE_FAST_MEM` | IRAM | sdkconfig | Place critical functions in IRAM |

**Additional Optimizations:**
- Scene cards omit shadows to improve scroll frame rate
- Fade animations use 20 discrete opacity steps to reduce mid-frame inconsistencies
- LVGL task pinned to CPU1 to avoid contention with LCD DMA on CPU0

### Driver
- Uses `Esp32HardwareTwai` from OpenMRN
- VFS path: `/dev/twai/twai0`
- Pins: TX=GPIO15, RX=GPIO16

### Initialization Sequence
1. Read `nodeid.txt` from SD for 12-digit hex Node ID
2. Create `Esp32HardwareTwai` instance
3. Call `twai.hw_init()`
4. Initialize OpenMRN SimpleCanStack
5. Add CAN port via `add_can_port_async("/dev/twai/twai0")`

### Auto-Apply on Boot
When enabled via LCC configuration (CDI):
1. Load first scene from `scenes.json`
2. Assume initial lighting state is all zeros
3. Start fade to first scene using configured duration (default 10 sec)
4. Progress bar on Scene Selector tab shows fade progress

### Power Saving (Screen Timeout)
The `screen_timeout` module provides automatic backlight control with smooth transitions:

| Setting | Default | Range | Description |
|---------|---------|-------|-------------|
| Timeout | 60 sec | 0, 10-3600 | Idle time before backlight off (0=disabled) |
| Fade Duration | 1 sec | Fixed | Visual fade-to-black transition time |

**State Machine:**
```
ACTIVE ──(timeout)──→ FADING_OUT ──(fade complete)──→ OFF
   ↑                      │                            │
   │                      └──(touch)──→ FADING_IN ─────┘
   │                                        │
   └────────────(fade complete)─────────────┘
```

**Implementation:**
- `screen_timeout_init()`: Initialize with CH422G handle and timeout from LCC config
- `screen_timeout_tick()`: Called every 500ms from main loop to check timeout
- `screen_timeout_notify_activity()`: Called from touch callback to reset timer

**Fade Animation:**
- Uses LVGL overlay on `lv_layer_top()` for smooth black fade effect
- 1 second fade-out before backlight turns off (less jarring than abrupt shutoff)
- 1 second fade-in when waking to restore UI gradually
- Touch during fade-out aborts and transitions to fade-in
- Animation uses `lv_anim` with opacity interpolation (LV_OPA_TRANSP ↔ LV_OPA_COVER)
- **Stepped Opacity**: Uses 20 discrete opacity levels to reduce banding artifacts
  caused by RGB LCD bounce buffer partial frame updates

**Hardware Limitation:** The CH422G I/O expander provides only digital on/off control
for the backlight pin. PWM dimming is not possible with this hardware design. The fade
effect is achieved via LVGL overlay opacity animation while backlight remains on.

### Event Production
- Event ID format: `{base_event_id[0:6]}.{param_offset}.{value}`
- 6 parameters: R (0), G (1), B (2), W (3), Brightness (4), Duration (5)
- Duration event triggers fade on LED controllers

---

## 6. Fade Algorithm (Normative)

### Duration-Triggered Fade Protocol

The touchscreen acts as a command station, sending target values and transition 
duration to LED controllers. LED controllers perform local high-fidelity fading.

**Touchscreen responsibilities:**
1. Calculate target RGBW + Brightness values from scene
2. Send all 6 LCC events (R, G, B, W, Brightness, Duration)
3. Track progress for UI display (progress bar)
4. Handle long fades (>255s) via segmentation

**LED controller responsibilities:**
1. Store pending R, G, B, W, Brightness as they arrive
2. On Duration event, capture current values as fade start
3. Interpolate from current to pending over Duration seconds
4. Update LEDs at ~60fps (16ms intervals) for smooth transitions
5. Duration=0 means instant apply (no interpolation)

### Long Fade Segmentation

For fades exceeding 255 seconds:

1. Divide total duration into N equal segments (each ≤255s)
2. For each segment:
   - Calculate intermediate target (linear interpolation)
   - Send 6 events with segment duration
   - Wait for segment to complete
3. All segments have equal duration = total / N
4. Progress = (completed_segments + current_segment_progress) / N

**Example: 5-minute (300s) fade**
- 2 segments of 150s each
- Segment 1: 50% of color delta over 150s
- Segment 2: remaining 50% over 150s

### Implementation Details (`fade_controller.c`)
- **State Machine**: IDLE → FADING → COMPLETE → IDLE
- **Segment Tracking**: current_segment / total_segments
- **Progress Tracking**: Overall progress across all segments for UI
- **Equal Segments**: All segments have identical duration (simpler math)
- **Immediate Apply**: Duration=0 sends target with 0s duration (instant)

### UI Integration
- **Scene Selector Tab** (leftmost): Card carousel with color preview circles, "Apply" starts fade, progress bar
- **Manual Control Tab**: RGBW sliders, color preview circle, "Apply" calls `fade_controller_apply_immediate()`
- **Progress Bar**: LVGL timer (100ms) polls `fade_controller_get_progress()`, hides when fade completes
- **Auto-Apply on Boot**: Calls `ui_scenes_start_progress_tracking()` to show fade progress
- **Scene Editing**: Edit modal with sliders, name input, and reorder buttons
- **Scene Cards**: Each card has edit (pencil) and delete (trash) buttons

### LVGL Thread Safety
All LVGL API calls must occur from the LVGL task context. When modifying UI from 
non-UI tasks, acquire the mutex via `ui_lock()`/`ui_unlock()`.

**Important:** LVGL callbacks already run in LVGL context. Functions that update 
UI from callbacks must use no-lock variants to avoid deadlock:
- `scene_storage_reload_ui()` — Use from non-UI tasks (acquires mutex)
- `scene_storage_reload_ui_no_lock()` — Use from LVGL callbacks (no mutex)

---

## 7. Color Preview Algorithm

The `ui_calculate_preview_color()` function approximates RGBW LED visual output for display.

### Mixing Rules
```c
// White blends towards white (80% max at W=255)
full_r = r + ((255 - r) * w) / 320;
full_g = g + ((255 - g) * w) / 320;
full_b = b + ((255 - b) * w) / 320;

// Brightness as intensity (gamma 0.5 via square root)
intensity = sqrt(brightness * 255);  // 0-255

// Final output
result = (full_rgb * intensity) / 255;
```

### Design Rationale
- **Additive Light Mixing**: RGB channels combine as light, not pigments
- **White Preservation**: High white values brighten but don't completely wash out color
- **Perceptual Brightness**: Square root curve makes low brightness values more visible

---

## 8. Firmware Update (OTA) Architecture

### Overview

The device supports over-the-air (OTA) firmware updates via the LCC Memory Configuration
Protocol. This enables firmware updates through JMRI's "Firmware Update" tool or the
OpenMRN `bootloader_client` command-line utility without physical access to the device.

### Boot Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ ESP32 ROM Bootloader                                            │
│ - Always available for USB/UART recovery                        │
├─────────────────────────────────────────────────────────────────┤
│ ESP-IDF Second-Stage Bootloader (0x1000)                        │
│ - Selects active OTA partition (ota_0 or ota_1)                 │
│ - Handles rollback on boot failure                              │
├─────────────────────────────────────────────────────────────────┤
│ Application (ota_0 or ota_1)                                    │
│ ├── Check RTC flag: bootloader_request                          │
│ │   ├── TRUE  → Run bootloader mode (CAN receive loop)          │
│ │   └── FALSE → Normal application startup (UI, LCC, etc.)      │
└─────────────────────────────────────────────────────────────────┘
```

### Partition Table

| Name     | Type | SubType | Offset   | Size    | Purpose                    |
|----------|------|---------|----------|---------|----------------------------|
| nvs      | data | nvs     | 0x9000   | 0x6000  | NVS key-value storage      |
| otadata  | data | ota     | 0xf000   | 0x2000  | OTA boot selection data    |
| phy_init | data | phy     | 0x11000  | 0x1000  | PHY calibration            |
| ota_0    | app  | ota_0   | 0x20000  | 0x1E0000| Application slot A (~1.9MB)|
| ota_1    | app  | ota_1   | 0x200000 | 0x1E0000| Application slot B (~1.9MB)|

### Components

| Component | File | Responsibility |
|-----------|------|----------------|
| Bootloader HAL | `app/bootloader_hal.cpp` | ESP32-specific OTA integration |
| LCC Node | `app/lcc_node.cpp` | Handles "enter bootloader" command |
| OpenMRN | `Esp32BootloaderHal.hxx` | LCC Memory Config Protocol handler |

### Firmware Update Process

1. **JMRI/Client** sends "Enter Bootloader" datagram to target node
2. **lcc_node** receives command, calls `bootloader_hal_request_reboot()`
3. **bootloader_hal** sets RTC memory flag, calls `esp_restart()`
4. **app_main** on reboot detects flag, calls `bootloader_hal_run()`
5. **Bootloader mode** runs minimal CAN stack (no LCD, no LVGL)
6. **JMRI/Client** streams firmware binary to memory space 0xEF
7. **Esp32BootloaderHal** validates and writes to alternate OTA partition
8. **On completion** sets new partition as active, reboots into new firmware

### Safety Features

| Feature | Implementation |
|---------|----------------|
| Rollback on failure | ESP-IDF `CONFIG_APP_ROLLBACK_ENABLE` |
| Chip ID validation | Rejects firmware for wrong ESP32 variant |
| Image magic check | Validates ESP-IDF image header |
| USB recovery | ROM bootloader always accessible |
| Partition isolation | New firmware written to inactive slot |

### JMRI Usage

1. Connect JMRI to LCC network (CAN-USB adapter or LCC hub)
2. Open **LCC Menu → Firmware Update**
3. Select target node by Node ID
4. Choose firmware `.bin` file (from `build/LCCLightingTouchscreen.bin`)
5. Click "Download" to start update
6. Device shows progress on LCD (blue header, status text, progress bar)
7. Device reboots automatically when complete

### Bootloader Display Module

Since this board has no LEDs, the `bootloader_display` module provides visual
feedback during firmware updates:

| Status | Display |
|--------|--------|
| Waiting | "Waiting for firmware..." (white) |
| Receiving | "Receiving firmware" + progress bar (yellow) |
| Writing | "Writing to flash..." + progress bar (orange) |
| Success | "Update successful! Rebooting..." (green) |
| Error | "Update failed!" / "Checksum error!" (red) |

The display uses direct framebuffer rendering with an embedded 8x8 bitmap font,
avoiding the overhead of LVGL during the update process.
