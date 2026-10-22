#include "loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

#include "../macho.h"
#include "error.h"
namespace podnative::runtime {
namespace {
struct Reader {
    const uint8_t* bytes;
    size_t size;
    void range(uint64_t offset, uint64_t length) const {
        if (offset > size || length > size - offset)
            throw Error("LOAD_BOUNDS", "file range exceeds executable");
    }
    uint32_t u32(size_t p) const {
        range(p, 4);
        return uint32_t(bytes[p]) | uint32_t(bytes[p + 1]) << 8 | uint32_t(bytes[p + 2]) << 16 |
               uint32_t(bytes[p + 3]) << 24;
    }
    uint64_t u64(size_t p) const { return u32(p) | uint64_t(u32(p + 4)) << 32; }
    std::string fixed(size_t p, size_t n) const {
        range(p, n);
        size_t count = 0;
        while (count < n && bytes[p + count]) count++;
        return std::string(reinterpret_cast<const char*>(bytes + p), count);
    }
    std::string commandString(size_t p, uint32_t length, uint32_t minimum) const {
        const uint32_t offset = u32(p + 8);
        if (offset < minimum || offset >= length)
            throw Error("LOAD_STRING", "invalid command string offset");
        const std::string value = fixed(p + offset, length - offset);
        if (value.empty() || value.size() == length - offset)
            throw Error("LOAD_STRING", "missing terminated command string");
        return value;
    }
};
struct Segment {
    Region region;
    uint32_t offset;
    uint32_t fileSize;
};
}  // namespace
LoadedImage loadMachO(const uint8_t* data, size_t size, size_t budget) {
    if (size && !data) throw Error("LOAD_ARGUMENT", "null executable data");
    auto inspection = inspectMachO(data, size);
    if (inspection.rfind("INSPECTED_ARM32:", 0) != 0) throw Error("LOAD_FORMAT", inspection);
    Reader r{data, size};
    // PIE is accepted: the image is mapped at its own vmaddrs (slide 0) and every
    // rebase/bind is applied transactionally against the export graph before any
    // instruction runs, so an unslid PIE image is exactly what the fixups describe.
    std::vector<Segment> segments;
    LoadedImage result;
    result.cpuSubtype = r.u32(8) & 0xffffff;
    result.arch = result.cpuSubtype == 6 ? IsaArch::ArmV6 : IsaArch::ArmV7;
    result.memory = std::make_unique<Memory>(budget);
    bool hasEntry = false, entryIsOffset = false;
    uint64_t entry = 0;
    size_t p = 28;
    bool hasSymbols = false, hasDynamicSymbols = false;
    uint32_t symbolCount = 0, symbolOffset = 0, stringOffset = 0, stringBytes = 0;
    struct IndirectSection {
        uint32_t address, count, first, stride, fileOffset;
        bool lazy, stub;
    };
    std::vector<IndirectSection> indirectSections;
    // Sections whose contents describe guest Objective-C metadata; read after mapping.
    std::vector<std::pair<uint32_t, uint32_t>> classListSections;
    size_t dynamicSymbols = 0;
    bool hasDyldInfo = false;
    std::array<std::pair<uint32_t, uint32_t>, 4> dyldStreams{};
    for (uint32_t i = 0; i < r.u32(16); i++) {
        uint32_t cmd = r.u32(p), length = r.u32(p + 4);
        if (cmd == 1) {
            const uint32_t vm = r.u32(p + 24), vmSize = r.u32(p + 28), offset = r.u32(p + 32),
                           fileSize = r.u32(p + 36);
            const uint32_t maximum = r.u32(p + 40), protection = r.u32(p + 44),
                           sectionCount = r.u32(p + 48);
            if (!vmSize || vm % Memory::PageSize || vmSize % Memory::PageSize ||
                uint64_t(vm) + vmSize > 0x100000000ULL)
                throw Error("LOAD_SEGMENT_VM", "invalid or unaligned segment virtual range", vm);
            if (fileSize > vmSize || (maximum & ~7u) || (protection & ~maximum))
                throw Error("LOAD_SEGMENT_PROTECTION", "invalid file/virtual size or permissions",
                            vm);
            r.range(offset, fileSize);
            // Validate all virtual ranges, including unbacked guards, for overlap.
            for (const auto& old : segments) {
                if (uint64_t(vm) < uint64_t(old.region.base) + old.region.size &&
                    uint64_t(old.region.base) < uint64_t(vm) + vmSize)
                    throw Error("LOAD_SEGMENT_OVERLAP", "overlapping Mach-O segments", vm);
            }
            for (uint32_t s = 0; s < sectionCount; s++) {
                const size_t q = p + 56 + size_t(s) * 68;
                const uint32_t address = r.u32(q + 32), sectionSize = r.u32(q + 36),
                               sectionOffset = r.u32(q + 40);
                const uint32_t relocationOffset = r.u32(q + 48), relocationCount = r.u32(q + 52),
                               type = r.u32(q + 56) & 0xff;
                if (type == 0 && r.fixed(q, 16) == "__objc_classlist") {
                    if (sectionSize % 4) throw Error("LOAD_OBJECTIVE_C", "unaligned class list");
                    classListSections.emplace_back(address, sectionSize);
                }
                if (type == 6 || type == 7 || type == 8) {
                    uint32_t stride = type == 8 ? r.u32(q + 64) : 4;
                    if (!stride || sectionSize % stride || indirectSections.size() >= 4096)
                        throw Error("DYLD_INDIRECT_SECTION",
                                    "invalid indirect section stride or budget");
                    indirectSections.push_back({address, sectionSize / stride, r.u32(q + 60),
                                                stride, sectionOffset, type == 7, type == 8});
                }
                if (address < vm || uint64_t(address) + sectionSize > uint64_t(vm) + vmSize)
                    throw Error("LOAD_SECTION_VM", "section outside its segment", address);
                // S_ZEROFILL and S_GB_ZEROFILL have no file bytes. TLS is not implemented.
                if (type >= 0x11 && type <= 0x15)
                    throw Error("LOAD_TLS_UNSUPPORTED",
                                "thread-local sections require guest TLS semantics");
                if (type != 1 && type != 0xc) {
                    r.range(sectionOffset, sectionSize);
                    if (sectionOffset < offset ||
                        uint64_t(sectionOffset) + sectionSize > uint64_t(offset) + fileSize)
                        throw Error("LOAD_SECTION_FILE", "section outside segment file bytes",
                                    address);
                }
                r.range(relocationOffset, uint64_t(relocationCount) * 8);
                if (relocationCount)
                    throw Error("LOAD_RELOCATION_UNSUPPORTED",
                                "section relocations are not implemented", address);
            }
            if (!protection && fileSize)
                throw Error("LOAD_GUARD_DATA", "inaccessible segment has initialized data", vm);
            if (segments.size() >= 256) throw Error("LOAD_LIMIT", "too many segments");
            segments.push_back(
                {{vm, vmSize, uint8_t(protection), r.fixed(p + 8, 16)}, offset, fileSize});
        } else if (cmd == 0x80000028) {  // LC_MAIN: file-relative entryoff, not a guest pointer.
            if (length < 24 || hasEntry)
                throw Error("LOAD_ENTRY", "truncated or duplicate entry point");
            if (r.u64(p + 16))
                throw Error("LOAD_STACK_UNSUPPORTED", "custom LC_MAIN stack size unsupported");
            entry = r.u64(p + 8);
            hasEntry = true;
            entryIsOffset = true;
        } else if (cmd == 5) {  // LC_UNIXTHREAD / ARM_THREAD_STATE, 16 registers plus CPSR.
            if (length != 84 || r.u32(p + 8) != 1 || r.u32(p + 12) != 17 || hasEntry)
                throw Error("LOAD_THREAD_STATE", "expected a single ARM_THREAD_STATE entry");
            for (size_t reg = 0; reg < 16; reg++) result.registers[reg] = r.u32(p + 16 + reg * 4);
            uint32_t cpsr = r.u32(p + 80);
            if ((cpsr & 0x1f) != 0 && (cpsr & 0x1f) != 0x10)
                throw Error("LOAD_CPSR", "privileged initial CPU mode unsupported");
            result.cpsr = (cpsr & 0xf8000020u) | 0x10;
            if (result.registers[13])
                throw Error("LOAD_STACK_UNSUPPORTED",
                            "nonzero initial thread SP requires ABI-specific stack setup");
            entry = result.registers[15];
            hasEntry = true;
        } else if (cmd == 0xc || cmd == 0x80000018 || cmd == 0x8000001f || cmd == 0x80000023 ||
                   cmd == 0x20) {
            if (length < 24) throw Error("LOAD_DYLIB", "truncated dylib command");
            result.dependencies.push_back(r.commandString(p, length, 24));
            result.requiresDyld = true;
        } else if (cmd == 0xe) {
            if (length < 12) throw Error("LOAD_DYLINKER", "truncated dynamic linker command");
            (void)r.commandString(p, length, 12);
            result.requiresDyld = true;
        } else if (cmd == 0x22 || cmd == 0x80000022) {
            if (length != 48 || hasDyldInfo)
                throw Error("LOAD_DYLD_INFO", "invalid or duplicate dyld info");
            hasDyldInfo = true;
            for (size_t n = 0; n < 5; n++) {
                const uint32_t offset = r.u32(p + 8 + n * 8), bytes = r.u32(p + 12 + n * 8);
                r.range(offset, bytes);
                if (n < 4) dyldStreams[n] = {offset, bytes};
                if (bytes) result.requiresDyld = true;
            }
        } else if (cmd == 2) {
            if (length != 24 || hasSymbols)
                throw Error("LOAD_SYMTAB", "invalid or duplicate symbol table");
            hasSymbols = true;
            symbolCount = r.u32(p + 12);
            symbolOffset = r.u32(p + 8);
            stringOffset = r.u32(p + 16);
            stringBytes = r.u32(p + 20);
            r.range(r.u32(p + 8), uint64_t(r.u32(p + 12)) * 12);
            r.range(r.u32(p + 16), r.u32(p + 20));
            // Symbol binding is explicitly deferred; even local-only tables need audit.
            if (r.u32(p + 12)) result.requiresDyld = true;
        } else if (cmd == 0xb) {  // LC_DYSYMTAB; validated, never silently bound.
            if (length != 80 || hasDynamicSymbols)
                throw Error("LOAD_DYSYMTAB", "invalid or duplicate dynamic symbol table");
            hasDynamicSymbols = true;
            dynamicSymbols = p;
            constexpr uint32_t widths[] = {8, 52, 4, 4, 8, 8};
            for (size_t table = 0; table < 6; table++)
                r.range(r.u32(p + 32 + table * 8),
                        uint64_t(r.u32(p + 36 + table * 8)) * widths[table]);
            result.requiresDyld = true;
        } else if (cmd == 0x1d || cmd == 0x1e || cmd == 0x26 || cmd == 0x29 || cmd == 0x2b ||
                   cmd == 0x2e) {
            // Code signature, segment split info, function starts, data-in-code, dylib code
            // signing DRs and linker optimisation hints: all {dataoff, datasize} into the
            // file. They are validated and then ignored, because none of them relocates code.
            if (length < 16) throw Error("LOAD_LINKEDIT", "truncated linkedit command");
            r.range(r.u32(p + 8), r.u32(p + 12));
        } else if (cmd == 0x21) {  // LC_ENCRYPTION_INFO
            // An encrypted image cannot be loaded: the text is ciphertext until the kernel
            // decrypts it, and this runtime does not decrypt. Refuse by name instead of
            // executing garbage.
            if (length < 20) throw Error("LOAD_METADATA", "truncated encryption info");
            r.range(r.u32(p + 8), r.u32(p + 12));
            if (r.u32(p + 16))
                throw Error("LOAD_ENCRYPTED", "image text is encrypted (cryptid != 0)");
        } else if (cmd == 0x2c) {  // LC_ENCRYPTION_INFO_64
            if (length < 24) throw Error("LOAD_METADATA", "truncated encryption info");
            r.range(r.u32(p + 8), r.u32(p + 12));
            if (r.u32(p + 16))
                throw Error("LOAD_ENCRYPTED", "image text is encrypted (cryptid != 0)");
        } else if (cmd == 0x2d) {  // LC_LINKER_OPTION: count of NUL-terminated strings
            const uint32_t count = r.u32(p + 8);
            if (length < 12 || count > (length - 12) / 2 || count > 1024)
                throw Error("LOAD_METADATA", "invalid linker option count");
            uint64_t cursor = p + 12;
            for (uint32_t option = 0; option < count; option++) {
                if (cursor >= uint64_t(p) + length)
                    throw Error("LOAD_METADATA", "truncated option");
                while (cursor < uint64_t(p) + length && data[cursor]) cursor++;
                if (cursor >= uint64_t(p) + length)
                    throw Error("LOAD_METADATA", "unterminated linker option");
                cursor++;
            }
        } else if (cmd == 0x31) {  // LC_NOTE: 16-byte owner, then a 64-bit file range
            if (length < 40) throw Error("LOAD_METADATA", "truncated note command");
            r.range(r.u64(p + 24), r.u64(p + 32));
        } else if (cmd == 0x27 || cmd == 0x12 || cmd == 0x13 || cmd == 0x14 || cmd == 0x15 ||
                   cmd == 0x8000001c) {
            // DYLD_ENVIRONMENT, SUB_FRAMEWORK, SUB_UMBRELLA, SUB_CLIENT, SUB_LIBRARY, RPATH:
            // command strings with no effect on loading. The environment block is validated
            // and then ignored on purpose, so a guest cannot inject DYLD_* variables here.
            if (length < 12) throw Error("LOAD_METADATA", "truncated string command");
            (void)r.commandString(p, length, 12);
        } else if (cmd == 0x16) {  // LC_TWOLEVEL_HINTS
            if (length < 16) throw Error("LOAD_METADATA", "truncated two-level hints");
            r.range(r.u32(p + 8), uint64_t(r.u32(p + 12)) * 4);
        } else if (cmd == 0x17) {  // LC_PREBIND_CKSUM
            if (length < 12) throw Error("LOAD_METADATA", "truncated prebind checksum");
        } else if (cmd == 0x1b || cmd == 0x25 || cmd == 0x24 || cmd == 0x2a || cmd == 0x2f ||
                   cmd == 0x30 || cmd == 0x1a || cmd == 0x19) {
            // UUID, version-min and source-version metadata. LC_SEGMENT_64 is refused: this
            // loader only ever maps 32-bit images.
            if (cmd == 0x19) throw Error("LOAD_FORMAT", "64-bit segments in a 32-bit image");
            const uint32_t minimum = cmd == 0x1b ? 24 : 16;
            if (length < minimum) throw Error("LOAD_METADATA", "truncated metadata command");
        } else {
            std::ostringstream out;
            out << "unsupported load command 0x" << std::hex << cmd;
            throw Error("LOAD_COMMAND_UNSUPPORTED", out.str());
        }
        p += length;
    }
    if (hasDynamicSymbols) {
        if (!hasSymbols) throw Error("LOAD_DYSYMTAB", "dynamic symbol table requires LC_SYMTAB");
        for (size_t span = 0; span < 3; span++) {
            uint32_t start = r.u32(dynamicSymbols + 8 + span * 8);
            uint32_t count = r.u32(dynamicSymbols + 12 + span * 8);
            if (start > symbolCount || count > symbolCount - start)
                throw Error("LOAD_DYSYMTAB", "symbol partition exceeds LC_SYMTAB");
            for (size_t previous = 0; previous < span; previous++) {
                uint32_t oldStart = r.u32(dynamicSymbols + 8 + previous * 8);
                uint32_t oldCount = r.u32(dynamicSymbols + 12 + previous * 8);
                if (count && oldCount && uint64_t(start) < uint64_t(oldStart) + oldCount &&
                    uint64_t(oldStart) < uint64_t(start) + count)
                    throw Error("LOAD_DYSYMTAB", "dynamic symbol partitions overlap");
            }
        }
        const uint32_t offset = r.u32(dynamicSymbols + 56);
        const uint32_t count = r.u32(dynamicSymbols + 60);
        for (uint32_t index = 0; index < count; index++) {
            uint32_t symbol = r.u32(uint64_t(offset) + uint64_t(index) * 4);
            constexpr uint32_t flags = 0xc0000000u;  // INDIRECT_SYMBOL_LOCAL / ABS
            if ((symbol & flags) ? bool(symbol & ~flags) : symbol >= symbolCount)
                throw Error("LOAD_DYSYMTAB", "invalid indirect symbol reference");
        }
    }
    if (hasSymbols) {
        size_t nameBytes = 0;
        for (uint32_t index = 0; index < symbolCount; index++) {
            uint64_t record = uint64_t(symbolOffset) + uint64_t(index) * 12;
            uint8_t type = data[record + 4];
            if ((type & 0xe0) || !(type & 1) || ((type & 0x0e) != 0x0e && (type & 0x0e) != 2))
                continue;
            uint32_t nameIndex = r.u32(record);
            if (nameIndex >= stringBytes) throw Error("LOAD_EXPORT", "invalid export string index");
            size_t start = size_t(stringOffset) + nameIndex, end = start;
            while (end < size_t(stringOffset) + stringBytes && data[end] && end - start <= 4096)
                end++;
            if (end == start || end == size_t(stringOffset) + stringBytes || end - start > 4096)
                throw Error("LOAD_EXPORT", "invalid export name");
            if (result.exports.size() >= 100000 || end - start > 8 * 1024 * 1024 - nameBytes)
                throw Error("LOAD_EXPORT", "export budget exceeded");
            nameBytes += end - start;
            uint32_t value = r.u32(record + 8);
            if ((type & 0x0e) == 0x0e) {
                bool inside = false;
                for (const auto& segment : segments)
                    if (segment.region.permissions && value >= segment.region.base &&
                        uint64_t(value) < uint64_t(segment.region.base) + segment.region.size)
                        inside = true;
                if (!inside) throw Error("LOAD_EXPORT", "defined section symbol outside image");
                if (data[record + 6] & 8) value |= 1;  // N_ARM_THUMB_DEF interworking bit.
            }
            std::string name(reinterpret_cast<const char*>(data + start), end - start);
            if ((type & 0x0e) == 0x0e) {
                // Section symbols describe real code. Keep them for stop-report
                // symbolication; local labels ("L..." / "l...") are compiler temporaries
                // and would only ever name a branch target, never a function.
                char first = name[0];
                bool label = first == 'L' || first == 'l' || name.compare(0, 2, "ltmp") == 0;
                if (!label && result.symbols.size() < 200000)
                    result.symbols.push_back({value & ~1u, name});
            }
            result.exports.push_back({std::move(name), {value, bool(data[record + 6] & 0x80)}});
        }
    }
    std::sort(
        result.symbols.begin(), result.symbols.end(),
        [](const FunctionSymbol& a, const FunctionSymbol& b) { return a.address < b.address; });
    std::vector<DyldSegment> dyldSegments;
    for (const auto& segment : segments)
        dyldSegments.push_back(
            {segment.region.base, segment.region.size, segment.region.permissions});
    size_t dyldNames = 0;
    for (size_t stream = 0; stream < 4; stream++) {
        const auto [offset, bytes] = dyldStreams[stream];
        if (!bytes) continue;
        auto decoded =
            decodeDyld(data + offset, bytes, dyldSegments, stream == 0, stream == 3, stream == 2);
        if (decoded.fixups.size() > 100000 - result.dyld.fixups.size())
            throw Error("DYLD_LIMIT", "combined fixup budget exceeded");
        result.dyld.needsCoalescing |= decoded.needsCoalescing;
        if (decoded.strongDefinitions.size() > 100000 - result.dyld.strongDefinitions.size())
            throw Error("DYLD_LIMIT", "combined weak declaration count");
        for (auto& name : decoded.strongDefinitions) {
            if (name.size() > 8 * 1024 * 1024 - dyldNames)
                throw Error("DYLD_LIMIT", "combined name budget");
            dyldNames += name.size();
            result.dyld.strongDefinitions.push_back(std::move(name));
        }
        for (auto& fixup : decoded.fixups) {
            if (fixup.ordinal > 0 && size_t(fixup.ordinal) > result.dependencies.size())
                throw Error("DYLD_ORDINAL", "binding references nonexistent dylib");
            if (fixup.symbol.size() > 8 * 1024 * 1024 - dyldNames)
                throw Error("DYLD_LIMIT", "combined symbol budget exceeded");
            dyldNames += fixup.symbol.size();
            result.dyld.fixups.push_back(std::move(fixup));
        }
    }
    std::map<uint32_t, size_t> planned;
    for (size_t index = 0; index < result.dyld.fixups.size(); index++)
        planned[result.dyld.fixups[index].address] = index;
    for (const auto& section : indirectSections) {
        if (!hasDynamicSymbols)
            throw Error("DYLD_INDIRECT_SECTION", "indirect section requires LC_DYSYMTAB");
        uint32_t table = r.u32(dynamicSymbols + 56), entries = r.u32(dynamicSymbols + 60);
        if (section.first > entries || section.count > entries - section.first)
            throw Error("DYLD_INDIRECT_SECTION", "section indexes exceed indirect table");
        if (section.stub) {
            // Classic ARM symbol stubs. reserved2 is the stub size: twelve bytes is the
            // three-word branch sequence we rewrite, four bytes is the compact
            // `ldr pc, [pc, #imm]` form clang emits when every stub forwards to a pointer
            // slot in __DATA (Minecraft PE ships 573 of those). Both are patchable, by
            // different means; anything else is refused by name.
            const bool threeWord = section.stride >= 12 && section.stride <= 64;
            if ((section.stride != 4 && !threeWord) || section.stride % 4)
                throw Error("DYLD_STUB", "symbol stub size is not patchable", section.address);
            if (section.count > 100000 - result.stubs.size())
                throw Error("DYLD_LIMIT", "symbol stub budget exceeded");
            result.legacyStubs += section.count;
            for (uint32_t index = 0; index < section.count; index++) {
                uint32_t symbol = r.u32(uint64_t(table) + uint64_t(section.first + index) * 4);
                if (symbol & 0xc0000000u) continue;  // Local stub with no external symbol.
                uint64_t record = uint64_t(symbolOffset) + uint64_t(symbol) * 12;
                uint32_t nameIndex = r.u32(record);
                if (nameIndex >= stringBytes || (data[record + 4] & 0xe0))
                    throw Error("DYLD_SYMBOL", "invalid stub nlist symbol");
                size_t start = size_t(stringOffset) + nameIndex, end = start;
                while (end < size_t(stringOffset) + stringBytes && data[end] && end - start <= 4096)
                    end++;
                if (end == start || end == size_t(stringOffset) + stringBytes || end - start > 4096)
                    throw Error("DYLD_SYMBOL", "invalid stub symbol name");
                int ordinal = data[record + 7];
                if (ordinal == 255)
                    ordinal = -1;
                else if (ordinal == 254)
                    ordinal = -2;
                if (ordinal > 0 && size_t(ordinal) > result.dependencies.size())
                    throw Error("DYLD_ORDINAL", "stub references a missing dylib");
                const uint32_t address = section.address + index * section.stride;
                LegacyStub stub{
                    address, section.stride,
                    std::string(reinterpret_cast<const char*>(data + start), end - start), ordinal,
                    bool(data[record + 6] & 0x40)};
                if (section.stride == 4) {
                    // Validate the encoding before trusting the slot it names: a compact stub
                    // must be `ldr pc, [pc, #imm]`, and the slot must be inside the image.
                    const uint32_t instruction =
                        r.u32(uint64_t(section.fileOffset) + uint64_t(index) * section.stride);
                    if ((instruction & 0xfffff000u) != 0xe59ff000u)
                        throw Error("DYLD_STUB", "compact stub is not an ldr pc, [pc, #imm]",
                                    address);
                    stub.compact = true;
                    stub.slot = address + 8 + (instruction & 0xfffu);
                }
                result.stubs.push_back(std::move(stub));
                if (result.stubs.back().symbol.size() > 8 * 1024 * 1024 - dyldNames)
                    throw Error("DYLD_LIMIT", "combined stub symbol budget exceeded");
                dyldNames += result.stubs.back().symbol.size();
            }
            continue;
        }
        if (section.count > 100000)
            throw Error("DYLD_LIMIT", "indirect pointer count exceeds budget");
        for (uint32_t index = 0; index < section.count; index++) {
            uint32_t symbol = r.u32(uint64_t(table) + uint64_t(section.first + index) * 4);
            if (symbol & 0xc0000000u)
                continue;  // Local/absolute pointer, not an external symbol binding.
            uint64_t record = uint64_t(symbolOffset) + uint64_t(symbol) * 12;
            uint32_t nameIndex = r.u32(record);
            if (nameIndex >= stringBytes || (data[record + 4] & 0xe0))
                throw Error("DYLD_SYMBOL", "invalid indirect nlist symbol");
            size_t start = size_t(stringOffset) + nameIndex, end = start;
            while (end < size_t(stringOffset) + stringBytes && data[end] && end - start <= 4096)
                end++;
            if (end == start || end == size_t(stringOffset) + stringBytes || end - start > 4096)
                throw Error("DYLD_SYMBOL", "invalid indirect symbol name");
            std::string name(reinterpret_cast<const char*>(data + start), end - start);
            int ordinal = data[record + 7];
            if (ordinal == 255)
                ordinal = -1;
            else if (ordinal == 254)
                ordinal = -2;
            if (ordinal > 0 && size_t(ordinal) > result.dependencies.size())
                throw Error("DYLD_ORDINAL", "indirect symbol references missing dylib");
            uint32_t address = section.address + index * 4;
            if (!result.memory) throw Error("DYLD_TARGET", "missing image storage");
            auto existing = planned.find(address);
            if (existing != planned.end()) {
                const auto& old = result.dyld.fixups[existing->second];
                if (old.rebase)
                    throw Error("DYLD_BIND_CONFLICT", "indirect section rebinds a pointer slot");
                // Already bound by the regular stream; the indirect stream is redundant here
                // and the earlier binding wins. Only a rebase slot is a real conflict.
                continue;
            }
            if (result.dyld.fixups.size() >= 100000 || name.size() > 8 * 1024 * 1024 - dyldNames)
                throw Error("DYLD_LIMIT", "combined indirect binding budget");
            dyldNames += name.size();
            result.dyld.fixups.push_back({address, false, bool(data[record + 6] & 0x40),
                                          section.lazy, ordinal, 0, std::move(name)});
            planned.emplace(address, result.dyld.fixups.size() - 1);
        }
    }
    if (!hasEntry) throw Error("LOAD_ENTRY_MISSING", "no LC_MAIN or supported LC_UNIXTHREAD");
    if (entryIsOffset) {
        bool found = false;
        for (const auto& s : segments) {
            if (entry >= s.offset && entry < uint64_t(s.offset) + s.fileSize &&
                (s.region.permissions & Execute)) {
                entry = uint64_t(s.region.base) + (entry - s.offset);
                found = true;
                break;
            }
        }
        if (!found) throw Error("LOAD_ENTRY", "file-relative entry is not in executable bytes");
    }
    if (entry > std::numeric_limits<uint32_t>::max())
        throw Error("LOAD_ENTRY", "entry exceeds ARM32 address space");
    const bool thumb = (entry & 1) || (result.cpsr & 0x20);
    const uint32_t pc = uint32_t(entry) & ~1u;
    if (!thumb && pc % 4) throw Error("LOAD_ENTRY_ALIGNMENT", "ARM entry is not word-aligned", pc);
    // All parsing/validation precedes any backing allocations, minimizing hostile-file work.
    for (const auto& s : segments)
        if (s.region.permissions) result.memory->map(s.region, data + s.offset, s.fileSize);
    if (!result.memory->permits(pc, thumb ? 2 : 4, Execute))
        throw Error("LOAD_ENTRY_ACCESS", "entry is not executable", pc);
    // Guest-defined Objective-C classes, read from the mapped list. A pointer outside a
    // readable region means the file is not self-consistent, so it is refused here instead
    // of being dereferenced on the first message.
    for (const auto& [listAddress, listBytes] : classListSections) {
        const uint32_t count = listBytes / 4;
        if (count > 4096 - result.objcClasses.size())
            throw Error("LOAD_OBJECTIVE_C", "class list exceeds budget");
        for (uint32_t index = 0; index < count; index++) {
            const uint32_t entry = result.memory->read32(listAddress + index * 4);
            if (!result.memory->permits(entry, 40, Read))
                throw Error("LOAD_OBJECTIVE_C", "class object is outside the image", entry);
            result.objcClasses.push_back(entry);
        }
    }
    constexpr uint32_t StackBase = 0x70000000, StackSize = 1024 * 1024;
    // Guard segments must also remain unmapped; a backed-only overlap check is insufficient.
    for (const auto& segment : segments) {
        if (uint64_t(StackBase) < uint64_t(segment.region.base) + segment.region.size &&
            uint64_t(segment.region.base) < uint64_t(StackBase) + StackSize)
            throw Error("LOAD_STACK_OVERLAP", "image or guard overlaps the reserved stack",
                        StackBase);
    }
    result.memory->map(
        {StackBase, StackSize, uint8_t(Read | Write), "PodNative stack (ABI not initialized)"});
    result.registers[13] = StackBase + StackSize - 16;
    result.registers[15] = pc;
    result.cpsr = (result.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
    return result;
}
void bindLegacyStubs(LoadedImage& image, const ExportGraph& graph, size_t requester,
                     const std::function<uint32_t(const std::string&)>& trap) {
    if (!image.memory) throw Error("DYLD_STUB", "stub binding requires mapped image storage");
    Memory& memory = *image.memory;
    for (const auto& stub : image.stubs) {
        std::optional<uint32_t> target = graph.resolve(requester, stub.ordinal, stub.symbol);
        if (!target && !stub.weak && trap) target = trap(stub.symbol);
        if (stub.compact) {
            // A one-word `ldr pc, [pc, #imm]` stub cannot hold a branch sequence: it loads its
            // destination from the pointer slot it names, so bind that slot. dyld leaves these
            // to lazy binding; we resolve them all before the guest starts, which is why the
            // same run cannot observe a half-bound table.
            if (!stub.slot || !memory.permits(stub.slot, 4, Write))
                throw Error("DYLD_STUB", "compact stub references an unwritable slot",
                            stub.address);
            memory.write32(stub.slot, target.value_or(0));
            continue;
        }
        if (!memory.permits(stub.address, 12, Execute))
            throw Error("DYLD_STUB", "symbol stub is not executable code", stub.address);
        // ldr r12, [pc, #0] / bx r12 / .word target. The guest reaches the implementation
        // in two instructions instead of through a lazy pointer we do not maintain.
        const uint32_t address = target.value_or(0);
        const uint32_t body[3] = {0xe59fc000u, 0xe12fff1cu, address};
        memory.patch(stub.address, body, sizeof(body));
        for (uint32_t offset = 12; offset < stub.stride; offset += 4)
            memory.patch32(stub.address + offset, 0xe1a00000u);  // mov r0, r0
    }
}
std::string LoadedImage::nearestSymbol(uint32_t address) const {
    if (symbols.empty()) return {};
    // Symbols are appended in symtab order, which is usually sorted but is not required to
    // be; sort a copy of the addresses once per stop instead of assuming order.
    auto found = std::upper_bound(
        symbols.begin(), symbols.end(), address,
        [](uint32_t value, const FunctionSymbol& symbol) { return value < symbol.address; });
    if (found == symbols.begin()) return {};
    --found;
    if (uint64_t(address) - found->address > 0x100000) return {};
    // Mach-O adds a leading underscore to C symbols; objective-C method names and
    // demangled-free assembler symbols do not have one. Drop it so a report reads
    // "main" rather than "_main" and keeps "-[View draw]" intact.
    if (found->name.size() > 1 && found->name[0] == '_' && found->name[1] != '_')
        return found->name.substr(1);
    return found->name;
}
std::string LoadedImage::summary() const {
    std::ostringstream s;
    s << "MAPPED_ARM32: " << memory->regions().size() << " regions, " << memory->allocated()
      << " guest bytes; entry=0x" << std::hex << registers[15] << (cpsr & 0x20 ? " Thumb" : " ARM")
      << " SP=0x" << registers[13];
    s << "\nInstruction set: " << (arch == IsaArch::ArmV6 ? "ARMv6 (ARM11/v6K)" : "ARMv7")
      << " from CPU subtype " << std::dec << cpuSubtype << ".";
    s << "\nImage mapping is complete; this summary alone does not start the guest.";
    s << "\n" << dyld.summary();
    if (!exports.empty()) s << "\nDefined exports=" << exports.size();
    if (dyld.needsCoalescing)
        s << "\nWeak/strong definitions require export-graph coalescing; strong declarations="
          << dyld.strongDefinitions.size();
    if (legacyStubs) {
        size_t compact = 0;
        for (const auto& stub : stubs) compact += stub.compact ? 1 : 0;
        s << "\nLegacy symbol stubs=" << std::dec << legacyStubs;
        if (stubs.empty())
            s << "; none carried an external symbol.";
        else
            s << "; each is redirected to its resolved import or to a named trap.";
        if (compact)
            s << ' ' << compact << " are one-word ldr pc stubs dispatched through their slot.";
    }
    for (const auto& dependency : dependencies) s << "\nrequires " << dependency;
    return s.str();
}
}  // namespace podnative::runtime
