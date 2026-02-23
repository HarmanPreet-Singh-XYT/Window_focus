#ifndef FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_
#define FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/encodable_value.h>
#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <optional>
#include <chrono>

namespace window_focus {

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);
std::string ConvertWindows1251ToUTF8(const std::string& windows1251_str);
std::string ConvertWStringToUTF8(const std::wstring& wstr);
std::string GetFocusedWindowTitle();
std::string GetFocusedWindowAppName();
std::string GetProcessName(DWORD processID);

// FIX: Inherit from std::enable_shared_from_this so background threads can
// safely extend the plugin's lifetime via shared_ptr rather than raw pointers.
class WindowFocusPlugin : public flutter::Plugin,
                          public std::enable_shared_from_this<WindowFocusPlugin> {
public:
    static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

    WindowFocusPlugin();
    virtual ~WindowFocusPlugin();

    // Disallow copy/move.
    WindowFocusPlugin(const WindowFocusPlugin&) = delete;
    WindowFocusPlugin& operator=(const WindowFocusPlugin&) = delete;

    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

    // Public so thread lambdas can call them after promoting the weak_ptr.
    void SafeInvokeMethod(const std::string& methodName, const std::string& message);
    void SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap data);
    void PostToMainThread(std::function<void()> task);
    void UpdateLastActivityTime();

    std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;

    // ---------- state accessed by threads ----------
    std::atomic<bool> isShuttingDown_;
    std::atomic<bool> userIsActive_;
    std::atomic<int>  threadCount_;
    std::atomic<uint64_t> lastKeyEventTime_{ 0 };

    bool enableDebug_ = false;
    bool monitorControllers_ = true;
    bool monitorAudio_ = true;
    bool monitorKeyboard_ = true;
    bool monitorHIDDevices_ = false;
    float audioThreshold_ = 0.01f;
    int inactivityThreshold_ = 60000; // ms

    std::chrono::steady_clock::time_point lastActivityTime;
    std::mutex activityMutex_;

    std::mutex channelMutex_;
    std::mutex screenshotMutex_;
    std::mutex mouseMutex_;

    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;

    POINT lastMousePosition_ = { 0, 0 };
    XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT] = {};

    std::vector<HANDLE> hidDeviceHandles_;
    std::vector<std::vector<BYTE>> lastHIDStates_;
    std::mutex hidDevicesMutex_;

private:
    // Hook callbacks (static so they can be passed to SetWindowsHookEx).
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    void SetHooks();
    void RemoveHooks();

    void CheckForInactivity();
    void StartFocusListener();
    void MonitorAllInputDevices();

    bool CheckControllerInput();
    bool CheckRawInput();
    bool CheckKeyboardInput();
    bool CheckSystemAudio();
    bool CheckHIDDevices();

    void InitializeHIDDevices();
    void CloseHIDDevices();

    std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);

    // FIX: weak_ptr + mutex replace the old raw atomic pointer.
    // Hooks promote this to a shared_ptr; if promotion fails the plugin is gone.
    static std::weak_ptr<WindowFocusPlugin> instance_;
    static std::mutex instanceMutex_;

    static HHOOK mouseHook_;
    static HHOOK keyboardHook_;
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_