// Exercise the exact pinned-engine coordinate helpers without building the
// Android/SDL runtime. The real engine defines this same orientation enum.
#[derive(Copy, Clone)]
pub enum DeviceOrientation {
    Portrait,
    PortraitUpsideDown,
    LandscapeLeft,
    LandscapeRight,
}
#[path = "../third_party/anyiose-engine/src/window/geometry.rs"]
mod geometry;
