#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// Guest console output: logcat on device, stdout on the host harness.
void guestLog(const std::string& text);
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
    const GuestHeap& guestHeap() const { return heap; }
    bool guestExitRequested() const { return !terminatedBy.empty(); }
    const std::string& exitRoutine() const { return terminatedBy; }
    uint32_t exitStatus() const { return exitStatusValue; }

   private:
    Memory& memory;
    GuestHeap heap;
    std::function<void(const std::string&)> logSink;
    uint32_t randomState = 1;
    uint32_t strtokCursor = 0;
    std::string terminatedBy;
    uint32_t exitStatusValue = 0;
    uint32_t errnoCell = 0;
    uint32_t errorBuffer = 0, errorBufferSize = 0;
    uint32_t tmCell = 0;
    uint32_t lconvCell = 0;
    // setjmp/longjmp: the guest buffer keeps a magic word and a handle to a host-side record,
    // so nothing about the host's register file is guessed at or written into guest memory.
    struct JumpRecord {
        uint32_t registers[8];  // r4-r11, the callee-saved set
        uint32_t stack = 0, resume = 0, cpsr = 0;
    };
    std::map<uint32_t, JumpRecord> jumpRecords;
    uint32_t nextJumpToken = 1;
    std::map<std::string, uint32_t> reservedStrings;
    std::map<uint32_t, std::string> zoneText;
    std::map<std::string, std::string> environmentValues;
    std::map<std::string, uint32_t> environmentCells;
    std::string readString(uint32_t address, size_t limit = 1024 * 1024) const;
    void writeString(uint32_t address, const std::string& text, size_t capacity);
    bool math(CpuState& cpu, Memory& memory, const std::string& name);
    bool numbers(const std::string& name, CpuState& cpu);
    void formatted(const std::string& name, CpuState& cpu);
    void emit(const std::string& text) const;
    void zero(uint32_t address, size_t bytes);
    // A NUL-terminated copy of `text` in the guest heap, allocated once per distinct text.
    uint32_t reserveString(const std::string& text);
    // Darwin's struct tm and struct lconv, written field by field in the platform layout.
    uint32_t tmBuffer();
    void writeTm(uint32_t address, const std::tm& parts);
    void readTm(uint32_t address, std::tm& parts);
    uint32_t localeconvCell();
};
}  // namespace podnative::runtime
