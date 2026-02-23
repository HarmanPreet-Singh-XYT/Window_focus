#include "window_focus_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <Windows.h>
#include <xinput.h>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <codecvt>
#include <locale>
#include <optional>
#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <sstream>
#include <vector>
#include <algorithm>
#include <gdiplus.h>
#include <setupapi.h>
#include <hidclass.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <queue>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace window_focus {

// Forward declaration to fix missing declaration before use in TakeScreenshot.
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);

// =====================================================================
// Plugin lifetime managed through shared_ptr / weak_ptr.
// =====================================================================
std::weak_ptr<WindowFocusPlugin> WindowFocusPlugin::instance_;
std::mutex WindowFocusPlugin::instanceMutex_;
HHOOK WindowFocusPlugin::mouseHook_ = nullptr;
HHOOK WindowFocusPlugin::keyboardHook_ = nullptr;

using CallbackMethod = std::function<void(const std::wstring&)>;

// =====================================================================
// SEH-isolated helper functions (no C++ objects with destructors allowed)
// =====================================================================

static bool ReadHIDDeviceSEH(HANDLE deviceHandle, BYTE* buffer, DWORD bufferSize,
                              OVERLAPPED* overlapped, DWORD* bytesRead, DWORD* outError) {
    *bytesRead = 0;
    *outError = ERROR_SUCCESS;
    __try {
        if (ReadFile(deviceHandle, buffer, bufferSize, bytesRead, overlapped)) {
            *outError = ERROR_SUCCESS;
            return true;
        } else {
            *outError = GetLastError();
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outError = GetExceptionCode();
        return false;
    }
}

static bool GetOverlappedResultSEH(HANDLE deviceHandle, OVERLAPPED* overlapped,
                                     DWORD* bytesRead, DWORD* outError) {
    *outError = ERROR_SUCCESS;
    __try {
        if (GetOverlappedResult(deviceHandle, overlapped, bytesRead, FALSE)) {
            return true;
        } else {
            *outError = GetLastError();
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *outError = GetExceptionCode();
        return false;
    }
}

static bool GetHIDAttributesSEH(HANDLE deviceHandle, HIDD_ATTRIBUTES* attributes) {
    __try {
        return HidD_GetAttributes(deviceHandle, attributes) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GetHIDPreparsedDataSEH(HANDLE deviceHandle, PHIDP_PREPARSED_DATA* preparsedData) {
    __try {
        return HidD_GetPreparsedData(deviceHandle, preparsedData) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GetHIDCapsSEH(PHIDP_PREPARSED_DATA preparsedData, HIDP_CAPS* caps) {
    __try {
        return (HidP_GetCaps(preparsedData, caps) == HIDP_STATUS_SUCCESS);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GetAudioPeakValueSEH(float* peakValue, bool* comErrorOut) {
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* defaultDevice = nullptr;
    IAudioMeterInformation* meterInfo = nullptr;
    bool success = false;

    *peakValue = 0.0f;
    *comErrorOut = false;

    __try {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)&deviceEnumerator
        );

        if (SUCCEEDED(hr) && deviceEnumerator) {
            hr = deviceEnumerator->GetDefaultAudioEndpoint(
                eRender, eConsole, &defaultDevice
            );
        }

        if (SUCCEEDED(hr) && defaultDevice) {
            hr = defaultDevice->Activate(
                __uuidof(IAudioMeterInformation),
                CLSCTX_ALL,
                nullptr,
                (void**)&meterInfo
            );
        }

        if (SUCCEEDED(hr) && meterInfo) {
            hr = meterInfo->GetPeakValue(peakValue);
            success = SUCCEEDED(hr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        success = false;
        *comErrorOut = true;
    }

    __try { if (meterInfo) meterInfo->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    __try { if (defaultDevice) defaultDevice->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    __try { if (deviceEnumerator) deviceEnumerator->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return success;
}

static DWORD XInputGetStateSEH(DWORD dwUserIndex, XINPUT_STATE* pState, bool* exceptionOccurred) {
    *exceptionOccurred = false;
    __try {
        return XInputGetState(dwUserIndex, pState);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionOccurred = true;
        return ERROR_DEVICE_NOT_CONNECTED;
    }
}

static HANDLE CreateHIDDeviceHandleSEH(const WCHAR* devicePath) {
    __try {
        return CreateFileW(
            devicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return INVALID_HANDLE_VALUE;
    }
}

static bool CloseHandleSEH(HANDLE handle) {
    __try {
        return CloseHandle(handle) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CancelIoSEH(HANDLE handle) {
    __try {
        return CancelIo(handle) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsHandleValid(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    __try {
        DWORD flags = 0;
        return GetHandleInformation(handle, &flags) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CheckKeyStateSEH(int vKey, bool* exceptionOccurred) {
    *exceptionOccurred = false;
    __try {
        SHORT state = GetAsyncKeyState(vKey);
        return (state & 0x8000) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionOccurred = true;
        return false;
    }
}

static bool GetKeyboardStateSEH(BYTE* keyState, bool* exceptionOccurred) {
    *exceptionOccurred = false;
    __try {
        return GetKeyboardState(keyState) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionOccurred = true;
        return false;
    }
}

// =====================================================================
// PlatformTaskDispatcher - thread-safe task marshalling to the UI thread.
// =====================================================================
class PlatformTaskDispatcher {
public:
    static PlatformTaskDispatcher& Get() {
        static PlatformTaskDispatcher instance;
        return instance;
    }

    void Initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hwnd_) return;

        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"WFPluginDispatcher";
        RegisterClassEx(&wc);

        hwnd_ = CreateWindowEx(0, L"WFPluginDispatcher", nullptr, 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               GetModuleHandle(nullptr), nullptr);

        currentGeneration_++;
    }

    void Shutdown() {
        HWND hwndToDestroy = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hwnd_) return;
            hwndToDestroy = hwnd_;
            hwnd_ = nullptr;
            currentGeneration_++;
        }

        MSG msg;
        while (PeekMessage(&msg, hwndToDestroy, WM_APP + 1, WM_APP + 1, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        DestroyWindow(hwndToDestroy);
    }

    void PostTask(std::function<void()> task) {
        HWND hwnd = nullptr;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hwnd = hwnd_;
            generation = currentGeneration_;
        }
        if (!hwnd) return;

        struct TaskPacket {
            std::function<void()> fn;
            uint64_t generation;
        };
        auto* packet = new TaskPacket{ std::move(task), generation };
        if (!PostMessage(hwnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(packet))) {
            delete packet;
        }
    }

private:
    HWND hwnd_ = nullptr;
    std::mutex mutex_;
    std::atomic<uint64_t> currentGeneration_{ 0 };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_APP + 1) {
            struct TaskPacket {
                std::function<void()> fn;
                uint64_t generation;
            };
            auto* packet = reinterpret_cast<TaskPacket*>(lParam);
            if (packet) {
                uint64_t live = PlatformTaskDispatcher::Get().currentGeneration_.load();
                if (packet->generation == live) {
                    try { packet->fn(); } catch (...) {}
                }
                delete packet;
            }
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
};


// =====================================================================
// Hook callbacks
// =====================================================================
LRESULT CALLBACK WindowFocusPlugin::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        std::shared_ptr<WindowFocusPlugin> inst;
        {
            std::lock_guard<std::mutex> lock(instanceMutex_);
            inst = instance_.lock();
        }

        if (inst && !inst->isShuttingDown_.load(std::memory_order_acquire)) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                KBDLLHOOKSTRUCT* pKeyboard = (KBDLLHOOKSTRUCT*)lParam;
                // FIX: enableDebug_ is now std::atomic<bool>, safe to read from any thread.
                if (inst->enableDebug_.load(std::memory_order_relaxed) && pKeyboard) {
                    std::cout << "[WindowFocus] Keyboard hook: key down vkCode="
                              << pKeyboard->vkCode << std::endl;
                }

                inst->UpdateLastActivityTime();

                auto now = std::chrono::steady_clock::now();
                auto epoch = now.time_since_epoch();
                inst->lastKeyEventTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();

                if (!inst->userIsActive_.load(std::memory_order_relaxed)) {
                    inst->userIsActive_.store(true, std::memory_order_relaxed);

                    std::weak_ptr<WindowFocusPlugin> weak = inst;
                    inst->PostToMainThread([weak]() {
                        if (auto p = weak.lock()) {
                            p->SafeInvokeMethod("onUserActive", "User is active");
                        }
                    });
                }
            }
        }
    }
    return CallNextHookEx(keyboardHook_, nCode, wParam, lParam);
}

LRESULT CALLBACK WindowFocusPlugin::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        std::shared_ptr<WindowFocusPlugin> inst;
        {
            std::lock_guard<std::mutex> lock(instanceMutex_);
            inst = instance_.lock();
        }

        if (inst && !inst->isShuttingDown_.load(std::memory_order_acquire)) {
            // FIX: enableDebug_ is now std::atomic<bool>.
            if (inst->enableDebug_.load(std::memory_order_relaxed)) {
                std::cout << "[WindowFocus] mouse hook detected action" << std::endl;
            }
            inst->UpdateLastActivityTime();
            if (!inst->userIsActive_.load(std::memory_order_relaxed)) {
                inst->userIsActive_.store(true, std::memory_order_relaxed);

                std::weak_ptr<WindowFocusPlugin> weak = inst;
                inst->PostToMainThread([weak]() {
                    if (auto p = weak.lock()) {
                        p->SafeInvokeMethod("onUserActive", "User is active");
                    }
                });
            }
        }
    }
    return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

void WindowFocusPlugin::SetHooks() {
    // FIX: enableDebug_ is now std::atomic<bool>.
    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] SetHooks: start\n";
    }
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, hInstance, 0);
    if (!mouseHook_) {
        std::cerr << "[WindowFocus] Failed to install mouse hook: " << GetLastError() << std::endl;
    } else if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] Mouse hook installed successfully\n";
    }

    keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
    if (!keyboardHook_) {
        std::cerr << "[WindowFocus] Failed to install keyboard hook: " << GetLastError() << std::endl;
    } else if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] Keyboard hook installed successfully\n";
    }
}

void WindowFocusPlugin::RemoveHooks() {
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
}

void WindowFocusPlugin::UpdateLastActivityTime() {
    std::lock_guard<std::mutex> lock(activityMutex_);
    lastActivityTime = std::chrono::steady_clock::now();
}

void WindowFocusPlugin::PostToMainThread(std::function<void()> task) {
    PlatformTaskDispatcher::Get().PostTask(std::move(task));
}

void WindowFocusPlugin::SafeInvokeMethod(const std::string& methodName, const std::string& message) {
    if (isShuttingDown_.load(std::memory_order_acquire)) return;

    try {
        std::lock_guard<std::mutex> lock(channelMutex_);
        if (channel && !isShuttingDown_.load(std::memory_order_acquire)) {
            channel->InvokeMethod(
                methodName,
                std::make_unique<flutter::EncodableValue>(message));
        }
    } catch (...) {
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cerr << "[WindowFocus] Exception invoking method: " << methodName << std::endl;
        }
    }
}

void WindowFocusPlugin::SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap data) {
    if (isShuttingDown_.load(std::memory_order_acquire)) return;

    try {
        std::lock_guard<std::mutex> lock(channelMutex_);
        if (channel && !isShuttingDown_.load(std::memory_order_acquire)) {
            channel->InvokeMethod(
                methodName,
                std::make_unique<flutter::EncodableValue>(std::move(data)));
        }
    } catch (...) {
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cerr << "[WindowFocus] Exception invoking method with map: " << methodName << std::endl;
        }
    }
}

std::string ConvertWindows1251ToUTF8(const std::string& windows1251_str) {
    if (windows1251_str.empty()) return std::string();

    int size_needed = MultiByteToWideChar(1251, 0, windows1251_str.c_str(), -1, NULL, 0);
    if (size_needed <= 0) return std::string();

    std::wstring utf16_str(size_needed, 0);
    MultiByteToWideChar(1251, 0, windows1251_str.c_str(), -1, &utf16_str[0], size_needed);

    size_needed = WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return std::string();

    std::string utf8_str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), -1, &utf8_str[0], size_needed, NULL, NULL);

    return utf8_str;
}

std::string ConvertWStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return std::string();

    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// static
void WindowFocusPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {

    PlatformTaskDispatcher::Get().Initialize();

    auto channel = std::make_shared<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(),
        "expert.kotelnikoff/window_focus",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_shared<WindowFocusPlugin>();
    plugin->channel = channel;

    {
        std::lock_guard<std::mutex> lock(instanceMutex_);
        instance_ = plugin;
    }

    plugin->SetHooks();
    plugin->CheckForInactivity();
    plugin->StartFocusListener();
    plugin->MonitorAllInputDevices();

    channel->SetMethodCallHandler(
        [plugin_weak = std::weak_ptr<WindowFocusPlugin>(plugin)](
            const auto& call,
            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
            if (auto p = plugin_weak.lock()) {
                p->HandleMethodCall(call, std::move(result));
            } else {
                result->Error("PLUGIN_DESTROYED", "Plugin has been unregistered.");
            }
        }
    );

    struct SharedOwner : public flutter::Plugin {
        std::shared_ptr<WindowFocusPlugin> ptr;
        explicit SharedOwner(std::shared_ptr<WindowFocusPlugin> p) : ptr(std::move(p)) {}
    };
    registrar->AddPlugin(std::make_unique<SharedOwner>(plugin));
}

std::string GetFocusedWindowTitle() {
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL) {
        return "";
    }

    int length = GetWindowTextLength(hwnd);
    if (length == 0) {
        return "";
    }

    std::string buffer(length + 1, '\0');
    GetWindowTextA(hwnd, buffer.data(), length + 1);
    buffer.resize(length);
    return buffer;
}

WindowFocusPlugin::WindowFocusPlugin()
    : isShuttingDown_(false)
    , threadCount_(0)
    , userIsActive_(true)
    // FIX: All monitoring flags and enableDebug_ are now std::atomic<bool>
    // so they can be safely written from the main thread and read from
    // background threads without a data race (which was UB and could crash).
    , monitorControllers_(false)
    , monitorAudio_(false)
    , monitorHIDDevices_(false)
    , monitorKeyboard_(true)
    , enableDebug_(false)
    , inactivityThreshold_(300000)   // 5 minutes default
    , audioThreshold_(0.01f)
    , lastKeyEventTime_(0) {

    lastActivityTime = std::chrono::steady_clock::now();
    ZeroMemory(lastControllerStates_, sizeof(lastControllerStates_));
    GetCursorPos(&lastMousePosition_);
}

WindowFocusPlugin::~WindowFocusPlugin() {
    isShuttingDown_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(instanceMutex_);
        if (instance_.lock().get() == this) {
            instance_.reset();
        }
    }

    RemoveHooks();

    // Signal all background threads to exit.
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        shutdownCv_.notify_all();
    }

    auto waitStart = std::chrono::steady_clock::now();
    while (threadCount_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - waitStart;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 5000) {
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cerr << "[WindowFocus] Timeout waiting for threads. Remaining: "
                          << threadCount_.load() << std::endl;
            }
            break;
        }
    }

    CloseHIDDevices();

    // Drain and shut down dispatcher AFTER background threads have exited.
    PlatformTaskDispatcher::Get().Shutdown();

    {
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel = nullptr;
    }
}

void WindowFocusPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    const auto& method_name = method_call.method_name();

    if (method_name == "setDebugMode") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("debug"));
            if (it != args->end()) {
                if (std::holds_alternative<bool>(it->second)) {
                    // FIX: atomic store — safe for background threads to read concurrently.
                    bool newDebugValue = std::get<bool>(it->second);
                    enableDebug_.store(newDebugValue, std::memory_order_relaxed);
                    std::cout << "[WindowFocus] C++: enableDebug_ set to " << (newDebugValue ? "true" : "false") << std::endl;
                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'debug'.");
        return;
    }

    if (method_name == "setControllerMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end()) {
                if (std::holds_alternative<bool>(it->second)) {
                    // FIX: atomic store.
                    monitorControllers_.store(std::get<bool>(it->second), std::memory_order_relaxed);
                    std::cout << "[WindowFocus] Controller monitoring set to " << (monitorControllers_.load() ? "true" : "false") << std::endl;
                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setAudioMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end()) {
                if (std::holds_alternative<bool>(it->second)) {
                    // FIX: atomic store.
                    monitorAudio_.store(std::get<bool>(it->second), std::memory_order_relaxed);
                    std::cout << "[WindowFocus] Audio monitoring set to " << (monitorAudio_.load() ? "true" : "false") << std::endl;
                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setAudioThreshold") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("threshold"));
            if (it != args->end()) {
                if (std::holds_alternative<double>(it->second)) {
                    // FIX: audioThreshold_ is now std::atomic<float>.
                    audioThreshold_.store(static_cast<float>(std::get<double>(it->second)), std::memory_order_relaxed);
                    std::cout << "[WindowFocus] Audio threshold set to " << audioThreshold_.load() << std::endl;
                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a double for 'threshold'.");
        return;
    }

    if (method_name == "setHIDMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end()) {
                if (std::holds_alternative<bool>(it->second)) {
                    bool newValue = std::get<bool>(it->second);
                    // FIX: atomic store before calling Init/Close so the
                    // background thread sees the updated value immediately.
                    bool oldValue = monitorHIDDevices_.exchange(newValue, std::memory_order_acq_rel);
                    if (newValue && !oldValue) {
                        InitializeHIDDevices();
                    } else if (!newValue && oldValue) {
                        CloseHIDDevices();
                    }
                    std::cout << "[WindowFocus] HID device monitoring set to " << (newValue ? "true" : "false") << std::endl;
                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setKeyboardMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end()) {
                if (std::holds_alternative<bool>(it->second)) {
                    bool newValue = std::get<bool>(it->second);
                    // FIX: atomic store.
                    bool oldValue = monitorKeyboard_.exchange(newValue, std::memory_order_acq_rel);
                    std::cout << "[WindowFocus] Keyboard monitoring set to " << (newValue ? "true" : "false") << std::endl;

                    if (newValue && !oldValue && !keyboardHook_) {
                        HINSTANCE hInstance = GetModuleHandle(nullptr);
                        keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
                        if (!keyboardHook_) {
                            std::cerr << "[WindowFocus] Failed to install keyboard hook: " << GetLastError() << std::endl;
                        }
                    } else if (!newValue && oldValue && keyboardHook_) {
                        UnhookWindowsHookEx(keyboardHook_);
                        keyboardHook_ = nullptr;
                    }

                    result->Success();
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setInactivityTimeOut") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("inactivityTimeOut"));
            if (it != args->end()) {
                if (std::holds_alternative<int>(it->second)) {
                    // FIX: inactivityThreshold_ is now std::atomic<int>.
                    inactivityThreshold_.store(std::get<int>(it->second), std::memory_order_relaxed);
                    std::cout << "Updated inactivityThreshold_ to " << inactivityThreshold_.load() << std::endl;
                    result->Success(flutter::EncodableValue(inactivityThreshold_.load()));
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected an integer argument.");
    } else if (method_name == "getPlatformVersion") {
        result->Success(flutter::EncodableValue("Windows: example"));
    } else if (method_name == "getIdleThreshold") {
        result->Success(flutter::EncodableValue(inactivityThreshold_.load(std::memory_order_relaxed)));
    } else if (method_name == "takeScreenshot") {
        bool activeWindowOnly = false;
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("activeWindowOnly"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                activeWindowOnly = std::get<bool>(it->second);
            }
        }
        auto screenshot = TakeScreenshot(activeWindowOnly);
        if (screenshot.has_value()) {
            result->Success(flutter::EncodableValue(screenshot.value()));
        } else {
            result->Error("SCREENSHOT_ERROR", "Failed to take screenshot");
        }
    } else if (method_name == "checkScreenRecordingPermission") {
        result->Success(flutter::EncodableValue(true));
    } else if (method_name == "requestScreenRecordingPermission") {
        result->Success();
    } else {
        result->NotImplemented();
    }
}

std::string GetProcessName(DWORD processID) {
    std::wstring processName = L"<unknown>";

    HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(hProcessSnap, &pe32)) {
            do {
                if (pe32.th32ProcessID == processID) {
                    processName = pe32.szExeFile;
                    break;
                }
            } while (Process32NextW(hProcessSnap, &pe32));
        }
        CloseHandle(hProcessSnap);
    }

    return ConvertWStringToUTF8(processName);
}

std::string GetFocusedWindowAppName() {
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL) {
        return "<no window in focus>";
    }

    DWORD processID;
    GetWindowThreadProcessId(hwnd, &processID);

    return GetProcessName(processID);
}

bool WindowFocusPlugin::CheckControllerInput() {
    // FIX: atomic load — no race with main thread writing monitorControllers_.
    if (!monitorControllers_.load(std::memory_order_relaxed) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    bool inputDetected = false;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) break;

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));

        bool exceptionOccurred = false;
        DWORD result = XInputGetStateSEH(i, &state, &exceptionOccurred);

        if (exceptionOccurred) {
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cerr << "[WindowFocus] Exception reading controller " << i << std::endl;
            }
            continue;
        }

        if (result == ERROR_SUCCESS) {
            if (state.dwPacketNumber != lastControllerStates_[i].dwPacketNumber) {
                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] Controller " << i << " input detected" << std::endl;
                }
                inputDetected = true;
                lastControllerStates_[i] = state;
            }
        }
    }

    return inputDetected;
}

bool WindowFocusPlugin::CheckRawInput() {
    if (isShuttingDown_.load(std::memory_order_acquire)) return false;

    POINT currentMousePos;
    if (GetCursorPos(&currentMousePos)) {
        std::lock_guard<std::mutex> lock(mouseMutex_);
        if (currentMousePos.x != lastMousePosition_.x || currentMousePos.y != lastMousePosition_.y) {
            lastMousePosition_ = currentMousePos;
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cout << "[WindowFocus] Mouse movement detected via cursor position" << std::endl;
            }
            return true;
        }
    }

    return false;
}

bool WindowFocusPlugin::CheckKeyboardInput() {
    // FIX: atomic load.
    if (!monitorKeyboard_.load(std::memory_order_relaxed) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    auto epoch = now.time_since_epoch();
    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
    uint64_t lastKeyTime = lastKeyEventTime_.load();

    if (lastKeyTime > 0 && (currentTime - lastKeyTime) < 200) {
        return true;
    }

    bool exceptionOccurred = false;

    for (int vk = 0x41; vk <= 0x5A; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) return true;
        }
        if (exceptionOccurred) break;
    }

    for (int vk = 0x30; vk <= 0x39; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) return true;
        }
        if (exceptionOccurred) break;
    }

    for (int vk = VK_F1; vk <= VK_F12; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) return true;
        }
        if (exceptionOccurred) break;
    }

    int specialKeys[] = {
        VK_SPACE, VK_RETURN, VK_TAB, VK_ESCAPE, VK_BACK, VK_DELETE,
        VK_SHIFT, VK_CONTROL, VK_MENU,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
        VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
        VK_INSERT, VK_SNAPSHOT, VK_SCROLL, VK_PAUSE,
        VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
        VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
        VK_MULTIPLY, VK_ADD, VK_SUBTRACT, VK_DECIMAL, VK_DIVIDE,
        VK_NUMLOCK, VK_CAPITAL,
        VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
        VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
        VK_LWIN, VK_RWIN, VK_APPS
    };

    int numSpecialKeys = sizeof(specialKeys) / sizeof(specialKeys[0]);
    for (int i = 0; i < numSpecialKeys; i++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(specialKeys[i], &exceptionOccurred)) {
            if (!exceptionOccurred) return true;
        }
        if (exceptionOccurred) break;
    }

    return false;
}

bool WindowFocusPlugin::CheckSystemAudio() {
    // FIX: atomic load.
    if (!monitorAudio_.load(std::memory_order_relaxed) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    // FIX: Remove thread_local ComGuard with destructor. Instead, initialise
    // COM once at thread start (in MonitorAllInputDevices) and uninitialise it
    // when that thread exits. This avoids a destructor running on a detached
    // thread after the plugin has been torn down.
    //
    // The calling thread (MonitorAllInputDevices) now owns COM lifetime via
    // the comInitialized flag passed in. Here we just call into the SEH helper.

    float peakValue = 0.0f;
    bool comError = false;
    bool success = GetAudioPeakValueSEH(&peakValue, &comError);

    if (comError && enableDebug_.load(std::memory_order_relaxed)) {
        std::cerr << "[WindowFocus] Exception in CheckSystemAudio" << std::endl;
    }

    // FIX: atomic load for audioThreshold_.
    if (success && peakValue > audioThreshold_.load(std::memory_order_relaxed)) {
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cout << "[WindowFocus] Audio detected, peak: " << peakValue << std::endl;
        }
        return true;
    }

    return false;
}

void WindowFocusPlugin::InitializeHIDDevices() {
    std::lock_guard<std::mutex> lock(hidDevicesMutex_);

    for (HANDLE handle : hidDeviceHandles_) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CancelIoSEH(handle);
            CloseHandleSEH(handle);
        }
    }
    hidDeviceHandles_.clear();
    lastHIDStates_.clear();

    if (isShuttingDown_.load(std::memory_order_acquire)) return;

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(
        &hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cerr << "[WindowFocus] Failed to get HID device info set" << std::endl;
        }
        return;
    }

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    DWORD memberIndex = 0;
    while (!isShuttingDown_.load(std::memory_order_acquire) && SetupDiEnumDeviceInterfaces(
        deviceInfoSet, nullptr, &hidGuid, memberIndex++, &deviceInterfaceData
    )) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet, &deviceInterfaceData,
            nullptr, 0, &requiredSize, nullptr
        );

        if (requiredSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA detailData =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
        if (!detailData) continue;

        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet, &deviceInterfaceData,
            detailData, requiredSize, nullptr, nullptr
        )) {
            HANDLE deviceHandle = CreateHIDDeviceHandleSEH(detailData->DevicePath);

            if (deviceHandle != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attributes;
                attributes.Size = sizeof(HIDD_ATTRIBUTES);

                if (GetHIDAttributesSEH(deviceHandle, &attributes)) {
                    PHIDP_PREPARSED_DATA preparsedData = nullptr;

                    if (GetHIDPreparsedDataSEH(deviceHandle, &preparsedData)) {
                        HIDP_CAPS caps;
                        ZeroMemory(&caps, sizeof(caps));

                        if (GetHIDCapsSEH(preparsedData, &caps)) {
                            bool isAudioDevice = (caps.UsagePage == 0x0B || caps.UsagePage == 0x0C);
                            bool isKeyboard    = (caps.UsagePage == 0x01 && caps.Usage == 0x06);
                            bool isMouse       = (caps.UsagePage == 0x01 && caps.Usage == 0x02);

                            if (!isAudioDevice && !isKeyboard && !isMouse && caps.InputReportByteLength > 0) {
                                hidDeviceHandles_.push_back(deviceHandle);
                                lastHIDStates_.push_back(std::vector<BYTE>(caps.InputReportByteLength, 0));

                                if (enableDebug_.load(std::memory_order_relaxed)) {
                                    std::cout << "[WindowFocus] HID device added: VID="
                                              << std::hex << attributes.VendorID
                                              << " PID=" << attributes.ProductID
                                              << std::dec << std::endl;
                                }
                            } else {
                                CloseHandleSEH(deviceHandle);
                            }
                        } else {
                            CloseHandleSEH(deviceHandle);
                        }

                        if (preparsedData) {
                            __try { HidD_FreePreparsedData(preparsedData); }
                            __except (EXCEPTION_EXECUTE_HANDLER) {}
                        }
                    } else {
                        CloseHandleSEH(deviceHandle);
                    }
                } else {
                    CloseHandleSEH(deviceHandle);
                }
            }
        }

        free(detailData);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] Initialized " << hidDeviceHandles_.size() << " HID devices" << std::endl;
    }
}

bool WindowFocusPlugin::CheckHIDDevices() {
    // FIX: atomic load.
    if (!monitorHIDDevices_.load(std::memory_order_relaxed) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(hidDevicesMutex_);

    if (hidDeviceHandles_.empty()) {
        return false;
    }

    bool inputDetected = false;
    std::vector<size_t> invalidDevices;

    for (size_t i = 0; i < hidDeviceHandles_.size(); i++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) break;

        HANDLE deviceHandle = hidDeviceHandles_[i];

        if (!IsHandleValid(deviceHandle)) {
            invalidDevices.push_back(i);
            continue;
        }

        std::vector<BYTE>& lastState = lastHIDStates_[i];
        if (lastState.empty()) continue;

        std::vector<BYTE> buffer(lastState.size(), 0);

        HANDLE hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (hEvent == nullptr) {
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cerr << "[WindowFocus] Failed to create event for HID device " << i << std::endl;
            }
            continue;
        }

        OVERLAPPED* pOvl = new OVERLAPPED{};
        pOvl->hEvent = hEvent;

        DWORD bytesRead = 0;
        DWORD bufferSize = static_cast<DWORD>(buffer.size());
        DWORD errorCode = 0;

        bool readSucceeded = ReadHIDDeviceSEH(
            deviceHandle, buffer.data(), bufferSize,
            pOvl, &bytesRead, &errorCode);

        auto cleanupOvl = [&]() {
            CancelIoSEH(deviceHandle);
            WaitForSingleObject(hEvent, 200);
            delete pOvl;
            CloseHandleSEH(hEvent);
        };

        if (readSucceeded) {
            if (bytesRead > 0 && buffer != lastState) {
                inputDetected = true;
                lastState = buffer;
                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] HID device " << i << " input detected" << std::endl;
                }
            }
            delete pOvl;
            CloseHandleSEH(hEvent);
        } else if (errorCode == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForSingleObject(hEvent, 10);
            if (waitResult == WAIT_OBJECT_0) {
                DWORD overlappedError = 0;
                if (GetOverlappedResultSEH(deviceHandle, pOvl, &bytesRead, &overlappedError)) {
                    if (bytesRead > 0 && buffer != lastState) {
                        inputDetected = true;
                        lastState = buffer;
                        if (enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "[WindowFocus] HID device " << i << " input detected (overlapped)" << std::endl;
                        }
                    }
                } else if (overlappedError == ERROR_INVALID_HANDLE ||
                           overlappedError == ERROR_DEVICE_NOT_CONNECTED) {
                    invalidDevices.push_back(i);
                }
                delete pOvl;
                CloseHandleSEH(hEvent);
            } else {
                cleanupOvl();
                if (waitResult == WAIT_FAILED) {
                    if (enableDebug_.load(std::memory_order_relaxed)) {
                        std::cerr << "[WindowFocus] HID device " << i << " wait failed: " << GetLastError() << std::endl;
                    }
                    invalidDevices.push_back(i);
                }
            }
        } else if (errorCode == ERROR_DEVICE_NOT_CONNECTED ||
                   errorCode == ERROR_GEN_FAILURE ||
                   errorCode == ERROR_INVALID_HANDLE ||
                   errorCode == ERROR_BAD_DEVICE) {
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cout << "[WindowFocus] HID device " << i << " disconnected (error: " << errorCode << ")" << std::endl;
            }
            invalidDevices.push_back(i);
            delete pOvl;
            CloseHandleSEH(hEvent);
        } else {
            if (enableDebug_.load(std::memory_order_relaxed)) {
                std::cerr << "[WindowFocus] Error reading HID device " << i
                          << " (code: 0x" << std::hex << errorCode << std::dec << ")" << std::endl;
            }
            invalidDevices.push_back(i);
            delete pOvl;
            CloseHandleSEH(hEvent);
        }

        if (inputDetected) break;
    }

    if (!invalidDevices.empty()) {
        std::sort(invalidDevices.begin(), invalidDevices.end());
        invalidDevices.erase(std::unique(invalidDevices.begin(), invalidDevices.end()), invalidDevices.end());

        for (auto it = invalidDevices.rbegin(); it != invalidDevices.rend(); ++it) {
            size_t idx = *it;
            if (idx < hidDeviceHandles_.size()) {
                HANDLE handle = hidDeviceHandles_[idx];
                if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                    CancelIoSEH(handle);
                    CloseHandleSEH(handle);
                }
                hidDeviceHandles_.erase(hidDeviceHandles_.begin() + idx);
                lastHIDStates_.erase(lastHIDStates_.begin() + idx);

                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] Removed invalid HID device at index " << idx << std::endl;
                }
            }
        }
    }

    return inputDetected;
}

void WindowFocusPlugin::CloseHIDDevices() {
    std::lock_guard<std::mutex> lock(hidDevicesMutex_);

    for (HANDLE handle : hidDeviceHandles_) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CancelIoSEH(handle);
            CloseHandleSEH(handle);
        }
    }
    hidDeviceHandles_.clear();
    lastHIDStates_.clear();

    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] Closed all HID devices" << std::endl;
    }
}

void WindowFocusPlugin::MonitorAllInputDevices() {
    if (monitorHIDDevices_.load(std::memory_order_relaxed)) {
        InitializeHIDDevices();
    }

    threadCount_++;
    std::weak_ptr<WindowFocusPlugin> weakSelf = shared_from_this();

    std::thread([weakSelf]() {
        // FIX: Initialise COM once for this thread's lifetime here, rather
        // than using a thread_local ComGuard inside CheckSystemAudio.
        // This ensures CoUninitialize() is called cleanly when this thread
        // exits, not from a destructor that may fire after plugin teardown.
        bool comInitialized = false;
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInitialized = SUCCEEDED(hr) || (hr == RPC_E_CHANGED_MODE);
        }

        auto lastHIDReinit = std::chrono::steady_clock::now();
        const auto hidReinitInterval = std::chrono::seconds(30);

        while (true) {
            // FIX: Acquire strong ref once per iteration and keep it for the
            // entire iteration body. Do not drop and re-acquire mid-iteration.
            // This eliminates the window where the object is partially destroyed
            // while the thread is still executing.
            auto self = weakSelf.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            bool inputDetected = false;

            try {
                if (!self->isShuttingDown_.load() && self->CheckKeyboardInput())   inputDetected = true;
                if (!self->isShuttingDown_.load() && self->CheckControllerInput()) inputDetected = true;
                if (!self->isShuttingDown_.load() && self->CheckRawInput())        inputDetected = true;
                if (!self->isShuttingDown_.load() && self->CheckSystemAudio())     inputDetected = true;
                if (!self->isShuttingDown_.load() && self->CheckHIDDevices())      inputDetected = true;
            } catch (...) {
                if (self->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Exception in MonitorAllInputDevices loop" << std::endl;
                }
            }

            if (self->monitorHIDDevices_.load(std::memory_order_relaxed) &&
                !self->isShuttingDown_.load(std::memory_order_acquire)) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastHIDReinit > hidReinitInterval) {
                    lastHIDReinit = now;
                    bool needsReinit = false;
                    {
                        std::lock_guard<std::mutex> lock(self->hidDevicesMutex_);
                        needsReinit = self->hidDeviceHandles_.empty();
                    }
                    if (needsReinit && !self->isShuttingDown_.load(std::memory_order_acquire)) {
                        if (self->enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "[WindowFocus] Re-initializing HID devices" << std::endl;
                        }
                        self->InitializeHIDDevices();
                    }
                }
            }

            if (inputDetected && !self->isShuttingDown_.load(std::memory_order_acquire)) {
                self->UpdateLastActivityTime();

                if (!self->userIsActive_.load(std::memory_order_relaxed)) {
                    self->userIsActive_.store(true, std::memory_order_relaxed);
                    std::weak_ptr<WindowFocusPlugin> weak = self;
                    self->PostToMainThread([weak]() {
                        if (auto p = weak.lock()) {
                            p->SafeInvokeMethod("onUserActive", "User is active");
                        }
                    });
                }
            }

            // FIX: Release the strong reference BEFORE sleeping so the
            // destructor's threadCount_ wait is not blocked for 100ms.
            self.reset();

            // Now sleep — if shutdown fires during the sleep, the cv wakes us.
            {
                auto s = weakSelf.lock();
                if (!s) break;
                std::unique_lock<std::mutex> lock(s->shutdownMutex_);
                // FIX: Hold s (strong ref) for the entire wait so the object
                // cannot be destroyed while we're waiting on its cv.
                if (s->shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [&s] { return s->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }
        }

        if (comInitialized) {
            CoUninitialize();
        }

        if (auto self = weakSelf.lock()) {
            self->threadCount_--;
        }
    }).detach();
}

void WindowFocusPlugin::CheckForInactivity() {
    threadCount_++;
    std::weak_ptr<WindowFocusPlugin> weakSelf = shared_from_this();

    std::thread([weakSelf]() {
        while (true) {
            // FIX: Acquire once, keep for full iteration body.
            auto self = weakSelf.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            auto now = std::chrono::steady_clock::now();
            int64_t duration;

            {
                std::lock_guard<std::mutex> lock(self->activityMutex_);
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - self->lastActivityTime).count();
            }

            // FIX: atomic load for inactivityThreshold_.
            if (duration > self->inactivityThreshold_.load(std::memory_order_relaxed) &&
                self->userIsActive_.load(std::memory_order_relaxed)) {
                self->userIsActive_.store(false, std::memory_order_relaxed);
                if (self->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] User inactive. Duration: " << duration
                              << "ms, Threshold: " << self->inactivityThreshold_.load() << "ms" << std::endl;
                }
                std::weak_ptr<WindowFocusPlugin> weak = self;
                self->PostToMainThread([weak]() {
                    if (auto p = weak.lock()) {
                        p->SafeInvokeMethod("onUserInactivity", "User is inactive");
                    }
                });
            }

            // Release strong ref before waiting.
            self.reset();

            {
                auto s = weakSelf.lock();
                if (!s) break;
                std::unique_lock<std::mutex> lock(s->shutdownMutex_);
                if (s->shutdownCv_.wait_for(lock, std::chrono::seconds(1),
                    [&s] { return s->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }
        }

        if (auto self = weakSelf.lock()) {
            self->threadCount_--;
        }
    }).detach();
}

void WindowFocusPlugin::StartFocusListener() {
    threadCount_++;
    std::weak_ptr<WindowFocusPlugin> weakSelf = shared_from_this();

    std::thread([weakSelf]() {
        HWND last_focused = nullptr;

        while (true) {
            // FIX: Acquire once and hold for full iteration including the
            // sleep/wait. This eliminates the original TOCTOU race where self
            // was reset mid-loop before the wait, so the destructor's
            // notify_all() could fire before this thread ever called wait_for.
            auto self = weakSelf.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            try {
                HWND current_focused = GetForegroundWindow();
                if (current_focused != last_focused) {
                    last_focused = current_focused;

                    if (current_focused != nullptr) {
                        int titleLen = GetWindowTextLengthA(current_focused);
                        std::string window_title(titleLen > 0 ? titleLen + 1 : 1, '\0');
                        if (titleLen > 0) {
                            GetWindowTextA(current_focused, window_title.data(), titleLen + 1);
                            window_title.resize(titleLen);
                        } else {
                            window_title.clear();
                        }

                        std::string appName = GetFocusedWindowAppName();
                        std::string windowTitle = GetFocusedWindowTitle();

                        if (self->enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "Current window title: " << window_title << std::endl;
                            std::cout << "Current window name: " << windowTitle << std::endl;
                            std::cout << "Current window appName: " << appName << std::endl;
                        }

                        std::string utf8_output = ConvertWindows1251ToUTF8(window_title);
                        std::string utf8_windowTitle = ConvertWindows1251ToUTF8(windowTitle);

                        flutter::EncodableMap data;
                        data[flutter::EncodableValue("title")]       = flutter::EncodableValue(utf8_output);
                        data[flutter::EncodableValue("appName")]     = flutter::EncodableValue(appName);
                        data[flutter::EncodableValue("windowTitle")] = flutter::EncodableValue(utf8_windowTitle);

                        if (!self->isShuttingDown_.load(std::memory_order_acquire)) {
                            std::weak_ptr<WindowFocusPlugin> weak = self;
                            self->PostToMainThread([weak, d = std::move(data)]() mutable {
                                if (auto p = weak.lock()) {
                                    p->SafeInvokeMethodWithMap("onFocusChange", std::move(d));
                                }
                            });
                        }
                    }
                }
            } catch (...) {
                if (self->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Exception in StartFocusListener loop" << std::endl;
                }
            }

            // FIX: Wait while still holding self (strong ref), so the object
            // cannot be destroyed mid-wait and notify_all() cannot be missed.
            {
                std::unique_lock<std::mutex> lock(self->shutdownMutex_);
                if (self->shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [&self] { return self->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }

            // Release after wait completes, just before the next iteration
            // re-acquires. This keeps the destructor wait short.
            self.reset();
        }

        if (auto self = weakSelf.lock()) {
            self->threadCount_--;
        }
    }).detach();
}

// =====================================================================
// GDI+ lifetime singleton — initialised once per process.
// =====================================================================
class GdiplusLifetime {
public:
    static GdiplusLifetime& Get() {
        static GdiplusLifetime inst;
        return inst;
    }
    bool IsValid() const { return token_ != 0; }

private:
    ULONG_PTR token_ = 0;

    GdiplusLifetime() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }
    ~GdiplusLifetime() {
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }
};

std::optional<std::vector<uint8_t>> WindowFocusPlugin::TakeScreenshot(bool activeWindowOnly) {
    std::lock_guard<std::mutex> lock(screenshotMutex_);

    if (!GdiplusLifetime::Get().IsValid()) {
        return std::nullopt;
    }

    HWND hwnd = activeWindowOnly ? GetForegroundWindow() : GetDesktopWindow();
    if (hwnd == NULL) hwnd = GetDesktopWindow();

    HDC hdcScreen = GetDC(NULL);
    HDC hdcWindow = GetDC(hwnd);
    HDC hdcMemDC  = CreateCompatibleDC(hdcWindow);

    if (!hdcScreen || !hdcWindow || !hdcMemDC) {
        if (hdcMemDC) DeleteDC(hdcMemDC);
        if (hdcWindow) ReleaseDC(hwnd, hdcWindow);
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        return std::nullopt;
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int width  = rc.right  - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) {
        DeleteDC(hdcMemDC);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return std::nullopt;
    }

    HBITMAP hbmScreen = CreateCompatibleBitmap(hdcWindow, width, height);
    if (!hbmScreen) {
        DeleteDC(hdcMemDC);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return std::nullopt;
    }

    HGDIOBJ oldBitmap = SelectObject(hdcMemDC, hbmScreen);
    BitBlt(hdcMemDC, 0, 0, width, height, hdcScreen, rc.left, rc.top, SRCCOPY);

    Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(hbmScreen, NULL);
    IStream* stream = NULL;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);

    if (FAILED(hr) || !stream) {
        delete bitmap;
        SelectObject(hdcMemDC, oldBitmap);
        DeleteObject(hbmScreen);
        DeleteDC(hdcMemDC);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        return std::nullopt;
    }

    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);
    bitmap->Save(stream, &pngClsid, NULL);

    STATSTG statstg;
    stream->Stat(&statstg, STATFLAG_DEFAULT);
    ULONG fileSize = (ULONG)statstg.cbSize.QuadPart;

    std::vector<uint8_t> data(fileSize);
    LARGE_INTEGER liZero = { 0 };
    stream->Seek(liZero, STREAM_SEEK_SET, NULL);
    ULONG bytesRead = 0;
    stream->Read(data.data(), fileSize, &bytesRead);

    stream->Release();
    delete bitmap;
    SelectObject(hdcMemDC, oldBitmap);
    DeleteObject(hbmScreen);
    DeleteDC(hdcMemDC);
    ReleaseDC(hwnd, hdcWindow);
    ReleaseDC(NULL, hdcScreen);

    if (bytesRead > 0) {
        return data;
    }
    return std::nullopt;
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

}  // namespace window_focus