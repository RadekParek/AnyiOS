// Run the real Core Animation deadline bookkeeping with Rust's monotonic clock.
// This does not benchmark presentation, drivers, or physical-device frame times.
#[path = "../third_party/anyiose-engine/src/frameworks/core_animation/composition_pacing.rs"]
mod composition_pacing;
