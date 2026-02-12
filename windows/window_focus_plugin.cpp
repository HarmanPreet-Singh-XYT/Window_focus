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
#include <gdiplus.h>
#include <setupapi.h>
#include <hidclass.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <atomic>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace window_focus {

WindowFocusPlugin* WindowFocusPlugin::instance_ = nullptr;
HHOOK WindowFocusPlugin::mouseHook_ = nullptr;

using CallbackMethod = std::function<void(const std::wstring&)>;

// =====================================================================
// SEH-isolated helper functions (no C++ objects with destructors allowed)
// These functions MUST NOT use std::vector, std::string, std::lock_guard,
// or any other C++ objects that require stack unwinding.
// =====================================================================

// SEH-protected ReadFile wrapper for HID devices
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

// SEH-protected GetOverlappedResult wrapper
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

// SEH-protected HidD_GetAttributes wrapper
static bool GetHIDAttributesSEH(HANDLE deviceHandle, HIDD_ATTRIBUTES* attributes) {
    __try {
        return HidD_GetAttributes(deviceHandle, attributes) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected HidD_GetPreparsedData wrapper
static bool GetHIDPreparsedDataSEH(HANDLE deviceHandle, PHIDP_PREPARSED_DATA* preparsedData) {
    __try {
        return HidD_GetPreparsedData(deviceHandle, preparsedData) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected HidP_GetCaps wrapper
static bool GetHIDCapsSEH(PHIDP_PREPARSED_DATA preparsedData, HIDP_CAPS* caps) {
    __try {
        return (HidP_GetCaps(preparsedData, caps) == HIDP_STATUS_SUCCESS);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-protected audio peak value retrieval
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

    if (meterInfo) {
        __try { meterInfo->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        meterInfo = nullptr;
    }
    if (defaultDevice) {
        __try { defaultDevice->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        defaultDevice = nullptr;
    }
    if (deviceEnumerator) {
        __try { deviceEnumerator->Release(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        deviceEnumerator = nullptr;
    }

    return success;
}

// SEH-protected XInputGetState wrapper
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

// SEH-protected CreateFile wrapper for HID device paths
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

// SEH-protected CloseHandle wrapper
static bool CloseHandleSEH(HANDLE handle) {
    __try {
        return CloseHandle(handle) ? true : false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// =====================================================================
// End of SEH-isolated helpers
// =====================================================================

LRESULT CALLBACK WindowFocusPlugin::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && instance_) {
    if (instance_->enableDebug_) {
      std::cout << "[WindowFocus] mouse hook detected action" << std::endl;
    }
    instance_->UpdateLastActivityTime();
    if (!instance_->userIsActive_) {
      instance_->userIsActive_ = true;
      if (instance_->channel) {
        std::lock_guard<std::mutex> lock(instance_->channelMutex_);
        instance_->channel->InvokeMethod(
          "onUserActive",
          std::make_unique<flutter::EncodableValue>("User is active"));
      }
    }
  }
  return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

void WindowFocusPlugin::SetHooks() {
  if (instance_ && instance_->enableDebug_) {
    std::cout << "[WindowFocus] SetHooks: start\n";
  }
  HINSTANCE hInstance = GetModuleHandle(nullptr);

  mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, hInstance, 0);
  if (!mouseHook_) {
    std::cerr << "[WindowFocus] Failed to install mouse hook: " << GetLastError() << std::endl;
  } else {
    if (instance_ && instance_->enableDebug_) {
      std::cout << "[WindowFocus] Mouse hook installed successfully\n";
    }
  }
}

void WindowFocusPlugin::RemoveHooks() {
  if (mouseHook_) {
    UnhookWindowsHookEx(mouseHook_);
    mouseHook_ = nullptr;
  }
}

void WindowFocusPlugin::UpdateLastActivityTime() {
  std::lock_guard<std::mutex> lock(activityMutex_);
  lastActivityTime = std::chrono::steady_clock::now();
}

std::string ConvertWindows1251ToUTF8(const std::string& windows1251_str) {
    int size_needed = MultiByteToWideChar(1251, 0, windows1251_str.c_str(), -1, NULL, 0);
    std::wstring utf16_str(size_needed, 0);
    MultiByteToWideChar(1251, 0, windows1251_str.c_str(), -1, &utf16_str[0], size_needed);

    size_needed = WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8_str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16_str.c_str(), -1, &utf8_str[0], size_needed, NULL, NULL);

    return utf8_str;
}

std::string ConvertWStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
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

    char* buffer = new char[length + 1];
    GetWindowTextA(hwnd, buffer, length + 1);

    std::string windowTitle(buffer);

    delete[] buffer;

    return windowTitle;
}

WindowFocusPlugin::WindowFocusPlugin() : isShuttingDown_(false) {
  instance_ = this;

  lastActivityTime = std::chrono::steady_clock::now();
  
  // Initialize controller states
  ZeroMemory(lastControllerStates_, sizeof(lastControllerStates_));
  
  // Initialize last mouse position
  GetCursorPos(&lastMousePosition_);
}

WindowFocusPlugin::~WindowFocusPlugin() {
  isShuttingDown_ = true;
  
  // Give threads time to finish
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  
  CloseHIDDevices();
  RemoveHooks();
  instance_ = nullptr;
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
            std::cout << "[WindowFocus] C++: enableDebug_ set to " << (enableDebug_ ? "true" : "false") << std::endl;
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
            std::cout << "[WindowFocus] Controller monitoring set to " << (monitorControllers_ ? "true" : "false") << std::endl;
            result->Success();
            return;
          }
        }
      }
      result->Error("Invalid argument", "Expected a bool for 'enabled'.");
      return;
  }

  // Set audio monitoring
  if (method_name == "setAudioMonitoring") {
      if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
        auto it = args->find(flutter::EncodableValue("enabled"));
        if (it != args->end()) {
          if (std::holds_alternative<bool>(it->second)) {
            monitorAudio_ = std::get<bool>(it->second);
            std::cout << "[WindowFocus] Audio monitoring set to " << (monitorAudio_ ? "true" : "false") << std::endl;
            result->Success();
            return;
          }
        }
      }
      result->Error("Invalid argument", "Expected a bool for 'enabled'.");
      return;
  }

  // Set audio threshold
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

  // Set HID device monitoring
  if (method_name == "setHIDMonitoring") {
      if (const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments())) {
        auto it = args->find(flutter::EncodableValue("enabled"));
        if (it != args->end()) {
          if (std::holds_alternative<bool>(it->second)) {
            bool newValue = std::get<bool>(it->second);
            if (newValue && !monitorHIDDevices_) {
              // Re-initialize HID devices if enabling
              InitializeHIDDevices();
            } else if (!newValue && monitorHIDDevices_) {
              // Close HID devices if disabling
              CloseHIDDevices();
            }
            monitorHIDDevices_ = newValue;
            std::cout << "[WindowFocus] HID device monitoring set to " << (monitorHIDDevices_ ? "true" : "false") << std::endl;
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
    if (!monitorControllers_) {
        return false;
    }

    bool inputDetected = false;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
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

// Check system audio playback - uses SEH-isolated helper
bool WindowFocusPlugin::CheckSystemAudio() {
    if (!monitorAudio_) {
        return false;
    }

    static bool comInitialized = false;
    if (!comInitialized) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            comInitialized = true;
        } else {
            return false;
        }
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

// Initialize HID devices - uses SEH-isolated helpers for all HID API calls
void WindowFocusPlugin::InitializeHIDDevices() {
    std::lock_guard<std::mutex> lock(hidDevicesMutex_);
    
    // Close any existing devices first
    for (HANDLE handle : hidDeviceHandles_) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandleSEH(handle);
        }
    }
    hidDeviceHandles_.clear();
    lastHIDStates_.clear();
    
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
            std::cerr << "[WindowFocus] Failed to get HID device info set" << std::endl;
        }
        return;
    }
    
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    
    DWORD memberIndex = 0;
    while (SetupDiEnumDeviceInterfaces(
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
        if (!detailData) continue;
        
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        
        if (SetupDiGetDeviceInterfaceDetail(
            deviceInfoSet,
            &deviceInterfaceData,
            detailData,
            requiredSize,
            nullptr,
            nullptr
        )) {
            // Use SEH-protected CreateFile
            HANDLE deviceHandle = CreateHIDDeviceHandleSEH(detailData->DevicePath);
            
            if (deviceHandle != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attributes;
                attributes.Size = sizeof(HIDD_ATTRIBUTES);
                
                // Use SEH-protected GetAttributes
                if (GetHIDAttributesSEH(deviceHandle, &attributes)) {
                    PHIDP_PREPARSED_DATA preparsedData = nullptr;
                    
                    // Use SEH-protected GetPreparsedData
                    if (GetHIDPreparsedDataSEH(deviceHandle, &preparsedData)) {
                        HIDP_CAPS caps;
                        ZeroMemory(&caps, sizeof(caps));
                        
                        // Use SEH-protected GetCaps
                        if (GetHIDCapsSEH(preparsedData, &caps)) {
                            // EXCLUDE audio devices (microphones, headsets, etc.)
                            bool isAudioDevice = (caps.UsagePage == 0x0B || caps.UsagePage == 0x0C);
                            
                            // INCLUDE all other HID devices with input capabilities
                            if (!isAudioDevice && caps.InputReportByteLength > 0) {
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
                                if (enableDebug_ && isAudioDevice) {
                                    std::cout << "[WindowFocus] Skipping audio device: VID=" 
                                              << std::hex << attributes.VendorID 
                                              << " PID=" << attributes.ProductID 
                                              << " UsagePage=0x" << caps.UsagePage << std::dec << std::endl;
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

// Check HID devices for input - uses SEH-isolated helpers
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
        
        // Validate handle before use
        if (deviceHandle == INVALID_HANDLE_VALUE || deviceHandle == nullptr) {
            invalidDevices.push_back(i);
            continue;
        }
        
        std::vector<BYTE>& lastState = lastHIDStates_[i];
        std::vector<BYTE> buffer(lastState.size());
        
        if (buffer.empty()) {
            continue;
        }
        
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        
        if (overlapped.hEvent == nullptr) {
            if (enableDebug_) {
                std::cerr << "[WindowFocus] Failed to create event for HID device " << i << std::endl;
            }
            continue;
        }
        
        DWORD bytesRead = 0;
        DWORD bufferSize = static_cast<DWORD>(buffer.size());
        DWORD errorCode = 0;

        // Use SEH-protected ReadFile
        bool readSucceeded = ReadHIDDeviceSEH(
            deviceHandle, buffer.data(), bufferSize,
            &overlapped, &bytesRead, &errorCode);

        if (readSucceeded) {
            // Read completed immediately
            if (bytesRead > 0 && buffer != lastState) {
                inputDetected = true;
                lastState = buffer;
                
                if (enableDebug_) {
                    std::cout << "[WindowFocus] HID device " << i << " input detected" << std::endl;
                }
            }
        } else if (errorCode == ERROR_IO_PENDING) {
            // Wait for a short time (non-blocking check)
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 10);
            if (waitResult == WAIT_OBJECT_0) {
                DWORD overlappedError = 0;
                // Use SEH-protected GetOverlappedResult
                if (GetOverlappedResultSEH(deviceHandle, &overlapped, &bytesRead, &overlappedError)) {
                    if (bytesRead > 0 && buffer != lastState) {
                        inputDetected = true;
                        lastState = buffer;
                        
                        if (enableDebug_) {
                            std::cout << "[WindowFocus] HID device " << i << " input detected (overlapped)" << std::endl;
                        }
                    }
                } else if (overlappedError == ERROR_INVALID_HANDLE || 
                           overlappedError == ERROR_DEVICE_NOT_CONNECTED) {
                    invalidDevices.push_back(i);
                }
            } else if (waitResult == WAIT_TIMEOUT) {
                // Normal - no data available yet, cancel the pending I/O
                CancelIo(deviceHandle);
            } else if (waitResult == WAIT_FAILED) {
                DWORD waitError = GetLastError();
                if (enableDebug_) {
                    std::cout << "[WindowFocus] HID device " << i << " wait failed: " << waitError << std::endl;
                }
                if (waitError == ERROR_INVALID_HANDLE) {
                    invalidDevices.push_back(i);
                }
            }
        } else if (errorCode == ERROR_DEVICE_NOT_CONNECTED || 
                   errorCode == ERROR_GEN_FAILURE || 
                   errorCode == ERROR_INVALID_HANDLE ||
                   errorCode == ERROR_BAD_DEVICE) {
            // Device was disconnected or is invalid
            if (enableDebug_) {
                std::cout << "[WindowFocus] HID device " << i << " disconnected or invalid (error: " << errorCode << ")" << std::endl;
            }
            invalidDevices.push_back(i);
        } else if (errorCode != ERROR_SUCCESS) {
            // SEH exception was caught or other error
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
    
    // Remove invalid devices (iterate in reverse to preserve indices)
    if (!invalidDevices.empty()) {
        // Remove duplicates first
        std::sort(invalidDevices.begin(), invalidDevices.end());
        invalidDevices.erase(std::unique(invalidDevices.begin(), invalidDevices.end()), invalidDevices.end());
        
        for (auto it = invalidDevices.rbegin(); it != invalidDevices.rend(); ++it) {
            size_t idx = *it;
            if (idx < hidDeviceHandles_.size()) {
                HANDLE handle = hidDeviceHandles_[idx];
                if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                    CancelIo(handle);
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

// Close all HID devices
void WindowFocusPlugin::CloseHIDDevices() {
    std::lock_guard<std::mutex> lock(hidDevicesMutex_);
    
    for (HANDLE handle : hidDeviceHandles_) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CancelIo(handle);
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
    // Initialize HID devices
    if (monitorHIDDevices_) {
        InitializeHIDDevices();
    }
    
    std::thread([this]() {
        // Track HID re-initialization interval
        auto lastHIDReinit = std::chrono::steady_clock::now();
        const auto hidReinitInterval = std::chrono::seconds(30);
        
        while (!isShuttingDown_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (isShuttingDown_) break;

            bool inputDetected = false;

            // Check controllers (XInput)
            if (CheckControllerInput()) {
                inputDetected = true;
            }

            if (isShuttingDown_) break;

            // Check raw input (mouse movement via cursor position)
            if (CheckRawInput()) {
                inputDetected = true;
            }

            if (isShuttingDown_) break;

            // Check system audio
            if (CheckSystemAudio()) {
                inputDetected = true;
            }

            if (isShuttingDown_) break;

            // Check HID devices (Logitech G29, flight sticks, etc.)
            if (CheckHIDDevices()) {
                inputDetected = true;
            }

            // Periodically re-initialize HID devices to pick up newly connected ones
            if (monitorHIDDevices_ && !isShuttingDown_) {
                auto now = std::chrono::steady_clock::now();
                if (now - lastHIDReinit > hidReinitInterval) {
                    lastHIDReinit = now;
                    bool needsReinit = false;
                    {
                        std::lock_guard<std::mutex> lock(hidDevicesMutex_);
                        needsReinit = hidDeviceHandles_.empty();
                    }
                    if (needsReinit) {
                        if (enableDebug_) {
                            std::cout << "[WindowFocus] Re-initializing HID devices (all disconnected)" << std::endl;
                        }
                        InitializeHIDDevices();
                    }
                }
            }

            // If any input was detected, update activity time
            if (inputDetected) {
                UpdateLastActivityTime();
                
                // If user was inactive, mark them as active
                if (!userIsActive_) {
                    userIsActive_ = true;
                    if (channel && !isShuttingDown_) {
                        std::lock_guard<std::mutex> lock(channelMutex_);
                        channel->InvokeMethod(
                            "onUserActive",
                            std::make_unique<flutter::EncodableValue>("User is active"));
                    }
                }
            }
        }
    }).detach();
}

void WindowFocusPlugin::CheckForInactivity() {
  std::thread([this]() {
   while (!isShuttingDown_) {
     std::this_thread::sleep_for(std::chrono::seconds(1));
     
     if (isShuttingDown_) break;
     
     auto now = std::chrono::steady_clock::now();
     int64_t duration;
     
     {
       std::lock_guard<std::mutex> lock(activityMutex_);
       duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActivityTime).count();
     }

     if (duration > inactivityThreshold_ && userIsActive_) {
       userIsActive_ = false;
        if (instance_ && instance_->enableDebug_) {
           std::cout << "[WindowFocus] User is inactive. Duration: " << duration << "ms, Threshold: " << inactivityThreshold_ << "ms" << std::endl;
         }
       if (channel && !isShuttingDown_) {
         std::lock_guard<std::mutex> lock(channelMutex_);
         channel->InvokeMethod("onUserInactivity",
           std::make_unique<flutter::EncodableValue>("User is inactive"));
       }
     }
   }
  }).detach();
}

void WindowFocusPlugin::StartFocusListener(){
    std::thread([this]() {
        HWND last_focused = nullptr;
        while (!isShuttingDown_) {
            HWND current_focused = GetForegroundWindow();
            if (current_focused != last_focused) {
                last_focused = current_focused;
                char title[256];
                GetWindowTextA(current_focused, title, sizeof(title));
                std::string appName = GetFocusedWindowAppName();
                std::string windowTitle = GetFocusedWindowTitle();
                std::string window_title(title);

                if (instance_ && instance_->enableDebug_) {
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
                
                if (channel && !isShuttingDown_) {
                    std::lock_guard<std::mutex> lock(channelMutex_);
                    channel->InvokeMethod("onFocusChange", std::make_unique<flutter::EncodableValue>(data));
                }
            }
            Sleep(100);
        }
    }).detach();
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
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    HWND hwnd = activeWindowOnly ? GetForegroundWindow() : GetDesktopWindow();
    if (hwnd == NULL) hwnd = GetDesktopWindow();

    HDC hdcScreen = GetDC(NULL);
    HDC hdcWindow = GetDC(hwnd);
    HDC hdcMemDC = CreateCompatibleDC(hdcWindow);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) {
        DeleteDC(hdcMemDC);
        ReleaseDC(hwnd, hdcWindow);
        ReleaseDC(NULL, hdcScreen);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return std::nullopt;
    }

    HBITMAP hbmScreen = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ oldBitmap = SelectObject(hdcMemDC, hbmScreen);

    if (activeWindowOnly) {
        BitBlt(hdcMemDC, 0, 0, width, height, hdcScreen, rc.left, rc.top, SRCCOPY);
    } else {
        BitBlt(hdcMemDC, 0, 0, width, height, hdcScreen, rc.left, rc.top, SRCCOPY);
    }

    Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(hbmScreen, NULL);
    IStream* stream = NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &stream);

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

    Gdiplus::GdiplusShutdown(gdiplusToken);

    if (bytesRead > 0) {
        return data;
    }
    return std::nullopt;
}

}  // namespace window_focus