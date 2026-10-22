#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// C++ and compiler-runtime support that an iOS ARMv7 image links against:
// libstdc++.6.dylib (operator new/delete, the Itanium C++ ABI entry points) and
// libgcc_s.1.dylib (the integer/64-bit helper routines the ARM backend emits).
//
// Every entry point either performs the operation for real or raises a named guest
// failure. Nothing here reports success for work it did not do: exception unwinding,
// for example, is refused with CXX_UNWIND because SjLj registration is not emulated.
class CxxRuntime final {
   public:
    CxxRuntime(Memory& memory, GuestHeap& heap)
        : memory(memory), heap(heap), logSink([](const std::string&) {}) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    void setLogSink(std::function<void(const std::string&)> sink);
    // Registered __cxa_atexit/_atexit handlers, run once at guest exit.
    size_t atExitCount() const { return atExit.size(); }
    size_t runAtExit();
    bool isCxxSymbol(const std::string& symbol) const;
    bool isCompilerHelper(const std::string& symbol) const;

   private:
    Memory& memory;
    GuestHeap& heap;
    std::function<void(const std::string&)> logSink;
    struct ExitHandler {
        uint32_t function, argument;
    };
    std::vector<ExitHandler> atExit;
    // Stack of live `__Unwind_SjLj_Register` frames (innermost last).
    std::vector<uint32_t> sjljFrames;
    // Head of the guest's SjLj frame chain (each frame stores its `prev` in guest memory).
    uint32_t sjljHead = 0;
    uint32_t errnoCell = 0;
    uint32_t allocate(uint32_t bytes, const char* what);
    uint32_t errnoAddress();
    void zero(uint32_t address, size_t bytes);
    std::string readString(uint32_t address) const;
    void emit(const std::string& text) const;
    bool compilerHelper(const std::string& name, CpuState& cpu);
    bool fortified(const std::string& name, CpuState& cpu);
    bool abi(const std::string& name, CpuState& cpu);
};
}  // namespace podnative::runtime
