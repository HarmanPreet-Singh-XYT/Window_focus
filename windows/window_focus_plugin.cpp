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

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace window_focus {

std::atomic<WindowFocusPlugin*> WindowFocusPlugin::instance_{nullptr};
HHOOK WindowFocusPlugin::mouseHook_ = nullptr;
HHOOK WindowFocusPlugin::keyboardHook_ = nullptr;

using CallbackMethod = std::function<void(const std::wstring&)>;

// =====================================================================
// RAII Helpers
// =====================================================================

class ComInitializer {
public:
    ComInitializer() : initialized_(false), needsUninitialize_(false) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            initialized_ = true;
            needsUninitialize_ = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            initialized_ = true;
            needsUninitialize_ = false;
        }
    }

    ~ComInitializer() {
        if (needsUninitialize_) {
            CoUninitialize();
        }
    }

    bool IsInitialized() const { return initialized_; }

    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;

private:
    bool initialized_;
    bool needsUninitialize_;
};

class GdiplusInitializer {
public:
    GdiplusInitializer() : token_(0), initialized_(false) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::Status status = Gdiplus::GdiplusStartup(&token_, &input, NULL);
        initialized_ = (status == Gdiplus::Ok);
    }

    ~GdiplusInitializer() {
        if (initialized_) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    bool IsInitialized() const { return initialized_; }

    GdiplusInitializer(const GdiplusInitializer&) = delete;
    GdiplusInitializer& operator=(const GdiplusInitializer&) = delete;

private:
    ULONG_PTR token_;
    bool initialized_;
};

class DcHandle {
public:
    DcHandle(HWND hwnd) : hwnd_(hwnd), hdc_(nullptr) {
        hdc_ = GetDC(hwnd);
    }
    ~DcHandle() {
        if (hdc_) {
            ReleaseDC(hwnd_, hdc_);
        }
    }
    HDC Get() const { return hdc_; }
    operator bool() const { return hdc_ != nullptr; }

    DcHandle(const DcHandle&) = delete;
    DcHandle& operator=(const DcHandle&) = delete;

private:
    HWND hwnd_;
    HDC hdc_;
};

class CompatibleDc {
public:
    CompatibleDc(HDC hdc) : hdc_(nullptr) {
        hdc_ = CreateCompatibleDC(hdc);
    }
    ~CompatibleDc() {
        if (hdc_) {
            DeleteDC(hdc_);
        }
    }
    HDC Get() const { return hdc_; }
    operator bool() const { return hdc_ != nullptr; }

    CompatibleDc(const CompatibleDc&) = delete;
    CompatibleDc& operator=(const CompatibleDc&) = delete;

private:
    HDC hdc_;
};

class BitmapHandle {
public:
    BitmapHandle(HBITMAP bmp) : bmp_(bmp) {}
    ~BitmapHandle() {
        if (bmp_) {
            DeleteObject(bmp_);
        }
    }
    HBITMAP Get() const { return bmp_; }
    operator bool() const { return bmp_ != nullptr; }

    BitmapHandle(const BitmapHandle&) = delete;
    BitmapHandle& operator=(const BitmapHandle&) = delete;

private:
    HBITMAP bmp_;
};

class SelectedObject {
public:
    SelectedObject(HDC hdc, HGDIOBJ obj) : hdc_(hdc), old_(nullptr) {
        if (hdc && obj) {
            old_ = SelectObject(hdc, obj);
        }
    }
    ~SelectedObject() {
        if (hdc_ && old_) {
            SelectObject(hdc_, old_);
        }
    }

    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;

private:
    HDC hdc_;
    HGDIOBJ old_;
};

// =====================================================================
// SEH-isolated helper functions
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

// =====================================================================
// End of SEH-isolated helpers
// =====================================================================

// Keyboard hook callback - uses atomic instance_ for thread safety
LRESULT CALLBACK WindowFocusPlugin::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        WindowFocusPlugin* inst = instance_.load(std::memory_order_acquire);
        if (inst && !inst->isShuttingDown_) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (inst->enableDebug_) {
                    KBDLLHOOKSTRUCT* pKeyboard = (KBDLLHOOKSTRUCT*)lParam;
                    if (pKeyboard) {
                        std::cout << "[WindowFocus] Keyboard hook: key down vkCode="
                                  << pKeyboard->vkCode << std::endl;
                    }
                }

                inst->UpdateLastActivityTime();

                auto now = std::chrono::steady_clock::now();
                auto epoch = now.time_since_epoch();
                inst->lastKeyEventTime_ = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();

                if (!inst->userIsActive_) {
                    inst->userIsActive_ = true;
                    inst->SafeInvokeMethod("onUserActive", "User is active");
                }
            }
        }
    }
    return CallNextHookEx(keyboardHook_, nCode, wParam, lParam);
}

// Mouse hook callback - uses atomic instance_ for thread safety
LRESULT CALLBACK WindowFocusPlugin::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        WindowFocusPlugin* inst = instance_.load(std::memory_order_acquire);
        if (inst && !inst->isShuttingDown_) {
            if (inst->enableDebug_) {
                std::cout << "[WindowFocus] mouse hook detected action" << std::endl;
            }
            inst->UpdateLastActivityTime();
            if (!inst->userIsActive_) {
                inst->userIsActive_ = true;
                inst->SafeInvokeMethod("onUserActive", "User is active");
            }
        }
    }
    return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

void WindowFocusPlugin::SetHooks() {
    WindowFocusPlugin* inst = instance_.load(std::memory_order_acquire);
    if (inst && inst->enableDebug_) {
        std::cout << "[WindowFocus] SetHooks: start\n";
    }
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // Install mouse hook with retry
    mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, hInstance, 0);
    if (!mouseHook_) {
        DWORD error = GetLastError();
        std::cerr << "[WindowFocus] Failed to install mouse hook: " << error << std::endl;

        Sleep(100);
        mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, hInstance, 0);
        if (mouseHook_) {
            std::cout << "[WindowFocus] Mouse hook installed on retry" << std::endl;
        } else {
            std::cerr << "[WindowFocus] Mouse hook retry also failed: " << GetLastError() << std::endl;
        }
    } else {
        if (inst && inst->enableDebug_) {
            std::cout << "[WindowFocus] Mouse hook installed successfully\n";
        }
    }

    // Install keyboard hook with retry
    keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
    if (!keyboardHook_) {
        DWORD error = GetLastError();
        std::cerr << "[WindowFocus] Failed to install keyboard hook: " << error << std::endl;

        Sleep(100);
        keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
        if (keyboardHook_) {
            std::cout << "[WindowFocus] Keyboard hook installed on retry" << std::endl;
        } else {
            std::cerr << "[WindowFocus] Keyboard hook retry also failed: " << GetLastError() << std::endl;
            SafeInvokeMethod("onError", "Keyboard hook installation failed - using polling fallback");
        }
    } else {
        if (inst && inst->enableDebug_) {
            std::cout << "[WindowFocus] Keyboard hook installed successfully\n";
        }
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

void WindowFocusPlugin::SafeInvokeMethod(const std::string& methodName, const std::string& message) {
    if (isShuttingDown_) return;

    try {
        std::lock_guard<std::mutex> lock(channelMutex_);
        if (channel && !isShuttingDown_) {
            channel->InvokeMethod(
                methodName,
                std::make_unique<flutter::EncodableValue>(message));
        }
    } catch (const std::exception& e) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Exception invoking method '"
                      << methodName << "': " << e.what() << std::endl;
        }
    } catch (...) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Unknown exception invoking method: "
                      << methodName << std::endl;
        }
    }
}

void WindowFocusPlugin::SafeInvokeMethodWithMap(const std::string& methodName, flutter::EncodableMap& data) {
    if (isShuttingDown_) return;

    try {
        std::lock_guard<std::mutex> lock(channelMutex_);
        if (channel && !isShuttingDown_) {
            channel->InvokeMethod(
                methodName,
                std::make_unique<flutter::EncodableValue>(data));
        }
    } catch (const std::exception& e) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Exception invoking method with map '"
                      << methodName << "': " << e.what() << std::endl;
        }
    } catch (...) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Unknown exception invoking method with map: "
                      << methodName << std::endl;
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

    auto channel = std::make_shared<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(),
        "expert.kotelnikoff/window_focus",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<WindowFocusPlugin>();
    plugin->channel = channel;

    plugin->SetHooks();
    plugin->CheckForInactivity();
    plugin->StartFocusListener();
    plugin->MonitorAllInputDevices();

    channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto& call,
                                        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
            plugin_pointer->HandleMethodCall(call, std::move(result));
        }
    );

    registrar->AddPlugin(std::move(plugin));
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

    std::string windowTitle(length + 1, '\0');
    int copied = GetWindowTextA(hwnd, &windowTitle[0], length + 1);
    if (copied <= 0) {
        return "";
    }
    windowTitle.resize(copied);
    return windowTitle;
}

WindowFocusPlugin::WindowFocusPlugin() : isShuttingDown_(false) {
    WindowFocusPlugin* expected = nullptr;
    if (!instance_.compare_exchange_strong(expected, this, std::memory_order_release)) {
        std::cerr << "[WindowFocus] WARNING: Multiple plugin instances created. "
                  << "Previous instance will be orphaned." << std::endl;
        if (mouseHook_) {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }
        if (keyboardHook_) {
            UnhookWindowsHookEx(keyboardHook_);
            keyboardHook_ = nullptr;
        }
        instance_.store(this, std::memory_order_release);
    }

    lastActivityTime = std::chrono::steady_clock::now();
    ZeroMemory(lastControllerStates_, sizeof(lastControllerStates_));
    GetCursorPos(&lastMousePosition_);
}

WindowFocusPlugin::~WindowFocusPlugin() {
    // 1. Set shutdown flag first
    isShuttingDown_ = true;

    // 2. Remove hooks BEFORE joining threads or nullifying instance
    //    This stops hook callbacks from firing
    RemoveHooks();

    // 3. Signal all threads to wake up and exit
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        shutdownCv_.notify_all();
    }

    // 4. Join all threads - guaranteed no use-after-free
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

    // 5. Close HID devices
    CloseHIDDevices();

    // 6. Nullify channel under lock
    {
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel = nullptr;
    }

    // 7. Nullify instance last
    instance_.store(nullptr, std::memory_order_release);
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
                    bool newDebugValue = std::get<bool>(it->second);
                    enableDebug_ = newDebugValue;
                    std::cout << "[WindowFocus] C++: enableDebug_ set to "
                              << (enableDebug_ ? "true" : "false") << std::endl;
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
                    monitorControllers_ = std::get<bool>(it->second);
                    std::cout << "[WindowFocus] Controller monitoring set to "
                              << (monitorControllers_ ? "true" : "false") << std::endl;
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
                    monitorAudio_ = std::get<bool>(it->second);
                    std::cout << "[WindowFocus] Audio monitoring set to "
                              << (monitorAudio_ ? "true" : "false") << std::endl;
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
                    audioThreshold_ = static_cast<float>(std::get<double>(it->second));
                    std::cout << "[WindowFocus] Audio threshold set to " << audioThreshold_ << std::endl;
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
                    if (newValue && !monitorHIDDevices_) {
                        InitializeHIDDevices();
                    } else if (!newValue && monitorHIDDevices_) {
                        CloseHIDDevices();
                    }
                    monitorHIDDevices_ = newValue;
                    std::cout << "[WindowFocus] HID device monitoring set to "
                              << (monitorHIDDevices_ ? "true" : "false") << std::endl;
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
                    monitorKeyboard_ = std::get<bool>(it->second);
                    std::cout << "[WindowFocus] Keyboard monitoring set to "
                              << (monitorKeyboard_ ? "true" : "false") << std::endl;

                    if (monitorKeyboard_ && !keyboardHook_) {
                        HINSTANCE hInstance = GetModuleHandle(nullptr);
                        keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
                        if (!keyboardHook_) {
                            std::cerr << "[WindowFocus] Failed to install keyboard hook: "
                                      << GetLastError() << std::endl;
                        }
                    } else if (!monitorKeyboard_ && keyboardHook_) {
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
                    inactivityThreshold_ = std::get<int>(it->second);
                    std::cout << "Updated inactivityThreshold_ to " << inactivityThreshold_ << std::endl;
                    result->Success(flutter::EncodableValue(inactivityThreshold_));
                    return;
                }
            }
        }
        result->Error("Invalid argument", "Expected an integer argument.");
    } else if (method_name == "getPlatformVersion") {
        result->Success(flutter::EncodableValue("Windows: example"));
    } else if (method_name == "getIdleThreshold") {
        result->Success(flutter::EncodableValue(inactivityThreshold_));
    } else if (method_name == "takeScreenshot") {
        bool activeWindowOnly = false;
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("activeWindowOnly"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                activeWindowOnly = std::get<bool>(it->second);
            }
        }
        try {
            auto screenshot = TakeScreenshot(activeWindowOnly);
            if (screenshot.has_value()) {
                result->Success(flutter::EncodableValue(screenshot.value()));
            } else {
                result->Error("SCREENSHOT_ERROR", "Failed to take screenshot");
            }
        } catch (const std::exception& e) {
            result->Error("SCREENSHOT_ERROR",
                         std::string("Exception taking screenshot: ") + e.what());
        } catch (...) {
            result->Error("SCREENSHOT_ERROR", "Unknown exception taking screenshot");
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
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        return ConvertWStringToUTF8(processName);
    }

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

    return ConvertWStringToUTF8(processName);
}

std::string GetFocusedWindowAppName() {
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL) {
        return "<no window in focus>";
    }

    DWORD processID = 0;
    GetWindowThreadProcessId(hwnd, &processID);
    if (processID == 0) {
        return "<unknown>";
    }

    return GetProcessName(processID);
}

bool WindowFocusPlugin::CheckControllerInput() {
    if (!monitorControllers_ || isShuttingDown_) {
        return false;
    }

    bool inputDetected = false;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        if (isShuttingDown_) break;

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));

        bool exceptionOccurred = false;
        DWORD result = XInputGetStateSEH(i, &state, &exceptionOccurred);

        if (exceptionOccurred) {
            if (enableDebug_) {
                std::cerr << "[WindowFocus] Exception reading controller " << i << std::endl;
            }
            continue;
        }

        if (result == ERROR_SUCCESS) {
            if (state.dwPacketNumber != lastControllerStates_[i].dwPacketNumber) {
                if (enableDebug_) {
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
    if (isShuttingDown_) return false;

    POINT currentMousePos;
    if (GetCursorPos(&currentMousePos)) {
        std::lock_guard<std::mutex> lock(mouseMutex_);
        if (currentMousePos.x != lastMousePosition_.x || currentMousePos.y != lastMousePosition_.y) {
            lastMousePosition_ = currentMousePos;
            if (enableDebug_) {
                std::cout << "[WindowFocus] Mouse movement detected via cursor position" << std::endl;
            }
            return true;
        }
    }

    return false;
}

bool WindowFocusPlugin::CheckKeyboardInput() {
    if (!monitorKeyboard_ || isShuttingDown_) {
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
        if (isShuttingDown_) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) {
                if (enableDebug_) {
                    std::cout << "[WindowFocus] Key detected (poll): vk=0x"
                              << std::hex << vk << std::dec << std::endl;
                }
                return true;
            }
        }
        if (exceptionOccurred) break;
    }

    for (int vk = 0x30; vk <= 0x39; vk++) {
        if (isShuttingDown_) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) {
                if (enableDebug_) {
                    std::cout << "[WindowFocus] Number key detected (poll): vk=0x"
                              << std::hex << vk << std::dec << std::endl;
                }
                return true;
            }
        }
        if (exceptionOccurred) break;
    }

    for (int vk = VK_F1; vk <= VK_F12; vk++) {
        if (isShuttingDown_) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred)) {
            if (!exceptionOccurred) {
                if (enableDebug_) {
                    std::cout << "[WindowFocus] Function key detected (poll): vk=0x"
                              << std::hex << vk << std::dec << std::endl;
                }
                return true;
            }
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
        if (isShuttingDown_) return false;
        if (CheckKeyStateSEH(specialKeys[i], &exceptionOccurred)) {
            if (!exceptionOccurred) {
                if (enableDebug_) {
                    std::cout << "[WindowFocus] Special key detected (poll): vk=0x"
                              << std::hex << specialKeys[i] << std::dec << std::endl;
                }
                return true;
            }
        }
        if (exceptionOccurred) break;
    }

    return false;
}

bool WindowFocusPlugin::CheckSystemAudio() {
    if (!monitorAudio_ || isShuttingDown_) {
        return false;
    }

    static thread_local ComInitializer comInit;
    if (!comInit.IsInitialized()) {
        return false;
    }

    float peakValue = 0.0f;
    bool comError = false;
    bool success = GetAudioPeakValueSEH(&peakValue, &comError);

    if (comError && enableDebug_) {
        std::cerr << "[WindowFocus] Exception in CheckSystemAudio" << std::endl;
    }

    if (success && peakValue > audioThreshold_) {
        if (enableDebug_) {
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

    if (isShuttingDown_) return;

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Failed to get HID device info set: "
                      << GetLastError() << std::endl;
        }
        return;
    }

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    DWORD memberIndex = 0;
    while (!isShuttingDown_ && SetupDiEnumDeviceInterfaces(
        deviceInfoSet,
        nullptr,
        &hidGuid,
        memberIndex++,
        &deviceInterfaceData
    )) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet,
            &deviceInterfaceData,
            nullptr,
            0,
            &requiredSize,
            nullptr
        );

        if (requiredSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA detailData =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
        if (!detailData) {
            if (enableDebug_) {
                std::cerr << "[WindowFocus] Failed to allocate memory for HID device detail" << std::endl;
            }
            continue;
        }

        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet,
            &deviceInterfaceData,
            detailData,
            requiredSize,
            nullptr,
            nullptr
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
                            bool isKeyboard = (caps.UsagePage == 0x01 && caps.Usage == 0x06);
                            bool isMouse = (caps.UsagePage == 0x01 && caps.Usage == 0x02);

                            if (!isAudioDevice && !isKeyboard && !isMouse && caps.InputReportByteLength > 0) {
                                hidDeviceHandles_.push_back(deviceHandle);
                                lastHIDStates_.push_back(std::vector<BYTE>(caps.InputReportByteLength, 0));

                                if (enableDebug_) {
                                    std::cout << "[WindowFocus] HID device added: VID="
                                              << std::hex << attributes.VendorID
                                              << " PID=" << attributes.ProductID
                                              << std::dec
                                              << " UsagePage=0x" << std::hex << caps.UsagePage
                                              << " Usage=0x" << caps.Usage << std::dec << std::endl;
                                }
                            } else {
                                if (enableDebug_) {
                                    const char* reason = isAudioDevice ? "audio" :
                                                        isKeyboard ? "keyboard" :
                                                        isMouse ? "mouse" : "no input";
                                    std::cout << "[WindowFocus] Skipping HID device (" << reason << "): VID="
                                              << std::hex << attributes.VendorID
                                              << " PID=" << attributes.ProductID
                                              << " UsagePage=0x" << caps.UsagePage
                                              << " Usage=0x" << caps.Usage << std::dec << std::endl;
                                }
                                CloseHandleSEH(deviceHandle);
                            }
                        } else {
                            CloseHandleSEH(deviceHandle);
                        }
                        if (preparsedData) {
                            HidD_FreePreparsedData(preparsedData);
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

    if (enableDebug_) {
        std::cout << "[WindowFocus] Initialized " << hidDeviceHandles_.size() << " HID devices" << std::endl;
    }
}

bool WindowFocusPlugin::CheckHIDDevices() {
    if (!monitorHIDDevices_ || isShuttingDown_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(hidDevicesMutex_);

    if (hidDeviceHandles_.empty()) {
        return false;
    }

    bool inputDetected = false;
    std::vector<size_t> invalidDevices;

    for (size_t i = 0; i < hidDeviceHandles_.size(); i++) {
        if (isShuttingDown_) break;

        HANDLE deviceHandle = hidDeviceHandles_[i];

        if (!IsHandleValid(deviceHandle)) {
            invalidDevices.push_back(i);
            continue;
        }

        std::vector<BYTE>& lastState = lastHIDStates_[i];

        if (lastState.empty()) {
            continue;
        }

        std::vector<BYTE> buffer(lastState.size(), 0);

        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        if (overlapped.hEvent == nullptr) {
            if (enableDebug_) {
                std::cerr << "[WindowFocus] Failed to create event for HID device " << i
                          << ": " << GetLastError() << std::endl;
            }
            continue;
        }

        DWORD bytesRead = 0;
        DWORD bufferSize = static_cast<DWORD>(buffer.size());
        DWORD errorCode = 0;

        bool readSucceeded = ReadHIDDeviceSEH(
            deviceHandle, buffer.data(), bufferSize,
            &overlapped, &bytesRead, &errorCode);

        if (readSucceeded) {
            if (bytesRead > 0 && buffer != lastState) {
                inputDetected = true;
                lastState = buffer;

                if (enableDebug_) {
                    std::cout << "[WindowFocus] HID device " << i << " input detected" << std::endl;
                }
            }
        } else if (errorCode == ERROR_IO_PENDING) {
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 10);
            if (waitResult == WAIT_OBJECT_0) {
                DWORD overlappedError = 0;
                if (GetOverlappedResultSEH(deviceHandle, &overlapped, &bytesRead, &overlappedError)) {
                    if (bytesRead > 0 && buffer != lastState) {
                        inputDetected = true;
                        lastState = buffer;

                        if (enableDebug_) {
                            std::cout << "[WindowFocus] HID device " << i
                                      << " input detected (overlapped)" << std::endl;
                        }
                    }
                } else if (overlappedError == ERROR_INVALID_HANDLE ||
                           overlappedError == ERROR_DEVICE_NOT_CONNECTED) {
                    invalidDevices.push_back(i);
                }
            } else if (waitResult == WAIT_TIMEOUT) {
                CancelIoSEH(deviceHandle);
            } else if (waitResult == WAIT_FAILED) {
                DWORD waitError = GetLastError();
                if (enableDebug_) {
                    std::cout << "[WindowFocus] HID device " << i
                              << " wait failed: " << waitError << std::endl;
                }
                if (waitError == ERROR_INVALID_HANDLE) {
                    invalidDevices.push_back(i);
                }
            }
        } else if (errorCode == ERROR_DEVICE_NOT_CONNECTED ||
                   errorCode == ERROR_GEN_FAILURE ||
                   errorCode == ERROR_INVALID_HANDLE ||
                   errorCode == ERROR_BAD_DEVICE) {
            if (enableDebug_) {
                std::cout << "[WindowFocus] HID device " << i
                          << " disconnected or invalid (error: " << errorCode << ")" << std::endl;
            }
            invalidDevices.push_back(i);
        } else if (errorCode != ERROR_SUCCESS) {
            if (enableDebug_) {
                std::cerr << "[WindowFocus] Error reading HID device " << i
                          << " (code: 0x" << std::hex << errorCode << std::dec << ")" << std::endl;
            }
            invalidDevices.push_back(i);
        }

        CloseHandleSEH(overlapped.hEvent);

        if (inputDetected) {
            break;
        }
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

                if (enableDebug_) {
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

    if (enableDebug_) {
        std::cout << "[WindowFocus] Closed all HID devices" << std::endl;
    }
}

void WindowFocusPlugin::MonitorAllInputDevices() {
    if (monitorHIDDevices_) {
        InitializeHIDDevices();
    }

    std::lock_guard<std::mutex> lock(threadsMutex_);
    threads_.emplace_back([this]() {
        auto lastHIDReinit = std::chrono::steady_clock::now();
        const auto hidReinitInterval = std::chrono::seconds(30);

        while (!isShuttingDown_) {
            {
                std::unique_lock<std::mutex> lock(shutdownMutex_);
                if (shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return isShuttingDown_.load(); })) {
                    break;
                }
            }

            if (isShuttingDown_) break;

            bool anyInputDetected = false;

            try {
                if (!isShuttingDown_ && CheckKeyboardInput()) {
                    anyInputDetected = true;
                }
                if (!isShuttingDown_ && CheckControllerInput()) {
                    anyInputDetected = true;
                }
                if (!isShuttingDown_ && CheckRawInput()) {
                    anyInputDetected = true;
                }
                if (!isShuttingDown_ && CheckSystemAudio()) {
                    anyInputDetected = true;
                }
                if (!isShuttingDown_ && CheckHIDDevices()) {
                    anyInputDetected = true;
                }
            } catch (const std::exception& e) {
                if (enableDebug_) {
                    std::cerr << "[WindowFocus] Exception in MonitorAllInputDevices: "
                              << e.what() << std::endl;
                }
            } catch (...) {
                if (enableDebug_) {
                    std::cerr << "[WindowFocus] Unknown exception in MonitorAllInputDevices" << std::endl;
                }
            }

            if (monitorHIDDevices_ && !isShuttingDown_) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastHIDReinit > hidReinitInterval) {
                    lastHIDReinit = now;
                    bool needsReinit = false;
                    {
                        std::lock_guard<std::mutex> lock(hidDevicesMutex_);
                        needsReinit = hidDeviceHandles_.empty();
                    }
                    if (needsReinit && !isShuttingDown_) {
                        if (enableDebug_) {
                            std::cout << "[WindowFocus] Re-initializing HID devices (all disconnected)" << std::endl;
                        }
                        InitializeHIDDevices();
                    }
                }
            }

            if (anyInputDetected && !isShuttingDown_) {
                UpdateLastActivityTime();

                if (!userIsActive_) {
                    userIsActive_ = true;
                    SafeInvokeMethod("onUserActive", "User is active");
                }
            }
        }
    });
}

void WindowFocusPlugin::CheckForInactivity() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    threads_.emplace_back([this]() {
        while (!isShuttingDown_) {
            {
                std::unique_lock<std::mutex> lock(shutdownMutex_);
                if (shutdownCv_.wait_for(lock, std::chrono::seconds(1),
                    [this] { return isShuttingDown_.load(); })) {
                    break;
                }
            }

            if (isShuttingDown_) break;

            auto now = std::chrono::steady_clock::now();
            int64_t duration;

            {
                std::lock_guard<std::mutex> lock(activityMutex_);
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastActivityTime).count();
            }

            if (duration > inactivityThreshold_ && userIsActive_) {
                userIsActive_ = false;
                if (enableDebug_) {
                    std::cout << "[WindowFocus] User is inactive. Duration: " << duration
                              << "ms, Threshold: " << inactivityThreshold_ << "ms" << std::endl;
                }
                SafeInvokeMethod("onUserInactivity", "User is inactive");
            }
        }
    });
}

void WindowFocusPlugin::StartFocusListener() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    threads_.emplace_back([this]() {
        HWND last_focused = nullptr;
        while (!isShuttingDown_) {
            try {
                HWND current_focused = GetForegroundWindow();
                if (current_focused != last_focused) {
                    last_focused = current_focused;

                    if (current_focused != nullptr) {
                        char title[256] = {0};
                        GetWindowTextA(current_focused, title, sizeof(title));
                        std::string appName = GetFocusedWindowAppName();
                        std::string windowTitle = GetFocusedWindowTitle();
                        std::string window_title(title);

                        if (enableDebug_) {
                            std::cout << "Current window title: " << window_title << std::endl;
                            std::cout << "Current window name: " << windowTitle << std::endl;
                            std::cout << "Current window appName: " << appName << std::endl;
                        }

                        std::string utf8_output = ConvertWindows1251ToUTF8(window_title);
                        std::string utf8_windowTitle = ConvertWindows1251ToUTF8(windowTitle);
                        flutter::EncodableMap data;

                        data[flutter::EncodableValue("title")] = flutter::EncodableValue(utf8_output);
                        data[flutter::EncodableValue("appName")] = flutter::EncodableValue(appName);
                        data[flutter::EncodableValue("windowTitle")] = flutter::EncodableValue(utf8_windowTitle);

                        SafeInvokeMethodWithMap("onFocusChange", data);
                    }
                }
            } catch (const std::exception& e) {
                if (enableDebug_) {
                    std::cerr << "[WindowFocus] Exception in StartFocusListener: "
                              << e.what() << std::endl;
                }
            } catch (...) {
                if (enableDebug_) {
                    std::cerr << "[WindowFocus] Unknown exception in StartFocusListener" << std::endl;
                }
            }

            {
                std::unique_lock<std::mutex> lock(shutdownMutex_);
                if (shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this] { return isShuttingDown_.load(); })) {
                    break;
                }
            }
        }
    });
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

std::optional<std::vector<uint8_t>> WindowFocusPlugin::TakeScreenshot(bool activeWindowOnly) {
    // RAII GDI+ initialization
    GdiplusInitializer gdipInit;
    if (!gdipInit.IsInitialized()) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] GDI+ startup failed" << std::endl;
        }
        return std::nullopt;
    }

    HWND hwnd = activeWindowOnly ? GetForegroundWindow() : GetDesktopWindow();
    if (hwnd == NULL) hwnd = GetDesktopWindow();

    // RAII DC handles
    DcHandle hdcScreen(NULL);
    DcHandle hdcWindow(hwnd);

    if (!hdcScreen || !hdcWindow) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Failed to get device contexts" << std::endl;
        }
        return std::nullopt;
    }

    CompatibleDc hdcMemDC(hdcWindow.Get());
    if (!hdcMemDC) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Failed to create compatible DC" << std::endl;
        }
        return std::nullopt;
    }

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] GetWindowRect failed: " << GetLastError() << std::endl;
        }
        return std::nullopt;
    }

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Invalid window dimensions: "
                      << width << "x" << height << std::endl;
        }
        return std::nullopt;
    }

    // RAII bitmap handle
    BitmapHandle hbmScreen(CreateCompatibleBitmap(hdcWindow.Get(), width, height));
    if (!hbmScreen) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] CreateCompatibleBitmap failed: "
                      << GetLastError() << std::endl;
        }
        return std::nullopt;
    }

    // RAII select object (auto-restores old bitmap)
    SelectedObject selectedBitmap(hdcMemDC.Get(), hbmScreen.Get());

    if (!BitBlt(hdcMemDC.Get(), 0, 0, width, height,
                hdcScreen.Get(), rc.left, rc.top, SRCCOPY)) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] BitBlt failed: " << GetLastError() << std::endl;
        }
        return std::nullopt;
    }

    // Create GDI+ bitmap from HBITMAP
    Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(hbmScreen.Get(), NULL);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Bitmap creation failed";
            if (bitmap) {
                std::cerr << ": status=" << bitmap->GetLastStatus();
            }
            std::cerr << std::endl;
        }
        if (bitmap) delete bitmap;
        return std::nullopt;
    }

    // Create stream for PNG encoding
    IStream* stream = NULL;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] CreateStreamOnHGlobal failed: 0x"
                      << std::hex << hr << std::dec << std::endl;
        }
        delete bitmap;
        return std::nullopt;
    }

    // Find PNG encoder
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] PNG encoder not found" << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    // Save bitmap to stream as PNG
    Gdiplus::Status gdipStatus = bitmap->Save(stream, &pngClsid, NULL);
    if (gdipStatus != Gdiplus::Ok) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Bitmap Save failed: " << gdipStatus << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    // Get stream size
    STATSTG statstg;
    hr = stream->Stat(&statstg, STATFLAG_DEFAULT);
    if (FAILED(hr)) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Stream Stat failed: 0x"
                      << std::hex << hr << std::dec << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    ULONG fileSize = (ULONG)statstg.cbSize.QuadPart;
    if (fileSize == 0) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Screenshot file size is 0" << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    // Read PNG data from stream
    std::vector<uint8_t> data(fileSize);
    LARGE_INTEGER liZero = { 0 };
    hr = stream->Seek(liZero, STREAM_SEEK_SET, NULL);
    if (FAILED(hr)) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Stream Seek failed: 0x"
                      << std::hex << hr << std::dec << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    ULONG bytesRead = 0;
    hr = stream->Read(data.data(), fileSize, &bytesRead);
    if (FAILED(hr)) {
        if (enableDebug_) {
            std::cerr << "[WindowFocus] Stream Read failed: 0x"
                      << std::hex << hr << std::dec << std::endl;
        }
        stream->Release();
        delete bitmap;
        return std::nullopt;
    }

    // Cleanup non-RAII resources
    stream->Release();
    delete bitmap;

    if (bytesRead > 0) {
        return data;
    }

    if (enableDebug_) {
        std::cerr << "[WindowFocus] Screenshot read 0 bytes" << std::endl;
    }
    return std::nullopt;
}

}  // namespace window_focus