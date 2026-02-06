import 'dart:async';
import 'package:flutter/services.dart';
import 'domain/domain.dart';

/// The WindowFocus plugin provides functionality for tracking user activity
/// and the currently active window on the Windows and macOS platforms.
class WindowFocus {
  /// Creates an instance of `WindowFocus` for tracking user activity and window focus.
  WindowFocus({
    bool debug = false,
    Duration duration = const Duration(seconds: 1),
    bool monitorAudio = true,
    bool monitorControllers = true,
    bool monitorHIDDevices = true,
    double audioThreshold = 0.001,
  }) {
    _debug = debug;
    _channel.setMethodCallHandler(_handleMethodCall);
    if (_debug) {
      setDebug(_debug);
    }
    setIdleThreshold(duration: duration);
    setAudioMonitoring(monitorAudio);
    setControllerMonitoring(monitorControllers);
    setHIDMonitoring(monitorHIDDevices);
    setAudioThreshold(audioThreshold);
  }

  static const MethodChannel _channel =
      MethodChannel('expert.kotelnikoff/window_focus');
  bool _debug = false;
  bool _userActive = true;

  final _focusChangeController = StreamController<AppWindowDto>.broadcast();
  final _userActiveController = StreamController<bool>.broadcast();

  Future<dynamic> _handleMethodCall(MethodCall call) async {
    switch (call.method) {
      case 'onFocusChange':
        final String appName = call.arguments['appName'] ?? '';
        final String windowTitle = call.arguments['windowTitle'] ?? '';
        final dto = AppWindowDto(appName: appName, windowTitle: windowTitle);
        _focusChangeController.add(dto);
        break;
      case 'onUserActiveChange':
        final bool active = call.arguments == true;
        _userActive = active;
        _userActiveController.add(_userActive);
        break;
      case 'onUserActive':
        _userActiveController.add(true);
        break;
      case 'onUserInactivity':
        _userActiveController.add(false);
        break;
      default:
        print('Unknown method from native: ${call.method}');
        break;
    }
    return null;
  }

  bool get isUserActive => _userActive;

  Stream<AppWindowDto> get onFocusChanged => _focusChangeController.stream;
  Stream<bool> get onUserActiveChanged => _userActiveController.stream;

  /// Takes a screenshot.
  Future<Uint8List?> takeScreenshot({bool activeWindowOnly = false}) async {
    return await _channel.invokeMethod<Uint8List>('takeScreenshot', {
      'activeWindowOnly': activeWindowOnly,
    });
  }

  // ============================================================
  // SCREEN RECORDING PERMISSION
  // ============================================================

  /// Checks if the application has permission to record the screen.
  ///
  /// On macOS 10.15+, this requires user authorization in
  /// System Preferences > Security & Privacy > Privacy > Screen Recording.
  Future<bool> checkScreenRecordingPermission() async {
    return await _channel
            .invokeMethod<bool>('checkScreenRecordingPermission') ??
        false;
  }

  /// Requests permission to record the screen.
  ///
  /// On macOS, this will trigger a system dialog or open the Privacy settings.
  Future<void> requestScreenRecordingPermission() async {
    await _channel.invokeMethod('requestScreenRecordingPermission');
  }

  // ============================================================
  // INPUT MONITORING PERMISSION (NEW)
  // ============================================================

  /// Checks if the application has Input Monitoring permission.
  ///
  /// On macOS 10.15+, this permission is required to monitor keyboard and mouse
  /// input from other applications. Without this permission, the event tap will
  /// fail to capture input when other apps are in focus.
  ///
  /// Returns `true` if permission is granted, `false` otherwise.
  /// On macOS versions before 10.15, always returns `true` as this permission
  /// didn't exist.
  ///
  /// ```dart
  /// final hasPermission = await _windowFocus.checkInputMonitoringPermission();
  /// if (!hasPermission) {
  ///   await _windowFocus.requestInputMonitoringPermission();
  /// }
  /// ```
  Future<bool> checkInputMonitoringPermission() async {
    return await _channel
            .invokeMethod<bool>('checkInputMonitoringPermission') ??
        false;
  }

  /// Requests Input Monitoring permission.
  ///
  /// On macOS 10.15+, this will either:
  /// - Show the system permission dialog (if permission hasn't been requested yet)
  /// - Open System Settings/Preferences to the Input Monitoring section (if already denied)
  ///
  /// **Important**: After the user grants permission, the application typically
  /// needs to be restarted for the event tap to work properly.
  ///
  /// ```dart
  /// await _windowFocus.requestInputMonitoringPermission();
  /// // Show a message to user to restart the app after granting permission
  /// ```
  Future<void> requestInputMonitoringPermission() async {
    await _channel.invokeMethod('requestInputMonitoringPermission');
  }

  /// Opens the Input Monitoring section in System Settings/Preferences.
  ///
  /// This is useful when you want to direct users to manually enable the permission
  /// without triggering the system permission request dialog.
  ///
  /// ```dart
  /// await _windowFocus.openInputMonitoringSettings();
  /// ```
  Future<void> openInputMonitoringSettings() async {
    await _channel.invokeMethod('openInputMonitoringSettings');
  }

  // ============================================================
  // CHECK ALL PERMISSIONS
  // ============================================================

  /// Checks all required permissions at once.
  ///
  /// Returns a [PermissionStatus] object containing the status of all permissions.
  /// This is more efficient than calling each check individually.
  ///
  /// ```dart
  /// final permissions = await _windowFocus.checkAllPermissions();
  /// if (!permissions.inputMonitoring) {
  ///   // Handle missing input monitoring permission
  /// }
  /// if (!permissions.screenRecording) {
  ///   // Handle missing screen recording permission
  /// }
  /// ```
  Future<PermissionStatus> checkAllPermissions() async {
    final result = await _channel.invokeMethod<Map>('checkAllPermissions');
    if (result != null) {
      return PermissionStatus(
        screenRecording: result['screenRecording'] as bool? ?? false,
        inputMonitoring: result['inputMonitoring'] as bool? ?? false,
      );
    }
    return PermissionStatus(screenRecording: false, inputMonitoring: false);
  }

  // ============================================================
  // IDLE THRESHOLD SETTINGS
  // ============================================================

  /// Sets the user inactivity timeout.
  Future<void> setIdleThreshold({required Duration duration}) async {
    await _channel.invokeMethod('setInactivityTimeOut', {
      'inactivityTimeOut': duration.inMilliseconds,
    });
  }

  /// Returns the currently set inactivity timeout.
  Future<Duration> get idleThreshold async {
    final res = await _channel.invokeMethod<int>('getIdleThreshold');
    print(res);
    return Duration(milliseconds: res ?? 60);
  }

  // ============================================================
  // DEBUG AND MONITORING SETTINGS
  // ============================================================

  /// Enables or disables debug mode for the plugin.
  Future<void> setDebug(bool value) async {
    _debug = value;
    await _channel.invokeMethod('setDebugMode', {
      'debug': value,
    });
  }

  /// Enables or disables controller/gamepad input monitoring.
  Future<void> setControllerMonitoring(bool enabled) async {
    await _channel.invokeMethod('setControllerMonitoring', {
      'enabled': enabled,
    });
  }

  /// Enables or disables system audio monitoring.
  Future<void> setAudioMonitoring(bool enabled) async {
    await _channel.invokeMethod('setAudioMonitoring', {
      'enabled': enabled,
    });
  }

  /// Sets the audio threshold for detecting user activity.
  Future<void> setAudioThreshold(double threshold) async {
    await _channel.invokeMethod('setAudioThreshold', {
      'threshold': threshold,
    });
  }

  /// Enables or disables HID device monitoring.
  Future<void> setHIDMonitoring(bool enabled) async {
    await _channel.invokeMethod('setHIDMonitoring', {
      'enabled': enabled,
    });
  }

  // ============================================================
  // LISTENERS
  // ============================================================

  /// Adds a listener for active window changes.
  StreamSubscription<AppWindowDto> addFocusChangeListener(
      void Function(AppWindowDto) listener) {
    return onFocusChanged.listen(listener);
  }

  /// Adds a listener for user activity changes.
  StreamSubscription<bool> addUserActiveListener(void Function(bool) listener) {
    return onUserActiveChanged.listen(listener);
  }

  void dispose() {
    _focusChangeController.close();
    _userActiveController.close();
  }
}

/// Represents the status of all required permissions.
class PermissionStatus {
  /// Whether screen recording permission is granted.
  final bool screenRecording;

  /// Whether input monitoring permission is granted.
  final bool inputMonitoring;

  PermissionStatus({
    required this.screenRecording,
    required this.inputMonitoring,
  });

  /// Returns true if all permissions are granted.
  bool get allGranted => screenRecording && inputMonitoring;

  @override
  String toString() {
    return 'PermissionStatus(screenRecording: $screenRecording, inputMonitoring: $inputMonitoring)';
  }
}
