#pragma once
#include "exports.h"
#include "frameworks.h"

namespace podnative::runtime {
// ARM code gates contain SVC + BX LR. Guest Thumb callers use BLX interworking.
// Dispatch authenticates both the trap PC and number; arbitrary guest SVCs are not APIs.
class Services final {
   public:
    explicit Services(Memory& memory);
    void install(ExportGraph& graph) const;
    void dispatch(const CpuStop& stop, CpuState& state);
    uint32_t address(const std::string& symbol) const;
    size_t count() const { return symbols.size(); }

   private:
    Memory& memory;
    CoreFoundation cf;
    std::vector<std::string> symbols;
    void invoke(const std::string& symbol, CpuState& state);
    static constexpr uint32_t Base = 0x6f000000;
};
}  // namespace podnative::runtime
