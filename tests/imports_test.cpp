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
    put(b, 76, 17);
    b[80] = 1;
    b[82] = 0x40;
    put(b, 88, 31);
    b[92] = 1;
    auto result = podnative::inspectRequirements(b.data(), b.size());
    assert(result.imports.size() == 3 && result.fixedFunctionImports && result.programmableImports);
    assert(result.compressedTextureImports);
    assert(result.imports.back().name == "_glMatrixMode" && result.imports.back().weak);
    // A required duplicate must override a weak reference, not disappear behind it.
    put(b, 88, 17);
    b[94] = 0;
    auto duplicate = podnative::inspectRequirements(b.data(), b.size());
    assert(duplicate.imports.size() == 2 && !duplicate.imports.back().weak);
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
