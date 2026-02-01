import Cocoa
import FlutterMacOS
import AppKit
import ApplicationServices
import Foundation
import IOKit
import IOKit.hid
import AVFoundation
import CoreAudio

public class WindowFocusPlugin: NSObject, FlutterPlugin {
    var channel: FlutterMethodChannel?
    var windowFocusObserver: WindowFocusObserver?
    var idleTracker: IdleTracker?

    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: "expert.kotelnikoff/window_focus", binaryMessenger: registrar.messenger)
        let instance = WindowFocusPlugin()
        registrar.addMethodCallDelegate(instance, channel: channel)

        instance.windowFocusObserver = WindowFocusObserver { (appName, windowTitle) in
            channel.invokeMethod("onFocusChange", arguments: ["appName": appName, "windowTitle": windowTitle]) { (result) in
            }
        }
        instance.idleTracker = IdleTracker(channel: channel)
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "getPlatformVersion":
            result("macOS " + ProcessInfo.processInfo.operatingSystemVersionString)
            
        case "setInactivityTimeOut":
            if let args = call.arguments as? [String: Any],
               let threshold = args["inactivityTimeOut"] as? TimeInterval {
                idleTracker?.setIdleThreshold(threshold / 1000.0)
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'inactivityTimeOut' parameter", details: nil))
            }
            
        case "getIdleThreshold":
            if let threshold = idleTracker?.idleThreshold {
                result(Int(threshold * 1000))
            } else {
                result(0)
            }
            
        case "setDebugMode":
            if let args = call.arguments as? [String: Any],
               let debug = args["debug"] as? Bool {
                idleTracker?.setDebugMode(debug)
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'debug' parameter", details: nil))
            }
            
        case "setControllerMonitoring":
            if let args = call.arguments as? [String: Any],
               let enabled = args["enabled"] as? Bool {
                idleTracker?.setControllerMonitoring(enabled)
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'enabled' parameter", details: nil))
            }
            
        case "setAudioMonitoring":
            if let args = call.arguments as? [String: Any],
               let enabled = args["enabled"] as? Bool {
                idleTracker?.setAudioMonitoring(enabled)
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'enabled' parameter", details: nil))
            }
            
        case "setAudioThreshold":
            if let args = call.arguments as? [String: Any],
               let threshold = args["threshold"] as? Double {
                idleTracker?.setAudioThreshold(Float(threshold))
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'threshold' parameter", details: nil))
            }
            
        case "setHIDMonitoring":
            if let args = call.arguments as? [String: Any],
               let enabled = args["enabled"] as? Bool {
                idleTracker?.setHIDMonitoring(enabled)
                result(nil)
            } else {
                result(FlutterError(code: "INVALID_ARGUMENT", message: "Expected 'enabled' parameter", details: nil))
            }
            
        case "takeScreenshot":
            if let args = call.arguments as? [String: Any],
               let activeWindowOnly = args["activeWindowOnly"] as? Bool {
                takeScreenshot(activeWindowOnly: activeWindowOnly, result: result)
            } else {
                takeScreenshot(activeWindowOnly: false, result: result)
            }
            
        case "checkScreenRecordingPermission":
            result(checkScreenRecordingPermission())
            
        case "requestScreenRecordingPermission":
            requestScreenRecordingPermission()
            result(nil)
            
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private func checkScreenRecordingPermission() -> Bool {
        if #available(macOS 10.15, *) {
            return CGPreflightScreenCaptureAccess()
        }
        return true
    }

    private func requestScreenRecordingPermission() {
        if #available(macOS 10.15, *) {
            CGRequestScreenCaptureAccess()
        } else {
            let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenRecording")!
            NSWorkspace.shared.open(url)
        }
    }

    private func takeScreenshot(activeWindowOnly: Bool, result: @escaping FlutterResult) {
        let displayID = CGMainDisplayID()
        var image: CGImage?

        if activeWindowOnly {
            let activeApp = NSWorkspace.shared.frontmostApplication
            let activePID = activeApp?.processIdentifier
            let activeName = activeApp?.localizedName ?? "Unknown"
            
            print("[WindowFocus] Active App: \(activeName), PID: \(String(describing: activePID))")

            let options = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
            if let infoList = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] {
                for info in infoList {
                    if let windowOwnerPID = info[kCGWindowOwnerPID as String] as? pid_t, windowOwnerPID == activePID {
                        if let windowID = info[kCGWindowNumber as String] as? CGWindowID {
                            let windowLayer = info[kCGWindowLayer as String] as? Int ?? 0
                            let windowName = info[kCGWindowName as String] as? String ?? "No Name"
                            let windowBounds = info[kCGWindowBounds as String] as? [String: Any] ?? [:]
                            
                            print("[WindowFocus] Potential window: \(windowName), ID: \(windowID), Layer: \(windowLayer), Bounds: \(windowBounds)")
                            
                            if windowLayer == 0 {
                                image = CGWindowListCreateImage(.null, .optionIncludingWindow, windowID, .bestResolution)
                                if image != nil {
                                    print("[WindowFocus] Successfully captured active app window ID: \(windowID)")
                                    break
                                }
                            }
                        }
                    }
                }
            }
            
            if image == nil {
                print("[WindowFocus] Active app window not found, trying top-most window")
                if let infoList = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] {
                    for info in infoList {
                        let windowLayer = info[kCGWindowLayer as String] as? Int ?? 0
                        if windowLayer == 0 {
                            if let windowID = info[kCGWindowNumber as String] as? CGWindowID {
                                print("[WindowFocus] Capturing top-most window ID: \(windowID)")
                                image = CGWindowListCreateImage(.null, .optionIncludingWindow, windowID, .bestResolution)
                                if image != nil { break }
                            }
                        }
                    }
                }
            }
        }

        if image == nil {
            print("[WindowFocus] Capturing full screen")
            image = CGDisplayCreateImage(displayID)
        }

        guard let cgImage = image else {
            result(FlutterError(code: "SCREENSHOT_ERROR", message: "Failed to capture screenshot", details: nil))
            return
        }

        let bitmapRep = NSBitmapImageRep(cgImage: cgImage)
        guard let imageData = bitmapRep.representation(using: .png, properties: [:]) else {
            result(FlutterError(code: "CONVERSION_ERROR", message: "Failed to convert image to PNG", details: nil))
            return
        }

        result(FlutterStandardTypedData(bytes: imageData))
    }
}

class WindowFocusObserver {
    private var focusedAppPID: pid_t = -1
    internal var focusedWindowID: CGWindowID = 0
    private let sendMessage: (String, String) -> Void

    init(sendMessage: @escaping (String, String) -> Void) {
        self.sendMessage = sendMessage

        NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(focusedAppChanged(_:)), name: NSWorkspace.didActivateApplicationNotification, object: nil)
        
        Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.checkWindowTitleChanged()
        }
    }

    private var lastWindowTitle: String = ""
    private var lastAppName: String = ""

    @objc private func focusedAppChanged(_ notification: Notification) {
        if let userInfo = notification.userInfo,
           let application = userInfo[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication {
            let pid = application.processIdentifier
            let appName = application.localizedName ?? "Unknown"
            
            focusedAppPID = pid
            lastAppName = appName
            
            let windowTitle = getActiveWindowTitle(for: pid)
            lastWindowTitle = windowTitle
            
            sendMessage(appName, windowTitle)
            updateFocusedWindowID()
        }
    }

    private func checkWindowTitleChanged() {
        guard focusedAppPID != -1 else { return }
        
        let currentTitle = getActiveWindowTitle(for: focusedAppPID)
        if currentTitle != lastWindowTitle {
            lastWindowTitle = currentTitle
            sendMessage(lastAppName, currentTitle)
        }
    }

    private func getActiveWindowTitle(for pid: pid_t) -> String {
        let options = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
        if let infoList = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] {
            for info in infoList {
                if let windowOwnerPID = info[kCGWindowOwnerPID as String] as? pid_t, windowOwnerPID == pid {
                    let windowLayer = info[kCGWindowLayer as String] as? Int ?? 0
                    if windowLayer == 0 {
                        let windowName = info[kCGWindowName as String] as? String ?? ""
                        if !windowName.isEmpty {
                            return windowName
                        }
                    }
                }
            }
        }
        return lastAppName.isEmpty ? "Unknown" : lastAppName
    }

    private func updateFocusedWindowID() {
        let options = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
        if let infoList = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] {
            for info in infoList {
                if let windowOwnerPID = info[kCGWindowOwnerPID as String] as? pid_t, windowOwnerPID == focusedAppPID {
                    if let windowID = info[kCGWindowNumber as String] as? CGWindowID {
                        let windowLayer = info[kCGWindowLayer as String] as? Int ?? 0
                        if windowLayer == 0 {
                            focusedWindowID = windowID
                            break
                        }
                    }
                }
            }
        }
    }

    deinit {
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }
}

public class IdleTracker: NSObject {
    private var lastActivityTime: Date = Date()
    private var timer: Timer?
    public var idleThreshold: TimeInterval = 5
    private let channel: FlutterMethodChannel
    private var debugMode: Bool = false
    private var userIsActive: Bool = true

    // New monitoring flags
    private var monitorControllers: Bool = true
    private var monitorAudio: Bool = true
    private var monitorHIDDevices: Bool = true
    private var audioThreshold: Float = 0.001

    // Event tap for keyboard/mouse
    private var eventTap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?

    // HID device tracking
    private var hidManager: IOHIDManager?
    private var hidDevices: [IOHIDDevice] = []
    private var lastHIDStates: [IOHIDDevice: Data] = [:]

    // Controller tracking (using IOKit for game controllers)
    private var controllerDevices: [IOHIDDevice] = []
    private var lastControllerStates: [IOHIDDevice: Data] = [:]

    // Audio monitoring
    private var audioEngine: AVAudioEngine?
    private var inputNode: AVAudioInputNode?
    private var audioTapInstalled: Bool = false
    private var lastAudioLevel: Float = 0.0
    private var audioCheckTimer: Timer?

    init(channel: FlutterMethodChannel) {
        self.channel = channel
        super.init()
        startTracking()
        setupEventTap()
        initializeHIDDevices()
        initializeAudioMonitoring()
        startInputMonitoring()
    }

    private func setupEventTap() {
        let eventMask = (1 << CGEventType.keyDown.rawValue) | 
                         (1 << CGEventType.keyUp.rawValue) | 
                         (1 << CGEventType.flagsChanged.rawValue) | 
                         (1 << CGEventType.mouseMoved.rawValue) | 
                         (1 << CGEventType.leftMouseDown.rawValue) | 
                         (1 << CGEventType.rightMouseDown.rawValue)
        
        if debugMode {
            print("[WindowFocus] Creating event tap with mask: \(eventMask)")
        }

        guard let eventTap = CGEvent.tapCreate(
            tap: .cgAnnotatedSessionEventTap,
            place: .headInsertEventTap,
            options: .listenOnly,
            eventsOfInterest: CGEventMask(eventMask),
            callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                if let refcon = refcon {
                    let tracker = Unmanaged<IdleTracker>.fromOpaque(refcon).takeUnretainedValue()
                    if tracker.debugMode {
                        print("[WindowFocus] EventTap detected event type: \(type.rawValue)")
                    }
                    tracker.userDidInteract()
                }
                return Unmanaged.passUnretained(event)
            },
            userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        ) else {
            if debugMode {
                print("[WindowFocus] Failed to create event tap with .cgAnnotatedSessionEventTap. Trying .cghidEventTap...")
            }
            
            if let eventTapHid = CGEvent.tapCreate(
                tap: .cghidEventTap,
                place: .headInsertEventTap,
                options: .listenOnly,
                eventsOfInterest: CGEventMask(eventMask),
                callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                    if let refcon = refcon {
                        let tracker = Unmanaged<IdleTracker>.fromOpaque(refcon).takeUnretainedValue()
                        if tracker.debugMode {
                            print("[WindowFocus] EventTap (HID) detected event type: \(type.rawValue)")
                        }
                        tracker.userDidInteract()
                    }
                    return Unmanaged.passUnretained(event)
                },
                userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
            ) {
                self.eventTap = eventTapHid
                setupRunLoopSource(eventTapHid)
                if debugMode {
                    print("[WindowFocus] Event tap (HID) created successfully")
                }
            } else {
                print("[WindowFocus] Failed to create event tap. Check Accessibility permissions.")
            }
            return
        }

        self.eventTap = eventTap
        setupRunLoopSource(eventTap)
        if debugMode {
            print("[WindowFocus] Event tap created successfully")
        }
    }

    private func setupRunLoopSource(_ eventTap: CFMachPort) {
        runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource, .commonModes)
        CGEvent.tapEnable(tap: eventTap, enable: true)
    }

    private func initializeHIDDevices() {
        guard monitorHIDDevices else { return }
        
        hidManager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        guard let manager = hidManager else {
            if debugMode {
                print("[WindowFocus] Failed to create HID manager")
            }
            return
        }

        // Match all HID devices except audio
        let matchingDict: [[String: Any]] = [
            // Game controllers
            [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_GamePad],
            [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_Joystick],
            // Additional input devices
            [kIOHIDDeviceUsagePageKey: kHIDPage_Digitizer],
            [kIOHIDDeviceUsagePageKey: kHIDPage_GenericDesktop, kIOHIDDeviceUsageKey: kHIDUsage_GD_MultiAxisController]
        ]
        
        IOHIDManagerSetDeviceMatchingMultiple(manager, matchingDict as CFArray)
        
        IOHIDManagerRegisterDeviceMatchingCallback(manager, { context, result, sender, device in
            guard let context = context else { return }
            let tracker = Unmanaged<IdleTracker>.fromOpaque(context).takeUnretainedValue()
            tracker.handleHIDDeviceAdded(device)
        }, UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()))
        
        IOHIDManagerRegisterDeviceRemovalCallback(manager, { context, result, sender, device in
            guard let context = context else { return }
            let tracker = Unmanaged<IdleTracker>.fromOpaque(context).takeUnretainedValue()
            tracker.handleHIDDeviceRemoved(device)
        }, UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()))
        
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
        IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        
        if debugMode {
            print("[WindowFocus] HID manager initialized")
        }
    }

    private func handleHIDDeviceAdded(_ device: IOHIDDevice) {
        // Check if it's an audio device and skip
        if let usagePage = IOHIDDeviceGetProperty(device, kIOHIDDeviceUsagePageKey as CFString) as? Int {
            // Skip telephony (0x0B) and consumer (0x0C) devices which often include audio
            if usagePage == 0x0B || usagePage == 0x0C {
                if debugMode {
                    print("[WindowFocus] Skipping audio HID device with usage page: 0x\(String(usagePage, radix: 16))")
                }
                return
            }
        }
        
        hidDevices.append(device)
        lastHIDStates[device] = Data()
        
        // Register input value callback
        IOHIDDeviceRegisterInputValueCallback(device, { context, result, sender, value in
            guard let context = context else { return }
            let tracker = Unmanaged<IdleTracker>.fromOpaque(context).takeUnretainedValue()
            tracker.handleHIDInput(from: sender)
        }, UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()))
        
        if debugMode {
            let vendorID = IOHIDDeviceGetProperty(device, kIOHIDVendorIDKey as CFString) as? Int ?? 0
            let productID = IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? Int ?? 0
            print("[WindowFocus] HID device added: VID=0x\(String(vendorID, radix: 16)) PID=0x\(String(productID, radix: 16))")
        }
    }

    private func handleHIDDeviceRemoved(_ device: IOHIDDevice) {
        if let index = hidDevices.firstIndex(where: { $0 == device }) {
            hidDevices.remove(at: index)
            lastHIDStates.removeValue(forKey: device)
            
            if debugMode {
                print("[WindowFocus] HID device removed")
            }
        }
    }

    private func handleHIDInput(from device: IOHIDDevice) {
        if !monitorHIDDevices { return }
        
        if debugMode {
            print("[WindowFocus] HID input detected")
        }
        userDidInteract()
    }

    private func initializeAudioMonitoring() {
        guard monitorAudio else { return }
        
        // Start a timer to periodically check system audio
        audioCheckTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.performAudioCheck()
        }
        
        if debugMode {
            print("[WindowFocus] Audio monitoring initialized with timer")
        }
    }
    
    private func performAudioCheck() {
        if checkSystemAudio() {
            DispatchQueue.main.async { [weak self] in
                self?.userDidInteract()
            }
        }
    }

    private func checkSystemAudio() -> Bool {
        guard monitorAudio else { return false }
        
        // Get the default output device
        var defaultDeviceID = AudioDeviceID(0)
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        
        var deviceIDSize = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &deviceIDSize,
            &defaultDeviceID
        )
        
        guard status == noErr else {
            if debugMode {
                print("[WindowFocus] Failed to get default output device: \(status)")
            }
            return false
        }
        
        // Check if the device is valid
        if defaultDeviceID == kAudioDeviceUnknown {
            return false
        }
        
        // Get the device's current volume/peak level
        // We'll check if there's any audio activity by looking at the device's running state
        propertyAddress.mSelector = kAudioDevicePropertyDeviceIsRunningSomewhere
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal
        
        var isRunning = UInt32(0)
        var isRunningSize = UInt32(MemoryLayout<UInt32>.size)
        
        let runningStatus = AudioObjectGetPropertyData(
            defaultDeviceID,
            &propertyAddress,
            0,
            nil,
            &isRunningSize,
            &isRunning
        )
        
        if runningStatus == noErr && isRunning != 0 {
            // Device is actively processing audio
            // Now try to get the actual peak value if available
            propertyAddress.mSelector = kAudioDevicePropertyVolumeScalar
            propertyAddress.mScope = kAudioDevicePropertyScopeOutput
            
            var volume = Float32(0)
            var volumeSize = UInt32(MemoryLayout<Float32>.size)
            
            let volumeStatus = AudioObjectGetPropertyData(
                defaultDeviceID,
                &propertyAddress,
                0,
                nil,
                &volumeSize,
                &volume
            )
            
            // If we can get volume and it's above threshold, or if the device is just running
            // (which usually means audio is playing), consider it as activity
            if volumeStatus == noErr && volume > audioThreshold {
                if debugMode {
                    print("[WindowFocus] Audio detected, volume: \(volume)")
                }
                return true
            }
            
            // Even if we can't get the exact volume, if the device is running, 
            // it's likely playing audio
            if debugMode {
                print("[WindowFocus] Audio device is running")
            }
            return true
        }
        
        return false
    }

    private func startInputMonitoring() {
        // Background monitoring is now handled by:
        // 1. HID callbacks for device input
        // 2. Audio timer in initializeAudioMonitoring
        // 3. Event tap for keyboard/mouse
        
        if debugMode {
            print("[WindowFocus] Input monitoring started")
        }
    }

    private func startTracking() {
        if let savedThreshold = UserDefaults.standard.object(forKey: "idleThreshold") as? TimeInterval {
            idleThreshold = savedThreshold
        }

        if debugMode {
            print("[WindowFocus] Debug: Started tracking with idleThreshold = \(idleThreshold)")
        }

        NSEvent.addGlobalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown, .rightMouseDown, .keyDown, .flagsChanged]) { [weak self] event in
            self?.userDidInteract()
        }
        
        NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown, .rightMouseDown, .keyDown, .flagsChanged]) { [weak self] event in
            self?.userDidInteract()
            return event
        }

        timer = Timer.scheduledTimer(timeInterval: 1, target: self, selector: #selector(checkIdleTime), userInfo: nil, repeats: true)
    }

    @objc private func checkIdleTime() {
        let idleTimeCombined = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: .null)
        let idleTimeHID = CGEventSource.secondsSinceLastEventType(.hidSystemState, eventType: .null)
        let manualIdleTime = Date().timeIntervalSince(lastActivityTime)
        
        let safeCombined = (idleTimeCombined >= 0 && (idleTimeCombined < 3600 || manualIdleTime > 3600)) ? idleTimeCombined : manualIdleTime
        let safeHID = (idleTimeHID >= 0 && (idleTimeHID < 3600 || manualIdleTime > 3600)) ? idleTimeHID : manualIdleTime
        
        let idleTime = min(safeCombined, safeHID, manualIdleTime)

        if debugMode {
            print("[WindowFocus] Debug: Idle (C/H/M): \(String(format: "%.2f", safeCombined))/\(String(format: "%.2f", safeHID))/\(String(format: "%.2f", manualIdleTime)) -> Final: \(String(format: "%.2f", idleTime)), Threshold: \(idleThreshold), Active: \(userIsActive)")
        }

        if idleTime >= idleThreshold {
            if userIsActive {
                userIsActive = false
                if debugMode {
                    print("[WindowFocus] Debug: User became inactive. Idle time = \(idleTime)")
                }
                channel.invokeMethod("onUserInactivity", arguments: nil)
            }
        } else {
            if !userIsActive {
                userIsActive = true
                if debugMode {
                    print("[WindowFocus] Debug: User became active. Idle time reset to \(idleTime)")
                }
                channel.invokeMethod("onUserActive", arguments: nil)
            }
        }
    }

    private func userDidInteract() {
        lastActivityTime = Date()
    }

    func setIdleThreshold(_ threshold: TimeInterval) {
        self.idleThreshold = threshold
        UserDefaults.standard.set(threshold, forKey: "idleThreshold")
        if debugMode {
            print("[WindowFocus] Debug: Updated idleThreshold to \(threshold)")
        }
    }

    func setDebugMode(_ debug: Bool) {
        self.debugMode = debug
        if debugMode {
            print("[WindowFocus] Debug mode enabled. Current idleThreshold = \(idleThreshold)")
        }
    }

    func setControllerMonitoring(_ enabled: Bool) {
        monitorControllers = enabled
        if debugMode {
            print("[WindowFocus] Controller monitoring set to \(enabled)")
        }
    }

    func setAudioMonitoring(_ enabled: Bool) {
        let wasEnabled = monitorAudio
        monitorAudio = enabled
        
        if enabled && !wasEnabled {
            // Start audio monitoring
            initializeAudioMonitoring()
        } else if !enabled && wasEnabled {
            // Stop audio monitoring
            audioCheckTimer?.invalidate()
            audioCheckTimer = nil
        }
        
        if debugMode {
            print("[WindowFocus] Audio monitoring set to \(enabled)")
        }
    }

    func setAudioThreshold(_ threshold: Float) {
        audioThreshold = threshold
        if debugMode {
            print("[WindowFocus] Audio threshold set to \(threshold)")
        }
    }

    func setHIDMonitoring(_ enabled: Bool) {
        let wasEnabled = monitorHIDDevices
        monitorHIDDevices = enabled
        
        if enabled && !wasEnabled {
            initializeHIDDevices()
        } else if !enabled && wasEnabled {
            closeHIDDevices()
        }
        
        if debugMode {
            print("[WindowFocus] HID device monitoring set to \(enabled)")
        }
    }

    private func closeHIDDevices() {
        if let manager = hidManager {
            IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        }
        hidDevices.removeAll()
        lastHIDStates.removeAll()
        
        if debugMode {
            print("[WindowFocus] Closed all HID devices")
        }
    }

    deinit {
        if let eventTap = eventTap {
            CGEvent.tapEnable(tap: eventTap, enable: false)
        }
        if let runLoopSource = runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), runLoopSource, .commonModes)
        }
        closeHIDDevices()
        audioCheckTimer?.invalidate()
        timer?.invalidate()
    }
}