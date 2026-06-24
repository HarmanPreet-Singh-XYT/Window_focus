fn main() {
    // Windows system libraries required by the plugin subsystems.
    // These are linked by the Rust staticlib so CMake doesn't need them separately.
    println!("cargo:rustc-link-lib=xinput");
    println!("cargo:rustc-link-lib=ole32");
    println!("cargo:rustc-link-lib=hid");
    println!("cargo:rustc-link-lib=setupapi");

    // Required by windows-rs runtime
    println!("cargo:rustc-link-lib=ntdll");
    println!("cargo:rustc-link-lib=userenv");
    println!("cargo:rustc-link-lib=ws2_32");
    println!("cargo:rustc-link-lib=bcrypt");
}
