#include "heap.h"

#include <algorithm>

#include "error.h"

namespace podnative::runtime {
GuestHeap::GuestHeap(Memory& m, uint32_t b, uint32_t s) : memory(m), base(b), size(s) {
    if (!s || s % Memory::PageSize || uint64_t(b) + s > 0x100000000ULL)
        throw Error("HEAP_CONFIG", "invalid guest heap range");
}
bool GuestHeap::grow() noexcept {
    if (committedSize >= size) return false;
    const uint32_t chunk = std::min(GrowStep, size - committedSize);
    try {
        memory.map({base + committedSize, chunk, uint8_t(Read | Write), "guest heap"});
    } catch (const Error&) {
        return false;
    }
    const uint32_t start = base + committedSize;
    committedSize += chunk;
    // Merge the new chunk with a free block that ends exactly where it starts, so the heap
    // keeps its single contiguous free tail instead of one block per growth step.
    auto next = freeBlocks.lower_bound(start);
    uint32_t length = chunk;
    if (next != freeBlocks.end() && uint64_t(start) + chunk == next->first) {
        length += next->second;
        next = freeBlocks.erase(next);
    }
    if (next != freeBlocks.begin()) {
        auto previous = std::prev(next);
        if (uint64_t(previous->first) + previous->second == start) {
            previous->second += length;
            return true;
        }
    }
    freeBlocks.emplace(start, length);
    return true;
}
bool GuestHeap::ensureMapped() noexcept {
    if (mapped) return true;
    if (!grow()) return false;
    mapped = true;
    return true;
}
uint32_t GuestHeap::allocate(uint32_t bytes, uint32_t alignment) noexcept {
    if (!bytes || alignment < 4 || alignment > 4096 || (alignment & (alignment - 1))) return 0;
    if (!ensureMapped()) return 0;
    for (auto block = freeBlocks.begin(); block != freeBlocks.end(); ++block) {
        const uint32_t freeStart = block->first, freeSize = block->second;
        const uint64_t start = (uint64_t(freeStart) + alignment - 1) & ~uint64_t(alignment - 1);
        const uint64_t end = start + bytes;
        if (end > uint64_t(freeStart) + freeSize) continue;
        const uint32_t padding = uint32_t(start - freeStart);
        const uint32_t remaining = freeSize - padding;
        freeBlocks.erase(block);
        if (padding) freeBlocks.emplace(freeStart, padding);
        if (remaining > bytes) freeBlocks.emplace(uint32_t(end), remaining - bytes);
        live.emplace(uint32_t(start), bytes);
        return uint32_t(start);
    }
    // The remaining free space is fragmented or too small: map more of the address space and
    // retry. This is how a real heap grows, and it keeps the host allocation proportional to
    // what the title actually uses.
    if (grow()) return allocate(bytes, alignment);
    return 0;
}
uint32_t GuestHeap::used() const {
    uint32_t total = 0;
    for (const auto& entry : live) total += entry.second;
    return total;
}
void GuestHeap::release(uint32_t pointer) {
    auto found = live.find(pointer);
    if (found == live.end()) throw Error("HEAP_FREE", "pointer is not a live allocation", pointer);
    uint32_t start = found->first, length = found->second;
    live.erase(found);
    auto next = freeBlocks.lower_bound(start);
    if (next != freeBlocks.end() && uint64_t(start) + length == next->first) {
        length += next->second;
        next = freeBlocks.erase(next);
    }
    if (next != freeBlocks.begin()) {
        auto previous = std::prev(next);
        if (uint64_t(previous->first) + previous->second == start) {
            start = previous->first;
            length += previous->second;
            freeBlocks.erase(previous);
        }
    }
    freeBlocks.emplace(start, length);
}
uint32_t GuestHeap::resize(uint32_t pointer, uint32_t bytes) {
    auto found = live.find(pointer);
    if (found == live.end())
        throw Error("HEAP_REALLOC", "pointer is not a live allocation", pointer);
    if (!bytes) {
        release(pointer);
        return 0;
    }
    if (bytes <= found->second) {
        // A shrinking realloc may keep or move the block; keeping it is valid C.
        if (bytes <= found->second / 2 + found->second / 4) {
            const uint32_t split = pointer + bytes;
            const uint32_t extra = found->second - bytes;
            live[pointer] = bytes;
            auto next = freeBlocks.lower_bound(split);
            if (next != freeBlocks.end() && uint64_t(split) + extra == next->first) {
                uint32_t merged = next->second;
                freeBlocks.erase(next);
                freeBlocks.emplace(split, extra + merged);
            } else {
                freeBlocks.emplace(split, extra);
            }
        }
        return pointer;
    }
    uint32_t replacement = allocate(bytes);
    if (!replacement) return 0;
    std::vector<uint8_t> bytesCopy(found->second);
    memory.read(pointer, bytesCopy.data(), bytesCopy.size());
    memory.write(replacement, bytesCopy.data(), bytesCopy.size());
    release(pointer);
    return replacement;
}
}  // namespace podnative::runtime
