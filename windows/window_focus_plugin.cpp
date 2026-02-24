#include "window_focus_plugin.h"

#include <windows.h>
#include <VersionHelpers.h>
#include <PowrProf.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <xinput.h>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
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

#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <queue>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace window_focus {

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

static bool FreePreparsedDataSEH(PHIDP_PREPARSED_DATA preparsedData) {
    __try {
        HidD_FreePreparsedData(preparsedData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FIX: SEH wrapper for audio peak - only does GetPeakValue on a pre-existing meter
static bool GetPeakFromMeterSEH(IAudioMeterInformation* meter, float* peakValue) {
    *peakValue = 0.0f;
    __try {
        HRESULT hr = meter->GetPeakValue(peakValue);
        return SUCCEEDED(hr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
        return CreateFileW(devicePath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
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
// RAII helper for overlapped HID reads
// =====================================================================
struct OverlappedGuard {
    OVERLAPPED ovl{};
    HANDLE hEvent = nullptr;
    HANDLE deviceHandle = INVALID_HANDLE_VALUE;
    bool completed = false;

    OverlappedGuard(HANDLE dev) : deviceHandle(dev) {
        hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (hEvent) {
            ovl.hEvent = hEvent;
        }
    }

    ~OverlappedGuard() {
        if (!completed && deviceHandle != INVALID_HANDLE_VALUE && IsHandleValid(deviceHandle)) {
            CancelIoSEH(deviceHandle);
        }
        if (hEvent) {
            if (!completed) {
                // FIX: Bounded wait to prevent hangs on stale/invalid handles
                WaitForSingleObject(hEvent, 3000);
            }
            CloseHandleSEH(hEvent);
        }
    }

    bool IsValid() const { return hEvent != nullptr; }
    void MarkComplete() { completed = true; }
    OVERLAPPED* Get() { return &ovl; }
    void InvalidateDevice() { deviceHandle = INVALID_HANDLE_VALUE; }

    OverlappedGuard(const OverlappedGuard&) = delete;
    OverlappedGuard& operator=(const OverlappedGuard&) = delete;
};

// =====================================================================
// RAII helper for GDI DC handles
// =====================================================================
struct ReleaseDCGuard {
    HWND hwnd;
    HDC hdc;
    ReleaseDCGuard(HWND w, HDC d) : hwnd(w), hdc(d) {}
    ~ReleaseDCGuard() { if (hdc) ReleaseDC(hwnd, hdc); }
    ReleaseDCGuard(const ReleaseDCGuard&) = delete;
    ReleaseDCGuard& operator=(const ReleaseDCGuard&) = delete;
};

struct DeleteDCGuard {
    HDC hdc;
    explicit DeleteDCGuard(HDC d) : hdc(d) {}
    ~DeleteDCGuard() { if (hdc) DeleteDC(hdc); }
    DeleteDCGuard(const DeleteDCGuard&) = delete;
    DeleteDCGuard& operator=(const DeleteDCGuard&) = delete;
};

struct DeleteObjectGuard {
    HGDIOBJ obj;
    explicit DeleteObjectGuard(HGDIOBJ o) : obj(o) {}
    ~DeleteObjectGuard() { if (obj) DeleteObject(obj); }
    DeleteObjectGuard(const DeleteObjectGuard&) = delete;
    DeleteObjectGuard& operator=(const DeleteObjectGuard&) = delete;
};

// =====================================================================
// FIX: Cached audio meter for long-running efficiency
// Avoids creating/destroying 3 COM objects every 100ms
// =====================================================================
class AudioMeterCache {
public:
    AudioMeterCache() = default;
    ~AudioMeterCache() { Reset(); }

    // Returns peak audio value using cached COM objects.
    // Recreates them on failure or every refreshInterval.
    float GetPeak(bool enableDebug) {
        auto now = std::chrono::steady_clock::now();

        // Recreate if stale, failed too many times, or not yet initialized
        bool needsRefresh = !meter_ ||
            consecutiveFailures_ > 3 ||
            (now - lastInitTime_) > refreshInterval_;

        if (needsRefresh) {
            Reset();
            if (!Init(enableDebug)) {
                return 0.0f;
            }
        }

        float peak = 0.0f;
        bool ok = GetPeakFromMeterSEH(meter_, &peak);

        if (!ok) {
            consecutiveFailures_++;
            if (enableDebug) {
                std::cerr << "[WindowFocus] AudioMeterCache: GetPeakValue failed ("
                          << consecutiveFailures_ << " consecutive)" << std::endl;
            }
            return 0.0f;
        }

        consecutiveFailures_ = 0;
        return peak;
    }

    // Force re-creation on next call (e.g., after system resume)
    void Invalidate() {
        Reset();
    }

private:
    IAudioMeterInformation* meter_ = nullptr;
    IMMDevice* device_ = nullptr;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    int consecutiveFailures_ = 0;
    std::chrono::steady_clock::time_point lastInitTime_{};
    static constexpr auto refreshInterval_ = std::chrono::seconds(60);

    bool Init(bool enableDebug) {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
        if (FAILED(hr) || !enumerator_) {
            if (enableDebug) std::cerr << "[WindowFocus] AudioMeterCache: Failed to create enumerator" << std::endl;
            Reset();
            return false;
        }

        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr) || !device_) {
            if (enableDebug) std::cerr << "[WindowFocus] AudioMeterCache: Failed to get default endpoint" << std::endl;
            Reset();
            return false;
        }

        hr = device_->Activate(__uuidof(IAudioMeterInformation),
            CLSCTX_ALL, nullptr, (void**)&meter_);
        if (FAILED(hr) || !meter_) {
            if (enableDebug) std::cerr << "[WindowFocus] AudioMeterCache: Failed to activate meter" << std::endl;
            Reset();
            return false;
        }

        lastInitTime_ = std::chrono::steady_clock::now();
        consecutiveFailures_ = 0;
        return true;
    }

    void Reset() {
        if (meter_) { try { meter_->Release(); } catch (...) {} meter_ = nullptr; }
        if (device_) { try { device_->Release(); } catch (...) {} device_ = nullptr; }
        if (enumerator_) { try { enumerator_->Release(); } catch (...) {} enumerator_ = nullptr; }
        consecutiveFailures_ = 0;
    }

    AudioMeterCache(const AudioMeterCache&) = delete;
    AudioMeterCache& operator=(const AudioMeterCache&) = delete;
};

// =====================================================================
// PlatformTaskDispatcher
// FIX: Added pending count to prevent message queue overflow
// FIX: Added power resume notification support
// =====================================================================
class PlatformTaskDispatcher {
public:
    struct TaskPacket {
        std::function<void()> fn;
        uint64_t generation;
    };

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

        // FIX: Register for power resume notifications
        if (hwnd_) {
            powerNotifyHandle_ = RegisterSuspendResumeNotification(hwnd_, DEVICE_NOTIFY_WINDOW_HANDLE);
        }
    }

    void Shutdown() {
        HWND hwndToDestroy = nullptr;
        HPOWERNOTIFY powerNotify = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hwnd_) return;
            hwndToDestroy = hwnd_;
            powerNotify = powerNotifyHandle_;
            hwnd_ = nullptr;
            powerNotifyHandle_ = nullptr;
            currentGeneration_++;
        }

        // FIX: Unregister power notification
        if (powerNotify) {
            UnregisterSuspendResumeNotification(powerNotify);
        }

        MSG msg;
        while (PeekMessage(&msg, hwndToDestroy, WM_APP + 1, WM_APP + 1, PM_REMOVE)) {
            auto* packet = reinterpret_cast<TaskPacket*>(msg.lParam);
            delete packet;
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

            // FIX: Backpressure - drop tasks if queue is too deep
            if (pendingCount_ > 500) {
                return;
            }
            pendingCount_++;
        }
        if (!hwnd) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingCount_--;
            return;
        }

        auto* packet = new TaskPacket{ std::move(task), generation };
        if (!PostMessage(hwnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(packet))) {
            delete packet;
            std::lock_guard<std::mutex> lock(mutex_);
            pendingCount_--;
        }
    }

private:
    HWND hwnd_ = nullptr;
    std::mutex mutex_;
    std::atomic<uint64_t> currentGeneration_{ 0 };
    int pendingCount_ = 0;  // Protected by mutex_
    HPOWERNOTIFY powerNotifyHandle_ = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_APP + 1) {
            auto* packet = reinterpret_cast<TaskPacket*>(lParam);
            if (packet) {
                auto& dispatcher = PlatformTaskDispatcher::Get();
                uint64_t live = dispatcher.currentGeneration_.load();

                {
                    std::lock_guard<std::mutex> lock(dispatcher.mutex_);
                    if (dispatcher.pendingCount_ > 0) {
                        dispatcher.pendingCount_--;
                    }
                }

                if (packet->generation == live) {
                    try { packet->fn(); } catch (...) {}
                }
                delete packet;
            }
            return 0;
        }

        // FIX: Handle power resume notifications
        if (msg == WM_POWERBROADCAST) {
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                std::shared_ptr<WindowFocusPlugin> inst;
                {
                    std::lock_guard<std::mutex> lock(WindowFocusPlugin::instanceMutex_);
                    inst = WindowFocusPlugin::instance_.lock();
                }
                if (inst && !inst->IsShuttingDown()) {
                    inst->OnSystemResume();
                }
            }
            return TRUE;
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
                if (inst->enableDebug_.load(std::memory_order_relaxed) && pKeyboard) {
                    std::cout << "[WindowFocus] Keyboard hook: key down vkCode="
                              << pKeyboard->vkCode << std::endl;
                }

                inst->UpdateLastActivityTime();

                auto now = std::chrono::steady_clock::now();
                inst->lastKeyEventTime_.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count(),
                    std::memory_order_release);

                if (!inst->userIsActive_.load(std::memory_order_acquire)) {
                    inst->userIsActive_.store(true, std::memory_order_release);
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
            if (inst->enableDebug_.load(std::memory_order_relaxed)) {
                std::cout << "[WindowFocus] mouse hook detected action" << std::endl;
            }
            inst->UpdateLastActivityTime();
            if (!inst->userIsActive_.load(std::memory_order_acquire)) {
                inst->userIsActive_.store(true, std::memory_order_release);
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
    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] SetHooks: start\n";
    }

    RemoveHooks();

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

    hooksInstalled_.store(mouseHook_ != nullptr && keyboardHook_ != nullptr,
                          std::memory_order_release);
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
    hooksInstalled_.store(false, std::memory_order_release);
}

void WindowFocusPlugin::UpdateLastActivityTime() {
    std::lock_guard<std::mutex> lock(activityMutex_);
    lastActivityTime = std::chrono::steady_clock::now();
}

// FIX: Public accessor for shutdown state (used by PlatformTaskDispatcher)
bool WindowFocusPlugin::IsShuttingDown() const {
    return isShuttingDown_.load(std::memory_order_acquire);
}

// FIX: System resume handler - resets activity time and reinitializes devices
void WindowFocusPlugin::OnSystemResume() {
    if (isShuttingDown_.load(std::memory_order_acquire)) return;

    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] System resume detected - resetting state" << std::endl;
    }

    // Reset activity time so we don't immediately trigger inactivity
    UpdateLastActivityTime();

    // Mark user as active since they just woke the machine
    if (!userIsActive_.load(std::memory_order_acquire)) {
        userIsActive_.store(true, std::memory_order_release);
        std::weak_ptr<WindowFocusPlugin> weak;
        {
            std::lock_guard<std::mutex> lock(instanceMutex_);
            weak = instance_;
        }
        PostToMainThread([weak]() {
            if (auto p = weak.lock()) {
                if (!p->isShuttingDown_.load(std::memory_order_acquire)) {
                    p->SafeInvokeMethod("onUserActive", "User is active (system resume)");
                }
            }
        });
    }

    // Signal that HID devices need reinit and audio cache needs refresh
    needsHIDReinit_.store(true, std::memory_order_release);
    needsAudioCacheReset_.store(true, std::memory_order_release);
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

std::string ConvertToUTF8(const std::string& input) {
    if (input.empty()) return std::string();

    int wideSize = MultiByteToWideChar(CP_ACP, 0, input.c_str(), (int)input.size(), nullptr, 0);
    if (wideSize <= 0) return std::string();

    std::wstring utf16_str(wideSize, 0);
    MultiByteToWideChar(CP_ACP, 0, input.c_str(), (int)input.size(), &utf16_str[0], wideSize);

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), (int)utf16_str.size(),
                                        nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0) return std::string();

    std::string utf8_str(utf8Size, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), (int)utf16_str.size(),
                        &utf8_str[0], utf8Size, nullptr, nullptr);

    return utf8_str;
}

std::string ConvertWStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                           nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return std::string();

    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

// =====================================================================
// FIX: Join threads with bounded timeout and message pumping
// =====================================================================
static void JoinThreadsWithMessagePump(std::vector<std::thread>& threads) {
    for (auto& t : threads) {
        if (!t.joinable()) continue;

        HANDLE h = t.native_handle();
        bool threadExited = false;

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (!threadExited && std::chrono::steady_clock::now() < deadline) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            DWORD waitResult = WaitForSingleObject(h, 50);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED) {
                threadExited = true;
            }
        }

        if (t.joinable()) {
            if (threadExited) {
                t.join();
            } else {
                // Last resort during system shutdown: detach rather than deadlock
                t.detach();
            }
        }
    }
    threads.clear();
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

// FIX: Single function to get window info from an HWND, avoiding redundant calls
struct WindowInfo {
    std::string title;
    std::string appName;
};

static WindowInfo GetWindowInfoFromHWND(HWND hwnd) {
    WindowInfo info;

    if (!hwnd) {
        info.title = "";
        info.appName = "<no window in focus>";
        return info;
    }

    // Get title once
    int titleLen = GetWindowTextLengthA(hwnd);
    if (titleLen > 0) {
        info.title.resize(titleLen + 1, '\0');
        GetWindowTextA(hwnd, info.title.data(), titleLen + 1);
        info.title.resize(titleLen);
    }

    // Get process name from the same HWND
    DWORD processID = 0;
    GetWindowThreadProcessId(hwnd, &processID);
    if (processID > 0) {
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
        info.appName = ConvertWStringToUTF8(processName);
    } else {
        info.appName = "<unknown>";
    }

    return info;
}

std::string GetFocusedWindowTitle() {
    HWND hwnd = GetForegroundWindow();
    if (hwnd == NULL) return "";

    int length = GetWindowTextLength(hwnd);
    if (length == 0) return "";

    std::string buffer(length + 1, '\0');
    GetWindowTextA(hwnd, buffer.data(), length + 1);
    buffer.resize(length);
    return buffer;
}

WindowFocusPlugin::WindowFocusPlugin()
    : isShuttingDown_(false)
    , userIsActive_(true)
    , monitorControllers_(false)
    , monitorAudio_(false)
    , monitorHIDDevices_(false)
    , monitorKeyboard_(true)
    , enableDebug_(false)
    , hooksInstalled_(false)
    , inactivityThreshold_(300000)
    , audioThreshold_(0.01f)
    , lastKeyEventTime_(0)
    , needsHIDReinit_(false)
    , needsAudioCacheReset_(false) {

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

    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        shutdownCv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        JoinThreadsWithMessagePump(threads_);
    }

    CloseHIDDevices();

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
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                bool val = std::get<bool>(it->second);
                enableDebug_.store(val, std::memory_order_relaxed);
                std::cout << "[WindowFocus] enableDebug_ set to " << (val ? "true" : "false") << std::endl;
                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'debug'.");
        return;
    }

    if (method_name == "setControllerMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                monitorControllers_.store(std::get<bool>(it->second), std::memory_order_release);
                std::cout << "[WindowFocus] Controller monitoring: "
                          << (monitorControllers_.load(std::memory_order_relaxed) ? "on" : "off") << std::endl;
                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setAudioMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                monitorAudio_.store(std::get<bool>(it->second), std::memory_order_release);
                std::cout << "[WindowFocus] Audio monitoring: "
                          << (monitorAudio_.load(std::memory_order_relaxed) ? "on" : "off") << std::endl;
                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setAudioThreshold") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("threshold"));
            if (it != args->end() && std::holds_alternative<double>(it->second)) {
                audioThreshold_.store(static_cast<float>(std::get<double>(it->second)),
                                      std::memory_order_release);
                std::cout << "[WindowFocus] Audio threshold: " << audioThreshold_.load() << std::endl;
                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a double for 'threshold'.");
        return;
    }

    if (method_name == "setHIDMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                bool newValue = std::get<bool>(it->second);
                bool oldValue = monitorHIDDevices_.exchange(newValue, std::memory_order_acq_rel);
                if (newValue && !oldValue) {
                    InitializeHIDDevices();
                } else if (!newValue && oldValue) {
                    CloseHIDDevices();
                }
                std::cout << "[WindowFocus] HID monitoring: " << (newValue ? "on" : "off") << std::endl;
                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setKeyboardMonitoring") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("enabled"));
            if (it != args->end() && std::holds_alternative<bool>(it->second)) {
                bool newValue = std::get<bool>(it->second);
                bool oldValue = monitorKeyboard_.exchange(newValue, std::memory_order_acq_rel);
                std::cout << "[WindowFocus] Keyboard monitoring: " << (newValue ? "on" : "off") << std::endl;

                if (newValue && !oldValue && !keyboardHook_) {
                    HINSTANCE hInstance = GetModuleHandle(nullptr);
                    keyboardHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
                    if (!keyboardHook_) {
                        std::cerr << "[WindowFocus] Failed to install keyboard hook: "
                                  << GetLastError() << std::endl;
                    }
                    hooksInstalled_.store(keyboardHook_ != nullptr && mouseHook_ != nullptr,
                                          std::memory_order_release);
                } else if (!newValue && oldValue && keyboardHook_) {
                    UnhookWindowsHookEx(keyboardHook_);
                    keyboardHook_ = nullptr;
                    hooksInstalled_.store(false, std::memory_order_release);
                }

                result->Success();
                return;
            }
        }
        result->Error("Invalid argument", "Expected a bool for 'enabled'.");
        return;
    }

    if (method_name == "setInactivityTimeOut") {
        if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
            auto it = args->find(flutter::EncodableValue("inactivityTimeOut"));
            if (it != args->end() && std::holds_alternative<int>(it->second)) {
                inactivityThreshold_.store(std::get<int>(it->second), std::memory_order_release);
                std::cout << "Updated inactivityThreshold_ to " << inactivityThreshold_.load() << std::endl;
                result->Success(flutter::EncodableValue(inactivityThreshold_.load()));
                return;
            }
        }
        result->Error("Invalid argument", "Expected an integer argument.");
    } else if (method_name == "getPlatformVersion") {
        result->Success(flutter::EncodableValue("Windows: example"));
    } else if (method_name == "getIdleThreshold") {
        result->Success(flutter::EncodableValue(inactivityThreshold_.load(std::memory_order_acquire)));
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
    if (hwnd == NULL) return "<no window in focus>";

    DWORD processID = 0;
    GetWindowThreadProcessId(hwnd, &processID);
    if (processID == 0) return "<unknown>";

    return GetProcessName(processID);
}

bool WindowFocusPlugin::CheckControllerInput() {
    if (!monitorControllers_.load(std::memory_order_acquire) ||
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
                std::cout << "[WindowFocus] Mouse movement detected" << std::endl;
            }
            return true;
        }
    }

    return false;
}

bool WindowFocusPlugin::PollKeyboardState() {
    if (isShuttingDown_.load(std::memory_order_acquire)) return false;

    bool exceptionOccurred = false;

    for (int vk = 0x41; vk <= 0x5A; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred) && !exceptionOccurred) return true;
        if (exceptionOccurred) return false;
    }

    for (int vk = 0x30; vk <= 0x39; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred) && !exceptionOccurred) return true;
        if (exceptionOccurred) return false;
    }

    for (int vk = VK_F1; vk <= VK_F12; vk++) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred) && !exceptionOccurred) return true;
        if (exceptionOccurred) return false;
    }

    static const int specialKeys[] = {
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

    for (int vk : specialKeys) {
        if (isShuttingDown_.load(std::memory_order_acquire)) return false;
        if (CheckKeyStateSEH(vk, &exceptionOccurred) && !exceptionOccurred) return true;
        if (exceptionOccurred) return false;
    }

    return false;
}

bool WindowFocusPlugin::CheckKeyboardInput() {
    if (!monitorKeyboard_.load(std::memory_order_acquire) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    if (hooksInstalled_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        uint64_t lastKeyTime = lastKeyEventTime_.load(std::memory_order_acquire);

        if (lastKeyTime > 0 && (currentTime - lastKeyTime) < 200) {
            return true;
        }
        return false;
    }

    return PollKeyboardState();
}

// FIX: CheckSystemAudio now uses cached audio meter via thread-local AudioMeterCache
bool WindowFocusPlugin::CheckSystemAudio() {
    if (!monitorAudio_.load(std::memory_order_acquire) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    // audioMeterCache_ is managed per-thread in MonitorAllInputDevices
    // This method is only called from that thread, so access is safe
    // The cache pointer is passed via the check method
    // (We use the member pointer set by the monitor thread)

    float peakValue = 0.0f;
    bool debug = enableDebug_.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(audioMeterMutex_);
        if (!audioMeterCache_) return false;

        // Check if system resume requested a cache reset
        if (needsAudioCacheReset_.exchange(false, std::memory_order_acq_rel)) {
            audioMeterCache_->Invalidate();
        }

        peakValue = audioMeterCache_->GetPeak(debug);
    }

    if (peakValue > audioThreshold_.load(std::memory_order_acquire)) {
        if (debug) {
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
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

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
        deviceInfoSet, nullptr, &hidGuid, memberIndex++, &deviceInterfaceData)) {

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet, &deviceInterfaceData,
            nullptr, 0, &requiredSize, nullptr);

        if (requiredSize == 0) continue;

        std::vector<BYTE> detailBuffer(requiredSize);
        PSP_DEVICE_INTERFACE_DETAIL_DATA detailData =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(detailBuffer.data());
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet, &deviceInterfaceData,
            detailData, requiredSize, nullptr, nullptr)) {
            continue;
        }

        HANDLE deviceHandle = CreateHIDDeviceHandleSEH(detailData->DevicePath);
        if (deviceHandle == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes;
        attributes.Size = sizeof(HIDD_ATTRIBUTES);

        if (!GetHIDAttributesSEH(deviceHandle, &attributes)) {
            CloseHandleSEH(deviceHandle);
            continue;
        }

        PHIDP_PREPARSED_DATA preparsedData = nullptr;
        if (!GetHIDPreparsedDataSEH(deviceHandle, &preparsedData)) {
            CloseHandleSEH(deviceHandle);
            continue;
        }

        HIDP_CAPS caps;
        ZeroMemory(&caps, sizeof(caps));
        bool gotCaps = GetHIDCapsSEH(preparsedData, &caps);

        FreePreparsedDataSEH(preparsedData);

        if (!gotCaps) {
            CloseHandleSEH(deviceHandle);
            continue;
        }

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
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (enableDebug_.load(std::memory_order_relaxed)) {
        std::cout << "[WindowFocus] Initialized " << hidDeviceHandles_.size()
                  << " HID devices" << std::endl;
    }
}

bool WindowFocusPlugin::CheckHIDDevices() {
    if (!monitorHIDDevices_.load(std::memory_order_acquire) ||
        isShuttingDown_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(hidDevicesMutex_);

    if (hidDeviceHandles_.empty()) return false;

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

        {
            OverlappedGuard ovlGuard(deviceHandle);
            if (!ovlGuard.IsValid()) {
                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Failed to create event for HID device "
                              << i << std::endl;
                }
                continue;
            }

            DWORD bytesRead = 0;
            DWORD bufferSize = static_cast<DWORD>(buffer.size());
            DWORD errorCode = 0;

            bool readSucceeded = ReadHIDDeviceSEH(
                deviceHandle, buffer.data(), bufferSize,
                ovlGuard.Get(), &bytesRead, &errorCode);

            if (readSucceeded) {
                ovlGuard.MarkComplete();
                if (bytesRead > 0 && buffer != lastState) {
                    inputDetected = true;
                    lastState = buffer;
                    if (enableDebug_.load(std::memory_order_relaxed)) {
                        std::cout << "[WindowFocus] HID device " << i
                                  << " input detected" << std::endl;
                    }
                }
            } else if (errorCode == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(ovlGuard.ovl.hEvent, 10);
                if (waitResult == WAIT_OBJECT_0) {
                    ovlGuard.MarkComplete();
                    DWORD overlappedError = 0;
                    if (GetOverlappedResultSEH(deviceHandle, ovlGuard.Get(),
                                                &bytesRead, &overlappedError)) {
                        if (bytesRead > 0 && buffer != lastState) {
                            inputDetected = true;
                            lastState = buffer;
                            if (enableDebug_.load(std::memory_order_relaxed)) {
                                std::cout << "[WindowFocus] HID device " << i
                                          << " input (overlapped)" << std::endl;
                            }
                        }
                    } else if (overlappedError == ERROR_INVALID_HANDLE ||
                               overlappedError == ERROR_DEVICE_NOT_CONNECTED) {
                        ovlGuard.InvalidateDevice();
                        ovlGuard.MarkComplete();
                        invalidDevices.push_back(i);
                    }
                } else if (waitResult == WAIT_FAILED) {
                    if (enableDebug_.load(std::memory_order_relaxed)) {
                        std::cerr << "[WindowFocus] HID device " << i
                                  << " wait failed: " << GetLastError() << std::endl;
                    }
                    ovlGuard.InvalidateDevice();
                    ovlGuard.MarkComplete();
                    invalidDevices.push_back(i);
                }
            } else if (errorCode == ERROR_DEVICE_NOT_CONNECTED ||
                       errorCode == ERROR_GEN_FAILURE ||
                       errorCode == ERROR_INVALID_HANDLE ||
                       errorCode == ERROR_BAD_DEVICE) {
                ovlGuard.InvalidateDevice();
                ovlGuard.MarkComplete();
                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] HID device " << i
                              << " disconnected (error: " << errorCode << ")" << std::endl;
                }
                invalidDevices.push_back(i);
            } else {
                ovlGuard.InvalidateDevice();
                ovlGuard.MarkComplete();
                if (enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Error reading HID device " << i
                              << " (code: 0x" << std::hex << errorCode << std::dec
                              << ")" << std::endl;
                }
                invalidDevices.push_back(i);
            }
        } // OverlappedGuard destroyed here

        if (inputDetected) break;
    }

    if (!invalidDevices.empty()) {
                std::sort(invalidDevices.begin(), invalidDevices.end());
        invalidDevices.erase(std::unique(invalidDevices.begin(), invalidDevices.end()),
                             invalidDevices.end());

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
                    std::cout << "[WindowFocus] Removed invalid HID device at index "
                              << idx << std::endl;
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
    if (monitorHIDDevices_.load(std::memory_order_acquire)) {
        InitializeHIDDevices();
    }

    std::weak_ptr<WindowFocusPlugin> weak = shared_from_this();

    std::lock_guard<std::mutex> tlock(threadsMutex_);
    threads_.emplace_back([weak]() {
        // COM initialization owned by this thread
        bool comInitialized = false;
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInitialized = SUCCEEDED(hr);
        }

        // FIX: Create thread-local audio meter cache and register it with the plugin
        auto audioCache = std::make_unique<AudioMeterCache>();
        {
            auto self = weak.lock();
            if (self) {
                std::lock_guard<std::mutex> lock(self->audioMeterMutex_);
                self->audioMeterCache_ = audioCache.get();
            }
        }

        auto lastHIDReinit = std::chrono::steady_clock::now();
        const auto hidReinitInterval = std::chrono::seconds(30);

        // FIX: Periodic full HID refresh to clear stale handles
        auto lastFullHIDRefresh = std::chrono::steady_clock::now();
        const auto fullHIDRefreshInterval = std::chrono::minutes(5);

        while (true) {
            auto self = weak.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            {
                std::unique_lock<std::mutex> lock(self->shutdownMutex_);
                if (self->shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [&self] { return self->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }

            // Re-check after waking
            self = weak.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            bool inputDetected = false;

            try {
                if (!self->isShuttingDown_.load(std::memory_order_acquire) &&
                    self->CheckKeyboardInput())   inputDetected = true;
                if (!self->isShuttingDown_.load(std::memory_order_acquire) &&
                    self->CheckControllerInput()) inputDetected = true;
                if (!self->isShuttingDown_.load(std::memory_order_acquire) &&
                    self->CheckRawInput())        inputDetected = true;
                if (!self->isShuttingDown_.load(std::memory_order_acquire) &&
                    self->CheckSystemAudio())     inputDetected = true;
                if (!self->isShuttingDown_.load(std::memory_order_acquire) &&
                    self->CheckHIDDevices())      inputDetected = true;
            } catch (...) {
                if (self->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Exception in MonitorAllInputDevices loop"
                              << std::endl;
                }
            }

            // FIX: Handle system resume signal for HID reinit
            if (self->needsHIDReinit_.exchange(false, std::memory_order_acq_rel)) {
                if (self->monitorHIDDevices_.load(std::memory_order_acquire) &&
                    !self->isShuttingDown_.load(std::memory_order_acquire)) {
                    if (self->enableDebug_.load(std::memory_order_relaxed)) {
                        std::cout << "[WindowFocus] System resume: reinitializing HID devices" << std::endl;
                    }
                    self->CloseHIDDevices();
                    self->InitializeHIDDevices();
                    lastHIDReinit = std::chrono::steady_clock::now();
                    lastFullHIDRefresh = std::chrono::steady_clock::now();
                }
            }

            // Periodic HID re-initialization (when list is empty)
            if (self->monitorHIDDevices_.load(std::memory_order_acquire) &&
                !self->isShuttingDown_.load(std::memory_order_acquire)) {
                auto now = std::chrono::steady_clock::now();

                // Quick reinit if all devices disappeared
                if (now - lastHIDReinit > hidReinitInterval) {
                    lastHIDReinit = now;
                    bool needsReinit = false;
                    {
                        std::lock_guard<std::mutex> lock(self->hidDevicesMutex_);
                        needsReinit = self->hidDeviceHandles_.empty();
                    }
                    if (needsReinit && !self->isShuttingDown_.load(std::memory_order_acquire)) {
                        if (self->enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "[WindowFocus] Re-initializing HID devices (empty list)" << std::endl;
                        }
                        self->InitializeHIDDevices();
                    }
                }

                // FIX: Full refresh to clear stale handles that passed IsHandleValid
                if (now - lastFullHIDRefresh > fullHIDRefreshInterval) {
                    lastFullHIDRefresh = now;
                    if (!self->isShuttingDown_.load(std::memory_order_acquire)) {
                        if (self->enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "[WindowFocus] Periodic full HID device refresh" << std::endl;
                        }
                        self->CloseHIDDevices();
                        self->InitializeHIDDevices();
                    }
                }
            }

            if (inputDetected && !self->isShuttingDown_.load(std::memory_order_acquire)) {
                self->UpdateLastActivityTime();

                if (!self->userIsActive_.load(std::memory_order_acquire)) {
                    self->userIsActive_.store(true, std::memory_order_release);
                    self->PostToMainThread([weak]() {
                        if (auto p = weak.lock()) {
                            if (!p->isShuttingDown_.load(std::memory_order_acquire)) {
                                p->SafeInvokeMethod("onUserActive", "User is active");
                            }
                        }
                    });
                }
            }

            // Release self before sleeping to avoid preventing destruction
            self.reset();
        }

        // FIX: Unregister audio cache before thread exits
        {
            auto self = weak.lock();
            if (self) {
                std::lock_guard<std::mutex> lock(self->audioMeterMutex_);
                self->audioMeterCache_ = nullptr;
            }
        }
        audioCache.reset();

        if (comInitialized) {
            CoUninitialize();
        }
    });
}

void WindowFocusPlugin::CheckForInactivity() {
    std::weak_ptr<WindowFocusPlugin> weak = shared_from_this();

    std::lock_guard<std::mutex> tlock(threadsMutex_);
    threads_.emplace_back([weak]() {
        while (true) {
            auto self = weak.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            {
                std::unique_lock<std::mutex> lock(self->shutdownMutex_);
                if (self->shutdownCv_.wait_for(lock, std::chrono::seconds(1),
                    [&self] { return self->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }

            self = weak.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            auto now = std::chrono::steady_clock::now();
            int64_t duration;

            {
                std::lock_guard<std::mutex> lock(self->activityMutex_);
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - self->lastActivityTime).count();
            }

            if (duration > self->inactivityThreshold_.load(std::memory_order_acquire) &&
                self->userIsActive_.load(std::memory_order_acquire)) {
                self->userIsActive_.store(false, std::memory_order_release);
                if (self->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cout << "[WindowFocus] User inactive. Duration: " << duration
                              << "ms, Threshold: " << self->inactivityThreshold_.load()
                              << "ms" << std::endl;
                }
                self->PostToMainThread([weak]() {
                    if (auto p = weak.lock()) {
                        if (!p->isShuttingDown_.load(std::memory_order_acquire)) {
                            p->SafeInvokeMethod("onUserInactivity", "User is inactive");
                        }
                    }
                });
            }

            // Release self before sleeping to avoid preventing destruction
            self.reset();
        }
    });
}

void WindowFocusPlugin::StartFocusListener() {
    std::weak_ptr<WindowFocusPlugin> weak = shared_from_this();

    std::lock_guard<std::mutex> tlock(threadsMutex_);
    threads_.emplace_back([weak]() {
        HWND last_focused = nullptr;

        // FIX: Debounce timer for focus changes
        auto lastFocusEventTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto focusDebounceInterval = std::chrono::milliseconds(250);

        while (true) {
            auto self = weak.lock();
            if (!self || self->isShuttingDown_.load(std::memory_order_acquire)) break;

            try {
                HWND current_focused = GetForegroundWindow();
                if (current_focused != last_focused) {
                    last_focused = current_focused;

                    // FIX: Debounce rapid focus changes
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastFocusEventTime < focusDebounceInterval) {
                        // Skip this event - too soon after the last one
                    } else if (current_focused != nullptr) {
                        lastFocusEventTime = now;

                        // FIX: Get all window info from single HWND in one call
                        WindowInfo winfo = GetWindowInfoFromHWND(current_focused);

                        if (self->enableDebug_.load(std::memory_order_relaxed)) {
                            std::cout << "Current window title: " << winfo.title << std::endl;
                            std::cout << "Current window appName: " << winfo.appName << std::endl;
                        }

                        std::string utf8_title = ConvertToUTF8(winfo.title);

                        flutter::EncodableMap data;
                        data[flutter::EncodableValue("title")]       = flutter::EncodableValue(utf8_title);
                        data[flutter::EncodableValue("appName")]     = flutter::EncodableValue(winfo.appName);
                        data[flutter::EncodableValue("windowTitle")] = flutter::EncodableValue(utf8_title);

                        if (!self->isShuttingDown_.load(std::memory_order_acquire)) {
                            self->PostToMainThread([weak, d = std::move(data)]() mutable {
                                if (auto p = weak.lock()) {
                                    if (!p->isShuttingDown_.load(std::memory_order_acquire)) {
                                        p->SafeInvokeMethodWithMap("onFocusChange", std::move(d));
                                    }
                                }
                            });
                        }
                    }
                }
            } catch (...) {
                auto self2 = weak.lock();
                if (self2 && self2->enableDebug_.load(std::memory_order_relaxed)) {
                    std::cerr << "[WindowFocus] Exception in StartFocusListener loop" << std::endl;
                }
            }

            // Release self before sleeping to avoid preventing destruction
            self.reset();

            {
                auto self2 = weak.lock();
                if (!self2 || self2->isShuttingDown_.load(std::memory_order_acquire)) break;

                std::unique_lock<std::mutex> lock(self2->shutdownMutex_);
                if (self2->shutdownCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [&self2] { return self2->isShuttingDown_.load(std::memory_order_acquire); })) {
                    break;
                }
            }
        }
    });
}

// =====================================================================
// GDI+ singleton
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

// FIX: Screenshot with proper encoder validation and RAII for GDI+ objects
std::optional<std::vector<uint8_t>> WindowFocusPlugin::TakeScreenshot(bool activeWindowOnly) {
    std::lock_guard<std::mutex> lock(screenshotMutex_);

    if (!GdiplusLifetime::Get().IsValid()) return std::nullopt;

    HWND hwnd = activeWindowOnly ? GetForegroundWindow() : GetDesktopWindow();
    if (hwnd == NULL) hwnd = GetDesktopWindow();

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return std::nullopt;
    ReleaseDCGuard screenDcGuard(NULL, hdcScreen);

    HDC hdcWindow = GetDC(hwnd);
    if (!hdcWindow) return std::nullopt;
    ReleaseDCGuard windowDcGuard(hwnd, hdcWindow);

    HDC hdcMemDC = CreateCompatibleDC(hdcWindow);
    if (!hdcMemDC) return std::nullopt;
    DeleteDCGuard memDcGuard(hdcMemDC);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int width  = rc.right  - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) return std::nullopt;

    HBITMAP hbmScreen = CreateCompatibleBitmap(hdcWindow, width, height);
    if (!hbmScreen) return std::nullopt;
    DeleteObjectGuard bitmapGuard(hbmScreen);

    HGDIOBJ oldBitmap = SelectObject(hdcMemDC, hbmScreen);
    BitBlt(hdcMemDC, 0, 0, width, height, hdcScreen, rc.left, rc.top, SRCCOPY);
    SelectObject(hdcMemDC, oldBitmap);

    // FIX: Use unique_ptr for Gdiplus::Bitmap (new can't return nullptr, check status instead)
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(hbmScreen, nullptr);
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        return std::nullopt;
    }

    // FIX: Validate encoder CLSID before using it
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cerr << "[WindowFocus] PNG encoder not found" << std::endl;
        }
        return std::nullopt;
    }

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        return std::nullopt;
    }

    // FIX: Check Save return value
    Gdiplus::Status saveStatus = bitmap->Save(stream, &pngClsid, NULL);
    if (saveStatus != Gdiplus::Ok) {
        stream->Release();
        if (enableDebug_.load(std::memory_order_relaxed)) {
            std::cerr << "[WindowFocus] Bitmap::Save failed with status " << saveStatus << std::endl;
        }
        return std::nullopt;
    }

    STATSTG statstg;
    hr = stream->Stat(&statstg, STATFLAG_DEFAULT);
    if (FAILED(hr)) {
        stream->Release();
        return std::nullopt;
    }

    ULONG fileSize = (ULONG)statstg.cbSize.QuadPart;
    if (fileSize == 0) {
        stream->Release();
        return std::nullopt;
    }

    std::vector<uint8_t> data(fileSize);
    LARGE_INTEGER liZero = {};
    stream->Seek(liZero, STREAM_SEEK_SET, NULL);
    ULONG bytesRead = 0;
    stream->Read(data.data(), fileSize, &bytesRead);

    stream->Release();

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

    std::vector<BYTE> codecBuffer(size);
    Gdiplus::ImageCodecInfo* pImageCodecInfo =
        reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecBuffer.data());

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

}  // namespace window_focus