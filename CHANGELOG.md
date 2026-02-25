# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v1.1.0] - 2026-02-24

### Fixed
- **First-boot LCC scan crash**: Resolved a race condition where the OpenMRN
  executor thread (priority 5) could preempt the main task (priority 1) before
  custom memory spaces were registered. This caused `std::map` corruption in the
  memory space registry when JMRI queried the node during its first LCC scan
  after power-on, resulting in a device reboot. Subsequent scans were unaffected
  because no further map insertions occurred.
- **TWAI ISR flash cache conflict**: Enabled `CONFIG_TWAI_ISR_IN_IRAM=y` to
  prevent potential cache-miss crashes when CAN bus interrupts fire during
  concurrent SPI (SD card) access.

### Added
- **Touch suppression during screen wake**: The touch that wakes the screen from
  sleep no longer triggers UI actions. Touch input is suppressed until the
  fade-in animation completes and the screen is fully active. This prevents
  accidental scene changes or slider adjustments from the waking touch.
- **LCC reboot command support**: `reboot()` override calls `esp_restart()` so
  that LCC `COMMAND_RESET` (0xA9) and factory-reset-then-reboot actually restart
  the device instead of being a no-op.
- **LCC bootloader entry support**: `enter_bootloader()` override calls
  `bootloader_hal_request_reboot()` so that LCC `COMMAND_ENTER_BOOTLOADER`
  (0xAB) and `COMMAND_FREEZE` on space 0xEF correctly enter firmware update mode.
- `screen_timeout_is_interactive()` API to query whether the screen is fully
  active and ready for user interaction.

### Changed
- OpenMRN executor stack size increased from 4096 to 8192 bytes for additional
  headroom during heavy LCC traffic.
- Executor thread now uses `delay_start=true` to ensure all custom memory spaces
  are registered before the node announces itself on the LCC bus.

## [v1.0.0] - 2025-12-15

### Added
- Initial release
- Manual RGBW + brightness control via touch sliders
- Scene management (save, load, edit, reorder, delete)
- Configurable linear fades up to 5 minutes with progress bar
- LCC/OpenLCB event production over CAN bus
- SD card configuration storage (node ID, scenes, splash image)
- Screen backlight timeout with fade animation
- Auto-apply first scene on boot
- OTA firmware updates via LCC/JMRI
- 3D printable mounting brackets
