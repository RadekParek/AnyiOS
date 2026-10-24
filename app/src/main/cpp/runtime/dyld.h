#pragma once
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "exports.h"
#include "memory.h"
namespace podnative::runtime {
constexpr size_t DyldFixupBudget = 1000000;
struct DyldSegment {
    uint32_t address, size;
    uint8_t permissions;
    // Final initprot and dyld-time maxprot are distinct for segments such as __DATA_CONST.
    uint8_t maxPermissions = 0;
};
struct DyldFixup {
    uint32_t address;
    bool rebase = false, weak = false, lazy = false;
    int ordinal = 0;
    int64_t addend = 0;
    std::string symbol;
};
struct DyldPlan {
    std::vector<DyldFixup> fixups;
    bool needsCoalescing = false;
    std::vector<std::string> strongDefinitions;
    std::string summary() const;
    // Optional trap hook binds required imports with no implementation to a faulting
    // gate, so execution can start and stop at the exact missing API. Weak imports
    // still bind to null, matching C semantics.
    void applyWithGraph(Memory& memory, uint32_t slide, const ExportGraph& graph, size_t requester,
                        const std::function<uint32_t(const std::string&)>& trap = {}) const;
    // All resolutions/ranges are checked before writes. No unresolved required import is zeroed.
    void apply(
        Memory& memory, uint32_t slide,
        const std::function<std::optional<uint32_t>(int, const std::string&, bool)>& resolve) const;
};
DyldPlan decodeDyld(const uint8_t* bytes, size_t size, const std::vector<DyldSegment>& segments,
                    bool rebase, bool lazy = false, bool weakStream = false);
}  // namespace podnative::runtime
