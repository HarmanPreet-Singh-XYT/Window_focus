/// Flutter Windows C API — loaded at runtime via GetProcAddress.
///
/// Since `flutter_windows.dll` is guaranteed to be in the process by the time
/// our DLL is loaded, we resolve all symbols via GetProcAddress and store them
/// in a process-global struct initialised in `WindowFocusPluginCApiRegisterWithRegistrar`.

use std::ffi::{c_char, c_void};
use once_cell::sync::OnceCell;
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
use windows::core::PCWSTR;

// ---- Opaque Flutter types ----

#[repr(C)]
pub struct FlutterDesktopPluginRegistrar {
    _private: [u8; 0],
}

#[repr(C)]
pub struct FlutterDesktopMessenger {
    _private: [u8; 0],
}

#[repr(C)]
pub struct FlutterPlatformMessageResponseHandle {
    _private: [u8; 0],
}

pub type FlutterDesktopPluginRegistrarRef = *mut FlutterDesktopPluginRegistrar;
pub type FlutterDesktopMessengerRef = *mut FlutterDesktopMessenger;
pub type FlutterDesktopMessageResponseHandleRef = *const FlutterPlatformMessageResponseHandle;

#[repr(C)]
pub struct FlutterDesktopMessage {
    pub struct_size: usize,
    pub channel: *const c_char,
    pub message: *const u8,
    pub message_size: usize,
    pub response_handle: FlutterDesktopMessageResponseHandleRef,
}

pub type FlutterDesktopMessageCallback = unsafe extern "C" fn(
    messenger: FlutterDesktopMessengerRef,
    message: *const FlutterDesktopMessage,
    user_data: *mut c_void,
);

// ---- Function pointer table ----

#[allow(dead_code)]
pub struct FlutterFunctions {
    pub registrar_get_messenger: unsafe extern "C" fn(
        FlutterDesktopPluginRegistrarRef,
    ) -> FlutterDesktopMessengerRef,

    pub registrar_set_destruction_handler: unsafe extern "C" fn(
        FlutterDesktopPluginRegistrarRef,
        unsafe extern "C" fn(FlutterDesktopPluginRegistrarRef),
    ),

    pub messenger_send: unsafe extern "C" fn(
        FlutterDesktopMessengerRef,
        *const c_char,
        *const u8,
        usize,
    ) -> bool,

    pub messenger_send_response: unsafe extern "C" fn(
        FlutterDesktopMessengerRef,
        FlutterDesktopMessageResponseHandleRef,
        *const u8,
        usize,
    ),

    pub messenger_set_callback: unsafe extern "C" fn(
        FlutterDesktopMessengerRef,
        *const c_char,
        Option<FlutterDesktopMessageCallback>,
        *mut c_void,
    ),

    pub messenger_add_ref: unsafe extern "C" fn(
        FlutterDesktopMessengerRef,
    ) -> FlutterDesktopMessengerRef,

    pub messenger_release: unsafe extern "C" fn(FlutterDesktopMessengerRef),

    pub messenger_is_available: unsafe extern "C" fn(FlutterDesktopMessengerRef) -> bool,

    pub messenger_lock: unsafe extern "C" fn(
        FlutterDesktopMessengerRef,
    ) -> FlutterDesktopMessengerRef,

    pub messenger_unlock: unsafe extern "C" fn(FlutterDesktopMessengerRef),
}

// SAFETY: All function pointers point into flutter_windows.dll which is loaded
// for the lifetime of the process.
unsafe impl Send for FlutterFunctions {}
unsafe impl Sync for FlutterFunctions {}

static FLUTTER_FNS: OnceCell<FlutterFunctions> = OnceCell::new();

/// Load all Flutter API functions from the already-loaded flutter_windows.dll.
/// Must be called before any other flutter_api function.
pub fn init() {
    let _ = FLUTTER_FNS.get_or_init(|| unsafe { load_flutter_functions() });
}

/// Get the loaded Flutter function table. Panics if `init()` was not called.
pub fn fns() -> &'static FlutterFunctions {
    FLUTTER_FNS.get().expect("flutter_api::init() not called")
}

unsafe fn load_flutter_functions() -> FlutterFunctions {
    // flutter_windows.dll is already loaded in the process by Flutter host.
    let dll_name: Vec<u16> = "flutter_windows.dll\0"
        .encode_utf16()
        .collect();
    let hmod = GetModuleHandleW(PCWSTR(dll_name.as_ptr()))
        .expect("flutter_windows.dll is not loaded — this should never happen");

    macro_rules! load_fn {
        ($name:literal, $ty:ty) => {{
            let sym = std::ffi::CStr::from_bytes_with_nul($name)
                .expect("bad symbol name")
                .as_ptr();
            let addr = GetProcAddress(hmod, windows::core::PCSTR(sym as *const u8))
                .unwrap_or_else(|| panic!("symbol {} not found in flutter_windows.dll", stringify!($name)));
            std::mem::transmute::<_, $ty>(addr)
        }};
    }

    FlutterFunctions {
        registrar_get_messenger: load_fn!(
            b"FlutterDesktopPluginRegistrarGetMessenger\0",
            unsafe extern "C" fn(FlutterDesktopPluginRegistrarRef) -> FlutterDesktopMessengerRef
        ),
        registrar_set_destruction_handler: load_fn!(
            b"FlutterDesktopPluginRegistrarSetDestructionHandler\0",
            unsafe extern "C" fn(
                FlutterDesktopPluginRegistrarRef,
                unsafe extern "C" fn(FlutterDesktopPluginRegistrarRef),
            )
        ),
        messenger_send: load_fn!(
            b"FlutterDesktopMessengerSend\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef, *const c_char, *const u8, usize) -> bool
        ),
        messenger_send_response: load_fn!(
            b"FlutterDesktopMessengerSendResponse\0",
            unsafe extern "C" fn(
                FlutterDesktopMessengerRef,
                FlutterDesktopMessageResponseHandleRef,
                *const u8,
                usize,
            )
        ),
        messenger_set_callback: load_fn!(
            b"FlutterDesktopMessengerSetCallback\0",
            unsafe extern "C" fn(
                FlutterDesktopMessengerRef,
                *const c_char,
                Option<FlutterDesktopMessageCallback>,
                *mut c_void,
            )
        ),
        messenger_add_ref: load_fn!(
            b"FlutterDesktopMessengerAddRef\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef) -> FlutterDesktopMessengerRef
        ),
        messenger_release: load_fn!(
            b"FlutterDesktopMessengerRelease\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef)
        ),
        messenger_is_available: load_fn!(
            b"FlutterDesktopMessengerIsAvailable\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef) -> bool
        ),
        messenger_lock: load_fn!(
            b"FlutterDesktopMessengerLock\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef) -> FlutterDesktopMessengerRef
        ),
        messenger_unlock: load_fn!(
            b"FlutterDesktopMessengerUnlock\0",
            unsafe extern "C" fn(FlutterDesktopMessengerRef)
        ),
    }
}

// ---- Safe wrappers ----

pub unsafe fn registrar_get_messenger(
    registrar: FlutterDesktopPluginRegistrarRef,
) -> FlutterDesktopMessengerRef {
    (fns().registrar_get_messenger)(registrar)
}

pub unsafe fn registrar_set_destruction_handler(
    registrar: FlutterDesktopPluginRegistrarRef,
    callback: unsafe extern "C" fn(FlutterDesktopPluginRegistrarRef),
) {
    (fns().registrar_set_destruction_handler)(registrar, callback)
}

pub unsafe fn messenger_send(
    messenger: FlutterDesktopMessengerRef,
    channel: *const c_char,
    message: *const u8,
    message_size: usize,
) -> bool {
    (fns().messenger_send)(messenger, channel, message, message_size)
}

pub unsafe fn messenger_set_callback(
    messenger: FlutterDesktopMessengerRef,
    channel: *const c_char,
    callback: Option<FlutterDesktopMessageCallback>,
    user_data: *mut c_void,
) {
    (fns().messenger_set_callback)(messenger, channel, callback, user_data)
}

pub unsafe fn messenger_add_ref(
    messenger: FlutterDesktopMessengerRef,
) -> FlutterDesktopMessengerRef {
    (fns().messenger_add_ref)(messenger)
}

pub unsafe fn messenger_release(messenger: FlutterDesktopMessengerRef) {
    (fns().messenger_release)(messenger)
}

pub unsafe fn messenger_lock(
    messenger: FlutterDesktopMessengerRef,
) -> FlutterDesktopMessengerRef {
    (fns().messenger_lock)(messenger)
}

pub unsafe fn messenger_unlock(messenger: FlutterDesktopMessengerRef) {
    (fns().messenger_unlock)(messenger)
}
