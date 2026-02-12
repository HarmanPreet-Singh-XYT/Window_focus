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
    bool monitorKeyboard = true,
    bool monitorMouse = true,
    bool monitorAudio = true,
    bool monitorControllers = true,
    bool monitorHIDDevices = true,
    double audioThreshold = 0.001,
  }) {
    _debug = debug;
    _channel.setMethodCallHandler(_handleMethodCall);

    // Initialize with error handling
    _initializePlugin(
      debug: debug,
      duration: duration,
      monitorKeyboard: monitorKeyboard,
      monitorMouse: monitorMouse,
      monitorAudio: monitorAudio,
      monitorControllers: monitorControllers,
      monitorHIDDevices: monitorHIDDevices,
      audioThreshold: audioThreshold,
    );
  }

  static const MethodChannel _channel =
      MethodChannel('expert.kotelnikoff/window_focus');
  bool _debug = false;
  bool _userActive = true;
  bool _isInitialized = false;

  final _focusChangeController = StreamController<AppWindowDto>.broadcast();
  final _userActiveController = StreamController<bool>.broadcast();
  final _errorController = StreamController<WindowFocusError>.broadcast();

  /// Stream of errors that occur in the plugin
  Stream<WindowFocusError> get onError => _errorController.stream;

  /// Initialize the plugin with error handling
  Future<void> _initializePlugin({
    required bool debug,
    required Duration duration,
    required bool monitorKeyboard,
    required bool monitorMouse,
    required bool monitorAudio,
    required bool monitorControllers,
    required bool monitorHIDDevices,
    required double audioThreshold,
  }) async {
    try {
      if (debug) {
        await setDebug(debug);
      }
      await setIdleThreshold(duration: duration);
      await setKeyboardMonitoring(monitorKeyboard);
      await setMouseMonitoring(monitorMouse);
      await setAudioMonitoring(monitorAudio);
      await setControllerMonitoring(monitorControllers);
      await setHIDMonitoring(monitorHIDDevices);
      await setAudioThreshold(audioThreshold);
      _isInitialized = true;

      if (_debug) {
        print('[WindowFocus] Plugin initialized successfully');
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.initialization,
          message: 'Failed to initialize plugin: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  Future<dynamic> _handleMethodCall(MethodCall call) async {
    try {
      switch (call.method) {
        case 'onFocusChange':
          _handleFocusChange(call);
          break;
        case 'onUserActiveChange':
          _handleUserActiveChange(call);
          break;
        case 'onUserActive':
          _handleUserActive();
          break;
        case 'onUserInactivity':
          _handleUserInactivity();
          break;
        default:
          if (_debug) {
            print('[WindowFocus] Unknown method from native: ${call.method}');
          }
          break;
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.methodCall,
          message: 'Error handling method call ${call.method}: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
    return null;
  }

  void _handleFocusChange(MethodCall call) {
    try {
      final arguments = call.arguments;
      if (arguments is Map) {
        final String appName = arguments['appName']?.toString() ?? '';
        final String windowTitle = arguments['windowTitle']?.toString() ?? '';
        final dto = AppWindowDto(appName: appName, windowTitle: windowTitle);

        if (!_focusChangeController.isClosed) {
          _focusChangeController.add(dto);
        }
      } else {
        if (_debug) {
          print(
              '[WindowFocus] Invalid arguments for onFocusChange: $arguments');
        }
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.focusChange,
          message: 'Error processing focus change: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  void _handleUserActiveChange(MethodCall call) {
    try {
      final bool active = call.arguments == true;
      _userActive = active;

      if (!_userActiveController.isClosed) {
        _userActiveController.add(_userActive);
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.activityChange,
          message: 'Error processing user active change: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  void _handleUserActive() {
    try {
      _userActive = true;

      if (!_userActiveController.isClosed) {
        _userActiveController.add(true);
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.activityChange,
          message: 'Error processing user active: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  void _handleUserInactivity() {
    try {
      _userActive = false;

      if (!_userActiveController.isClosed) {
        _userActiveController.add(false);
      }
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.activityChange,
          message: 'Error processing user inactivity: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  void _handleError(WindowFocusError error) {
    if (_debug) {
      print('[WindowFocus] Error: ${error.message}');
      if (error.stackTrace != null) {
        print('[WindowFocus] Stack trace: ${error.stackTrace}');
      }
    }

    if (!_errorController.isClosed) {
      _errorController.add(error);
    }
  }

  bool get isUserActive => _userActive;
  bool get isInitialized => _isInitialized;

  Stream<AppWindowDto> get onFocusChanged => _focusChangeController.stream;
  Stream<bool> get onUserActiveChanged => _userActiveController.stream;

  /// Takes a screenshot.
  Future<Uint8List?> takeScreenshot({bool activeWindowOnly = false}) async {
    try {
      final result = await _channel.invokeMethod<Uint8List>('takeScreenshot', {
        'activeWindowOnly': activeWindowOnly,
      });
      return result;
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.screenshot,
          message: 'Failed to take screenshot: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return null;
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.screenshot,
          message: 'Unexpected error taking screenshot: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return null;
    }
  }

  // ============================================================
  // SCREEN RECORDING PERMISSION
  // ============================================================

  /// Checks if the application has permission to record the screen.
  ///
  /// On macOS 10.15+, this requires user authorization in
  /// System Preferences > Security & Privacy > Privacy > Screen Recording.
  Future<bool> checkScreenRecordingPermission() async {
    try {
      final result =
          await _channel.invokeMethod<bool>('checkScreenRecordingPermission');
      return result ?? false;
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Failed to check screen recording permission: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return false;
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Unexpected error checking screen recording permission: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return false;
    }
  }

  /// Requests permission to record the screen.
  ///
  /// On macOS, this will trigger a system dialog or open the Privacy settings.
  Future<void> requestScreenRecordingPermission() async {
    try {
      await _channel.invokeMethod('requestScreenRecordingPermission');
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message:
              'Failed to request screen recording permission: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message:
              'Unexpected error requesting screen recording permission: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
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
  Future<bool> checkInputMonitoringPermission() async {
    try {
      final result =
          await _channel.invokeMethod<bool>('checkInputMonitoringPermission');
      return result ?? false;
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Failed to check input monitoring permission: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return false;
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Unexpected error checking input monitoring permission: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return false;
    }
  }

  /// Requests Input Monitoring permission.
  ///
  /// On macOS 10.15+, this will either:
  /// - Show the system permission dialog (if permission hasn't been requested yet)
  /// - Open System Settings/Preferences to the Input Monitoring section (if already denied)
  ///
  /// **Important**: After the user grants permission, the application typically
  /// needs to be restarted for the event tap to work properly.
  Future<void> requestInputMonitoringPermission() async {
    try {
      await _channel.invokeMethod('requestInputMonitoringPermission');
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message:
              'Failed to request input monitoring permission: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message:
              'Unexpected error requesting input monitoring permission: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Opens the Input Monitoring section in System Settings/Preferences.
  ///
  /// This is useful when you want to direct users to manually enable the permission
  /// without triggering the system permission request dialog.
  Future<void> openInputMonitoringSettings() async {
    try {
      await _channel.invokeMethod('openInputMonitoringSettings');
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.settings,
          message: 'Failed to open input monitoring settings: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.settings,
          message: 'Unexpected error opening input monitoring settings: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  // ============================================================
  // CHECK ALL PERMISSIONS
  // ============================================================

  /// Checks all required permissions at once.
  ///
  /// Returns a [PermissionStatus] object containing the status of all permissions.
  /// This is more efficient than calling each check individually.
  Future<PermissionStatus> checkAllPermissions() async {
    try {
      final result = await _channel.invokeMethod<Map>('checkAllPermissions');
      if (result != null) {
        return PermissionStatus(
          screenRecording: result['screenRecording'] as bool? ?? false,
          inputMonitoring: result['inputMonitoring'] as bool? ?? false,
        );
      }
      return PermissionStatus(screenRecording: false, inputMonitoring: false);
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Failed to check all permissions: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return PermissionStatus(screenRecording: false, inputMonitoring: false);
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.permission,
          message: 'Unexpected error checking all permissions: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return PermissionStatus(screenRecording: false, inputMonitoring: false);
    }
  }

  // ============================================================
  // IDLE THRESHOLD SETTINGS
  // ============================================================

  /// Sets the user inactivity timeout.
  Future<void> setIdleThreshold({required Duration duration}) async {
    try {
      await _channel.invokeMethod('setInactivityTimeOut', {
        'inactivityTimeOut': duration.inMilliseconds,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set idle threshold: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting idle threshold: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Returns the currently set inactivity timeout.
  Future<Duration> get idleThreshold async {
    try {
      final res = await _channel.invokeMethod<int>('getIdleThreshold');
      if (_debug) {
        print('[WindowFocus] Idle threshold: $res ms');
      }
      return Duration(milliseconds: res ?? 60000);
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to get idle threshold: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return const Duration(seconds: 60);
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error getting idle threshold: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
      return const Duration(seconds: 60);
    }
  }

  // ============================================================
  // DEBUG AND MONITORING SETTINGS
  // ============================================================

  /// Enables or disables debug mode for the plugin.
  Future<void> setDebug(bool value) async {
    try {
      _debug = value;
      await _channel.invokeMethod('setDebugMode', {
        'debug': value,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set debug mode: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting debug mode: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Enables or disables keyboard input monitoring.
  ///
  /// When enabled, keyboard input from any application (including when your app
  /// is not in focus) will be detected as user activity.
  ///
  /// Default: true
  Future<void> setKeyboardMonitoring(bool enabled) async {
    try {
      await _channel.invokeMethod('setKeyboardMonitoring', {
        'enabled': enabled,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set keyboard monitoring: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting keyboard monitoring: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Enables or disables mouse input monitoring.
  ///
  /// When enabled, mouse movements and clicks from any application (including when
  /// your app is not in focus) will be detected as user activity.
  ///
  /// Default: true
  Future<void> setMouseMonitoring(bool enabled) async {
    try {
      await _channel.invokeMethod('setMouseMonitoring', {
        'enabled': enabled,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set mouse monitoring: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting mouse monitoring: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Enables or disables controller/gamepad input monitoring.
  Future<void> setControllerMonitoring(bool enabled) async {
    try {
      await _channel.invokeMethod('setControllerMonitoring', {
        'enabled': enabled,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set controller monitoring: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting controller monitoring: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Enables or disables system audio monitoring.
  Future<void> setAudioMonitoring(bool enabled) async {
    try {
      await _channel.invokeMethod('setAudioMonitoring', {
        'enabled': enabled,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set audio monitoring: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting audio monitoring: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Sets the audio threshold for detecting user activity.
  Future<void> setAudioThreshold(double threshold) async {
    try {
      await _channel.invokeMethod('setAudioThreshold', {
        'threshold': threshold,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set audio threshold: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting audio threshold: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  /// Enables or disables HID device monitoring.
  Future<void> setHIDMonitoring(bool enabled) async {
    try {
      await _channel.invokeMethod('setHIDMonitoring', {
        'enabled': enabled,
      });
    } on PlatformException catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Failed to set HID monitoring: ${e.message}',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    } catch (e, stackTrace) {
      _handleError(
        WindowFocusError(
          type: WindowFocusErrorType.configuration,
          message: 'Unexpected error setting HID monitoring: $e',
          originalError: e,
          stackTrace: stackTrace,
        ),
      );
    }
  }

  // ============================================================
  // LISTENERS
  // ============================================================

  /// Adds a listener for active window changes.
  StreamSubscription<AppWindowDto> addFocusChangeListener(
      void Function(AppWindowDto) listener) {
    return onFocusChanged.listen(
      listener,
      onError: (error) {
        if (_debug) {
          print('[WindowFocus] Error in focus change listener: $error');
        }
      },
      cancelOnError: false,
    );
  }

  /// Adds a listener for user activity changes.
  StreamSubscription<bool> addUserActiveListener(void Function(bool) listener) {
    return onUserActiveChanged.listen(
      listener,
      onError: (error) {
        if (_debug) {
          print('[WindowFocus] Error in user active listener: $error');
        }
      },
      cancelOnError: false,
    );
  }

  /// Adds a listener for plugin errors.
  StreamSubscription<WindowFocusError> addErrorListener(
      void Function(WindowFocusError) listener) {
    return onError.listen(
      listener,
      onError: (error) {
        if (_debug) {
          print('[WindowFocus] Error in error listener: $error');
        }
      },
      cancelOnError: false,
    );
  }

  void dispose() {
    try {
      if (!_focusChangeController.isClosed) {
        _focusChangeController.close();
      }
      if (!_userActiveController.isClosed) {
        _userActiveController.close();
      }
      if (!_errorController.isClosed) {
        _errorController.close();
      }
    } catch (e) {
      if (_debug) {
        print('[WindowFocus] Error disposing: $e');
      }
    }
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

/// Types of errors that can occur in the WindowFocus plugin
enum WindowFocusErrorType {
  /// Error during plugin initialization
  initialization,

  /// Error handling method calls from native code
  methodCall,

  /// Error processing focus change events
  focusChange,

  /// Error processing activity change events
  activityChange,

  /// Error taking screenshots
  screenshot,

  /// Error checking or requesting permissions
  permission,

  /// Error opening settings
  settings,

  /// Error with configuration/settings
  configuration,

  /// Unknown or unexpected error
  unknown,
}

/// Represents an error that occurred in the WindowFocus plugin
class WindowFocusError {
  /// The type of error
  final WindowFocusErrorType type;

  /// Human-readable error message
  final String message;

  /// The original error object (if available)
  final Object? originalError;

  /// Stack trace (if available)
  final StackTrace? stackTrace;

  /// Timestamp when the error occurred
  final DateTime timestamp;

  WindowFocusError({
    required this.type,
    required this.message,
    this.originalError,
    this.stackTrace,
  }) : timestamp = DateTime.now();

  @override
  String toString() {
    final buffer = StringBuffer();
    buffer.writeln('WindowFocusError(');
    buffer.writeln('  type: $type,');
    buffer.writeln('  message: $message,');
    buffer.writeln('  timestamp: $timestamp,');
    if (originalError != null) {
      buffer.writeln('  originalError: $originalError,');
    }
    if (stackTrace != null) {
      buffer.writeln('  stackTrace: $stackTrace');
    }
    buffer.write(')');
    return buffer.toString();
  }
}
