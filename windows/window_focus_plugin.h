#ifndef FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_
#define FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <chrono>
#include <memory>
#include <vector>
#include <windows.h>
#include <xinput.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <hidsdi.h>

#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace window_focus {

class WindowFocusPlugin : public flutter::Plugin {
 public:
  // Method for registering the plugin
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  // Constructor / Destructor
  WindowFocusPlugin();
  virtual ~WindowFocusPlugin();

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  // == 1) Singleton-like instance pointer
  static WindowFocusPlugin* instance_;

  // == 2) Static hooks and hook variables
  static HHOOK keyboardHook_;
  static HHOOK mouseHook_;
  
  static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);

  // == 3) Regular (non-static) fields
  std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;
  
  bool userIsActive_ = true;
  int inactivityThreshold_ = 1000;
  bool enableDebug_ = false;
  
  std::chrono::steady_clock::time_point lastActivityTime;
  
  // Controller/gamepad detection
  bool monitorControllers_ = true;
  XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT];
  POINT lastMousePosition_;
  
  // Audio detection
  bool monitorAudio_ = true;
  float audioThreshold_ = 0.001f; // Minimum audio level to consider as activity
  
  // HID device detection
  bool monitorHIDDevices_ = true;
  std::vector<HANDLE> hidDeviceHandles_;
  std::vector<std::vector<BYTE>> lastHIDStates_; // Store last state for each device

  // == 4) Internal methods
  void SetHooks();
  void RemoveHooks();
  void UpdateLastActivityTime();
  
  void CheckForInactivity();
  void StartFocusListener();
  
  // Input monitoring
  void MonitorAllInputDevices();
  bool CheckControllerInput();
  bool CheckRawInput();
  
  // NEW: Audio detection
  bool CheckSystemAudio();
  
  // NEW: HID device detection
  void InitializeHIDDevices();
  bool CheckHIDDevices();
  void CloseHIDDevices();
  
  std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_