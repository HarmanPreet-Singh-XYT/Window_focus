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
            
        case "setKeyboardMonitoring":
            if let args = call.arguments as? [String: Any],
            let enabled = args["enabled"] as? Bool {
                idleTracker?.setKeyboardMonitoring(enabled)
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
            
        case "checkInputMonitoringPermission":
            result(checkInputMonitoringPermission())
            
        case "requestInputMonitoringPermission":
            requestInputMonitoringPermission()
            result(nil)

        case "openInputMonitoringSettings":
            openInputMonitoringSettings()
            result(nil)
                
        case "checkAllPermissions":
            let permissions: [String: Bool] = [
                "screenRecording": checkScreenRecordingPermission(),
                "inputMonitoring": checkInputMonitoringPermission()
            ]
            result(permissions)
            
        default:
            result(FlutterMethodNotImplemented)
        }
    }
    
    private func checkInputMonitoringPermission() -> Bool {
        if #available(macOS 10.15, *) {
            let status = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)
            switch status {
            case kIOHIDAccessTypeGranted:
                return true
            case kIOHIDAccessTypeDenied, kIOHIDAccessTypeUnknown:
                return false
            default:
                return false
            }
        }
        return true
    }

    private func requestInputMonitoringPermission() {
        if #available(macOS 10.15, *) {
            let status = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)
            
            if status == kIOHIDAccessTypeUnknown {
                IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)
            } else if status == kIOHIDAccessTypeDenied {
                openInputMonitoringSettings()
            }
        }
    }

    private func openInputMonitoringSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent") {
            NSWorkspace.shared.open(url)
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

    // Monitoring flags
    private var monitorControllers: Bool = true
    private var monitorAudio: Bool = true
    private var monitorHIDDevices: Bool = true
    private var monitorKeyboard: Bool = true
    private var audioThreshold: Float = 0.001

    // Event tap for keyboard/mouse
    private var eventTap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?

    // Separate keyboard event tap (for when keyboard monitoring is toggled independently)
    private var keyboardEventTap: CFMachPort?
    private var keyboardRunLoopSource: CFRunLoopSource?

    // Global/local event monitors
    private var globalEventMonitor: Any?
    private var localEventMonitor: Any?
    private var keyboardGlobalMonitor: Any?
    private var keyboardLocalMonitor: Any?

    // HID device tracking
    private var hidManager: IOHIDManager?
    private var hidDevices: [IOHIDDevice] = []
    private var lastHIDStates: [IOHIDDevice: Data] = [:]

    // Audio monitoring
    private var audioEngine: AVAudioEngine?
    private var inputNode: AVAudioInputNode?
    private var audioTapInstalled: Bool = false
    private var lastAudioLevel: Float = 0.0
    private var audioCheckTimer: Timer?

    // Keyboard activity tracking
    private var lastKeyboardActivityTime: Date = Date.distantPast
    private var keyboardPollTimer: Timer?

    init(channel: FlutterMethodChannel) {
        self.channel = channel
        super.init()
        startTracking()
        setupEventTap()
        setupKeyboardMonitoring()
        initializeHIDDevices()
        initializeAudioMonitoring()
        startInputMonitoring()
    }

    // MARK: - Event Tap Setup

    private func setupEventTap() {
        // This event tap handles mouse events primarily
        // Keyboard events are handled separately for independent toggling
        let eventMask = (1 << CGEventType.mouseMoved.rawValue) |
                         (1 << CGEventType.leftMouseDown.rawValue) |
                         (1 << CGEventType.rightMouseDown.rawValue) |
                         (1 << CGEventType.leftMouseUp.rawValue) |
                         (1 << CGEventType.rightMouseUp.rawValue) |
                         (1 << CGEventType.scrollWheel.rawValue) |
                         (1 << CGEventType.leftMouseDragged.rawValue) |
                         (1 << CGEventType.rightMouseDragged.rawValue)
        
        if debugMode {
            print("[WindowFocus] Creating mouse event tap with mask: \(eventMask)")
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
                        print("[WindowFocus] Mouse EventTap detected event type: \(type.rawValue)")
                    }
                    tracker.userDidInteract()
                }
                return Unmanaged.passUnretained(event)
            },
            userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        ) else {
            if debugMode {
                print("[WindowFocus] Failed to create mouse event tap with .cgAnnotatedSessionEventTap. Trying .cghidEventTap...")
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
                            print("[WindowFocus] Mouse EventTap (HID) detected event type: \(type.rawValue)")
                        }
                        tracker.userDidInteract()
                    }
                    return Unmanaged.passUnretained(event)
                },
                userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
            ) {
                self.eventTap = eventTapHid
                setupRunLoopSource(eventTapHid, source: &runLoopSource)
                if debugMode {
                    print("[WindowFocus] Mouse event tap (HID) created successfully")
                }
            } else {
                print("[WindowFocus] Failed to create mouse event tap. Check Accessibility permissions.")
            }
            return
        }

        self.eventTap = eventTap
        setupRunLoopSource(eventTap, source: &runLoopSource)
        if debugMode {
            print("[WindowFocus] Mouse event tap created successfully")
        }
    }

    // MARK: - Keyboard Monitoring Setup

    private func setupKeyboardMonitoring() {
        guard monitorKeyboard else {
            if debugMode {
                print("[WindowFocus] Keyboard monitoring is disabled, skipping setup")
            }
            return
        }

        // Method 1: CGEvent tap for keyboard events (system-wide, works across all apps)
        setupKeyboardEventTap()

        // Method 2: NSEvent monitors as fallback
        setupKeyboardNSEventMonitors()

        // Method 3: Polling fallback for edge cases
        startKeyboardPolling()

        if debugMode {
            print("[WindowFocus] Keyboard monitoring setup complete (event tap + NSEvent monitors + polling)")
        }
    }

    private func setupKeyboardEventTap() {
        let keyboardEventMask = (1 << CGEventType.keyDown.rawValue) |
                                 (1 << CGEventType.keyUp.rawValue) |
                                 (1 << CGEventType.flagsChanged.rawValue)

        if debugMode {
            print("[WindowFocus] Creating keyboard event tap with mask: \(keyboardEventMask)")
        }

        guard let kbEventTap = CGEvent.tapCreate(
            tap: .cgAnnotatedSessionEventTap,
            place: .headInsertEventTap,
            options: .listenOnly,
            eventsOfInterest: CGEventMask(keyboardEventMask),
            callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                if let refcon = refcon {
                    let tracker = Unmanaged<IdleTracker>.fromOpaque(refcon).takeUnretainedValue()
                    
                    if tracker.monitorKeyboard {
                        if tracker.debugMode {
                            let keyCode = event.getIntegerValueField(.keyboardEventKeycode)
                            switch type {
                            case .keyDown:
                                print("[WindowFocus] Keyboard: key down, keyCode=\(keyCode)")
                            case .keyUp:
                                print("[WindowFocus] Keyboard: key up, keyCode=\(keyCode)")
                            case .flagsChanged:
                                let flags = event.flags
                                print("[WindowFocus] Keyboard: flags changed, flags=\(flags.rawValue)")
                            default:
                                print("[WindowFocus] Keyboard: event type \(type.rawValue)")
                            }
                        }
                        
                        tracker.lastKeyboardActivityTime = Date()
                        tracker.userDidInteract()
                    }
                }
                return Unmanaged.passUnretained(event)
            },
            userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        ) else {
            if debugMode {
                print("[WindowFocus] Failed to create keyboard event tap with .cgAnnotatedSessionEventTap. Trying .cghidEventTap...")
            }

            // Fallback to HID event tap
            if let kbEventTapHid = CGEvent.tapCreate(
                tap: .cghidEventTap,
                place: .headInsertEventTap,
                options: .listenOnly,
                eventsOfInterest: CGEventMask(keyboardEventMask),
                callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                    if let refcon = refcon {
                        let tracker = Unmanaged<IdleTracker>.fromOpaque(refcon).takeUnretainedValue()
                        if tracker.monitorKeyboard {
                            if tracker.debugMode {
                                print("[WindowFocus] Keyboard (HID tap): event type \(type.rawValue)")
                            }
                            tracker.lastKeyboardActivityTime = Date()
                            tracker.userDidInteract()
                        }
                    }
                    return Unmanaged.passUnretained(event)
                },
                userInfo: UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
            ) {
                self.keyboardEventTap = kbEventTapHid
                setupRunLoopSource(kbEventTapHid, source: &keyboardRunLoopSource)
                if debugMode {
                    print("[WindowFocus] Keyboard event tap (HID) created successfully")
                }
            } else {
                print("[WindowFocus] Failed to create keyboard event tap. Check Input Monitoring permissions.")
                print("[WindowFocus] Will rely on NSEvent monitors and polling for keyboard detection.")
            }
            return
        }

        self.keyboardEventTap = kbEventTap
        setupRunLoopSource(kbEventTap, source: &keyboardRunLoopSource)
        if debugMode {
            print("[WindowFocus] Keyboard event tap created successfully")
        }
    }

    private func setupKeyboardNSEventMonitors() {
        // Global monitor - catches keyboard events when OTHER apps are in focus
        keyboardGlobalMonitor = NSEvent.addGlobalMonitorForEvents(
            matching: [.keyDown, .keyUp, .flagsChanged]
        ) { [weak self] event in
            guard let self = self, self.monitorKeyboard else { return }
            
            if self.debugMode {
                print("[WindowFocus] Keyboard (global NSEvent): keyCode=\(event.keyCode), type=\(event.type.rawValue)")
            }
            
            self.lastKeyboardActivityTime = Date()
            self.userDidInteract()
        }

        // Local monitor - catches keyboard events when OUR app is in focus
        keyboardLocalMonitor = NSEvent.addLocalMonitorForEvents(
            matching: [.keyDown, .keyUp, .flagsChanged]
        ) { [weak self] event in
            guard let self = self, self.monitorKeyboard else { return event }
            
            if self.debugMode {
                print("[WindowFocus] Keyboard (local NSEvent): keyCode=\(event.keyCode), type=\(event.type.rawValue)")
            }
            
            self.lastKeyboardActivityTime = Date()
            self.userDidInteract()
            return event
        }

        if debugMode {
            print("[WindowFocus] Keyboard NSEvent monitors installed")
        }
    }

    private func startKeyboardPolling() {
        // Polling fallback: check CGEventSource for keyboard idle time
        // This catches cases where event taps might not fire
        keyboardPollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self, self.monitorKeyboard else { return }
            self.pollKeyboardState()
        }

        if debugMode {
            print("[WindowFocus] Keyboard polling started (0.5s interval)")
        }
    }

    private func pollKeyboardState() {
        // Check CGEventSource for recent keyboard activity
        let keyboardIdleTime = CGEventSource.secondsSinceLastEventType(
            .hidSystemState,
            eventType: .keyDown
        )

        // If keyboard was used in the last 1 second, consider it activity
        if keyboardIdleTime >= 0 && keyboardIdleTime < 1.0 {
            let timeSinceLastKeyboard = Date().timeIntervalSince(lastKeyboardActivityTime)
            
            // Only trigger if we haven't already detected this via event tap/NSEvent
            if timeSinceLastKeyboard > 1.0 {
                if debugMode {
                    print("[WindowFocus] Keyboard activity detected via polling (idle: \(String(format: "%.2f", keyboardIdleTime))s)")
                }
                lastKeyboardActivityTime = Date()
                userDidInteract()
            }
        }

        // Also check modifier keys (Shift, Ctrl, Alt, Cmd) via flagsChanged
        let flagsIdleTime = CGEventSource.secondsSinceLastEventType(
            .hidSystemState,
            eventType: .flagsChanged
        )

        if flagsIdleTime >= 0 && flagsIdleTime < 1.0 {
            let timeSinceLastKeyboard = Date().timeIntervalSince(lastKeyboardActivityTime)
            if timeSinceLastKeyboard > 1.0 {
                if debugMode {
                    print("[WindowFocus] Modifier key activity detected via polling (idle: \(String(format: "%.2f", flagsIdleTime))s)")
                }
                lastKeyboardActivityTime = Date()
                userDidInteract()
            }
        }
    }

    private func removeKeyboardMonitoring() {
        // Remove keyboard event tap
        if let kbTap = keyboardEventTap {
            CGEvent.tapEnable(tap: kbTap, enable: false)
            keyboardEventTap = nil
        }
        if let kbSource = keyboardRunLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), kbSource, .commonModes)
            keyboardRunLoopSource = nil
        }

        // Remove NSEvent monitors
        if let globalMonitor = keyboardGlobalMonitor {
            NSEvent.removeMonitor(globalMonitor)
            keyboardGlobalMonitor = nil
        }
        if let localMonitor = keyboardLocalMonitor {
            NSEvent.removeMonitor(localMonitor)
            keyboardLocalMonitor = nil
        }

        // Stop polling timer
        keyboardPollTimer?.invalidate()
        keyboardPollTimer = nil

        if debugMode {
            print("[WindowFocus] Keyboard monitoring removed")
        }
    }

    // MARK: - Run Loop Source Helper

    private func setupRunLoopSource(_ eventTap: CFMachPort, source: inout CFRunLoopSource?) {
        source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0)
        if let src = source {
            CFRunLoopAddSource(CFRunLoopGetCurrent(), src, .commonModes)
        }
        CGEvent.tapEnable(tap: eventTap, enable: true)
    }

    // MARK: - HID Devices

    private func initializeHIDDevices() {
        guard monitorHIDDevices || monitorControllers else { return }
        
        hidManager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        guard let manager = hidManager else {
            if debugMode {
                print("[WindowFocus] Failed to create HID manager")
            }
            return
        }

        IOHIDManagerSetDeviceMatching(manager, nil)
        
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
        let openResult = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        
        if debugMode {
            if openResult == kIOReturnSuccess {
                print("[WindowFocus] HID manager initialized successfully")
            } else {
                print("[WindowFocus] HID manager open failed with error: \(openResult)")
            }
        }
    }

    private func handleHIDDeviceAdded(_ device: IOHIDDevice) {
        let usagePage = IOHIDDeviceGetProperty(device, kIOHIDDeviceUsagePageKey as CFString) as? Int ?? 0
        let usage = IOHIDDeviceGetProperty(device, kIOHIDDeviceUsageKey as CFString) as? Int ?? 0
        let vendorID = IOHIDDeviceGetProperty(device, kIOHIDVendorIDKey as CFString) as? Int ?? 0
        let productID = IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? Int ?? 0
        let productName = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "Unknown"
        
        if debugMode {
            print("[WindowFocus] HID device found: \(productName)")
            print("  VID: 0x\(String(vendorID, radix: 16)), PID: 0x\(String(productID, radix: 16))")
            print("  Usage Page: 0x\(String(usagePage, radix: 16)), Usage: 0x\(String(usage, radix: 16))")
        }
        
        var shouldMonitor = false
        
        let isAudioDevice = (usagePage == 0x0B || usagePage == 0x0C)
        let isKeyboard = (usagePage == 0x01 && usage == 0x06)
        let isMouse = (usagePage == 0x01 && usage == 0x02)
        let isGameController = (usagePage == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08))
        let isDigitizer = (usagePage == 0x0D)
        let isMultiAxisController = (usagePage == 0x01 && usage == 0x08)
        let isOtherDesktopDevice = (usagePage == 0x01 && !isKeyboard && !isMouse)
        
        if monitorControllers && isGameController {
            shouldMonitor = true
            if debugMode {
                print("[WindowFocus] → Game controller detected, will monitor")
            }
        } else if monitorHIDDevices {
            // Skip keyboards and mice (monitored via event taps and NSEvent)
            // Skip audio devices
            if !isAudioDevice && !isKeyboard && !isMouse {
                shouldMonitor = true
                if debugMode {
                    if isDigitizer {
                        print("[WindowFocus] → Drawing tablet/digitizer detected, will monitor")
                    } else if isMultiAxisController {
                        print("[WindowFocus] → Multi-axis controller detected, will monitor")
                    } else if isOtherDesktopDevice {
                        print("[WindowFocus] → Generic input device detected, will monitor")
                    } else {
                        print("[WindowFocus] → HID input device detected, will monitor")
                    }
                }
            } else {
                if debugMode {
                    if isAudioDevice {
                        print("[WindowFocus] → Audio device, skipping")
                    } else if isKeyboard {
                        print("[WindowFocus] → Keyboard (monitored via event tap), skipping HID")
                    } else if isMouse {
                        print("[WindowFocus] → Mouse (monitored via event tap), skipping HID")
                    }
                }
            }
        } else {
            if debugMode {
                print("[WindowFocus] → Monitoring disabled for this device type")
            }
        }
        
        if !shouldMonitor {
            return
        }
        
        hidDevices.append(device)
        lastHIDStates[device] = Data()
        
        IOHIDDeviceRegisterInputValueCallback(device, { context, result, sender, value in
            guard let context = context, let sender = sender else { return }
            let tracker = Unmanaged<IdleTracker>.fromOpaque(context).takeUnretainedValue()
            tracker.handleHIDInput(from: sender)
        }, UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()))
        
        if debugMode {
            print("[WindowFocus] ✓ Successfully registered HID device (\(hidDevices.count) total)")
        }
    }

    private func handleHIDDeviceRemoved(_ device: IOHIDDevice) {
        if let index = hidDevices.firstIndex(where: { \$0 == device }) {
            hidDevices.remove(at: index)
            lastHIDStates.removeValue(forKey: device)
            
            if debugMode {
                print("[WindowFocus] HID device removed")
            }
        }
    }

    private func handleHIDInput(from devicePointer: UnsafeMutableRawPointer) {
        if !monitorHIDDevices && !monitorControllers { return }
        
        let device = Unmanaged<IOHIDDevice>.fromOpaque(devicePointer).takeUnretainedValue()
        
        if debugMode {
            let productName = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "Unknown"
            print("[WindowFocus] HID input detected from: \(productName)")
        }
        
        userDidInteract()
    }

    // MARK: - Audio Monitoring

    private func initializeAudioMonitoring() {
        guard monitorAudio else { return }
        
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
        
        if defaultDeviceID == kAudioDeviceUnknown {
            return false
        }
        
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
            
            if volumeStatus == noErr && volume > audioThreshold {
                if debugMode {
                    print("[WindowFocus] Audio detected, volume: \(volume)")
                }
                return true
            }
            
            if debugMode {
                print("[WindowFocus] Audio device is running")
            }
            return true
        }
        
        return false
    }

    // MARK: - Input Monitoring Start

    private func startInputMonitoring() {
        if debugMode {
            print("[WindowFocus] Input monitoring started")
            print("[WindowFocus]   Mouse: event tap")
            print("[WindowFocus]   Keyboard: \(monitorKeyboard ? "event tap + NSEvent + polling" : "disabled")")
            print("[WindowFocus]   Controllers: \(monitorControllers ? "HID" : "disabled")")
            print("[WindowFocus]   HID devices: \(monitorHIDDevices ? "enabled" : "disabled")")
            print("[WindowFocus]   Audio: \(monitorAudio ? "enabled" : "disabled")")
        }
    }

    // MARK: - Tracking

    private func startTracking() {
        if let savedThreshold = UserDefaults.standard.object(forKey: "idleThreshold") as? TimeInterval {
            idleThreshold = savedThreshold
        }

        if debugMode {
            print("[WindowFocus] Debug: Started tracking with idleThreshold = \(idleThreshold)")
        }

        // Mouse event monitors (always active)
        globalEventMonitor = NSEvent.addGlobalMonitorForEvents(
            matching: [.mouseMoved, .leftMouseDown, .rightMouseDown, .scrollWheel, .leftMouseDragged, .rightMouseDragged]
        ) { [weak self] event in
            self?.userDidInteract()
        }
        
        localEventMonitor = NSEvent.addLocalMonitorForEvents(
            matching: [.mouseMoved, .leftMouseDown, .rightMouseDown, .scrollWheel, .leftMouseDragged, .rightMouseDragged]
        ) { [weak self] event in
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
            let keyboardIdleStr: String
            if monitorKeyboard {
                let kbIdle = Date().timeIntervalSince(lastKeyboardActivityTime)
                keyboardIdleStr = String(format: "%.2f", kbIdle)
            } else {
                keyboardIdleStr = "disabled"
            }
            
            print("[WindowFocus] Debug: Idle (C/H/M/KB): \(String(format: "%.2f", safeCombined))/\(String(format: "%.2f", safeHID))/\(String(format: "%.2f", manualIdleTime))/\(keyboardIdleStr) -> Final: \(String(format: "%.2f", idleTime)), Threshold: \(idleThreshold), Active: \(userIsActive)")
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

    // MARK: - Configuration Methods

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
            print("[WindowFocus]   Keyboard monitoring: \(monitorKeyboard)")
            print("[WindowFocus]   Controller monitoring: \(monitorControllers)")
            print("[WindowFocus]   HID monitoring: \(monitorHIDDevices)")
            print("[WindowFocus]   Audio monitoring: \(monitorAudio)")
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
            initializeAudioMonitoring()
        } else if !enabled && wasEnabled {
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
            if enabled {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
                    self?.printConnectedDevices()
                }
            }
        }
    }

    /// Enables or disables keyboard monitoring.
    ///
    /// When enabled, keyboard input is detected via three methods:
    /// 1. **CGEvent tap** - system-wide, catches all keyboard events across all apps
    /// 2. **NSEvent monitors** - global and local monitors as fallback
    /// 3. **CGEventSource polling** - checks keyboard idle time as final fallback
    ///
    /// The keyboard monitoring does NOT log or record which keys are pressed —
    /// it only detects that keyboard activity occurred for resetting the inactivity timer.
    func setKeyboardMonitoring(_ enabled: Bool) {
        let wasEnabled = monitorKeyboard
        monitorKeyboard = enabled

        if enabled && !wasEnabled {
            // Enable keyboard monitoring
            setupKeyboardMonitoring()
            if debugMode {
                print("[WindowFocus] Keyboard monitoring enabled")
            }
        } else if !enabled && wasEnabled {
            // Disable keyboard monitoring
            removeKeyboardMonitoring()
            if debugMode {
                print("[WindowFocus] Keyboard monitoring disabled")
            }
        }
    }
    
    private func printConnectedDevices() {
        print("[WindowFocus] === Currently Monitored Devices ===")
        print("[WindowFocus] Total devices: \(hidDevices.count)")
        for (index, device) in hidDevices.enumerated() {
            let productName = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "Unknown"
            let vendorID = IOHIDDeviceGetProperty(device, kIOHIDVendorIDKey as CFString) as? Int ?? 0
            let productID = IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? Int ?? 0
            print("[WindowFocus] [\(index)] \(productName) (VID: 0x\(String(vendorID, radix: 16)), PID: 0x\(String(productID, radix: 16)))")
        }
        print("[WindowFocus] ==================================")
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

    // MARK: - Cleanup

    deinit {
        // Remove mouse event tap
        if let eventTap = eventTap {
            CGEvent.tapEnable(tap: eventTap, enable: false)
        }
        if let runLoopSource = runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), runLoopSource, .commonModes)
        }

        // Remove keyboard monitoring
        removeKeyboardMonitoring()

        // Remove NSEvent monitors for mouse
        if let globalMonitor = globalEventMonitor {
            NSEvent.removeMonitor(globalMonitor)
        }
        if let localMonitor = localEventMonitor {
            NSEvent.removeMonitor(localMonitor)
        }

        // Close HID devices
        closeHIDDevices()

        // Stop timers
        audioCheckTimer?.invalidate()
        timer?.invalidate()
    }
}