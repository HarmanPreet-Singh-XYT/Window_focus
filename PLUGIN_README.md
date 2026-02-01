# Window Focus Plugin - Complete User Activity Detection

A comprehensive Flutter plugin for Windows that monitors user activity through multiple input sources including keyboard, mouse, game controllers, racing wheels, and system audio.

## Features

### Input Detection Methods

1. **Keyboard & Mouse** ✓
   - Global keyboard hooks
   - Mouse movement tracking
   - Click detection

2. **XInput Controllers** ✓
   - Xbox controllers
   - Xbox-compatible gamepads
   - Any XInput-compatible device

3. **HID Devices** ✓ NEW
   - Racing wheels (Logitech G29, G920, Thrustmaster, etc.)
   - Flight sticks and HOTAS controllers
   - Non-XInput game controllers
   - Drawing tablets (Wacom, Huion, XP-Pen, etc.)
   - Custom USB input devices
   - **ANY HID input device** (except microphones/audio devices)

4. **System Audio** ✓ NEW
   - Detects audio playback
   - Prevents idle state when watching videos/movies
   - Prevents idle state when listening to music
   - Configurable sensitivity threshold

5. **Window Focus Tracking** ✓
   - Monitors active window changes
   - Provides app name and window title

## Installation

Add to your `pubspec.yaml`:

```yaml
dependencies:
  window_focus:
    path: ../path/to/window_focus
```

## Usage

### Basic Setup

```dart
import 'package:window_focus/window_focus.dart';

// Create instance with default settings
final windowFocus = WindowFocus();

// Listen for user activity changes
windowFocus.addUserActiveListener((isActive) {
  if (isActive) {
    print('User is active');
  } else {
    print('User is inactive');
  }
});

// Listen for window focus changes
windowFocus.addFocusChangeListener((appWindow) {
  print('Active app: ${appWindow.appName}');
  print('Window title: ${appWindow.windowTitle}');
});
```

### Advanced Configuration

```dart
final windowFocus = WindowFocus(
  debug: true,                              // Enable debug logging
  duration: Duration(seconds: 30),          // Set idle timeout
  monitorAudio: true,                       // Monitor system audio
  monitorControllers: true,                 // Monitor XInput controllers
  monitorHIDDevices: true,                  // Monitor racing wheels, etc.
  audioThreshold: 0.001,                    // Audio sensitivity (0.0 - 1.0)
);
```

### Dynamic Configuration

You can enable/disable monitoring features at runtime:

```dart
// Disable audio monitoring
await windowFocus.setAudioMonitoring(false);

// Enable HID device monitoring
await windowFocus.setHIDMonitoring(true);

// Adjust audio sensitivity
// Lower = more sensitive (detects quieter sounds)
// Higher = less sensitive (only louder sounds)
await windowFocus.setAudioThreshold(0.005);

// Change inactivity timeout
await windowFocus.setIdleThreshold(duration: Duration(minutes: 5));
```

## Configuration Options

### Constructor Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `debug` | `bool` | `false` | Enable debug logging to console |
| `duration` | `Duration` | `1 second` | Inactivity timeout before user is marked idle |
| `monitorAudio` | `bool` | `true` | Monitor system audio playback |
| `monitorControllers` | `bool` | `true` | Monitor XInput controllers (Xbox controllers) |
| `monitorHIDDevices` | `bool` | `true` | Monitor HID devices (racing wheels, flight sticks) |
| `audioThreshold` | `double` | `0.001` | Minimum audio level to detect (0.0 - 1.0) |

### Runtime Methods

#### Audio Monitoring
```dart
// Enable/disable audio detection
await windowFocus.setAudioMonitoring(true);

// Adjust sensitivity
// 0.001 = Very sensitive (almost any audio)
// 0.01 = Moderately sensitive
// 0.1 = Less sensitive (only louder audio)
await windowFocus.setAudioThreshold(0.002);
```

#### Controller Monitoring
```dart
// Enable/disable XInput controller detection
await windowFocus.setControllerMonitoring(true);
```

#### HID Device Monitoring
```dart
// Enable/disable racing wheels, flight sticks, etc.
await windowFocus.setHIDMonitoring(true);
```

#### Other Settings
```dart
// Enable debug mode
await windowFocus.setDebug(true);

// Set inactivity timeout
await windowFocus.setIdleThreshold(duration: Duration(minutes: 2));

// Get current timeout
Duration timeout = await windowFocus.idleThreshold;
```

## Use Cases

### 1. Media Player Application
```dart
// Prevent idle when watching movies or listening to music
final windowFocus = WindowFocus(
  duration: Duration(minutes: 5),
  monitorAudio: true,          // Don't go idle during video/audio playback
  monitorControllers: false,   // Don't need controller detection
  monitorHIDDevices: false,    // Don't need racing wheel detection
);
```

### 2. Racing Game
```dart
// Detect all gaming inputs including racing wheels
final windowFocus = WindowFocus(
  duration: Duration(seconds: 30),
  monitorControllers: true,    // Xbox controllers
  monitorHIDDevices: true,     // Logitech G29, etc.
  monitorAudio: false,         // Don't count game audio as activity
);
```

### 3. Flight Simulator
```dart
// Detect flight stick and HOTAS controllers
final windowFocus = WindowFocus(
  duration: Duration(minutes: 1),
  monitorHIDDevices: true,     // Flight sticks, throttle controls
  monitorControllers: true,    // Xbox controller for secondary controls
);
```

### 4. Video Streaming App
```dart
// Maximum sensitivity for watching content
final windowFocus = WindowFocus(
  duration: Duration(minutes: 10),
  monitorAudio: true,
  audioThreshold: 0.0005,      // Very sensitive to quiet background audio
);
```

## Detected Devices

### HID Devices (New - ALL Input Devices)
The plugin now detects **ALL HID input devices** except microphones/audio devices:
- **Racing Wheels**: Logitech G29, G920, G923, Thrustmaster T150, T300, etc.
- **Flight Controllers**: Thrustmaster HOTAS, Logitech Flight Stick
- **Game Controllers**: Non-XInput gamepads and joysticks
- **Drawing Tablets**: Wacom, Huion, XP-Pen, etc.
- **Custom HID Devices**: Any custom USB input device
- **Other Input Devices**: Foot pedals, button boxes, stream decks, etc.

**Excluded**: Microphones, headsets, and other audio HID devices (to avoid detecting microphone input as user activity)

### XInput Devices
- Xbox One controllers
- Xbox 360 controllers
- Xbox Series X/S controllers
- Any XInput-compatible gamepad

### Audio Detection
- System default audio output
- Detects any audio playback (videos, music, games, system sounds)
- Configurable sensitivity threshold

## Technical Details

### Audio Detection
Uses Windows Core Audio API (`IAudioMeterInformation`) to monitor peak audio levels on the default audio output device. The `audioThreshold` parameter determines the minimum peak level to consider as activity.

### HID Device Detection
Uses Windows HID API and `SetupAPI` to enumerate and monitor **ALL HID input devices**. The plugin:
- Enumerates all HID devices on the system
- Filters out audio/telephony devices (Usage Page 0x0B and 0x0C) to exclude microphones
- Monitors any device with input capabilities (`InputReportByteLength > 0`)
- Uses non-blocking overlapped I/O for efficient polling
- Stores device state to detect changes

**Detected HID Categories:**
- Generic Desktop Controls (0x01): Mouse, keyboard, joystick, gamepad, multi-axis controllers
- Digitizers (0x0D): Drawing tablets, touch screens, pens
- Physical Interface Devices (0x0F): Force feedback devices
- Custom/Vendor-defined devices
- **Excluded**: Consumer devices (0x0C) and Telephony (0x0B) to avoid microphone detection

### Performance
- Audio monitoring: Checked every 100ms
- HID devices: Non-blocking overlapped I/O
- XInput controllers: State polling every 100ms
- Mouse position: Polled every 100ms
- Minimal CPU impact

### Thread Safety
All monitoring runs in separate threads using non-blocking I/O operations to prevent any impact on the UI thread.

## Troubleshooting

### HID Devices Not Detected

1. **Check Device Compatibility**
   - Device must be HID-compliant
   - Check if device appears in Device Manager under "Human Interface Devices"
   - Audio devices (microphones, headsets) are intentionally excluded

2. **Enable Debug Mode**
   ```dart
   final windowFocus = WindowFocus(debug: true);
   ```
   This will show which HID devices are detected at startup, including their Usage Page and Usage IDs.

3. **Verify Device Type**
   - The plugin detects ALL HID input devices except audio/telephony devices
   - This includes: game controllers, drawing tablets, custom USB devices, etc.
   - Keyboard and mouse are also detected (via separate hooks for better performance)

### Audio Not Detected

1. **Check Audio Threshold**
   - Try lowering the threshold: `await windowFocus.setAudioThreshold(0.0001);`
   - Very quiet audio might need a lower threshold

2. **Verify Default Audio Device**
   - The plugin monitors the system default audio output
   - Check Windows Sound settings

3. **Enable Debug Mode**
   - Debug logs will show audio peak levels

## Example Application

```dart
import 'package:flutter/material.dart';
import 'package:window_focus/window_focus.dart';

void main() {
  runApp(MyApp());
}

class MyApp extends StatefulWidget {
  @override
  _MyAppState createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  late WindowFocus _windowFocus;
  bool _isActive = true;
  String _currentApp = 'Unknown';

  @override
  void initState() {
    super.initState();
    
    _windowFocus = WindowFocus(
      debug: true,
      duration: Duration(seconds: 10),
      monitorAudio: true,
      monitorControllers: true,
      monitorHIDDevices: true,
      audioThreshold: 0.001,
    );

    _windowFocus.addUserActiveListener((isActive) {
      setState(() {
        _isActive = isActive;
      });
    });

    _windowFocus.addFocusChangeListener((appWindow) {
      setState(() {
        _currentApp = appWindow.appName;
      });
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: Text('Window Focus Demo'),
        ),
        body: Center(
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(
                'User Status: ${_isActive ? "Active" : "Inactive"}',
                style: TextStyle(
                  fontSize: 24,
                  color: _isActive ? Colors.green : Colors.red,
                ),
              ),
              SizedBox(height: 20),
              Text(
                'Current App: $_currentApp',
                style: TextStyle(fontSize: 18),
              ),
            ],
          ),
        ),
      ),
    );
  }

  @override
  void dispose() {
    _windowFocus.dispose();
    super.dispose();
  }
}
```