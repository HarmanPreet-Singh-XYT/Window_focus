use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};

/// Mirrors all configuration atomics from the C++ WindowFocusPlugin class.
/// All fields are independently readable/writable from any thread.
pub struct Config {
    pub enable_debug: AtomicBool,
    /// Inactivity threshold in milliseconds (default 300_000 = 5 min).
    pub inactivity_threshold_ms: AtomicI32,
    pub monitor_audio: AtomicBool,
    /// Audio threshold stored as f32 bits (use f32::from_bits / f32::to_bits).
    pub audio_threshold_bits: AtomicU32,
    pub monitor_controllers: AtomicBool,
    pub monitor_hid: AtomicBool,
    pub monitor_keyboard: AtomicBool,
}

impl Config {
    pub const fn new() -> Self {
        Self {
            enable_debug: AtomicBool::new(false),
            inactivity_threshold_ms: AtomicI32::new(300_000),
            monitor_audio: AtomicBool::new(false),
            audio_threshold_bits: AtomicU32::new(0x3C23D70A), // 0.01f32.to_bits()
            monitor_controllers: AtomicBool::new(false),
            monitor_hid: AtomicBool::new(false),
            monitor_keyboard: AtomicBool::new(true),
        }
    }

    pub fn audio_threshold(&self) -> f32 {
        f32::from_bits(self.audio_threshold_bits.load(Ordering::Acquire))
    }

    pub fn set_audio_threshold(&self, v: f32) {
        self.audio_threshold_bits.store(v.to_bits(), Ordering::Release);
    }
}

/// Millisecond timestamp from a steady clock (for key event timing).
pub fn now_millis() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
