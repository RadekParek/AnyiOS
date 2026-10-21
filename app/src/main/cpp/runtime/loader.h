#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"
#include "dyld.h"
#include "memory.h"
namespace podnative::runtime {
struct LoadedImage {
    std::unique_ptr<Memory> memory;
    std::array<uint32_t, 16> registers{};
    uint32_t cpsr = 0x10;  // ARM user mode; Thumb flag derives from the validated entry.
    std::vector<std::string> dependencies;
    bool requiresDyld = false;
    DyldPlan dyld;
    size_t legacyStubs = 0;
    std::vector<std::pair<std::string, ExportDefinition>> exports;
    uint32_t cpuSubtype = 9;  // CPU_SUBTYPE_ARM_V7 unless the header says otherwise.
    IsaArch arch = IsaArch::ArmV7;
    std::string summary() const;
};
// Fixed-address mapping at the un-slid vmaddrs. PIE images are accepted because every
// rebase is applied against the export graph before execution; a slide is simply zero.
// This does not execute code or resolve imports.
LoadedImage loadMachO(const uint8_t* data, size_t size, size_t budget = 256 * 1024 * 1024);
}  // namespace podnative::runtime
