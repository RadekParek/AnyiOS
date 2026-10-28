#include "macho_container.h"

#include <algorithm>
#include <vector>

#include "runtime/error.h"
namespace podnative {
using runtime::Error;
namespace {
int arm32SubtypeRank(uint32_t subtype) {
    // Match the pinned ARM32 runtime's accepted Mach-O subtypes. Capability bits
    // signal requirements this backend does not implement and must not be masked.
    if (subtype & 0xff000000u) return -1;
    switch (subtype) {
        case 9:
            return 5;  // ARMv7
        case 11:
            return 4;  // ARMv7s
        case 10:
            return 3;  // ARMv7f
        case 6:
            return 2;  // ARMv6
        case 0:
            return 1;  // Generic ARM
        case 7:
            return 0;  // ARMv5TEJ fallback; ARM/Thumb, no Jazelle bytecode
        default:
            return -1;
    }
}
}  // namespace
MachOSlice selectArm32Slice(const uint8_t* b, size_t n, uint64_t fileSize) {
    if (!b || n < 4 || fileSize < 4 || n > fileSize)
        throw Error("CONTAINER_TRUNCATED", "executable header is incomplete");
    auto read = [b, n](size_t p, size_t count, bool little) {
        if (p > n || count > n - p)
            throw Error("FAT_TABLE_TRUNCATED", "architecture table is incomplete");
        uint64_t value = 0;
        for (size_t i = 0; i < count; i++)
            value = (value << 8) | b[p + (little ? count - 1 - i : i)];
        return value;
    };
    const uint32_t magic = uint32_t(read(0, 4, false));
    if (magic == 0xcefaedfe) {
        if (n < 28 || fileSize < 28) throw Error("MACHO_TRUNCATED", "ARM32 header is incomplete");
        if (read(4, 4, true) != 12) throw Error("MACHO_CPU", "thin executable is not ARM32");
        const auto subtype = uint32_t(read(8, 4, true));
        if (arm32SubtypeRank(subtype) < 0)
            throw Error("MACHO_CPU_SUBTYPE",
                        "thin ARM32 executable uses an unsupported CPU subtype or capability");
        return {0, fileSize, subtype, false, false, false};
    }
    bool little = magic == 0xbebafeca || magic == 0xbfbafeca;
    bool wide = magic == 0xcafebabf || magic == 0xbfbafeca;
    if (magic != 0xcafebabe && magic != 0xbebafeca && !wide)
        throw Error(
            "MACHO_FORMAT",
            "choose an ARM32 Mach-O or universal binary containing ARM32; ARM64-only games are unsupported");
    uint64_t count = read(4, 4, little), stride = wide ? 32 : 20;
    if (!count || count > 64)
        throw Error("FAT_ARCH_COUNT", "universal binary must contain 1–64 architecture records");
    const uint64_t tableEnd = 8 + count * stride;
    if (tableEnd > n || tableEnd > fileSize)
        throw Error("FAT_TABLE_TRUNCATED", "architecture table exceeds available bytes");
    struct Range {
        uint64_t start, end;
    };
    std::vector<Range> ranges;
    MachOSlice chosen{};
    bool containsArm32 = false;
    bool containsArm64 = false;
    bool containsArm64_32 = false;
    bool containsOther = false;
    int best = -1;
    for (uint64_t i = 0; i < count; i++) {
        const size_t p = size_t(8 + i * stride);
        const uint32_t cpu = uint32_t(read(p, 4, little)),
                       subtype = uint32_t(read(p + 4, 4, little));
        containsArm32 = containsArm32 || cpu == 12;
        containsArm64 = containsArm64 || cpu == 0x0100000c;
        containsArm64_32 = containsArm64_32 || cpu == 0x0200000c;
        containsOther = containsOther || (cpu != 12 && cpu != 0x0100000c && cpu != 0x0200000c);
        const uint64_t offset = read(p + 8, wide ? 8 : 4, little),
                       size = read(p + (wide ? 16 : 12), wide ? 8 : 4, little);
        const uint64_t alignment = read(p + (wide ? 24 : 16), 4, little);
        if (wide && read(p + 28, 4, little))
            throw Error("FAT_RESERVED", "nonzero FAT64 reserved field");
        if (!size || offset < tableEnd || offset > fileSize || size > fileSize - offset)
            throw Error("FAT_SLICE_BOUNDS",
                        "architecture slice lies outside its file or overlaps the table");
        if (alignment > 31 || offset % (uint64_t(1) << alignment))
            throw Error("FAT_ALIGNMENT", "invalid architecture slice alignment");
        for (const auto& r : ranges)
            if (offset < r.end && r.start < offset + size)
                throw Error("FAT_SLICE_OVERLAP", "architecture slices overlap");
        ranges.push_back({offset, offset + size});
        if (cpu == 12) {
            // Prefer existing v7/v6/generic slices; use ARMv5TEJ only as a fallback.
            const int rank = arm32SubtypeRank(subtype);
            if (rank > best) {
                best = rank;
                chosen = {offset, size, subtype, true, containsArm64, containsArm64_32};
            }
        }
    }
    if (best >= 0) {
        chosen.containsArm64 = containsArm64;
        chosen.containsArm64_32 = containsArm64_32;
    }
    if (best < 0) {
        std::string architectures;
        auto appendArchitecture = [&architectures](const char* name) {
            if (!architectures.empty()) architectures += " + ";
            architectures += name;
        };
        if (containsArm32) appendArchitecture("ARM32");
        if (containsArm64) appendArchitecture("ARM64");
        if (containsArm64_32) appendArchitecture("ARM64_32");
        if (containsOther) appendArchitecture("other CPU architectures");
        if (architectures.empty()) architectures = "no recognized CPU architectures";
        throw Error("FAT_NO_ARM32",
                    "universal binary contains " + architectures +
                        " but no supported ARM32 slice; ARM64-only iOS games are unsupported");
    }
    if (chosen.size < 28)
        throw Error("FAT_SLICE_TRUNCATED", "selected ARM32 slice has no complete Mach-O header");
    return chosen;
}
}  // namespace podnative
