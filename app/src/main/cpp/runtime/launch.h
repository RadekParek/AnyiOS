#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace podnative::runtime {
// Real, bounded loader attempt. Never jump into guest code with unresolved services.
std::string attemptLaunch(const uint8_t* data, size_t size);
}  // namespace podnative::runtime
