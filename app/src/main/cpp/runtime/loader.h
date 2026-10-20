#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "memory.h"
namespace podnative::runtime {
struct LoadedImage {
    std::unique_ptr<Memory> memory;
    std::array<uint32_t, 16> registers{};
    uint32_t cpsr = 0x10;  // ARM user mode; Thumb flag derives from the validated entry.
    std::vector<std::string> dependencies;
    bool requiresDyld = false;
    std::string summary() const;
};
// Fixed-address, non-PIE mapping only. This does not execute code or resolve imports.
LoadedImage loadMachO(const uint8_t* data, size_t size, size_t budget = 256 * 1024 * 1024);
}  // namespace podnative::runtime
