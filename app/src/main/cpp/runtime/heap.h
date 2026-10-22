#pragma once
#include <cstdint>
#include <map>

#include "memory.h"

namespace podnative::runtime {
// Guest-visible heap inside the emulated address space. Metadata stays host-side so
// a guest buffer overrun cannot corrupt the allocator's bookkeeping.
class GuestHeap final {
   public:
    // The guest sees a 512 MiB heap, but only the pages the title actually touches are
    // mapped: the address space is committed in GrowStep chunks on demand. 32 MiB ran Angry
    // Birds out of memory at 204M instructions and 128 MiB ran it out at 1.46G instructions
    // on a single 49 MB request, so the ceiling is the 32-bit window below the HLE gates
    // rather than a guess about what a 2010 title needs.
    explicit GuestHeap(Memory& memory, uint32_t base = 0x40000000,
                       uint32_t size = 512 * 1024 * 1024);
    // Returns 0 on exhaustion, exactly like a failing guest malloc.
    uint32_t allocate(uint32_t bytes, uint32_t alignment = 8) noexcept;
    void release(uint32_t pointer);
    uint32_t resize(uint32_t pointer, uint32_t bytes);
    bool owns(uint32_t pointer) const { return live.count(pointer) != 0; }
    size_t blocks() const { return live.size(); }
    // Bytes the guest currently holds, and the heap's total size, for the launch report.
    uint32_t used() const;
    // The largest contiguous run of free bytes. A heap can be mostly empty and still be
    // unable to answer one large request, and a report that shows only totals hides that.
    uint32_t largestFreeBlock() const;
    uint32_t capacity() const { return size; }
    // Bytes of the heap that are actually mapped in the guest address space.
    uint32_t committed() const { return committedSize; }
    // Requests at or above LargeRequestBytes, counted rather than logged. A title that asks
    // for hundreds of megabytes is either building a real pool or computing a size from
    // something this runtime told it, and the launch report should show which shape the run
    // had. Counted here rather than in the callers so malloc, realloc and operator new all
    // land in one figure, and counted once per guest request even when serving it has to map
    // more of the address space and retry.
    static constexpr uint32_t LargeRequestBytes = 32 * 1024 * 1024;
    uint32_t largeRequests() const { return largeRequestCount; }
    uint32_t largestRequest() const { return largestRequestBytes; }

   private:
    Memory& memory;
    const uint32_t base, size;
    static constexpr uint32_t GrowStep = 8 * 1024 * 1024;
    uint32_t committedSize = 0;
    bool mapped = false;
    uint32_t largeRequestCount = 0, largestRequestBytes = 0;
    // allocate() without the request accounting, so growing the heap and retrying does not
    // count one guest request once per growth step.
    uint32_t allocateFrom(uint32_t bytes, uint32_t alignment) noexcept;
    std::map<uint32_t, uint32_t> freeBlocks, live;
    bool ensureMapped() noexcept;
    // Maps the next GrowStep of the heap. Returns false when the whole range is mapped.
    bool grow() noexcept;
};
}  // namespace podnative::runtime
