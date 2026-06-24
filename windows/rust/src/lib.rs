mod activity;
mod audio;
mod channel;
mod codec;
mod config;
mod dispatcher;
mod flutter_api;
mod focus;
mod hid;
mod hooks;
mod inactivity;
mod keyboard;
mod method_dispatch;
mod monitor;
mod mouse;
mod plugin;
mod xinput;

use std::sync::Arc;

/// Called by Flutter's generated plugin registrant.
/// This is the single entry point for the pure-Rust DLL.
#[no_mangle]
pub unsafe extern "C" fn WindowFocusPluginCApiRegisterWithRegistrar(
    registrar: *mut flutter_api::FlutterDesktopPluginRegistrar,
) {
    // 1. Load Flutter API functions from flutter_windows.dll (already in process)
    flutter_api::init();

    // 2. Get the messenger from the registrar
    let messenger = flutter_api::registrar_get_messenger(registrar);

    // 3. Initialise the platform task dispatcher (hidden HWND + power events)
    dispatcher::Dispatcher::init();

    // 4. Create the MethodChannel
    let channel = Arc::new(channel::MethodChannel::new(
        messenger,
        "expert.kotelnikoff/window_focus",
    ));

    // 5. Register the method call handler
    let handler_channel = Arc::clone(&channel);
    channel.set_method_call_handler(channel::make_method_call_handler(handler_channel));

    // 6. Initialise plugin state and spawn background threads
    plugin::init(Arc::clone(&channel));

    // 7. Install low-level keyboard/mouse hooks
    hooks::install();

    // 8. Register the destruction handler so we clean up when Flutter unloads
    flutter_api::registrar_set_destruction_handler(registrar, on_registrar_destroyed);
}

unsafe extern "C" fn on_registrar_destroyed(
    _registrar: *mut flutter_api::FlutterDesktopPluginRegistrar,
) {
    hooks::remove();
    plugin::shutdown();
    dispatcher::Dispatcher::shutdown();
}
