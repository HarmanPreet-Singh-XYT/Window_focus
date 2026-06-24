/// MethodChannel — send/receive Flutter method calls using StandardMethodCodec.

use std::ffi::{c_void, CString};
use std::sync::Arc;

use crate::codec::{self, EncodableValue};
use crate::flutter_api::{self, FlutterDesktopMessageResponseHandleRef, FlutterDesktopMessengerRef};

/// A Flutter MethodChannel backed by the messenger ref.
/// Holds an AddRef'd copy of the messenger for its lifetime.
pub struct MethodChannel {
    /// AddRef'd messenger — released in Drop.
    messenger: FlutterDesktopMessengerRef,
    /// Channel name as a NUL-terminated C string.
    channel_name: CString,
}

// SAFETY: FlutterDesktopMessengerRef is a raw pointer into a Flutter-owned struct
// that lives for the duration of the plugin. The Flutter API is thread-safe when
// the messenger is locked (we lock before sending).
unsafe impl Send for MethodChannel {}
unsafe impl Sync for MethodChannel {}

impl MethodChannel {
    /// Create a new MethodChannel, AddRef-ing the messenger.
    pub fn new(messenger: FlutterDesktopMessengerRef, channel_name: &str) -> Self {
        let messenger = unsafe { flutter_api::messenger_add_ref(messenger) };
        Self {
            messenger,
            channel_name: CString::new(channel_name).expect("channel name contains NUL"),
        }
    }

    /// Send a method call to Dart from the platform thread (no locking required).
    /// `arg` is a plain string — encodes as EncodableValue::String.
    pub fn invoke_method_string(&self, method: &str, arg: &str) {
        let payload = codec::encode_method_call(
            method,
            &EncodableValue::String(arg.to_owned()),
        );
        unsafe {
            flutter_api::messenger_send(
                self.messenger,
                self.channel_name.as_ptr(),
                payload.as_ptr(),
                payload.len(),
            );
        }
    }

    /// Send a method call to Dart from a background thread (locks messenger first).
    pub fn invoke_method_string_locked(&self, method: &str, arg: &str) {
        let payload = codec::encode_method_call(
            method,
            &EncodableValue::String(arg.to_owned()),
        );
        unsafe {
            let locked = flutter_api::messenger_lock(self.messenger);
            flutter_api::messenger_send(
                locked,
                self.channel_name.as_ptr(),
                payload.as_ptr(),
                payload.len(),
            );
            flutter_api::messenger_unlock(locked);
        }
    }

    /// Send a method call to Dart from a background thread with a map argument (locks messenger).
    pub fn invoke_method_map_locked(&self, method: &str, pairs: &[(&str, &str)]) {
        let map_pairs: Vec<(EncodableValue, EncodableValue)> = pairs
            .iter()
            .map(|(k, v)| {
                (
                    EncodableValue::String(k.to_string()),
                    EncodableValue::String(v.to_string()),
                )
            })
            .collect();

        let payload = codec::encode_method_call(
            method,
            &EncodableValue::Map(map_pairs),
        );

        unsafe {
            let locked = flutter_api::messenger_lock(self.messenger);
            flutter_api::messenger_send(
                locked,
                self.channel_name.as_ptr(),
                payload.as_ptr(),
                payload.len(),
            );
            flutter_api::messenger_unlock(locked);
        }
    }

    /// Register a handler for incoming method calls from Dart.
    /// The handler receives the method name and optional args.
    ///
    /// The handler is boxed and leaked as a raw pointer to satisfy the C callback ABI.
    /// It is freed when the channel callback is cleared (on shutdown, pass None).
    pub fn set_method_call_handler<F>(&self, handler: F)
    where
        F: Fn(&str, Option<&EncodableValue>, FlutterDesktopMessageResponseHandleRef) + Send + Sync + 'static,
    {
        let boxed: Box<dyn Fn(&str, Option<&EncodableValue>, FlutterDesktopMessageResponseHandleRef) + Send + Sync + 'static> =
            Box::new(handler);
        let raw = Box::into_raw(Box::new(boxed));

        unsafe {
            flutter_api::messenger_set_callback(
                self.messenger,
                self.channel_name.as_ptr(),
                Some(message_callback),
                raw as *mut c_void,
            );
        }
    }

}

impl Drop for MethodChannel {
    fn drop(&mut self) {
        unsafe {
            flutter_api::messenger_release(self.messenger);
        }
    }
}

// ---- Raw C callback ----

unsafe extern "C" fn message_callback(
    messenger: FlutterDesktopMessengerRef,
    message: *const crate::flutter_api::FlutterDesktopMessage,
    user_data: *mut c_void,
) {
    let msg = &*message;
    let bytes = std::slice::from_raw_parts(msg.message, msg.message_size);
    let response_handle = msg.response_handle;

    // Decode the method call
    let (method, args) = match codec::decode_method_call(bytes) {
        Some(pair) => pair,
        None => {
            // Malformed — send not-implemented
            let resp = codec::encode_not_implemented();
            (flutter_api::fns().messenger_send_response)(
                messenger,
                response_handle,
                resp.as_ptr(),
                resp.len(),
            );
            return;
        }
    };

    // Retrieve handler
    let handler_ptr = user_data
        as *mut Box<dyn Fn(&str, Option<&EncodableValue>, FlutterDesktopMessageResponseHandleRef) + Send + Sync + 'static>;

    if handler_ptr.is_null() {
        let resp = codec::encode_not_implemented();
        (flutter_api::fns().messenger_send_response)(
            messenger,
            response_handle,
            resp.as_ptr(),
            resp.len(),
        );
        return;
    }

    let handler = &*handler_ptr;
    handler(method.as_str(), args.as_ref(), response_handle);
}

/// Helper: create a response-sending closure bound to a channel Arc.
/// Used by method_dispatch::handle to send back results.
pub struct ResponseSender {
    pub messenger: FlutterDesktopMessengerRef,
    pub handle: FlutterDesktopMessageResponseHandleRef,
}

// SAFETY: response handles are only valid on the thread that called us, but we
// use them synchronously within the same callback invocation.
unsafe impl Send for ResponseSender {}

impl ResponseSender {
    pub fn success(self, value: EncodableValue) {
        let encoded = codec::encode_success(&value);
        unsafe {
            (flutter_api::fns().messenger_send_response)(
                self.messenger,
                self.handle,
                encoded.as_ptr(),
                encoded.len(),
            );
        }
    }

    pub fn error(self, code: &str, message: &str) {
        let encoded = codec::encode_error(code, message);
        unsafe {
            (flutter_api::fns().messenger_send_response)(
                self.messenger,
                self.handle,
                encoded.as_ptr(),
                encoded.len(),
            );
        }
    }

    pub fn not_implemented(self) {
        let encoded = codec::encode_not_implemented();
        unsafe {
            (flutter_api::fns().messenger_send_response)(
                self.messenger,
                self.handle,
                encoded.as_ptr(),
                encoded.len(),
            );
        }
    }
}

/// Convenience: build a MethodChannel handler closure that calls method_dispatch::handle
/// and replies via ResponseSender.
pub fn make_method_call_handler(
    channel: Arc<MethodChannel>,
) -> impl Fn(&str, Option<&EncodableValue>, FlutterDesktopMessageResponseHandleRef) + Send + Sync + 'static
{
    move |method, args, response_handle| {
        let sender = ResponseSender {
            messenger: channel.messenger,
            handle: response_handle,
        };
        crate::method_dispatch::handle(method, args, sender);
    }
}
