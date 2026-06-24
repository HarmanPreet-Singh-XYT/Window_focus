use parking_lot::{Condvar, Mutex};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Instant;

/// Shared activity state, accessed by hooks, polling threads, and the inactivity timer.
pub struct ActivityTracker {
    pub last_activity: Mutex<Instant>,
    pub user_is_active: AtomicBool,
    /// Timestamp of last keyboard hook event in milliseconds (monotonic epoch).
    pub last_key_event_ms: AtomicU64,
    /// Shutdown signal for threads waiting on shutdown_cv.
    pub shutdown: AtomicBool,
    pub shutdown_cv: Condvar,
    pub shutdown_mu: Mutex<()>,
}

impl ActivityTracker {
    pub fn new() -> Self {
        Self {
            last_activity: Mutex::new(Instant::now()),
            user_is_active: AtomicBool::new(true),
            last_key_event_ms: AtomicU64::new(0),
            shutdown: AtomicBool::new(false),
            shutdown_cv: Condvar::new(),
            shutdown_mu: Mutex::new(()),
        }
    }

    pub fn update_last_activity(&self) {
        *self.last_activity.lock() = Instant::now();
    }

    pub fn elapsed_ms(&self) -> u64 {
        self.last_activity.lock().elapsed().as_millis() as u64
    }

    /// Signal all waiting threads to wake and check the shutdown flag.
    pub fn notify_shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
        self.shutdown_cv.notify_all();
    }
}
