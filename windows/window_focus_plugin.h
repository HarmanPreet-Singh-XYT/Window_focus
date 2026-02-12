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
#include <chrono>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <functional>

namespace window_focus {

class WindowFocusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  WindowFocusPlugin();
  virtual ~WindowFocusPlugin();

  WindowFocusPlugin(const WindowFocusPlugin&) = delete;
  WindowFocusPlugin& operator=(const WindowFocusPlugin&) = delete;

  std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Hook management
  void SetHooks();
  void RemoveHooks();
  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

  // Activity tracking
  void UpdateLastActivityTime();
  void CheckForInactivity();
  void StartFocusListener();

  // Input monitoring
  void MonitorAllInputDevices();
  bool CheckControllerInput();
  bool CheckRawInput();
  bool CheckKeyboardInput();
  bool CheckSystemAudio();

  // HID device management
  void InitializeHIDDevices();
  bool CheckHIDDevices();
  void CloseHIDDevices();

  // Screenshot
  std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);

  // Safe Flutter method invocation
  void SafeInvokeMethod(const std::string& methodName, const std::string& message);
  void SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap& data);

  // Singleton (atomic for thread safety)
  static std::atomic<WindowFocusPlugin*> instance_;
  static HHOOK mouseHook_;
  static HHOOK keyboardHook_;

  // Thread management - joinable threads instead of detached
  std::vector<std::thread> threads_;
  std::mutex threadsMutex_;

  // Shutdown coordination
  std::atomic<bool> isShuttingDown_;
  std::mutex shutdownMutex_;
  std::condition_variable shutdownCv_;

  // Activity state
  std::chrono::steady_clock::time_point lastActivityTime;
  std::mutex activityMutex_;
  std::atomic<bool> userIsActive_{true};
  int inactivityThreshold_ = 60000;

  // Keyboard monitoring
  std::atomic<bool> monitorKeyboard_{true};
  std::atomic<uint64_t> lastKeyEventTime_{0};

  // Mouse monitoring
  POINT lastMousePosition_ = {0, 0};
  std::mutex mouseMutex_;

  // Controller monitoring
  std::atomic<bool> monitorControllers_{false};
  XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT];

  // Audio monitoring
  std::atomic<bool> monitorAudio_{false};
  float audioThreshold_ = 0.01f;

  // HID device monitoring
  std::atomic<bool> monitorHIDDevices_{false};
  std::vector<HANDLE> hidDeviceHandles_;
  std::vector<std::vector<BYTE>> lastHIDStates_;
  std::mutex hidDevicesMutex_;

  // Flutter channel mutex
  std::mutex channelMutex_;

  // Debug mode
  std::atomic<bool> enableDebug_{false};
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_