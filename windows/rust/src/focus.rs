/// Window focus monitoring thread.
/// Polls GetForegroundWindow every 500ms (interruptible by shutdown).
/// Debounces rapid focus changes at 250ms.
/// Sends "onFocusChange" with {title, appName, windowTitle} on change.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{CloseHandle, HWND};
use windows::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
    TH32CS_SNAPPROCESS,
};
use windows::Win32::UI::WindowsAndMessaging::{
    GetForegroundWindow, GetWindowTextLengthW, GetWindowTextW, GetWindowThreadProcessId,
};

use crate::activity::ActivityTracker;
use crate::channel::MethodChannel;
use crate::config::Config;

pub fn start(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<AtomicBool>,
) {
    std::thread::Builder::new()
        .name("WF-Focus".into())
        .spawn(move || run(activity, config, channel, is_shutting_down))
        .expect("failed to spawn focus thread");
}

fn run(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<AtomicBool>,
) {
    let mut last_hwnd: isize = 0;
    let mut last_event_time = Instant::now() - Duration::from_secs(1);
    const DEBOUNCE: Duration = Duration::from_millis(250);

    loop {
        // Sleep 500ms or until shutdown
        {
            let mut guard = activity.shutdown_mu.lock();
            activity
                .shutdown_cv
                .wait_for(&mut guard, Duration::from_millis(500));
        }

        if is_shutting_down.load(Ordering::Acquire) || activity.shutdown.load(Ordering::Acquire) {
            break;
        }

        let hwnd = unsafe { GetForegroundWindow() };
        let hwnd_raw = hwnd.0 as isize;

        if hwnd_raw == last_hwnd {
            continue;
        }
        last_hwnd = hwnd_raw;

        if hwnd.0.is_null() {
            continue;
        }

        let now = Instant::now();
        if now.duration_since(last_event_time) < DEBOUNCE {
            continue;
        }
        last_event_time = now;

        let title = get_window_title(hwnd);
        let app_name = get_app_name(hwnd);

        if config.enable_debug.load(Ordering::Relaxed) {
            eprintln!("[WindowFocus] title={:?} appName={:?}", title, app_name);
        }

        channel.invoke_method_map_locked(
            "onFocusChange",
            &[
                ("title", title.as_str()),
                ("appName", app_name.as_str()),
                ("windowTitle", title.as_str()),
            ],
        );
    }
}

fn get_window_title(hwnd: HWND) -> String {
    let len = unsafe { GetWindowTextLengthW(hwnd) };
    if len <= 0 {
        return String::new();
    }
    let mut buf = vec![0u16; (len + 1) as usize];
    let written = unsafe { GetWindowTextW(hwnd, &mut buf) };
    if written <= 0 {
        return String::new();
    }
    String::from_utf16_lossy(&buf[..written as usize])
}

fn get_app_name(hwnd: HWND) -> String {
    let mut pid = 0u32;
    unsafe { GetWindowThreadProcessId(hwnd, Some(&mut pid)) };
    if pid == 0 {
        return "<unknown>".into();
    }

    let snap = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    let snap = match snap {
        Ok(h) => h,
        Err(_) => return "<unknown>".into(),
    };

    let mut entry = PROCESSENTRY32W {
        dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
        ..Default::default()
    };

    let mut found = String::from("<unknown>");
    unsafe {
        if Process32FirstW(snap, &mut entry).is_ok() {
            loop {
                if entry.th32ProcessID == pid {
                    let name_len = entry
                        .szExeFile
                        .iter()
                        .position(|&c| c == 0)
                        .unwrap_or(entry.szExeFile.len());
                    found = String::from_utf16_lossy(&entry.szExeFile[..name_len]);
                    break;
                }
                if Process32NextW(snap, &mut entry).is_err() {
                    break;
                }
            }
        }
        let _ = CloseHandle(snap);
    }

    found
}
