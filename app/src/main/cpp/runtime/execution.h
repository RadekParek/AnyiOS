#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
    // Human-readable blocker for the crash screen: kind, subject and next action.
    std::string blockerKind;     // Startup, Unimplemented API, Guest fault, Trap, Exit
    std::string blockerSubject;  // the exact symbol or fault code
};
// Bounded real execution: guest instructions run until the budget, wall clock,
// a fault, or the first import that has no implementation.
GuestRun runGuest(LoadedImage& image, Services& services, const ExportGraph& graph,
                  size_t requester, uint64_t instructionBudget, double seconds);
std::string describeRun(GuestRun& run);
}  // namespace podnative::runtime
