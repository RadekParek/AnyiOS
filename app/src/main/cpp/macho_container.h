#pragma once
#include <cstddef>
#include <cstdint>
namespace podnative {
struct MachOSlice {
    uint64_t offset;
    uint64_t size;
    uint32_t subtype;
    bool universal;
};
// Only the header/table need be resident; totalSize is the actual file length.
// Thin files and FAT/FAT64 in either byte order are accepted; only ARM32 is selected.
MachOSlice selectArm32Slice(const uint8_t* header, size_t headerSize, uint64_t totalSize);
}  // namespace podnative
