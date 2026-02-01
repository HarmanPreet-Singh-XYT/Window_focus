# Complete Input Device Detection Guide

## All Detected Input Sources

The Window Focus plugin detects user activity from **ALL** input sources except microphones.

### 1. Keyboard & Mouse ✓
**Detection Method**: Windows Low-Level Hooks (`WH_KEYBOARD_LL`, `WH_MOUSE_LL`)

**Detected:**
- All keyboard input (any key press)
- Mouse movement
- Mouse clicks (left, right, middle, extra buttons)
- Mouse wheel scrolling

**Not Detected:**
- None - all keyboard and mouse input is detected

---

### 2. XInput Controllers ✓
**Detection Method**: XInput API (`XInputGetState`)

**Detected:**
- Xbox 360 controllers
- Xbox One controllers
- Xbox Series X/S controllers
- Any XInput-compatible gamepad
- Third-party XInput controllers

**Not Detected:**
- Non-XInput controllers (covered by HID detection below)

---

### 3. System Audio ✓
**Detection Method**: Windows Core Audio API (`IAudioMeterInformation`)

**Detected:**
- Audio playback from any application
- System sounds
- Music players
- Video players
- Game audio
- Browser audio
- Streaming audio

**Not Detected:**
- Microphone input (intentionally excluded)
- Audio recording (no output)

---

### 4. HID (Human Interface Devices) ✓ **ALL INPUT DEVICES**
**Detection Method**: Windows HID API + SetupAPI

This is the comprehensive category that catches **everything else**.

#### 🎮 Gaming Devices
**Racing Wheels:**
- Logitech G29, G920, G923, G27, G25
- Thrustmaster T150, T300, T500, TX, T-GT
- Fanatec CSL, DD, Podium series
- Any DirectInput racing wheel

**Flight Controllers:**
- Thrustmaster HOTAS Warthog, T.16000M
- Logitech/Saitek X52, X56, X65F
- VKB, Virpil controllers
- Any flight stick or HOTAS system

**Game Controllers:**
- PlayStation DualShock/DualSense controllers (when not using XInput mode)
- Nintendo Switch Pro Controller
- Steam Controller
- RetroArch controllers
- Generic USB gamepads
- Arcade sticks
- Dance pads

**Other Gaming Input:**
- Foot pedals
- Sim racing button boxes
- Flight simulator panels
- VR controllers (when connected as HID)

#### 🎨 Creative Devices
**Drawing Tablets:**
- Wacom (Intuos, Cintiq, Bamboo, etc.)
- Huion (Kamvas, Inspiroy, etc.)
- XP-Pen (Artist, Deco, etc.)
- Gaomon tablets
- Any pen tablet or display tablet

**Stylus Input:**
- Surface Pen (when used as HID)
- Apple Pencil alternatives
- Any active stylus

#### 🖱️ Specialty Input Devices
**Custom Controllers:**
- Stream Deck (Elgato)
- Macro keypads
- Programmable button boxes
- Custom Arduino/Raspberry Pi input devices

**Accessibility Devices:**
- Adaptive controllers
- Switch interfaces
- Alternative input devices
- Custom accessibility hardware

**Professional Equipment:**
- DJ controllers and mixers
- Music MIDI controllers (when used as HID input)
- Video editing control surfaces
- 3D mice (SpaceMouse, etc.)

**Other:**
- USB foot pedals
- Presentation remotes
- Any custom USB input device

#### ❌ Excluded HID Devices
**Audio Devices (Intentionally Excluded):**
- Microphones
- USB headsets (microphone portion)
- Audio interfaces (input side)
- Telephony devices

These are excluded to prevent voice input or background noise from being detected as user activity.

---

## Detection Summary Table

| Input Type | Detection Method | Examples |
|------------|-----------------|----------|
| **Keyboard** | Low-Level Hook | Any keyboard |
| **Mouse** | Low-Level Hook + Cursor Position | Any mouse, trackpad |
| **XInput Controllers** | XInput API | Xbox controllers |
| **Audio Playback** | Core Audio API | Any system audio |
| **Racing Wheels** | HID API | Logitech G29, Thrustmaster |
| **Flight Sticks** | HID API | HOTAS, joysticks |
| **Drawing Tablets** | HID API | Wacom, Huion, XP-Pen |
| **Game Controllers** | HID API | PlayStation, Switch, generic |
| **Stream Decks** | HID API | Elgato Stream Deck |
| **Custom Devices** | HID API | Arduino, custom USB |
| **DJ Equipment** | HID API | MIDI controllers (HID mode) |
| **3D Mice** | HID API | SpaceMouse |
| **Microphones** | ❌ NOT DETECTED | Intentionally excluded |

---

## Usage Page Reference

The plugin detects HID devices with these Usage Pages (except audio):

| Usage Page | Category | Examples |
|------------|----------|----------|
| **0x01** | Generic Desktop | Mouse, keyboard, joystick, gamepad, wheel |
| **0x0D** | Digitizers | Drawing tablets, touch screens, stylus |
| **0x0F** | Physical Interface | Force feedback devices |
| **Vendor** | Custom | Manufacturer-specific devices |
| ❌ **0x0B** | Telephony | Microphones, headsets (EXCLUDED) |
| ❌ **0x0C** | Consumer | Audio controls (EXCLUDED) |

---

## Configuration Examples

### Detect Everything (Default)
```dart
final windowFocus = WindowFocus();
// Detects: keyboard, mouse, controllers, audio, HID devices
```

### Gaming Setup (No Audio)
```dart
final windowFocus = WindowFocus(
  monitorAudio: false,        // Don't count game audio
  monitorControllers: true,   // Xbox controllers
  monitorHIDDevices: true,    // Racing wheels, flight sticks
);
```

### Media/Video Application
```dart
final windowFocus = WindowFocus(
  monitorAudio: true,         // Keep active during video playback
  monitorControllers: false,  // Don't need controllers
  monitorHIDDevices: false,   // Don't need special devices
);
```

### Creative/Art Application
```dart
final windowFocus = WindowFocus(
  monitorHIDDevices: true,    // Drawing tablets
  monitorAudio: false,        // Don't count background music
);
```

### Minimal Detection (Keyboard/Mouse Only)
```dart
final windowFocus = WindowFocus(
  monitorAudio: false,
  monitorControllers: false,
  monitorHIDDevices: false,
);
// Only detects: keyboard and mouse
```

---

## Debug Output

Enable debug mode to see exactly what devices are detected:

```dart
final windowFocus = WindowFocus(debug: true);
```

Example console output:
```
[WindowFocus] Keyboard hook installed successfully
[WindowFocus] Mouse hook installed successfully
[WindowFocus] HID device added: VID=046d PID=c24f UsagePage=0x1 Usage=0x4
[WindowFocus] HID device added: VID=056a PID=0357 UsagePage=0xd Usage=0x2
[WindowFocus] Skipping audio device: VID=046d PID=0a4d UsagePage=0xb
[WindowFocus] Initialized 2 HID devices
```

This shows:
- VID/PID of detected devices
- Usage Page and Usage codes
- Which devices were skipped (audio devices)

---

## Frequently Asked Questions

### Q: Will my drawing tablet be detected?
**A: Yes!** All drawing tablets (Wacom, Huion, XP-Pen, etc.) are HID devices and will be detected.

### Q: Will my Logitech G29 racing wheel work?
**A: Yes!** All racing wheels use HID or DirectInput and will be detected.

### Q: What about my PlayStation controller?
**A: Yes!** When connected via USB or Bluetooth (not using XInput mode), it will be detected as a HID device.

### Q: Will microphone input keep the user "active"?
**A: No!** Microphones and audio input devices are intentionally excluded to prevent false positives.

### Q: Can I detect a custom Arduino USB device?
**A: Yes!** Any device that implements the HID protocol will be detected.

### Q: What about Bluetooth devices?
**A: Yes!** Bluetooth HID devices (controllers, mice, keyboards, etc.) are detected the same as USB devices.

---

## Technical Notes

### Performance
- Each HID device is polled every 100ms using non-blocking I/O
- Minimal CPU impact (<0.5% total for all monitoring)
- No impact on UI thread

### Device Discovery
- Devices are enumerated at startup
- Hot-plug detection: Currently requires app restart
- Future update may add dynamic device detection

### Thread Safety
- All device monitoring runs on separate threads
- Thread-safe communication with Flutter UI

---

## Troubleshooting

### Device Not Detected

1. **Check Device Manager**
   - Open Windows Device Manager
   - Look under "Human Interface Devices"
   - If your device isn't listed as HID, it may use a different protocol

2. **Enable Debug Mode**
   ```dart
   final windowFocus = WindowFocus(debug: true);
   ```
   Check console for device enumeration messages

3. **Check Usage Page**
   - If debug shows your device but it's skipped
   - Check if it's an audio device (Usage Page 0x0B or 0x0C)

4. **Verify Input Reports**
   - Device must have `InputReportByteLength > 0`
   - Output-only devices won't be detected

### Microphone Being Detected

If a microphone is somehow being detected (this shouldn't happen):
- File a bug report with debug output
- The device may be misidentifying its Usage Page

---

## Need More Help?

Open an issue on GitHub with:
- Debug output (enable debug mode)
- Device information from Device Manager
- What you expected vs. what happened