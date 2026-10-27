// Compile the actual Android JNI export for the host-only JVM bridge test.
#![allow(dead_code)]
#[path = "../../third_party/anyiose-engine/src/android_network.rs"]
mod android_network;

#[no_mangle]
pub extern "system" fn Java_org_anyiose_android_NetworkMonitorHarness_nativeGetRoute(
    _env: *mut std::ffi::c_void,
    _class: *mut std::ffi::c_void,
) -> i32 {
    android_network::route()
}
