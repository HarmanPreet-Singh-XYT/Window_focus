/// Mouse movement detection via GetCursorPos polling.
/// The low-level mouse hook (hooks.rs) handles real-time detection;
/// this polls as an additional check from the monitoring loop.

use parking_lot::Mutex;
use windows::Win32::Foundation::POINT;
use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

static LAST_POS: Mutex<POINT> = Mutex::new(POINT { x: 0, y: 0 });

pub fn check_mouse_moved() -> bool {
    let mut current = POINT { x: 0, y: 0 };
    if unsafe { GetCursorPos(&mut current) }.is_ok() {
        let mut last = LAST_POS.lock();
        if current.x != last.x || current.y != last.y {
            *last = current;
            return true;
        }
    }
    false
}
