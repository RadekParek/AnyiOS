#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cpu.h"
#include "exports.h"
#include "loader.h"
#include "services.h"

namespace podnative::runtime {
struct GuestRun {
    uint64_t instructions = 0;
    uint32_t pc = 0;
    // Name of the guest function the PC landed in, when the image still carries symbols.
    std::string pcSymbol;
    std::string stop = "NOT_STARTED";  // EXITED, BUDGET, FAULT, UNIMPLEMENTED, TRAPPED
    std::string detail;
    size_t serviceCalls = 0;
    size_t trappedImports = 0;
    std::vector<std::string> called;
    // Raw return addresses behind the stop, symbolized below before the report is printed.
    std::vector<uint32_t> frames;
    // Guest call chain at the stop, innermost first, as symbolized names (or addresses).
    // Read from the frame-pointer chain, which is all an ARMv6 bin can offer without DWARF;
    // it is evidence of where the guest was, not a promise of a complete stack.
    std::vector<std::string> backtrace;
    // Periodic progress samples: instructions retired and the guest PC at that point. A run
    // with no time limit needs this, because "still running" alone says nothing about
    // whether the title is getting anywhere.
    struct Sample {
        uint64_t instructions;
        uint32_t pc;
        std::string lastCall;
    };
    std::vector<Sample> samples;
    // Human-readable blocker for the crash screen: kind, subject and next action.
    std::string blockerKind;     // Startup, Unimplemented API, Guest fault, Trap, Exit
    std::string blockerSubject;  // the exact symbol or fault code
    // What the CPU engine did to execute it: which guest-memory path translated code used
    // and how much of its translated code it had to throw away.
    CpuCounters engine;
};
// Real execution. With a zero instruction budget and zero seconds the guest runs until it
// stops by itself: a fault, an unimplemented import, or its own exit.
GuestRun runGuest(LoadedImage& image, Services& services, const ExportGraph& graph,
                  size_t requester, uint64_t instructionBudget, double seconds);
std::string describeRun(GuestRun& run);
}  // namespace podnative::runtime
