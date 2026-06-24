use windows::Win32::UI::Input::XboxController::{XInputGetState, XINPUT_STATE};
use crate::config::Config;
use std::sync::atomic::Ordering;

pub fn check_controllers(last_states: &mut [XINPUT_STATE; 4], config: &Config) -> bool {
    let mut detected = false;

    for i in 0u32..4 {
        let mut state = XINPUT_STATE::default();
        let result = unsafe { XInputGetState(i, &mut state) };
        if result != 0 {
            continue; // ERROR_DEVICE_NOT_CONNECTED or other error
        }

        if state.dwPacketNumber != last_states[i as usize].dwPacketNumber {
            if config.enable_debug.load(Ordering::Relaxed) {
                eprintln!("[WindowFocus] Controller {} input detected", i);
            }
            last_states[i as usize] = state;
            detected = true;
        }
    }

    detected
}
