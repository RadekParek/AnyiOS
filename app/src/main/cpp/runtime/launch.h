#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace podnative::runtime {
// How long the guest is allowed to run, and how much of its console output the report
// keeps. The defaults are the bounded probe; a live session passes a real time budget.
struct LaunchOptions {
    double seconds = 2.5;
    uint64_t instructions = 2000000;
    // Guest printf/NSLog lines kept for the report. Negative disables capture.
    int logLines = 24;
};
// Real, bounded loader attempt. Never jump into guest code with unresolved services.
// `bundleHost` and `containerHost` are the host directories that back the guest sandbox
// (/var/mobile and /var/mobile/Documents); when they are empty the guest has no file
// access at all and every file call is refused by name.
std::string attemptLaunch(const uint8_t* data, size_t size, const std::string& bundleHost = {},
                          const std::string& containerHost = {}, const LaunchOptions& options = {});
}  // namespace podnative::runtime
