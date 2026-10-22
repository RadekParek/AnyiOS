#include "memory.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

#include "error.h"
namespace podnative::runtime {
namespace {
uint64_t endOf(uint32_t base, size_t size) {
    if (size > uint64_t(0x100000000ULL) - base)
        throw Error("MEMORY_OVERFLOW", "range exceeds ARM32 address space", base);
    return uint64_t(base) + size;
}
}  // namespace
void Memory::map(Region region, const uint8_t* initial, size_t initialSize) {
    const uint64_t end = endOf(region.base, region.size);
    if (!region.size || region.base % PageSize || region.size % PageSize)
        throw Error("MEMORY_ALIGNMENT", "mapping must contain aligned, whole pages", region.base);
    if ((region.permissions & ~uint8_t(7)) || !region.permissions)
        throw Error("MEMORY_PERMISSION", "invalid mapping permissions", region.base);
    if (initialSize > region.size || (initialSize && !initial))
        throw Error("MEMORY_INITIAL_DATA", "invalid initialization range", region.base);
    auto next = storage_.lower_bound(region.base);
    if ((next != storage_.end() && next->first < end) ||
        (next != storage_.begin() &&
         uint64_t(std::prev(next)->first) + std::prev(next)->second.region.size > region.base))
        throw Error("MEMORY_OVERLAP", "mapping overlaps an existing region", region.base);
    if (region.size > budget_ - allocated_ || storage_.size() >= 1024)
        throw Error("MEMORY_BUDGET", "guest allocation/region budget exceeded", region.base);
    Storage storage{std::move(region), std::vector<uint8_t>(region.size, 0)};
    if (initialSize) std::memcpy(storage.bytes.data(), initial, initialSize);
    const auto size = storage.region.size;
    storage_.emplace(storage.region.base, std::move(storage));
    allocated_ += size;
    ++revision_;
}
const Memory::Storage& Memory::at(uint32_t address, uint8_t permission) const {
    // A null access is reported as such: the guest dereferenced a pointer that a missing
    // object or a nil message result left at zero. Naming it removes the guesswork a plain
    // "unmapped at 0x0" leaves behind.
    auto unmapped = [&](uint32_t fault) -> const Storage& {
        if (fault < PageSize)
            throw Error("MEMORY_NULL",
                        "null pointer dereference: the guest read or wrote address " +
                            std::to_string(fault) +
                            ", which no object, allocation or mapped image region owns",
                        fault);
        throw Error("MEMORY_UNMAPPED", "unmapped guest access", fault);
    };
    auto it = storage_.upper_bound(address);
    if (it == storage_.begin()) unmapped(address);
    --it;
    if (uint64_t(address) >= uint64_t(it->first) + it->second.region.size) unmapped(address);
    if ((it->second.region.permissions & permission) != permission)
        throw Error("MEMORY_ACCESS", "guest permission violation", address);
    return it->second;
}
void Memory::validate(uint32_t address, size_t size, uint8_t permission) const {
    const uint64_t end = endOf(address, size);
    uint64_t cursor = address;
    while (cursor < end) {
        const auto& s = at(uint32_t(cursor), permission);
        cursor = std::min(end, uint64_t(s.region.base) + s.region.size);
    }
}
bool Memory::permits(uint32_t address, size_t size, uint8_t permission) const {
    try {
        validate(address, size, permission);
        return true;
    } catch (const Error&) {
        return false;
    }
}
void Memory::copyOut(uint32_t address, void* destination, size_t size, uint8_t permission) const {
    if (size && !destination) throw Error("MEMORY_ARGUMENT", "null destination", address);
    validate(address, size, permission);
    auto* out = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        const auto& s = at(current, permission);
        const size_t offset = current - s.region.base;
        const size_t count = std::min(size - done, s.bytes.size() - offset);
        std::memcpy(out + done, s.bytes.data() + offset, count);
        done += count;
    }
}
void Memory::read(uint32_t address, void* destination, size_t size) const {
    copyOut(address, destination, size, Read);
}
void Memory::write(uint32_t address, const void* source, size_t size) {
    if (size && !source) throw Error("MEMORY_ARGUMENT", "null source", address);
    // Prevalidate every byte range before mutating; faults must not partially update a guest call.
    validate(address, size, Write);
    const auto* input = static_cast<const uint8_t*>(source);
    size_t done = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        auto it = std::prev(storage_.upper_bound(current));
        const size_t offset = current - it->first;
        const size_t count = std::min(size - done, it->second.bytes.size() - offset);
        std::memcpy(it->second.bytes.data() + offset, input + done, count);
        done += count;
    }
    if (size) ++revision_;
}
void Memory::patch(uint32_t address, const void* source, size_t size) {
    if (size && !source) throw Error("MEMORY_ARGUMENT", "null patch source", address);
    validate(address, size, Read);
    const auto* input = static_cast<const uint8_t*>(source);
    size_t done = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        auto it = std::prev(storage_.upper_bound(current));
        const size_t offset = current - it->first;
        const size_t count = std::min(size - done, it->second.bytes.size() - offset);
        std::memcpy(it->second.bytes.data() + offset, input + done, count);
        done += count;
    }
    if (size) ++revision_;
}
void Memory::patch32(uint32_t address, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                             uint8_t(value >> 24)};
    patch(address, bytes, 4);
}
uint32_t Memory::read32(uint32_t address) const {
    uint8_t b[4];
    read(address, b, 4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
void Memory::write32(uint32_t address, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                             uint8_t(value >> 24)};
    write(address, bytes, 4);
}
uint32_t Memory::fetch32(uint32_t address) const {
    if (address % 4) throw Error("CPU_ALIGNMENT", "ARM instruction is not word-aligned", address);
    uint8_t b[4];
    copyOut(address, b, 4, Execute);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
uint16_t Memory::fetch16(uint32_t address) const {
    if (address % 2)
        throw Error("CPU_ALIGNMENT", "Thumb instruction is not halfword-aligned", address);
    uint8_t b[2];
    copyOut(address, b, 2, Execute);
    return uint16_t(b[0]) | uint16_t(b[1]) << 8;
}
std::vector<Region> Memory::regions() const {
    std::vector<Region> result;
    for (const auto& pair : storage_) result.push_back(pair.second.region);
    return result;
}
}  // namespace podnative::runtime
