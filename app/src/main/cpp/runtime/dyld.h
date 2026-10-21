#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "exports.h"
#include "memory.h"
namespace podnative::runtime {
struct DyldSegment {
    uint32_t address, size;
    uint8_t permissions;
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
    void applyWithGraph(Memory& memory, uint32_t slide, const ExportGraph& graph,
                        size_t requester) const;
    // All resolutions/ranges are checked before writes. No unresolved required import is zeroed.
    void apply(
        Memory& memory, uint32_t slide,
        const std::function<std::optional<uint32_t>(int, const std::string&)>& resolve) const;
};
DyldPlan decodeDyld(const uint8_t* bytes, size_t size, const std::vector<DyldSegment>& segments,
                    bool rebase, bool lazy = false, bool weakStream = false);
}  // namespace podnative::runtime
