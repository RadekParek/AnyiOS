#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace podnative::runtime {
// Real, bounded loader attempt. Never jump into guest code with unresolved services.
// `bundleHost` and `containerHost` are the host directories that back the guest sandbox
// (/var/mobile and /var/mobile/Documents); when they are empty the guest has no file
// access at all and every file call is refused by name.
std::string attemptLaunch(const uint8_t* data, size_t size, const std::string& bundleHost = {},
                          const std::string& containerHost = {});
}  // namespace podnative::runtime
