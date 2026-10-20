#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace podnative {
struct ImportedSymbol {
    std::string name;
    bool weak;
};
struct MachORequirements {
    std::vector<ImportedSymbol> imports;
    std::vector<std::string> libraries;
    bool fixedFunctionImports = false;
    bool programmableImports = false;
    bool compressedTextureImports = false;
    std::string summary() const;
};
// Static requirements evidence, not a claim about which paths actually execute.
MachORequirements inspectRequirements(const uint8_t* data, size_t size);
}  // namespace podnative
