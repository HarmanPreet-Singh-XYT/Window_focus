#ifndef FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_
#define FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <chrono>
#include <vector>
#include <mutex>
#include <atomic>
#include <Windows.h>
#include <xinput.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <hidsdi.h>

namespace window_focus {

class WindowFocusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  WindowFocusPlugin();

  virtual ~WindowFocusPlugin();

  // Disallow copy and assign.
  WindowFocusPlugin(const WindowFocusPlugin&) = delete;
  WindowFocusPlugin& operator=(const WindowFocusPlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void CheckForInactivity();
  void StartFocusListener();
  void MonitorAllInputDevices();
  void UpdateLastActivityTime();
  
  static void SetHooks();
  static void RemoveHooks();
  
  std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);

  // Hook procedures
  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);

  // Input monitoring methods
  bool CheckControllerInput();
  bool CheckRawInput();
  bool CheckSystemAudio();
  bool CheckHIDDevices();
  void InitializeHIDDevices();
  void CloseHIDDevices();

  std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;

 private:
  static WindowFocusPlugin* instance_;
  static HHOOK mouseHook_;

  // Thread safety
  std::mutex activityMutex_;
  std::mutex channelMutex_;
  std::mutex hidDevicesMutex_;
  std::mutex mouseMutex_;
  std::atomic<bool> isShuttingDown_;

  // Activity tracking
  std::chrono::steady_clock::time_point lastActivityTime;
  bool userIsActive_ = true;
  int inactivityThreshold_ = 60000; // Default 60 seconds

  // Debug mode
  bool enableDebug_ = false;

  // Controller monitoring
  bool monitorControllers_ = true;
  XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT];

  // Mouse monitoring
  POINT lastMousePosition_;

  // Audio monitoring
  bool monitorAudio_ = false;
  float audioThreshold_ = 0.01f; // 1% peak threshold

  // HID device monitoring
  bool monitorHIDDevices_ = false;
  std::vector<HANDLE> hidDeviceHandles_;
  std::vector<std::vector<BYTE>> lastHIDStates_;
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_