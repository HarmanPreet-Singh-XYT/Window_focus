/// Main input monitoring thread.
/// Polls every 500ms for: keyboard (fallback), mouse, XInput controllers,
/// HID devices, and system audio.
/// On input detection, updates activity and fires "onUserActive".

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::activity::ActivityTracker;
use crate::channel::MethodChannel;
use crate::config::Config;

pub fn start(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<AtomicBool>,
    needs_hid_reinit: Arc<AtomicBool>,
    needs_audio_cache_reset: Arc<AtomicBool>,
) {
    std::thread::Builder::new()
        .name("WF-Monitor".into())
        .spawn(move || {
            run(
                activity,
                config,
                channel,
                is_shutting_down,
                needs_hid_reinit,
                needs_audio_cache_reset,
            )
        })
        .expect("failed to spawn monitor thread");
}

fn run(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<AtomicBool>,
    needs_hid_reinit: Arc<AtomicBool>,
    needs_audio_cache_reset: Arc<AtomicBool>,
) {
    // COM must be initialized on this thread for audio monitoring
    let _com = crate::audio::ComGuard::init();

    let mut audio_cache = crate::audio::AudioMeterCache::new();
    let mut hid_state = crate::hid::HidState::new();
    let mut controller_states = [Default::default(); 4usize];

    let mut last_hid_reinit = Instant::now();
    let mut last_full_hid_refresh = Instant::now();
    const HID_REINIT_INTERVAL: Duration = Duration::from_secs(30);
    const HID_FULL_REFRESH: Duration = Duration::from_secs(300); // 5 min

    loop {
        // Wait 500ms or until shutdown
        {
            let mut guard = activity.shutdown_mu.lock();
            let timed_out = activity
                .shutdown_cv
                .wait_for(&mut guard, Duration::from_millis(500))
                .timed_out();
            drop(guard);
            let _ = timed_out;
        }

        if is_shutting_down.load(Ordering::Acquire) || activity.shutdown.load(Ordering::Acquire) {
            break;
        }

        let mut input_detected = false;

        // Keyboard (polling fallback — hook sets lastKeyEventMs for real-time)
        if config.monitor_keyboard.load(Ordering::Acquire) {
            if crate::keyboard::check_keyboard_input(&config, &activity) {
                input_detected = true;
            }
        }

        // Mouse position
        if crate::mouse::check_mouse_moved() {
            input_detected = true;
        }

        // XInput controllers
        if config.monitor_controllers.load(Ordering::Acquire) {
            if crate::xinput::check_controllers(&mut controller_states, &config) {
                input_detected = true;
            }
        }

        // System audio
        if config.monitor_audio.load(Ordering::Acquire) {
            if needs_audio_cache_reset.swap(false, Ordering::AcqRel) {
                audio_cache.invalidate();
            }
            if audio_cache.check(&config) {
                input_detected = true;
            }
        }

        // HID devices
        if config.monitor_hid.load(Ordering::Acquire) {
            // Honor reinit request (from power resume or setHIDMonitoring enabling)
            if needs_hid_reinit.swap(false, Ordering::AcqRel) {
                hid_state.close_all(&config);
                hid_state.init(&config);
                last_hid_reinit = Instant::now();
                last_full_hid_refresh = Instant::now();
            }

            // Quick reinit if device list is empty
            if last_hid_reinit.elapsed() > HID_REINIT_INTERVAL && hid_state.is_empty() {
                last_hid_reinit = Instant::now();
                hid_state.init(&config);
            }

            // Periodic full refresh to clear stale handles
            if last_full_hid_refresh.elapsed() > HID_FULL_REFRESH {
                last_full_hid_refresh = Instant::now();
                hid_state.close_all(&config);
                hid_state.init(&config);
            }

            if hid_state.check(&config) {
                input_detected = true;
            }
        } else if !hid_state.is_empty() {
            // HID was disabled — close open handles
            hid_state.close_all(&config);
        }

        if input_detected {
            activity.update_last_activity();
            if !activity.user_is_active.swap(true, Ordering::AcqRel) {
                channel.invoke_method_string_locked("onUserActive", "User is active");
            }
        }
    }

    // Cleanup
    hid_state.close_all(&config);
}
