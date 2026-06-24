/// Keyboard input detection.
/// Primary: hook sets `last_key_event_ms`; we check recency here.
/// Fallback: poll GetAsyncKeyState when hooks are not installed.

use std::sync::atomic::Ordering;
use windows::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;

use crate::activity::ActivityTracker;
use crate::config::{now_millis, Config};

pub fn check_keyboard_input(_config: &Config, activity: &ActivityTracker) -> bool {
    let hook_installed = crate::hooks::keyboard_hook_raw() != 0;

    if hook_installed {
        // Hook path: return true if a key event fired within the last 200ms
        let now = now_millis();
        let last = activity.last_key_event_ms.load(Ordering::Acquire);
        last > 0 && (now.saturating_sub(last)) < 200
    } else {
        // Fallback: poll a broad set of virtual key codes
        poll_keyboard_state()
    }
}

fn poll_keyboard_state() -> bool {
    // A-Z
    for vk in 0x41u16..=0x5Au16 {
        if key_down(vk) { return true; }
    }
    // 0-9
    for vk in 0x30u16..=0x39u16 {
        if key_down(vk) { return true; }
    }
    // F1-F12
    for vk in 0x70u16..=0x7Bu16 {
        if key_down(vk) { return true; }
    }
    // Common special keys (same set as C++ fallback)
    const SPECIAL: &[u16] = &[
        0x20, // VK_SPACE
        0x0D, // VK_RETURN
        0x09, // VK_TAB
        0x1B, // VK_ESCAPE
        0x08, // VK_BACK
        0x2E, // VK_DELETE
        0x10, // VK_SHIFT
        0x11, // VK_CONTROL
        0x12, // VK_MENU
        0xA0, // VK_LSHIFT
        0xA1, // VK_RSHIFT
        0xA2, // VK_LCONTROL
        0xA3, // VK_RCONTROL
        0xA4, // VK_LMENU
        0xA5, // VK_RMENU
        0x25, // VK_LEFT
        0x27, // VK_RIGHT
        0x26, // VK_UP
        0x28, // VK_DOWN
        0x24, // VK_HOME
        0x23, // VK_END
        0x21, // VK_PRIOR
        0x22, // VK_NEXT
        0x2D, // VK_INSERT
        0x5B, // VK_LWIN
        0x5C, // VK_RWIN
    ];
    for &vk in SPECIAL {
        if key_down(vk) { return true; }
    }
    false
}

fn key_down(vk: u16) -> bool {
    let state = unsafe { GetAsyncKeyState(vk as i32) };
    (state & -0x8000i16) != 0
}
