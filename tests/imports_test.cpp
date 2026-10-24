#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "macho_info.h"
#include "runtime/error.h"
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, size_t p, uint32_t value) {
    for (size_t i = 0; i < 4; i++) b.at(p + i) = uint8_t(value >> (i * 8));
}
int main() {
    Bytes b(512);
    put(b, 0, 0xfeedface);
    put(b, 4, 12);
    put(b, 12, 2);
    put(b, 16, 1);
    put(b, 20, 24);
    put(b, 28, 2);
    put(b, 32, 24);
    put(b, 36, 64);
    put(b, 40, 3);
    put(b, 44, 128);
    put(b, 48, 128);
    const char names[] = "\0_glCreateShader\0_glMatrixMode\0_glCompressedTexImage2D\0";
    std::memcpy(b.data() + 128, names, sizeof(names));
    put(b, 64, 1);
    b[68] = 1;
    b[71] = 10;
    put(b, 76, 17);
    b[80] = 1;
    b[82] = 0x40;
    b[83] = 10;  // the Mach-O high n_desc byte carries a one-based library ordinal
    put(b, 88, 31);
    b[92] = 1;
    auto result = podnative::inspectRequirements(b.data(), b.size());
    assert(result.imports.size() == 3 && result.fixedFunctionImports && result.programmableImports);
    assert(result.compressedTextureImports);
    assert(result.imports.back().name == "_glMatrixMode" && result.imports.back().weak);
    assert(result.imports.back().libraryOrdinal == 10);
    const std::string filteredReport = result.summary([](const podnative::ImportedSymbol& symbol) {
        return symbol.libraryOrdinal == 10 &&
               (symbol.name == "_glCreateShader" || symbol.name == "_glMatrixMode");
    });
    assert(filteredReport.find("2 resolve to registered runtime exports and are omitted, 1 remain "
                               "unresolved") != std::string::npos);
    assert(filteredReport.find("IMPORT_REPORT_FILTER: nlist ordinals") != std::string::npos);
    assert(filteredReport.find("_glCreateShader") == std::string::npos);
    assert(filteredReport.find("_glMatrixMode") == std::string::npos);
    assert(filteredReport.find("_glCompressedTexImage2D") != std::string::npos);
    // A required duplicate must override a weak reference, not disappear behind it.
    put(b, 88, 17);
    b[94] = 0;
    b[95] = 10;
    auto duplicate = podnative::inspectRequirements(b.data(), b.size());
    assert(duplicate.imports.size() == 2 && !duplicate.imports.back().weak);
    put(b, 76, 1);
    b[83] = 11;
    auto sameNameDifferentOrdinal = podnative::inspectRequirements(b.data(), b.size());
    assert(std::count_if(sameNameDifferentOrdinal.imports.begin(),
                         sameNameDifferentOrdinal.imports.end(),
                         [](const auto& item) { return item.name == "_glCreateShader"; }) == 2);
    put(b, 76, 17);
    b[83] = 10;
    b[71] = 0xfe;
    auto flatLookup = podnative::inspectRequirements(b.data(), b.size());
    assert(flatLookup.imports.front().name == "_glCreateShader");
    assert(flatLookup.imports.back().name == "_glMatrixMode");
    auto shaderImport =
        std::find_if(flatLookup.imports.begin(), flatLookup.imports.end(),
                     [](const auto& item) { return item.name == "_glCreateShader"; });
    assert(shaderImport != flatLookup.imports.end() && shaderImport->libraryOrdinal == -2);
    b[71] = 0xff;
    auto executableLookup = podnative::inspectRequirements(b.data(), b.size());
    shaderImport = std::find_if(executableLookup.imports.begin(), executableLookup.imports.end(),
                                [](const auto& item) { return item.name == "_glCreateShader"; });
    assert(shaderImport != executableLookup.imports.end() && shaderImport->libraryOrdinal == -1);
    b[71] = 10;
    put(b, 88, 31);
    b[68] = 0xf;  // N_SECT defined symbol must not be an unresolved import.
    result = podnative::inspectRequirements(b.data(), b.size());
    assert(result.imports.size() == 2 && !result.programmableImports);
    put(b, 76, 128);
    bool caught = false;
    try {
        (void)podnative::inspectRequirements(b.data(), b.size());
    } catch (const podnative::runtime::Error& e) {
        caught = e.code == "IMPORT_STRING";
    }
    assert(caught);
    put(b, 76, 17);
    put(b, 48, 18);
    caught = false;
    try {
        (void)podnative::inspectRequirements(b.data(), b.size());
    } catch (const podnative::runtime::Error& e) {
        caught = e.code == "IMPORT_STRING";
    }
    assert(caught);
    std::cout << "Static import bounds, weak/defined symbols and GLES evidence tests passed\n";
}
