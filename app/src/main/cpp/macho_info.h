#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace podnative {
struct ImportedSymbol {
    std::string name;
    bool weak = false;
    // Mach-O's n_desc stores the two-level library ordinal in its high byte. Special
    // executable/dynamic-lookup ordinals are normalized to the ExportGraph convention.
    int libraryOrdinal = 0;
};
struct MachORequirements {
    std::vector<ImportedSymbol> imports;
    std::vector<std::string> libraries;
    bool fixedFunctionImports = false;
    bool programmableImports = false;
    bool compressedTextureImports = false;
    std::string summary() const;
    // Optional dyld-resolution predicate used by the in-app report to hide imports the
    // runtime can resolve for this exact library ordinal. The default preserves a complete
    // static evidence dump.
    std::string summary(const std::function<bool(const ImportedSymbol&)>& hasRuntimeBinding) const;
};
// Static requirements evidence, not a claim about which paths actually execute.
MachORequirements inspectRequirements(const uint8_t* data, size_t size);
}  // namespace podnative
