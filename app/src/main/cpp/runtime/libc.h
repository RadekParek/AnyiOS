#pragma once
#include <cstdint>
#include <string>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// Genuine C runtime subset for guest apps. Every buffer access goes through Memory, so
// a bogus pointer faults instead of touching host memory. Unimplemented names return false.
class Libc final {
   public:
    explicit Libc(Memory& memory) : memory(memory), heap(memory) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    GuestHeap& guestHeap() { return heap; }

   private:
    Memory& memory;
    GuestHeap heap;
    uint32_t randomState = 1;
    std::string readString(uint32_t address, size_t limit = 1024 * 1024) const;
    void writeString(uint32_t address, const std::string& text, size_t capacity);
    bool math(CpuState& cpu, const std::string& name);
    bool numbers(const std::string& name, CpuState& cpu);
    void zero(uint32_t address, size_t bytes);
};
}  // namespace podnative::runtime
