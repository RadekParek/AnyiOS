#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// Genuine C runtime subset for guest apps. Every buffer access goes through Memory, so
// a bogus pointer faults instead of touching host memory. Unimplemented names return false.
class Libc final {
   public:
    explicit Libc(Memory& memory)
        : memory(memory), heap(memory), logSink([](const std::string&) {}) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    // Guest printf/puts output: logcat on device, a host sink in tests.
    void setLogSink(std::function<void(const std::string&)> sink);
    // Variables visible to guest getenv. Values are copied into the guest heap on first
    // use, so the returned pointer stays valid for the life of the process.
    void setEnvironment(const std::map<std::string, std::string>& values);
    GuestHeap& guestHeap() { return heap; }

   private:
    Memory& memory;
    GuestHeap heap;
    std::function<void(const std::string&)> logSink;
    uint32_t randomState = 1;
    std::map<std::string, std::string> environmentValues;
    std::map<std::string, uint32_t> environmentCells;
    std::string readString(uint32_t address, size_t limit = 1024 * 1024) const;
    void writeString(uint32_t address, const std::string& text, size_t capacity);
    bool math(CpuState& cpu, Memory& memory, const std::string& name);
    bool numbers(const std::string& name, CpuState& cpu);
    void formatted(const std::string& name, CpuState& cpu);
    void emit(const std::string& text) const;
    void zero(uint32_t address, size_t bytes);
};
}  // namespace podnative::runtime
