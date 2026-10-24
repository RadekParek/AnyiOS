#pragma once

#include <string>
#include <vector>

#include "cpu.h"

namespace podnative::runtime {
// A bounded, guest-memory-safe subset of Apple's CommonCrypto C API. The digest
// entry points implement their standard algorithms; CCCrypt currently implements
// AES in CBC/ECB mode with optional PKCS#7 padding. Unsupported algorithms return
// kCCUnimplemented rather than claiming to have transformed the input.
class CommonCrypto final {
   public:
    std::vector<std::string> symbols() const;
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory) const;
};
}  // namespace podnative::runtime
