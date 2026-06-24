/// Method dispatch: maps Flutter method names to handler functions.
/// Receives decoded EncodableValue args and sends back typed responses
/// via ResponseSender.

use std::sync::atomic::Ordering;

use crate::channel::ResponseSender;
use crate::codec::EncodableValue;

/// Main dispatch entry point. Called from the MethodChannel callback.
pub fn handle(method: &str, args: Option<&EncodableValue>, sender: ResponseSender) {
    let state = match crate::plugin::get() {
        Some(s) => s,
        None => {
            sender.error("NOT_INITIALIZED", "Plugin not initialized");
            return;
        }
    };

    match method {
        "setDebugMode" => {
            match args_bool(args, "debug") {
                Some(v) => {
                    state.config.enable_debug.store(v, Ordering::Relaxed);
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected bool for 'debug'"),
            }
        }

        "setControllerMonitoring" => {
            match args_bool(args, "enabled") {
                Some(v) => {
                    state.config.monitor_controllers.store(v, Ordering::Release);
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected bool for 'enabled'"),
            }
        }

        "setAudioMonitoring" => {
            match args_bool(args, "enabled") {
                Some(v) => {
                    state.config.monitor_audio.store(v, Ordering::Release);
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected bool for 'enabled'"),
            }
        }

        "setAudioThreshold" => {
            match args_f64(args, "threshold") {
                Some(v) => {
                    state.config.set_audio_threshold(v as f32);
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected double for 'threshold'"),
            }
        }

        "setHIDMonitoring" => {
            match args_bool(args, "enabled") {
                Some(new_val) => {
                    let old_val = state.config.monitor_hid.swap(new_val, Ordering::AcqRel);
                    if new_val && !old_val {
                        state.needs_hid_reinit.store(true, Ordering::Release);
                    }
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected bool for 'enabled'"),
            }
        }

        "setKeyboardMonitoring" => {
            match args_bool(args, "enabled") {
                Some(v) => {
                    state.config.monitor_keyboard.store(v, Ordering::Release);
                    sender.success(EncodableValue::Null);
                }
                None => sender.error("Invalid argument", "Expected bool for 'enabled'"),
            }
        }

        "setInactivityTimeOut" => {
            match args_i32(args, "inactivityTimeOut") {
                Some(v) => {
                    state.config.inactivity_threshold_ms.store(v, Ordering::Release);
                    sender.success(EncodableValue::Int32(v));
                }
                None => sender.error("Invalid argument", "Expected integer for 'inactivityTimeOut'"),
            }
        }

        "getPlatformVersion" => {
            sender.success(EncodableValue::String("Windows: Rust".to_owned()));
        }

        "getIdleThreshold" => {
            let v = state.config.inactivity_threshold_ms.load(Ordering::Acquire);
            sender.success(EncodableValue::Int32(v));
        }

        "checkScreenRecordingPermission" => {
            sender.success(EncodableValue::Bool(true));
        }

        "requestScreenRecordingPermission" => {
            sender.success(EncodableValue::Null);
        }

        "takeScreenshot" => {
            sender.error("NOT_IMPLEMENTED", "Screenshot not supported in Rust port");
        }

        _ => sender.not_implemented(),
    }
}

// ---- Argument extraction helpers ----

fn args_bool(args: Option<&EncodableValue>, key: &str) -> Option<bool> {
    args?.map_get(key)?.as_bool()
}

fn args_f64(args: Option<&EncodableValue>, key: &str) -> Option<f64> {
    args?.map_get(key)?.as_f64()
}

fn args_i32(args: Option<&EncodableValue>, key: &str) -> Option<i32> {
    args?.map_get(key)?.as_i32()
}
