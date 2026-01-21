# ESP32-S3 LCC Lighting Scene Controller — Specification

## 1. Purpose and Scope

This project implements an ESP32-S3–based LCC lighting scene controller with a touch LCD user interface.

The device:
- Connects to an LCC/OpenLCB CAN bus using OpenMRN
- Sends lighting commands to existing follower lighting controller boards
- Provides a touch UI for manual RGBW + brightness control and scene selection
- Stores configuration and scenes on an SD card
- Uses ESP-IDF v5.4, FreeRTOS, LVGL, and OpenMRN

Out of scope:
- Acting as a lighting follower
- Direct LED driving
- Editing follower firmware

---

## 2. Target Platform and Constraints

### Hardware
- Board: Waveshare ESP32-S3 Touch LCD 4.3B
- MCU: ESP32-S3
- Display: RGB LCD with backlight via CH422G I/O expander
- Touch: Capacitive
- Storage: SD card (SPI, CS via CH422G)
- CAN: LCC bus via onboard CAN transceiver

### Software Stack
- ESP-IDF: v5.1.6 (mandatory — see Build Notes below)
- RTOS: FreeRTOS
- UI: LVGL 8.x (via ESP-IDF component registry)
- LCC: OpenMRN (git submodule at `components/OpenMRN`)
- Filesystem: FATFS on SD card
- Image decoding: esp_jpeg (ESP-IDF component)

### Build Notes

**ESP-IDF Version Constraint**: This project requires **ESP-IDF 5.1.6** (GCC 12.2.0). 
ESP-IDF 5.3+ (GCC 13.x) and 5.4+ (GCC 14.x) have a newlib/libstdc++ incompatibility 
that causes OpenMRN compilation failures with errors in `<bits/char_traits.h>` 
related to `std::construct_at` and `std::pointer_traits::pointer_to`.

**Windows Symlink Fix**: On Windows, the files in `components/OpenMRN/include/esp-idf/` 
may appear as plain text files containing relative paths (broken Unix symlinks). 
These must be converted to proper C `#include` wrapper files. Example:
```c
// components/OpenMRN/include/esp-idf/openmrn_features.h
// Wrapper for esp-idf platform - include parent openmrn_features.h
#include "../openmrn_features.h"
```

**WiFi Driver Exclusion**: `EspIdfWiFi.cxx` uses ESP-IDF 5.3+ APIs (`channel_bitmap`, 
`WIFI_EVENT_HOME_CHANNEL_CHANGE`) and must be excluded from the build in 
`components/OpenMRN/CMakeLists.txt` when using ESP-IDF 5.1.6.

**FAT Long Filename Support**: `scenes.json` requires FAT LFN support. Enable in 
`sdkconfig`:
```
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
```

### Hard Constraints
- LVGL calls must only occur from the UI task
- SD card I/O must not occur in the UI task
- CAN traffic must obey defined rate limits
- Scene file writes must be atomic

---

## 3. LCC Event Model (Authoritative)

### 3.1 Base Event ID Format

`05.01.01.01.9F.60.0x.00`


Where `x` selects the lighting parameter:

| Parameter   | x | Description |
|------------|---|-------------|
| Red        | 0 | Red channel target (0-255) |
| Green      | 1 | Green channel target (0-255) |
| Blue       | 2 | Blue channel target (0-255) |
| White      | 3 | White channel target (0-255) |
| Brightness | 4 | Master brightness target (0-255) |
| Duration   | 5 | Transition time in seconds (0-255) |

### 3.2 Value Encoding

- The final `.00` byte encodes an unsigned 8-bit value
- Valid range: 0–255
- Brightness is not a multiplier; it is a peer parameter
- Brightness behavior is implemented in WS2814 hardware
- Duration value of 0 means instant apply (no fade)
- Duration value of 1-255 triggers fade over that many seconds

### 3.3 Transmission Protocol

**Duration-Triggered Fade Architecture:**

The touchscreen sends all 6 parameters as a command set. The Duration event 
triggers the fade on LED controllers, which perform local interpolation at ~60fps.

1. Touchscreen sends: R, G, B, W, Brightness (target values)
2. Touchscreen sends: Duration (triggers fade on receivers)
3. LED controllers capture pending targets when RGBW+Br received
4. LED controllers start local fade when Duration received
5. Local fade runs at ~60fps for smooth transitions

**Benefits of this architecture:**
- Only 6 LCC events per scene change (minimal bus traffic)
- High-fidelity fading at 60fps on LED controllers
- No dropped updates due to bus congestion
- Touchscreen is free for UI interaction during fades

### 3.4 Long Fade Segmentation

For fades exceeding 255 seconds (the maximum Duration value):

1. Total duration is divided into N equal-length segments
2. Each segment is ≤255 seconds
3. Intermediate target colors are calculated proportionally
4. Each segment sends 6 events with its portion of the transition
5. Touchscreen tracks overall progress for UI display

**Example:** 10-minute (600s) fade:
- 3 segments of 200s each
- Segment 1: 33% of color delta, 200s duration
- Segment 2: 66% of color delta, 200s duration  
- Segment 3: 100% of color delta, 200s duration

---

## 4. Configuration Files

### File: `nodeid.txt` (SD Card)

Plain text file containing the 6-byte LCC node ID in dotted hex format:
```
05.01.01.01.9F.60.00
```

Rules:
- Read at boot before LCC initialization
- Missing file causes SD card error screen
- Changes require device restart

### LCC Configuration (CDI/ACDI)

The device uses OpenMRN's CDI (Configuration Description Information) for:
- **Base Event ID**: 8-byte event ID prefix stored at CDI offset 132
- **Startup Behavior**: Auto-apply settings (see below)
- **User Name/Description**: Stored in ACDI user space (space 251)

**CDI Memory Layout:**
| Offset | Size | Content |
|--------|------|---------|
| 0 | 4 | InternalConfigData (version, etc.) |
| 4 | 1 | Auto-Apply Enabled (0=disabled, 1=enabled) |
| 5 | 2 | Auto-Apply Duration (seconds, 0-300) |
| 7 | 2 | Screen Backlight Timeout (seconds, 0=disabled, 10-3600) |
| 9 | ... | Reserved |
| 132 | 8 | Base Event ID |

**Startup Configuration:**
| Setting | Default | Range | Description |
|---------|---------|-------|-------------|
| Auto-Apply Enabled | 1 | 0-1 | Apply first scene on boot |
| Auto-Apply Duration | 10 | 0-300 | Fade duration in seconds |
| Screen Timeout | 60 | 0, 10-3600 | Backlight timeout in seconds (0=disabled) |

**SNIP (Simple Node Information Protocol):**
- Manufacturer: "IvanBuilds"
- Model: "LCC Touchscreen Controller"
- Hardware Version: "ESP32S3 LCD 4.3B"
- Software Version: Read from `SNIP_STATIC_DATA`

**Implementation Note:** User info (name/description) uses `space="251"` (ACDI user
space) with `origin="1"` to avoid conflicts with manufacturer info at origin 0.

---

## 5. Scene File Format

### File: `scenes.json`

```json
{
  "version": 1,
  "scenes": [
    { "name": "sunrise", "brightness": 180, "r": 255, "g": 120, "b": 40, "w": 0 },
    { "name": "night",   "brightness": 30,  "r": 10,  "g": 10,  "b": 40, "w": 0 }
  ]
}

```

Rules:
- Values are integers 0–255
- Scene names must be unique
- Writes must be atomic
- Version mismatches must be detected

---

## 6. Functional Requirements
### Boot

#### FR-001
Display splashscreen.jpg from SD at boot using esp_jpeg decoder.
AC: Displayed within 1500 ms or fallback shown.

#### FR-002
Initialize OpenMRN using Node ID from SD card `/sdcard/nodeid.txt`.
AC: Node visible on LCC network with configured ID.

#### FR-003
Transition to main UI within 5 s of LCC readiness or timeout.

#### FR-004 (Implemented)
If SD card is not detected at boot, display error screen with:
- Warning icon and "SD Card Not Detected" title
- Instructions listing required files (nodeid.txt, scenes.json)
- Screen persists until device restart

**Implementation Note:** SD card retry is not performed after error screen is shown
because `waveshare_sd_init()` reinitializes CH422G and SPI, which interferes with
the RGB LCD display operation. User must insert card and restart device.

#### FR-005 (Implemented)
Auto-apply first scene on boot when enabled via LCC configuration:
- Configurable enable/disable (default: enabled)
- Configurable fade duration (default: 10 seconds, range 0-300)
- Assumes initial lighting state is all zeros (lights off)
- Progress bar shows fade progress on Scene Selector tab

AC: First scene fades in over configured duration after UI loads.

#### FR-006 (Implemented)
Screen backlight timeout for power saving:
- Configurable timeout via LCC configuration (default: 60 seconds)
- Set to 0 to disable timeout (always on)
- Minimum enabled timeout: 10 seconds, maximum: 3600 seconds
- Touch-to-wake: Any touch restores backlight immediately
- Backlight is on/off only (not dimmable - hardware limitation)

AC: Backlight turns off after configured idle period; touch wakes screen.

### Main UI

#### FR-010
Provide two tabs: Scene Selector (left) and Manual Control (right).
Scene Selector is the default tab shown on startup.

### Manual Control

#### FR-020
Provide sliders for Brightness, R, G, B, W.

#### FR-021
No CAN traffic until Apply is pressed.

#### FR-022
Apply transmits all parameters respecting rate limits.

#### FR-023
Save Scene opens modal dialog with Save and Cancel.

#### FR-024 (Implemented)
Display color preview circle showing approximate light output from current RGBW + brightness settings.
- Circle updates in real-time as sliders are adjusted
- Uses additive light mixing algorithm
- White channel blends towards white while preserving hue
- Brightness acts as intensity using gamma curve

AC: Circle color reflects approximate visual output of RGBW combination.

### Scene Selector

#### FR-040
Display horizontal swipeable card carousel loaded from SD.
- Cards are 240×260 pixels with 20px gap
- Each card shows color preview circle, scene name, and RGBW values
- Carousel uses center snap scrolling
- Selected card shows blue border highlight
- Each card has edit button (top-left) and delete button (top-right) with confirmation modal
- Padding constrains scroll so first/last cards center properly

#### FR-041
Transition duration slider: 0–300 s.

#### FR-042
Apply performs linear fade to target scene.

#### FR-043
Progress bar reflects transition completion.

### Scene Editing

#### FR-044 (Implemented)
Each scene card shall have an Edit button that opens an edit modal.
- Edit button displayed in top-left corner of card (blue circle with pencil icon)
- Tapping Edit opens modal with current scene values pre-populated

AC: Tapping Edit button shows modal with current scene values.

#### FR-045 (Implemented)
Edit modal shall allow modifying scene name, brightness, and RGBW values.
- Text input field for scene name with on-screen keyboard
- Five sliders for brightness, R, G, B, W (0-255 each)
- Color preview circle updates in real-time as values change

AC: All fields are editable and changes are reflected in color preview.

#### FR-046 (Implemented)
Edit modal shall provide Move Left/Right buttons to reorder scenes.
- Move Left button shifts scene one position earlier in carousel
- Move Right button shifts scene one position later in carousel
- Buttons are disabled at respective ends of the scene list

AC: Scene position in carousel changes after reorder operation.

#### FR-047 (Implemented)
Scene edits shall be validated before saving:
- Name cannot be empty
- Name must be unique (except when unchanged)

AC: Save operation fails gracefully if validation fails.

#### FR-048 (Implemented)
Scene edits shall be written atomically to SD card.
- Scene storage maintains in-memory cache
- Full scenes.json file is rewritten on each change
- File operations are flushed before closing

AC: Power loss during save does not corrupt scenes.json.

### CAN Rate Limiting

#### FR-050
Minimum transmission interval: 20 ms.

#### FR-051
If step size < 1, increase interval to meet duration.

#### FR-052
Total duration accuracy: ±2%.

---

### Firmware Update (OTA)

#### FR-060
Device shall support over-the-air firmware updates via LCC Memory Configuration Protocol.
- Uses OpenLCB Firmware Upgrade Standard (memory space 0xEF)
- Compatible with JMRI Firmware Update tool
- Compatible with OpenMRN bootloader_client command-line tool

AC: Firmware update initiated from JMRI completes successfully and device boots new firmware.

#### FR-061
Firmware update shall use ESP-IDF OTA partition scheme.
- Dual OTA partitions (ota_0, ota_1) for safe updates
- Automatic rollback on boot failure (via esp_ota_mark_app_valid_cancel_rollback)
- USB/UART flashing remains available for development

AC: Failed firmware update can be recovered via USB flashing.

#### FR-062
Device shall validate firmware before accepting update.
- ESP32 chip ID validation (rejects firmware built for wrong chip variant)
- Image magic byte verification
- ESP-IDF OTA APIs handle checksum validation

AC: Firmware for ESP32 (non-S3) is rejected with error.

#### FR-063
Device shall enter bootloader mode when requested via LCC.
- "Enter Bootloader" command received via Memory Configuration datagram
- RTC memory flag set to persist across reboot
- Minimal bootloader mode runs CAN-only (no LCD/UI)

AC: Device reboots into bootloader mode within 1 second of command.

#### FR-064
Bootloader mode shall provide visual/log feedback.
- Serial console logs bootloader state
- Log messages indicate firmware receive progress
- Automatic reboot on completion or timeout

AC: Firmware update progress visible in serial monitor.

---

## 7. Non-Functional Requirements
- UI must not block > 50 ms
- Scene save must be power-loss safe
- Logging must not exceed 5 Hz during fades
- CAN disconnect must not require reboot