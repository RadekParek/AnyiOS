#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace podnative {
// Inspection only: never maps or executes untrusted executable bytes.
std::string inspectMachO(const uint8_t* bytes, size_t size);
}  // namespace podnative
