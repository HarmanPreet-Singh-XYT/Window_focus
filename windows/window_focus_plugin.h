#ifndef FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_
#define FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/encodable_value.h>

#include <Windows.h>
#include <xinput.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <hidsdi.h>

#include <memory>
#include <string>
#include <optional>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>

namespace window_focus {

class WindowFocusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  WindowFocusPlugin();
  virtual ~WindowFocusPlugin();

  // Disallow copy and assign.
  WindowFocusPlugin(const WindowFocusPlugin&) = delete;
  WindowFocusPlugin& operator=(const WindowFocusPlugin&) = delete;

  // Method channel
  std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;

  // Static members for hooks
  static WindowFocusPlugin* instance_;
  static HHOOK mouseHook_;
  static HHOOK keyboardHook_;
  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

  // Debug flag
  std::atomic<bool> enableDebug_{false};

  // Activity tracking
  std::atomic<bool> userIsActive_{true};
  std::chrono::steady_clock::time_point lastActivityTime;
  std::mutex activityMutex_;

  // Inactivity threshold in milliseconds
  int inactivityThreshold_ = 60000;

  // Controller monitoring
  std::atomic<bool> monitorControllers_{true};
  XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT];

  // Audio monitoring
  std::atomic<bool> monitorAudio_{false};
  float audioThreshold_ = 0.01f;

  // HID device monitoring
  std::atomic<bool> monitorHIDDevices_{true};
  std::vector<HANDLE> hidDeviceHandles_;
  std::vector<std::vector<BYTE>> lastHIDStates_;
  std::mutex hidDevicesMutex_;

  // Keyboard monitoring
  std::atomic<bool> monitorKeyboard_{true};
  std::atomic<uint64_t> lastKeyEventTime_{0};

  // Mouse tracking
  POINT lastMousePosition_;
  std::mutex mouseMutex_;

  // Channel mutex
  std::mutex channelMutex_;

  // Shutdown flag
  std::atomic<bool> isShuttingDown_;

  // Thread tracking and shutdown synchronization
  std::atomic<int> threadCount_;
  std::mutex shutdownMutex_;
  std::condition_variable shutdownCv_;

  // Methods
  void SetHooks();
  void RemoveHooks();
  void UpdateLastActivityTime();

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Safe method invocation helpers
  void SafeInvokeMethod(const std::string& methodName, const std::string& message);
  void SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap& data);

  // Input monitoring
  bool CheckControllerInput();
  bool CheckRawInput();
  bool CheckSystemAudio();
  bool CheckKeyboardInput();
  void InitializeHIDDevices();
  bool CheckHIDDevices();
  void CloseHIDDevices();
  void MonitorAllInputDevices();

  // Inactivity and focus
  void CheckForInactivity();
  void StartFocusListener();

  // Screenshot
  std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_