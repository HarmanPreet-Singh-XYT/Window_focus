/// Global keyboard and mouse hooks (WH_KEYBOARD_LL / WH_MOUSE_LL).

use std::sync::atomic::Ordering;
use windows::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
use windows::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, SetWindowsHookExW, UnhookWindowsHookEx, HHOOK, KBDLLHOOKSTRUCT,
    WH_KEYBOARD_LL, WH_MOUSE_LL, WM_KEYDOWN, WM_SYSKEYDOWN,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;

static KEYBOARD_HOOK: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);
static MOUSE_HOOK: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);

pub fn install() {
    remove();

    let hmod = unsafe { GetModuleHandleW(None).unwrap_or_default() };

    let kb = unsafe { SetWindowsHookExW(WH_KEYBOARD_LL, Some(keyboard_proc), hmod, 0) };
    match kb {
        Ok(h) => KEYBOARD_HOOK.store(h.0 as isize, Ordering::Release),
        Err(e) => eprintln!("[WindowFocus] Failed to install keyboard hook: {e}"),
    }

    let ms = unsafe { SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_proc), hmod, 0) };
    match ms {
        Ok(h) => MOUSE_HOOK.store(h.0 as isize, Ordering::Release),
        Err(e) => eprintln!("[WindowFocus] Failed to install mouse hook: {e}"),
    }
}

pub fn remove() {
    let kb_raw = KEYBOARD_HOOK.swap(0, Ordering::AcqRel);
    if kb_raw != 0 {
        unsafe { let _ = UnhookWindowsHookEx(HHOOK(kb_raw as *mut _)); }
    }

    let ms_raw = MOUSE_HOOK.swap(0, Ordering::AcqRel);
    if ms_raw != 0 {
        unsafe { let _ = UnhookWindowsHookEx(HHOOK(ms_raw as *mut _)); }
    }
}

pub fn keyboard_hook_raw() -> isize {
    KEYBOARD_HOOK.load(Ordering::Acquire)
}

unsafe extern "system" fn keyboard_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    let kb_raw = KEYBOARD_HOOK.load(Ordering::Acquire);

    if code >= 0 {
        if wparam.0 as u32 == WM_KEYDOWN || wparam.0 as u32 == WM_SYSKEYDOWN {
            if let Some(state) = crate::plugin::get() {
                if !state.is_shutting_down.load(Ordering::Acquire)
                    && state.config.monitor_keyboard.load(Ordering::Acquire)
                {
                    if state.config.enable_debug.load(Ordering::Relaxed) {
                        let kbd = &*(lparam.0 as *const KBDLLHOOKSTRUCT);
                        eprintln!("[WindowFocus] Keyboard hook: vkCode={}", kbd.vkCode);
                    }

                    state.activity.last_key_event_ms.store(
                        crate::config::now_millis(),
                        Ordering::Release,
                    );
                    state.activity.update_last_activity();

                    if !state.activity.user_is_active.swap(true, Ordering::AcqRel) {
                        state.channel.invoke_method_string_locked(
                            "onUserActive",
                            "User is active",
                        );
                    }
                }
            }
        }
    }

    CallNextHookEx(HHOOK(kb_raw as *mut _), code, wparam, lparam)
}

unsafe extern "system" fn mouse_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    let ms_raw = MOUSE_HOOK.load(Ordering::Acquire);

    if code >= 0 {
        if let Some(state) = crate::plugin::get() {
            if !state.is_shutting_down.load(Ordering::Acquire) {
                if state.config.enable_debug.load(Ordering::Relaxed) {
                    eprintln!("[WindowFocus] Mouse hook detected action");
                }
                state.activity.update_last_activity();
                if !state.activity.user_is_active.swap(true, Ordering::AcqRel) {
                    state.channel.invoke_method_string_locked(
                        "onUserActive",
                        "User is active",
                    );
                }
            }
        }
    }

    CallNextHookEx(HHOOK(ms_raw as *mut _), code, wparam, lparam)
}
