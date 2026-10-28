#include "macho.h"

#include <cstring>
#include <sstream>
#include <string>
namespace podnative {
std::string inspectMachO(const uint8_t* b, size_t n) {
    auto u32 = [b](size_t p) {
        return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 |
               uint32_t(b[p + 3]) << 24;
    };
    if (n < 28) return "MACHO_TRUNCATED: 28-byte header required";
    if (u32(0) != 0xfeedface)
        return "MACHO_FORMAT: only thin little-endian 32-bit Mach-O supported; unsupported fat, "
               "big-endian or non-Mach-O input";
    if (u32(4) != 12) return "MACHO_CPU: executable is not ARM32";
    const uint32_t rawSubtype = u32(8);
    // Keep the inspector consistent with the FAT selector and AnyiOSEMU:
    // capability bits are requirements, not bits we can silently discard.
    const uint32_t subtype = rawSubtype & 0xffffff;
    if ((rawSubtype & 0xff000000u) || (subtype != 0 && subtype != 6 && subtype != 7 &&
                                       subtype != 9 && subtype != 10 && subtype != 11))
        return "MACHO_CPU_SUBTYPE: unsupported ARM32 subtype or capability " +
               std::to_string(rawSubtype);
    if (u32(12) != 2) return "MACHO_TYPE: expected MH_EXECUTE";
    uint32_t count = u32(16), total = u32(20);
    if (total > n - 28 || count > total / 8) return "MACHO_COMMANDS: invalid command bounds";
    size_t p = 28, end = 28 + size_t(total);
    bool encrypted = false;
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 8) return "MACHO_COMMANDS: truncated command";
        uint32_t cmd = u32(p), len = u32(p + 4);
        if (len < 8 || len % 4 || len > end - p) return "MACHO_COMMANDS: invalid command length";
        if (cmd == 0x21) {
            if (len < 20) return "MACHO_ENCRYPTION: truncated encryption command";
            if (u32(p + 8) > n || u32(p + 12) > n - u32(p + 8))
                return "MACHO_ENCRYPTION: invalid file range";
            encrypted |= u32(p + 16) != 0;
        }
        if (cmd == 1) {
            if (len < 56) return "MACHO_SEGMENT: truncated segment";
            if (u32(p + 32) > n || u32(p + 36) > n - u32(p + 32))
                return "MACHO_SEGMENT: invalid file range";
            if (u32(p + 48) > (len - 56) / 68) return "MACHO_SEGMENT: section table out of bounds";
        }
        p += len;
    }
    if (p != end) return "MACHO_COMMANDS: command size mismatch";
    if (encrypted)
        return "MACHO_ENCRYPTED: protected executable cannot be executed; no DRM bypass is "
               "provided";
    std::ostringstream out;
    out << "INSPECTED_ARM32: " << count << " load commands; CPU subtype " << subtype << " ("
        << (subtype == 6    ? "ARMv6"
            : subtype == 7  ? "ARMv5TEJ (ARM/Thumb; no Jazelle bytecode)"
            : subtype == 9  ? "ARMv7"
            : subtype == 10 ? "ARMv7f"
            : subtype == 11 ? "ARMv7s"
                            : "generic ARM")
        << "). Header inspection only. Separate mapping/dyld diagnostics follow; guest "
           "execution is reported separately and no launch claim is made here.";
    return out.str();
}
}  // namespace podnative
