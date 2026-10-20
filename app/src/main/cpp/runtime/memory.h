#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace podnative::runtime {
enum Permission : uint8_t { Read = 1, Write = 2, Execute = 4 };
struct Region {
    uint32_t base;
    uint32_t size;
    uint8_t permissions;
    std::string label;
};
// Guest execute permission is metadata only. These are never executable host mappings.
// Single CPU-owner thread; graphics receives copied/validated bytes, never backing pointers.
class Memory final {
   public:
    static constexpr uint32_t PageSize = 4096;
    explicit Memory(size_t budget = 256 * 1024 * 1024) : budget_(budget) {}
    void map(Region region, const uint8_t* initial = nullptr, size_t initialSize = 0);
    void read(uint32_t address, void* destination, size_t size) const;
    void write(uint32_t address, const void* source, size_t size);
    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    uint32_t fetch32(uint32_t address) const;
    uint16_t fetch16(uint32_t address) const;
    bool permits(uint32_t address, size_t size, uint8_t permission) const;
    std::vector<Region> regions() const;
    size_t allocated() const { return allocated_; }
    uint64_t revision() const { return revision_; }

   private:
    struct Storage {
        Region region;
        std::vector<uint8_t> bytes;
    };
    std::map<uint32_t, Storage> storage_;
    size_t budget_;
    size_t allocated_ = 0;
    uint64_t revision_ = 0;
    const Storage& at(uint32_t address, uint8_t permission) const;
    void validate(uint32_t address, size_t size, uint8_t permission) const;
    void copyOut(uint32_t address, void* destination, size_t size, uint8_t permission) const;
};
}  // namespace podnative::runtime
