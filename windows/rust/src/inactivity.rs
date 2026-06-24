/// Inactivity timer thread.
/// Wakes every second, checks elapsed time since last activity, and fires
/// "onUserInactivity" via the Flutter method channel when the threshold is exceeded.

use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;

use crate::activity::ActivityTracker;
use crate::channel::MethodChannel;
use crate::config::Config;

pub fn start(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<std::sync::atomic::AtomicBool>,
) {
    std::thread::Builder::new()
        .name("WF-Inactivity".into())
        .spawn(move || run(activity, config, channel, is_shutting_down))
        .expect("failed to spawn inactivity thread");
}

fn run(
    activity: Arc<ActivityTracker>,
    config: Arc<Config>,
    channel: Arc<MethodChannel>,
    is_shutting_down: Arc<std::sync::atomic::AtomicBool>,
) {
    loop {
        // Sleep 1 second (interruptible by shutdown signal)
        {
            let mut guard = activity.shutdown_mu.lock();
            let timed_out = activity
                .shutdown_cv
                .wait_for(&mut guard, Duration::from_secs(1))
                .timed_out();
            if !timed_out {
                // Notified — re-check shutdown
            }
        }

        if is_shutting_down.load(Ordering::Acquire) || activity.shutdown.load(Ordering::Acquire) {
            break;
        }

        let elapsed = activity.elapsed_ms();
        let threshold = config.inactivity_threshold_ms.load(Ordering::Acquire) as u64;

        if elapsed > threshold && activity.user_is_active.load(Ordering::Acquire) {
            activity.user_is_active.store(false, Ordering::Release);

            if config.enable_debug.load(Ordering::Relaxed) {
                eprintln!(
                    "[WindowFocus] User inactive. elapsed={}ms threshold={}ms",
                    elapsed, threshold
                );
            }

            channel.invoke_method_string_locked("onUserInactivity", "User is inactive");
        }
    }
}
