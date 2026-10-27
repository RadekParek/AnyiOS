// Host-only unit tests for the Android route-to-reachability policy. The JNI
// entry point and real network changes still require an Android device.
#[allow(dead_code)]
#[path = "../third_party/anyiose-engine/src/android_network.rs"]
mod android_network;
