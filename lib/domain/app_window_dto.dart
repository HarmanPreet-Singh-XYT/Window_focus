
/// A data transfer object representing the active window information.
///
/// On **Windows**:
/// - [windowTitle] contains the title of the active window.
/// - [appName] contains the name of the application associated with the active window.
///
/// On **macOS**:
/// - [appName] contains the name of the application associated with the active window.
/// - [windowTitle] contains the title of the active window (if available).
///   Note: Accessing window titles on macOS might require Screen Recording permissions.
///   If the window title cannot be retrieved, it will fall back to the application name.
///
/// Example:
/// ```dart
/// final activeWindow = AppWindowDto(appName: "chrome.exe", windowTitle: "Google - Chrome");
/// print(activeWindow); // Output: Window title: Google - Chrome. AppName chrome.exe
/// ```
class AppWindowDto{
  /// The name of the application associated with the active window.
  final String appName;
  /// The title of the active window.
  final String windowTitle;
  /// The native window handle (HWND on Windows) observed at the moment this
  /// event was generated, as a raw integer address. Null on platforms where
  /// this isn't available (currently macOS). Callers that need to resolve
  /// richer per-window info (exe path, AUMID, etc.) should query against
  /// THIS exact handle rather than re-querying "the current foreground
  /// window" a moment later — focus can change again in the interim (e.g. a
  /// transient Search/Widgets overlay), causing a query against "whatever is
  /// foreground now" to silently resolve a different window than the one
  /// that actually triggered this event.
  final int? hwnd;

  /// Constructs an instance of [AppWindowDto].
  AppWindowDto({required this.appName, required this.windowTitle, this.hwnd});

  /// Returns a string representation of the active window details.
  @override
  String toString() {
    return 'Window title: $windowTitle. AppName $appName. Hwnd $hwnd';
  }

  /// Checks if two [AppWindowDto] objects are equal.
  @override
  bool operator ==(Object other) {
    if (identical(this, other)) return true;
    if (other is! AppWindowDto) return false;
    return other.appName == appName &&
        other.windowTitle == windowTitle &&
        other.hwnd == hwnd;
  }

  /// Returns a hash code for the [AppWindowDto] object.
  @override
  int get hashCode => Object.hash(appName, windowTitle, hwnd);

}