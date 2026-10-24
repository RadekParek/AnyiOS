#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace podnative::runtime {
// How long the guest is allowed to run, and how much of its console output the report
// keeps. Both limits default to zero, which means "no limit": a title runs until it stops,
// faults or exits, exactly like an app on a device. A positive value is a caller's own
// probe budget and is honoured only because the caller asked for it.
struct LaunchOptions {
    double seconds = 0;         // 0 = run until the guest stops
    uint64_t instructions = 0;  // 0 = no instruction cap
    // Guest printf/NSLog lines kept for the report. Negative disables capture.
    int logLines = 24;
    // "Verbose logging": name every service the guest calls, every unwind decision and every
    // GL error. Off by default; it only ever adds log lines, never changes behaviour.
    bool verbose = false;
    // Exact program counters for the instruction trace. Kept separate from `verbose` because
    // single-stepping costs roughly ten times the execution rate: a verbose run that only
    // needs names and arguments should not pay for it.
    bool precise = false;
    // Settings ▸ "Native OpenGL ES": resolve GL against the device's own driver first and
    // keep the bundled ANGLE as the fallback. Off, the bundled ANGLE is the only driver tried
    // first, so GL behaves the same on every device rather than depending on the GPU vendor.
    // Either way an ES 1.1 title whose fixed-function entry points no loaded driver exports
    // is served by the bridge emulating them over ES 2.0.
    bool nativeGles = false;
    // UIKit's design-space bounds are supplied by the Android launcher; legacy landscape titles
    // generally expect 480x320 points even when the physical panel has a different resolution.
    int screenWidth = 320;
    int screenHeight = 480;
    // New IPA imports stay ZIP-backed. The runtime indexes the selected Payload/*.app and
    // caches verified files on first access; guest writes land in the private overlay.
    std::string bundleArchive, bundleArchivePrefix, bundleCache, bundleOverlay;
    // Optional host cancellation and rendered-frame delivery used by the Android game view. A
    // null callback keeps command-line/host diagnostics unchanged and avoids pixel readback.
    const std::atomic<bool>* cancelRequested = nullptr;
    std::function<void(int, int, const std::vector<uint8_t>&)> frameSink;
};
// Real, bounded loader attempt. Never jump into guest code with unresolved services.
// `bundleHost` and `containerHost` are the host directories that back the guest sandbox
// (/var/mobile and /var/mobile/Documents); when they are empty the guest has no file
// access at all and every file call is refused by name.
std::string attemptLaunch(const uint8_t* data, size_t size, const std::string& bundleHost = {},
                          const std::string& containerHost = {}, const LaunchOptions& options = {});
}  // namespace podnative::runtime
