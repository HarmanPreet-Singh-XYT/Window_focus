/// PlatformTaskDispatcher — hidden HWND for cross-thread callbacks and power events.
///
/// Background threads call `Dispatcher::post(task)` to run a closure on the
/// Windows message-pump thread (the platform thread Flutter runs on).
/// The dispatcher also handles WM_POWERBROADCAST to detect system resume.

use once_cell::sync::OnceCell;
use windows::Win32::Foundation::{HANDLE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::Power::{
    PowerRegisterSuspendResumeNotification, PowerUnregisterSuspendResumeNotification, HPOWERNOTIFY,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, PostMessageW,
    RegisterClassExW, HWND_MESSAGE, WNDCLASSEXW, WM_APP, WM_POWERBROADCAST,
    CS_HREDRAW, CS_VREDRAW, WS_OVERLAPPED, PostQuitMessage, WM_DESTROY,
    DEVICE_NOTIFY_WINDOW_HANDLE,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::core::PCWSTR;

const WM_TASK: u32 = WM_APP + 1;
const PBT_APMRESUMEAUTOMATIC: u32 = 0x0012;
const PBT_APMRESUMESUSPEND: u32 = 0x0007;

struct DispatcherState {
    hwnd: HWND,
    power_notify: HPOWERNOTIFY,
}

// SAFETY: HWND and HPOWERNOTIFY are handles safe to share across threads for
// PostMessageW calls (thread-safe Win32 API).
unsafe impl Send for DispatcherState {}
unsafe impl Sync for DispatcherState {}

static DISPATCHER: OnceCell<DispatcherState> = OnceCell::new();

pub struct Dispatcher;

impl Dispatcher {
    /// Initialise the dispatcher: register window class, create message-only HWND,
    /// and register for suspend/resume power notifications.
    /// Must be called on the platform thread.
    pub fn init() {
        let _ = DISPATCHER.get_or_init(|| unsafe { create_dispatcher() });
    }

    /// Post a task closure to run on the platform thread.
    /// Safe to call from any thread.
    #[allow(dead_code)]
    pub fn post(task: Box<dyn FnOnce() + Send + 'static>) {
        if let Some(state) = DISPATCHER.get() {
            // Double-box: Box<dyn FnOnce> is unsized — wrap to get a thin pointer.
            let raw = Box::into_raw(Box::new(task));
            unsafe {
                let _ = PostMessageW(
                    state.hwnd,
                    WM_TASK,
                    WPARAM(0),
                    LPARAM(raw as isize),
                );
            }
        }
    }

    /// Destroy the dispatcher window and unregister power notification.
    /// Must be called on the platform thread.
    pub fn shutdown() {
        if let Some(state) = DISPATCHER.get() {
            unsafe {
                let _ = PowerUnregisterSuspendResumeNotification(state.power_notify);
                let _ = DestroyWindow(state.hwnd);
            }
        }
    }
}

unsafe fn create_dispatcher() -> DispatcherState {
    let hinstance = GetModuleHandleW(PCWSTR::null())
        .expect("GetModuleHandleW failed");

    // Register window class (ignore re-registration errors for hot-restart)
    let class_name: Vec<u16> = "WF_Dispatcher\0".encode_utf16().collect();

    let wc = WNDCLASSEXW {
        cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
        style: CS_HREDRAW | CS_VREDRAW,
        lpfnWndProc: Some(wnd_proc),
        hInstance: hinstance.into(),
        lpszClassName: PCWSTR(class_name.as_ptr()),
        ..Default::default()
    };

    let _ = RegisterClassExW(&wc);

    // Create a message-only window (parented to HWND_MESSAGE)
    let hwnd = CreateWindowExW(
        Default::default(),
        PCWSTR(class_name.as_ptr()),
        PCWSTR::null(),
        WS_OVERLAPPED,
        0, 0, 0, 0,
        HWND_MESSAGE,
        None,
        hinstance,
        None,
    ).expect("CreateWindowExW failed for dispatcher");

    // Register for power suspend/resume notifications via powrprof
    let mut notify_handle: *mut core::ffi::c_void = std::ptr::null_mut();
    let _ = PowerRegisterSuspendResumeNotification(
        DEVICE_NOTIFY_WINDOW_HANDLE,
        HANDLE(hwnd.0),
        &mut notify_handle,
    );
    let power_notify = HPOWERNOTIFY(notify_handle as isize);

    DispatcherState { hwnd, power_notify }
}

unsafe extern "system" fn wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_TASK {
        let task_ptr = lparam.0 as *mut Box<dyn FnOnce() + Send + 'static>;
        if !task_ptr.is_null() {
            let task = Box::from_raw(task_ptr);
            task();
        }
        return LRESULT(0);
    }

    if msg == WM_POWERBROADCAST {
        let event = wparam.0 as u32;
        if event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND {
            crate::plugin::on_system_resume();
        }
        return LRESULT(1);
    }

    if msg == WM_DESTROY {
        PostQuitMessage(0);
        return LRESULT(0);
    }

    DefWindowProcW(hwnd, msg, wparam, lparam)
}
