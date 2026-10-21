#pragma once
#include <cstdint>
#include <map>

#include "memory.h"

namespace podnative::runtime {
// Guest-visible heap inside the emulated address space. Metadata stays host-side so
// a guest buffer overrun cannot corrupt the allocator's bookkeeping.
class GuestHeap final {
   public:
    explicit GuestHeap(Memory& memory, uint32_t base = 0x40000000,
                       uint32_t size = 32 * 1024 * 1024);
    // Returns 0 on exhaustion, exactly like a failing guest malloc.
    uint32_t allocate(uint32_t bytes, uint32_t alignment = 8) noexcept;
    void release(uint32_t pointer);
    uint32_t resize(uint32_t pointer, uint32_t bytes);
    bool owns(uint32_t pointer) const { return live.count(pointer) != 0; }
    size_t blocks() const { return live.size(); }

   private:
    Memory& memory;
    const uint32_t base, size;
    bool mapped = false;
    std::map<uint32_t, uint32_t> freeBlocks, live;
    bool ensureMapped() noexcept;
};
}  // namespace podnative::runtime
