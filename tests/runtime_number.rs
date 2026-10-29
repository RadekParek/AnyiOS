// Exercise the actual pinned-engine NSNumber comparison/hash representation.
// This small host harness supplies only the object marker trait; it is not a
// replacement for guest Objective-C or full emulator integration tests.
#[allow(dead_code)]
mod objc {
    pub trait HostObject: Default + 'static {}
}

#[allow(dead_code)]
#[path = "../third_party/anyiose-engine/src/frameworks/foundation/ns_value/number.rs"]
mod number;
