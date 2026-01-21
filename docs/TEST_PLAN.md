# Test Plan

> **Note:** This project uses manual testing during development. The test cases 
> below document expected behavior for reference and future regression testing.
> Formal unit tests and automated integration tests are not currently implemented.

## Manual Test Cases

### Fade Controller Behavior
- Segment calculation for long fades (>255s) divides into equal segments
- Progress calculation works across multiple segments
- Immediate apply (Duration=0) sends events instantly

### Scene Management
- JSON parsing handles valid scenes correctly
- Missing or corrupt scenes.json falls back gracefully
- Scene edits persist across reboots

### Configuration
- Valid nodeid.txt is parsed correctly
- Invalid node ID format falls back to default
- LCC configuration changes persist via CDI

---

## Functional Verification Checklist

### Manual Control Tab
- [ ] Sliders adjust RGBW and brightness values (0-255)
- [ ] Color preview circle updates in real-time
- [ ] Apply button sends 6 LCC events (R,G,B,W,Br,Dur=0)
- [ ] Save Scene modal allows naming and saving current values

### Scene Selector Tab
- [ ] Scene cards display with color preview circles
- [ ] Horizontal scroll with center-snapping works
- [ ] Duration slider adjusts fade time (0-300s)
- [ ] Apply button starts fade with progress bar
- [ ] Edit button opens modal with sliders and name input
- [ ] Preview button in edit modal sends current values
- [ ] Delete button shows confirmation modal
- [ ] Scene reordering (move left/right) works

### Fade Behavior
- [ ] Short fades (<255s) send single command set
- [ ] Long fades (>255s) segment into equal chunks
- [ ] Progress bar tracks overall fade progress
- [ ] Fade interruption works (new apply during fade)

### Boot Behavior
- [ ] Splash screen displays on startup
- [ ] Auto-apply (if enabled) fades to first scene
- [ ] Missing SD card shows error screen
- [ ] Missing nodeid.txt uses default node ID

### Power Saving
- [ ] Screen dims after configured timeout
- [ ] Touch wakes screen with fade-in animation
- [ ] Timeout of 0 keeps screen always on

### LCC Integration
- [ ] Node appears on LCC network with configured ID
- [ ] Base event ID configurable via CDI tools (JMRI)
- [ ] Events use correct format per INTERFACES.md
- [ ] OTA firmware update works via JMRI

---

