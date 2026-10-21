#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "cpu.h"
#include "frameworks.h"

namespace podnative::runtime {
// Session-owned CoreGraphics subset. Geometry is computed with the shared float
// structs in frameworks.h; colour spaces are opaque guest handles with reference
// counts, never host pointers. Anything outside this list stays unimplemented and
// reaches the guest as a hard trap.
class CoreGraphics final {
   public:
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory);
    size_t liveColorSpaces() const { return colorSpaces.size(); }

   private:
    std::map<uint32_t, uint32_t> colorSpaces;  // handle -> reference count
    uint32_t next = 0x6f500000;
};
}  // namespace podnative::runtime
