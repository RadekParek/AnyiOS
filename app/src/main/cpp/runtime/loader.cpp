#include "loader.h"

#include <algorithm>
#include <cstring>
#include <limits>
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
    if (r.u32(24) & 0x200000)
        throw Error("LOAD_PIE_UNSUPPORTED", "PIE images need a validated rebasing implementation");
    std::vector<Segment> segments;
    LoadedImage result;
    result.memory = std::make_unique<Memory>(budget);
    bool hasEntry = false, entryIsOffset = false;
    uint64_t entry = 0;
    size_t p = 28;
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
            if (length < 48) throw Error("LOAD_DYLD_INFO", "truncated dyld info");
            for (size_t n = 0; n < 5; n++) {
                const uint32_t offset = r.u32(p + 8 + n * 8), bytes = r.u32(p + 12 + n * 8);
                r.range(offset, bytes);
                if (bytes) result.requiresDyld = true;
            }
        } else if (cmd == 2) {
            if (length < 24) throw Error("LOAD_SYMTAB", "truncated symbol table");
            r.range(r.u32(p + 8), uint64_t(r.u32(p + 12)) * 12);
            r.range(r.u32(p + 16), r.u32(p + 20));
            // Symbol binding is explicitly deferred; even local-only tables need audit.
            if (r.u32(p + 12)) result.requiresDyld = true;
        } else if (cmd == 0x1d || cmd == 0x26 ||
                   cmd == 0x29) {  // code signature / function starts / data-in-code
            if (length < 16) throw Error("LOAD_LINKEDIT", "truncated linkedit command");
            r.range(r.u32(p + 8), r.u32(p + 12));
        } else if (cmd == 0x21 || cmd == 0x1b || cmd == 0x25 || cmd == 0x2a || cmd == 0x24 ||
                   cmd == 0x2f || cmd == 0x30) {
            // Validated inspection metadata; not execution or relocation commands.
            const uint32_t minimum = cmd == 0x21 ? 20 : (cmd == 0x1b ? 24 : 16);
            if (length < minimum) throw Error("LOAD_METADATA", "truncated metadata command");
        } else {
            std::ostringstream out;
            out << "unsupported load command 0x" << std::hex << cmd;
            throw Error("LOAD_COMMAND_UNSUPPORTED", out.str());
        }
        p += length;
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
std::string LoadedImage::summary() const {
    std::ostringstream s;
    s << "MAPPED_ARM32: " << memory->regions().size() << " regions, " << memory->allocated()
      << " guest bytes; entry=0x" << std::hex << registers[15] << (cpsr & 0x20 ? " Thumb" : " ARM")
      << " SP=0x" << registers[13];
    s << "\nExecution BLOCKED: " << (requiresDyld ? "dyld/symbol resolution required; " : "")
      << "guest CPU, application ABI and frameworks not initialized.";
    for (const auto& dependency : dependencies) s << "\nrequires " << dependency;
    return s.str();
}
}  // namespace podnative::runtime
