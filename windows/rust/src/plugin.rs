/// Central plugin state, held in a process-global OnceCell.
/// Initialised by WindowFocusPluginCApiRegisterWithRegistrar.

use once_cell::sync::OnceCell;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use crate::activity::ActivityTracker;
use crate::channel::MethodChannel;
use crate::config::Config;

pub struct PluginState {
    pub config: Arc<Config>,
    pub activity: Arc<ActivityTracker>,
    pub channel: Arc<MethodChannel>,
    /// Set to true before shutdown; subsystems check this to exit their loops.
    pub is_shutting_down: Arc<AtomicBool>,
    /// Signals the monitoring thread to re-initialise HID devices (after resume).
    pub needs_hid_reinit: Arc<AtomicBool>,
    /// Signals the monitoring thread to reset the audio COM cache (after resume).
    pub needs_audio_cache_reset: Arc<AtomicBool>,
}

static PLUGIN_STATE: OnceCell<PluginState> = OnceCell::new();

/// Called from the platform thread after the messenger and channel are set up.
pub fn init(channel: Arc<MethodChannel>) {
    let state = PluginState {
        config: Arc::new(Config::new()),
        activity: Arc::new(ActivityTracker::new()),
        channel,
        is_shutting_down: Arc::new(AtomicBool::new(false)),
        needs_hid_reinit: Arc::new(AtomicBool::new(false)),
        needs_audio_cache_reset: Arc::new(AtomicBool::new(false)),
    };

    // Ignore error if called twice (hot-restart in debug Flutter)
    let _ = PLUGIN_STATE.set(state);

    // Spawn background threads
    if let Some(s) = PLUGIN_STATE.get() {
        crate::inactivity::start(
            Arc::clone(&s.activity),
            Arc::clone(&s.config),
            Arc::clone(&s.channel),
            Arc::clone(&s.is_shutting_down),
        );
        crate::monitor::start(
            Arc::clone(&s.activity),
            Arc::clone(&s.config),
            Arc::clone(&s.channel),
            Arc::clone(&s.is_shutting_down),
            Arc::clone(&s.needs_hid_reinit),
            Arc::clone(&s.needs_audio_cache_reset),
        );
        crate::focus::start(
            Arc::clone(&s.activity),
            Arc::clone(&s.config),
            Arc::clone(&s.channel),
            Arc::clone(&s.is_shutting_down),
        );
    }
}

/// Called from the destruction handler (platform thread).
pub fn shutdown() {
    if let Some(s) = PLUGIN_STATE.get() {
        s.is_shutting_down.store(true, Ordering::Release);
        s.activity.notify_shutdown();
    }
}

/// Called from the dispatcher when the system resumes from sleep/hibernate.
pub fn on_system_resume() {
    if let Some(s) = PLUGIN_STATE.get() {
        if s.is_shutting_down.load(Ordering::Acquire) {
            return;
        }
        s.activity.update_last_activity();
        s.needs_hid_reinit.store(true, Ordering::Release);
        s.needs_audio_cache_reset.store(true, Ordering::Release);

        // Signal user active (they woke the machine)
        if !s.activity.user_is_active.swap(true, Ordering::AcqRel) {
            s.channel.invoke_method_string("onUserActive", "User is active (system resume)");
        }
    }
}

/// Get the global plugin state. Returns None if not initialised.
pub fn get() -> Option<&'static PluginState> {
    PLUGIN_STATE.get()
}
