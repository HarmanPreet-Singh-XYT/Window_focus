cmake_minimum_required(VERSION 3.14)

# Project-level configuration.
set(PROJECT_NAME "window_focus")
project(${PROJECT_NAME} LANGUAGES NONE)

cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "window_focus_plugin")

# ---- Locate Cargo ----
find_program(CARGO_EXECUTABLE cargo
  HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin"
  REQUIRED
)

# ---- Map CMake build type to cargo profile ----
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(CARGO_PROFILE "--release")
  set(CARGO_PROFILE_DIR "release")
else()
  set(CARGO_PROFILE "")
  set(CARGO_PROFILE_DIR "debug")
endif()

# ---- Map CMake generator platform to Rust target triple ----
if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(CARGO_TARGET "aarch64-pc-windows-msvc")
else()
  set(CARGO_TARGET "x86_64-pc-windows-msvc")
endif()

set(RUST_CRATE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust")
set(RUST_DLL     "${RUST_CRATE_DIR}/target/${CARGO_TARGET}/${CARGO_PROFILE_DIR}/window_focus_plugin.dll")
set(RUST_IMPLIB  "${RUST_CRATE_DIR}/target/${CARGO_TARGET}/${CARGO_PROFILE_DIR}/window_focus_plugin.dll.lib")

# ---- Build step ----
add_custom_command(
  OUTPUT "${RUST_DLL}" "${RUST_IMPLIB}"
  COMMAND "${CARGO_EXECUTABLE}" build ${CARGO_PROFILE} --target ${CARGO_TARGET}
          --manifest-path "${RUST_CRATE_DIR}/Cargo.toml"
  WORKING_DIRECTORY "${RUST_CRATE_DIR}"
  COMMENT "Building Rust ${PLUGIN_NAME} (${CARGO_PROFILE_DIR})"
  VERBATIM
)

add_custom_target(window_focus_rust_build ALL
  DEPENDS "${RUST_DLL}" "${RUST_IMPLIB}"
)

# ---- Plugin library target (IMPORTED so Flutter's CMake can link against it) ----
add_library(${PLUGIN_NAME} SHARED IMPORTED GLOBAL)
set_target_properties(${PLUGIN_NAME} PROPERTIES
  IMPORTED_LOCATION "${RUST_DLL}"
  IMPORTED_IMPLIB   "${RUST_IMPLIB}"
)
add_dependencies(${PLUGIN_NAME} window_focus_rust_build)

# Expose the include directory containing the C API declaration the Rust
# DLL exports, so the generated plugin registrant can call it.
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ---- Bundle the DLL alongside the application ----
set(window_focus_bundled_libraries
  "${RUST_DLL}"
  PARENT_SCOPE
)

# === Tests ===
# Disabled: the C++ test runner has been removed together with the C++ sources.
# Re-enable when a Rust-native test harness is added.
