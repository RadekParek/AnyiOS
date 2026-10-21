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
    std::string stop = "NOT_STARTED";  // EXITED, BUDGET, FAULT, UNIMPLEMENTED, TRAPPED
    std::string detail;
    size_t serviceCalls = 0;
    size_t trappedImports = 0;
    std::vector<std::string> called;
};
// Bounded real execution: guest instructions run until the budget, wall clock,
// a fault, or the first import that has no implementation.
GuestRun runGuest(LoadedImage& image, Services& services, const ExportGraph& graph,
                  size_t requester, uint64_t instructionBudget, double seconds);
std::string describeRun(const GuestRun& run);
}  // namespace podnative::runtime
