/// HID device enumeration and overlapped I/O monitoring.

use std::sync::atomic::Ordering;

use windows::core::PCWSTR;
use windows::Win32::Devices::DeviceAndDriverInstallation::{
    SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInterfaces, SetupDiGetClassDevsW,
    SetupDiGetDeviceInterfaceDetailW, SP_DEVICE_INTERFACE_DATA,
    SP_DEVICE_INTERFACE_DETAIL_DATA_W, DIGCF_DEVICEINTERFACE, DIGCF_PRESENT,
};
use windows::Win32::Devices::HumanInterfaceDevice::{
    HidD_FreePreparsedData, HidD_GetAttributes, HidD_GetHidGuid, HidD_GetPreparsedData,
    HidP_GetCaps, HIDD_ATTRIBUTES, HIDP_CAPS, HIDP_STATUS_SUCCESS, PHIDP_PREPARSED_DATA,
};
use windows::Win32::Foundation::{
    CloseHandle, ERROR_BAD_DEVICE, ERROR_DEVICE_NOT_CONNECTED, ERROR_GEN_FAILURE,
    ERROR_INVALID_HANDLE, ERROR_IO_PENDING, GENERIC_READ, HANDLE, INVALID_HANDLE_VALUE,
};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, ReadFile, FILE_FLAG_OVERLAPPED, FILE_SHARE_READ, FILE_SHARE_WRITE,
    OPEN_EXISTING,
};
use windows::Win32::System::IO::{CancelIo, GetOverlappedResult, OVERLAPPED};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};

use crate::config::Config;

struct HidDevice {
    handle: HANDLE,
    last_state: Vec<u8>,
}

pub struct HidState {
    devices: Vec<HidDevice>,
}

impl HidState {
    pub fn new() -> Self {
        Self { devices: Vec::new() }
    }

    pub fn is_empty(&self) -> bool {
        self.devices.is_empty()
    }

    pub fn init(&mut self, config: &Config) {
        let debug = config.enable_debug.load(Ordering::Relaxed);

        let hid_guid = unsafe { HidD_GetHidGuid() };

        let dev_info = unsafe {
            SetupDiGetClassDevsW(
                Some(&hid_guid),
                PCWSTR::null(),
                None,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
            )
        };
        let dev_info = match dev_info {
            Ok(d) => d,
            Err(e) => {
                if debug { eprintln!("[WindowFocus] SetupDiGetClassDevsW failed: {e}"); }
                return;
            }
        };

        let mut index = 0u32;
        loop {
            let mut iface_data = SP_DEVICE_INTERFACE_DATA {
                cbSize: std::mem::size_of::<SP_DEVICE_INTERFACE_DATA>() as u32,
                ..Default::default()
            };

            let ok = unsafe {
                SetupDiEnumDeviceInterfaces(dev_info, None, &hid_guid, index, &mut iface_data)
            };
            if ok.is_err() {
                break;
            }
            index += 1;

            let mut required = 0u32;
            unsafe {
                let _ = SetupDiGetDeviceInterfaceDetailW(
                    dev_info,
                    &iface_data,
                    None,
                    0,
                    Some(&mut required),
                    None,
                );
            }
            if required == 0 {
                continue;
            }

            // Allocate detail buffer (cbSize u32 + DevicePath [u16; N])
            let detail_size = required as usize;
            let mut detail_buf: Vec<u8> = vec![0u8; detail_size];
            let detail_ptr = detail_buf.as_mut_ptr() as *mut SP_DEVICE_INTERFACE_DETAIL_DATA_W;
            unsafe {
                (*detail_ptr).cbSize =
                    std::mem::size_of::<SP_DEVICE_INTERFACE_DETAIL_DATA_W>() as u32;
                if SetupDiGetDeviceInterfaceDetailW(
                    dev_info,
                    &iface_data,
                    Some(detail_ptr),
                    detail_size as u32,
                    None,
                    None,
                )
                .is_err()
                {
                    continue;
                }
            }

            let path_ptr = unsafe { PCWSTR((*detail_ptr).DevicePath.as_ptr()) };
            let handle = unsafe {
                CreateFileW(
                    path_ptr,
                    GENERIC_READ.0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    None,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    None,
                )
            };
            let handle = match handle {
                Ok(h) if h != INVALID_HANDLE_VALUE => h,
                _ => continue,
            };

            // Read HID attributes (returns BOOLEAN, not Result)
            let mut attrs = HIDD_ATTRIBUTES {
                Size: std::mem::size_of::<HIDD_ATTRIBUTES>() as u32,
                ..Default::default()
            };
            let got_attrs = unsafe { HidD_GetAttributes(handle, &mut attrs) };
            if !got_attrs.as_bool() {
                unsafe { let _ = CloseHandle(handle); }
                continue;
            }

            // Get preparsed data (returns BOOLEAN)
            let mut preparsed = PHIDP_PREPARSED_DATA::default();
            let got_preparsed = unsafe { HidD_GetPreparsedData(handle, &mut preparsed) };
            if !got_preparsed.as_bool() || preparsed.0 == 0 {
                unsafe { let _ = CloseHandle(handle); }
                continue;
            }

            let mut caps = HIDP_CAPS::default();
            let caps_status = unsafe { HidP_GetCaps(preparsed, &mut caps) };
            unsafe { HidD_FreePreparsedData(preparsed) };

            if caps_status != HIDP_STATUS_SUCCESS {
                unsafe { let _ = CloseHandle(handle); }
                continue;
            }

            // Exclude audio, keyboard, mouse
            let is_audio    = caps.UsagePage == 0x0B || caps.UsagePage == 0x0C;
            let is_keyboard = caps.UsagePage == 0x01 && caps.Usage == 0x06;
            let is_mouse    = caps.UsagePage == 0x01 && caps.Usage == 0x02;

            if is_audio || is_keyboard || is_mouse || caps.InputReportByteLength == 0 {
                unsafe { let _ = CloseHandle(handle); }
                continue;
            }

            if debug {
                eprintln!(
                    "[WindowFocus] HID device added: VID={:04X} PID={:04X}",
                    attrs.VendorID, attrs.ProductID
                );
            }

            self.devices.push(HidDevice {
                handle,
                last_state: vec![0u8; caps.InputReportByteLength as usize],
            });
        }

        unsafe { let _ = SetupDiDestroyDeviceInfoList(dev_info); };

        if debug {
            eprintln!("[WindowFocus] Initialized {} HID devices", self.devices.len());
        }
    }

    pub fn close_all(&mut self, config: &Config) {
        let debug = config.enable_debug.load(Ordering::Relaxed);
        for dev in self.devices.drain(..) {
            if dev.handle != INVALID_HANDLE_VALUE {
                unsafe {
                    let _ = CancelIo(dev.handle);
                    let _ = CloseHandle(dev.handle);
                }
            }
        }
        if debug {
            eprintln!("[WindowFocus] Closed all HID devices");
        }
    }

    pub fn check(&mut self, config: &Config) -> bool {
        let debug = config.enable_debug.load(Ordering::Relaxed);
        let mut input_detected = false;
        let mut invalid: Vec<usize> = Vec::new();

        for (i, dev) in self.devices.iter_mut().enumerate() {
            if dev.handle == INVALID_HANDLE_VALUE {
                invalid.push(i);
                continue;
            }

            let buf_size = dev.last_state.len();
            let mut buf = vec![0u8; buf_size];

            let event = match unsafe {
                CreateEventW(None, windows::Win32::Foundation::TRUE, windows::Win32::Foundation::FALSE, PCWSTR::null())
            } {
                Ok(e) => e,
                Err(_) => { invalid.push(i); continue; }
            };

            let mut ovl = OVERLAPPED::default();
            ovl.hEvent = event;

            let mut bytes_read = 0u32;
            let read_result = unsafe {
                ReadFile(
                    dev.handle,
                    Some(&mut buf),
                    Some(&mut bytes_read),
                    Some(&mut ovl),
                )
            };

            match read_result {
                Ok(_) => {
                    // Completed immediately
                    if bytes_read > 0 && buf != dev.last_state {
                        dev.last_state = buf.clone();
                        input_detected = true;
                        if debug { eprintln!("[WindowFocus] HID device {} input detected", i); }
                    }
                    unsafe { let _ = CloseHandle(event); }
                }
                Err(ref e) => {
                    let code = e.code().0 as u32;
                    if code == ERROR_IO_PENDING.0 {
                        let wait = unsafe { WaitForSingleObject(event, 10) };
                        if wait.0 == 0 {
                            let ovl_ok = unsafe {
                                GetOverlappedResult(dev.handle, &ovl, &mut bytes_read, false)
                            };
                            match ovl_ok {
                                Ok(_) if bytes_read > 0 && buf != dev.last_state => {
                                    dev.last_state = buf.clone();
                                    input_detected = true;
                                    if debug { eprintln!("[WindowFocus] HID device {} input (overlapped)", i); }
                                }
                                Err(ref oe) => {
                                    let ec = oe.code().0 as u32;
                                    if ec == ERROR_INVALID_HANDLE.0 || ec == ERROR_DEVICE_NOT_CONNECTED.0 {
                                        invalid.push(i);
                                    }
                                }
                                _ => {}
                            }
                        }
                        unsafe {
                            let _ = CancelIo(dev.handle);
                            let _ = CloseHandle(event);
                        }
                    } else if code == ERROR_DEVICE_NOT_CONNECTED.0
                        || code == ERROR_GEN_FAILURE.0
                        || code == ERROR_INVALID_HANDLE.0
                        || code == ERROR_BAD_DEVICE.0
                    {
                        if debug { eprintln!("[WindowFocus] HID device {} disconnected (0x{:X})", i, code); }
                        unsafe { let _ = CloseHandle(event); }
                        invalid.push(i);
                    } else {
                        if debug { eprintln!("[WindowFocus] HID device {} error 0x{:X}", i, code); }
                        unsafe {
                            let _ = CancelIo(dev.handle);
                            let _ = CloseHandle(event);
                        }
                        invalid.push(i);
                    }
                }
            }

            if input_detected { break; }
        }

        // Remove invalid devices in reverse index order
        invalid.sort_unstable();
        invalid.dedup();
        for &idx in invalid.iter().rev() {
            if idx < self.devices.len() {
                let dev = self.devices.remove(idx);
                if dev.handle != INVALID_HANDLE_VALUE {
                    unsafe {
                        let _ = CancelIo(dev.handle);
                        let _ = CloseHandle(dev.handle);
                    }
                }
                if debug { eprintln!("[WindowFocus] Removed invalid HID device at index {}", idx); }
            }
        }

        input_detected
    }
}
