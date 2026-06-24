/// System audio peak monitoring via Windows Core Audio COM API.
/// AudioMeterCache maintains long-lived COM objects (recreated every 60s or on failure)
/// to avoid the overhead of creating/destroying 3 COM objects every 500ms.
///
/// All COM objects must live on the monitoring thread (COM apartment affinity).
/// ComGuard initialises/uninitialises COM for the calling thread's lifetime.

use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use windows::Win32::Media::Audio::{
    eConsole, eRender, IMMDeviceEnumerator, MMDeviceEnumerator,
};
use windows::Win32::Media::Audio::Endpoints::IAudioMeterInformation;
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoUninitialize, CLSCTX_ALL, COINIT_MULTITHREADED,
};

use crate::config::Config;

const REFRESH_INTERVAL: Duration = Duration::from_secs(60);
const MAX_FAILURES: u32 = 3;

pub struct ComGuard;

impl ComGuard {
    /// Initialise COM for the current thread (multithreaded apartment).
    /// Returns a guard that calls CoUninitialize on drop.
    /// Ignores S_FALSE (already initialized on this thread).
    pub fn init() -> Self {
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        }
        ComGuard
    }
}

impl Drop for ComGuard {
    fn drop(&mut self) {
        unsafe { CoUninitialize() }
    }
}

/// Cached COM audio meter — keeps IMMDeviceEnumerator/IMMDevice/IAudioMeterInformation
/// alive across calls to avoid repeated COM object creation.
pub struct AudioMeterCache {
    enumerator: Option<IMMDeviceEnumerator>,
    meter: Option<IAudioMeterInformation>,
    last_init: Option<Instant>,
    consecutive_failures: u32,
}

impl AudioMeterCache {
    pub fn new() -> Self {
        Self {
            enumerator: None,
            meter: None,
            last_init: None,
            consecutive_failures: 0,
        }
    }

    /// Force recreation on next call (e.g., after system resume).
    pub fn invalidate(&mut self) {
        self.meter = None;
        self.enumerator = None;
        self.last_init = None;
        self.consecutive_failures = 0;
    }

    /// Return true if audio peak exceeds the configured threshold.
    pub fn check(&mut self, config: &Config) -> bool {
        let debug = config.enable_debug.load(Ordering::Relaxed);
        let threshold = config.audio_threshold();

        let needs_refresh = self.meter.is_none()
            || self.consecutive_failures > MAX_FAILURES
            || self
                .last_init
                .map(|t| t.elapsed() > REFRESH_INTERVAL)
                .unwrap_or(true);

        if needs_refresh {
            self.invalidate();
            if !self.init(debug) {
                return false;
            }
        }

        match self.get_peak(debug) {
            Some(peak) => {
                self.consecutive_failures = 0;
                if debug && peak > threshold {
                    eprintln!("[WindowFocus] Audio detected, peak={:.4}", peak);
                }
                peak > threshold
            }
            None => {
                self.consecutive_failures += 1;
                if debug {
                    eprintln!(
                        "[WindowFocus] AudioMeterCache: GetPeakValue failed ({}x)",
                        self.consecutive_failures
                    );
                }
                false
            }
        }
    }

    fn init(&mut self, debug: bool) -> bool {
        unsafe {
            let enumerator: Result<IMMDeviceEnumerator, _> =
                CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL);
            let enumerator = match enumerator {
                Ok(e) => e,
                Err(e) => {
                    if debug { eprintln!("[WindowFocus] AudioMeterCache: enumerator failed: {e}"); }
                    return false;
                }
            };

            let device = match enumerator.GetDefaultAudioEndpoint(eRender, eConsole) {
                Ok(d) => d,
                Err(e) => {
                    if debug { eprintln!("[WindowFocus] AudioMeterCache: default endpoint failed: {e}"); }
                    return false;
                }
            };

            let meter: Result<IAudioMeterInformation, _> = device.Activate(CLSCTX_ALL, None);
            let meter = match meter {
                Ok(m) => m,
                Err(e) => {
                    if debug { eprintln!("[WindowFocus] AudioMeterCache: meter activate failed: {e}"); }
                    return false;
                }
            };

            self.enumerator = Some(enumerator);
            self.meter = Some(meter);
            self.last_init = Some(Instant::now());
            self.consecutive_failures = 0;
            true
        }
    }

    fn get_peak(&self, _debug: bool) -> Option<f32> {
        let meter = self.meter.as_ref()?;
        unsafe { meter.GetPeakValue().ok() }
    }
}
