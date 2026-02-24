#ifndef FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_
#define FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/encodable_value.h>

#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <functional>

namespace window_focus {

// Forward declaration — defined in .cpp
class AudioMeterCache;
class PlatformTaskDispatcher;

class WindowFocusPlugin : public std::enable_shared_from_this<WindowFocusPlugin> {
public:
    static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

    WindowFocusPlugin();
    virtual ~WindowFocusPlugin();

    // Prevent copy/move
    WindowFocusPlugin(const WindowFocusPlugin&) = delete;
    WindowFocusPlugin& operator=(const WindowFocusPlugin&) = delete;
    WindowFocusPlugin(WindowFocusPlugin&&) = delete;
    WindowFocusPlugin& operator=(WindowFocusPlugin&&) = delete;

    // ---- Public API used by PlatformTaskDispatcher for power resume ----
    bool IsShuttingDown() const;
    void OnSystemResume();

    // ---- Singleton instance (weak so destructor isn't blocked) ----
    // Public so PlatformTaskDispatcher WndProc can access them
    static std::weak_ptr<WindowFocusPlugin> instance_;
    static std::mutex instanceMutex_;

private:
    // Allow PlatformTaskDispatcher to access private members for power events
    friend class PlatformTaskDispatcher;

    // ---- Method channel ----
    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

    std::shared_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;
    std::mutex channelMutex_;

    // ---- Thread-safe method invocation ----
    void SafeInvokeMethod(const std::string& methodName, const std::string& message);
    void SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap data);
    void PostToMainThread(std::function<void()> task);

    // ---- Hooks ----
    static HHOOK mouseHook_;
    static HHOOK keyboardHook_;
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void SetHooks();
    void RemoveHooks();
    std::atomic<bool> hooksInstalled_{false};

    // ---- Activity tracking ----
    void UpdateLastActivityTime();
    std::chrono::steady_clock::time_point lastActivityTime;
    std::mutex activityMutex_;
    std::atomic<bool> userIsActive_{true};

    // ---- Input monitoring threads ----
    void MonitorAllInputDevices();
    void CheckForInactivity();
    void StartFocusListener();

    // ---- Joinable thread management ----
    std::vector<std::thread> threads_;
    std::mutex threadsMutex_;

    // ---- Shutdown ----
    std::atomic<bool> isShuttingDown_{false};
    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;

    // ---- Keyboard ----
    bool CheckKeyboardInput();
    bool PollKeyboardState();  // Fallback when hooks not installed
    std::atomic<uint64_t> lastKeyEventTime_{0};
    std::atomic<bool> monitorKeyboard_{true};

    // ---- Mouse ----
    bool CheckRawInput();
    POINT lastMousePosition_{};
    std::mutex mouseMutex_;

    // ---- Controller (XInput) ----
    bool CheckControllerInput();
    XINPUT_STATE lastControllerStates_[XUSER_MAX_COUNT]{};
    std::atomic<bool> monitorControllers_{false};

    // ---- Audio ----
    bool CheckSystemAudio();
    std::atomic<bool> monitorAudio_{false};
    std::atomic<float> audioThreshold_{0.01f};

    // FIX: Cached audio meter — pointer owned by monitor thread,
    //      accessed under audioMeterMutex_
    std::mutex audioMeterMutex_;
    AudioMeterCache* audioMeterCache_ = nullptr;  // Non-owning; monitor thread owns the object

    // FIX: Signal from power resume to invalidate audio cache
    std::atomic<bool> needsAudioCacheReset_{false};

    // ---- HID devices ----
    void InitializeHIDDevices();
    bool CheckHIDDevices();
    void CloseHIDDevices();
    std::vector<HANDLE> hidDeviceHandles_;
    std::vector<std::vector<BYTE>> lastHIDStates_;
    std::mutex hidDevicesMutex_;
    std::atomic<bool> monitorHIDDevices_{false};

    // FIX: Signal from power resume to reinitialize HID devices
    std::atomic<bool> needsHIDReinit_{false};

    // ---- Screenshot ----
    std::optional<std::vector<uint8_t>> TakeScreenshot(bool activeWindowOnly);
    std::mutex screenshotMutex_;

    // ---- Configuration ----
    std::atomic<bool> enableDebug_{false};
    std::atomic<int> inactivityThreshold_{300000};
};

}  // namespace window_focus

#endif  // FLUTTER_PLUGIN_WINDOW_FOCUS_PLUGIN_H_