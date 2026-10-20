#include "macho_info.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

#include "macho.h"
#include "runtime/error.h"
namespace podnative {
namespace {
using runtime::Error;
std::string printable(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c >= 32 && c < 127)
            out << char(c);
        else
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    return out.str();
}
}  // namespace
MachORequirements inspectRequirements(const uint8_t* data, size_t size) {
    if (size && !data) throw Error("IMPORT_ARGUMENT", "null executable data");
    const std::string base = inspectMachO(data, size);
    if (base.rfind("INSPECTED_ARM32:", 0) != 0) throw Error("IMPORT_FORMAT", base);
    auto bounds = [size](uint64_t p, uint64_t n) {
        if (p > size || n > size - p)
            throw Error("IMPORT_BOUNDS", "symbol or string range exceeds executable");
    };
    auto u32 = [&](size_t p) {
        bounds(p, 4);
        return uint32_t(data[p]) | uint32_t(data[p + 1]) << 8 | uint32_t(data[p + 2]) << 16 |
               uint32_t(data[p + 3]) << 24;
    };
    auto name = [&](uint64_t p, uint64_t end) {
        if (p >= end) throw Error("IMPORT_STRING", "invalid string-table offset");
        bounds(p, end - p);
        size_t count = 0;
        while (p + count < end && data[p + count] && count < 4096) count++;
        if (p + count == end || count == 4096)
            throw Error("IMPORT_STRING", "unterminated or oversized symbol/library name");
        return std::string(reinterpret_cast<const char*>(data + p), count);
    };
    MachORequirements result;
    bool sawSymbols = false;
    std::map<std::string, size_t> seen;
    size_t nameBytes = 0;
    size_t p = 28;
    for (uint32_t i = 0; i < u32(16); i++) {
        const uint32_t cmd = u32(p), length = u32(p + 4);
        if (cmd == 2) {
            if (length < 24 || sawSymbols)
                throw Error("IMPORT_SYMTAB", "truncated or duplicate symbol table");
            sawSymbols = true;
            const uint32_t offset = u32(p + 8), count = u32(p + 12), strings = u32(p + 16),
                           stringSize = u32(p + 20);
            bounds(offset, uint64_t(count) * 12);
            bounds(strings, stringSize);
            if (count > 250000)
                throw Error("IMPORT_LIMIT", "symbol count exceeds 250,000 analysis limit");
            for (uint32_t s = 0; s < count; s++) {
                const size_t q = size_t(offset) + size_t(s) * 12;
                const uint8_t type = data[q + 4];
                if ((type & 0xe0) || (type & 0x0e) != 0 || !(type & 1))
                    continue;  // STAB, defined, local.
                const uint32_t index = u32(q);
                if (index >= stringSize)
                    throw Error("IMPORT_STRING",
                                "undefined symbol name index exceeds string table");
                std::string symbol =
                    name(uint64_t(strings) + index, uint64_t(strings) + stringSize);
                if (symbol.empty()) throw Error("IMPORT_STRING", "empty undefined symbol name");
                const uint16_t description = uint16_t(data[q + 6]) | uint16_t(data[q + 7]) << 8;
                const auto existing = seen.find(symbol);
                if (existing != seen.end()) {
                    result.imports[existing->second].weak &= bool(description & 0x40);
                    continue;
                }
                if (result.imports.size() >= 4096 || symbol.size() > 1024 * 1024 - nameBytes)
                    throw Error("IMPORT_LIMIT", "import-name analysis budget exceeded");
                nameBytes += symbol.size();
                seen.emplace(symbol, result.imports.size());
                result.imports.push_back({symbol, bool(description & 0x40)});
                if (symbol == "_glMatrixMode" || symbol == "_glVertexPointer" ||
                    symbol == "_glTexEnvf" || symbol == "_glOrthof" || symbol == "_glLightfv")
                    result.fixedFunctionImports = true;
                if (symbol == "_glCreateShader" || symbol == "_glShaderSource" ||
                    symbol == "_glUseProgram")
                    result.programmableImports = true;
                if (symbol == "_glCompressedTexImage2D" || symbol == "_glCompressedTexSubImage2D")
                    result.compressedTextureImports = true;
            }
        } else if (cmd == 0xc || cmd == 0x80000018 || cmd == 0x8000001f || cmd == 0x80000023 ||
                   cmd == 0x20) {
            if (length < 24) throw Error("IMPORT_DYLIB", "truncated library command");
            const uint32_t offset = u32(p + 8);
            if (offset < 24 || offset >= length)
                throw Error("IMPORT_DYLIB", "invalid library name offset");
            auto library = name(p + offset, p + length);
            if (result.libraries.size() >= 1024 || library.size() > 1024 * 1024 - nameBytes)
                throw Error("IMPORT_LIMIT", "library-name analysis budget exceeded");
            nameBytes += library.size();
            result.libraries.push_back(std::move(library));
        }
        p += length;
    }
    std::sort(result.imports.begin(), result.imports.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}
std::string MachORequirements::summary() const {
    std::ostringstream out;
    out << "STATIC_IMPORT_EVIDENCE: " << imports.size() << " unique undefined symbols, "
        << libraries.size() << " libraries. Not a runtime trace.";
    out << "\nAbsence of an import is not proof of API absence; dyld bind streams are not analyzed.";
    out << "\nFixed-function GLES imports=" << fixedFunctionImports
        << " programmable GLES imports=" << programmableImports;
    out << "\nCompressed texture API imports=" << compressedTextureImports
        << "; actual formats (including PVRTC) unknown.";
    constexpr size_t Limit = 60;
    for (size_t i = 0; i < std::min(libraries.size(), Limit); i++)
        out << "\nlibrary " << printable(libraries[i]);
    for (size_t i = 0; i < std::min(imports.size(), Limit); i++)
        out << "\n" << (imports[i].weak ? "weak " : "required ") << printable(imports[i].name);
    if (imports.size() > Limit || libraries.size() > Limit)
        out << "\nDisplay limited to 60 names per category.";
    return out.str();
}
}  // namespace podnative
