#include "services.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "error.h"

namespace podnative::runtime {
Services::Services(Memory& m)
    : memory(m),
      libc(m),
      symbols{
          // libSystem: C runtime.
          "_malloc",
          "_calloc",
          "_realloc",
          "_free",
          "_memcpy",
          "_memmove",
          "_memset",
          "_memcmp",
          "_memchr",
          "_bzero",
          "_strlen",
          "_strcmp",
          "_strncmp",
          "_strcasecmp",
          "_strncasecmp",
          "_strcpy",
          "_strncpy",
          "_strcat",
          "_strlcat",
          "_strchr",
          "_strrchr",
          "_strstr",
          "_strdup",
          "_strerror_r",
          "_atoi",
          "_atol",
          "_atof",
          "_strtol",
          "_strtoul",
          "_strtoll",
          "_strtoull",
          "_strtod",
          "_rand",
          "_srand",
          "_time",
          "_gettimeofday",
          "_OSAtomicCompareAndSwap32Barrier",
          "_OSAtomicAnd32OrigBarrier",
          "_OSAtomicOr32OrigBarrier",
          // libm, re-exported through libSystem.
          "_sin",
          "_cos",
          "_pow",
          "_fmod",
          "_sinf",
          "_cosf",
          "_tanf",
          "_acosf",
          "_atanf",
          "_atan2f",
          "_powf",
          // CoreFoundation.
          "_CFBitVectorCreate",
          "_CFBitVectorCreateMutableCopy",
          "_CFBitVectorGetBitAtIndex",
          "_CFBitVectorSetBitAtIndex",
          "_CFRetain",
          "_CFRelease",
      } {
    std::vector<uint8_t> code(Memory::PageSize);
    auto put = [&](size_t offset, uint32_t word) {
        for (unsigned n = 0; n < 4; n++) code[offset + n] = uint8_t(word >> (n * 8));
    };
    for (size_t n = 0; n < symbols.size(); n++) {
        put(n * 8, 0xef000000 | uint32_t(n + 1));
        put(n * 8 + 4, 0xe12fff1e);
    }
    // Remaining slots are undefined instructions, not zero-filled ARM no-ops.
    for (size_t n = symbols.size() * 8; n < code.size(); n += 4) put(n, 0xe7f000f0);
    memory.map({Base, Memory::PageSize, uint8_t(Read | Execute), "AnyiOS HLE call gates"},
               code.data(), code.size());
    float constants[] = {1,
                         0,
                         0,
                         1,
                         0,
                         0,
                         std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::infinity(),
                         0,
                         0};
    memory.map({Base + Memory::PageSize, Memory::PageSize, Read, "CoreGraphics constants"},
               reinterpret_cast<const uint8_t*>(constants), sizeof(constants));
}
uint32_t Services::address(const std::string& name) const {
    auto it = std::find(symbols.begin(), symbols.end(), name);
    if (it == symbols.end()) throw Error("HLE_SYMBOL", "API has no implementation");
    return Base + uint32_t(it - symbols.begin()) * 8;
}
void Services::install(ExportGraph& graph) const {
    auto system = graph.addImage("/usr/lib/libSystem.B.dylib");
    graph.alias(system, "/usr/lib/libSystem.dylib");
    graph.alias(system, "/usr/lib/libm.dylib");
    graph.alias(system, "/usr/lib/system/libsystem_c.dylib");
    auto foundation =
        graph.addImage("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
    auto graphics =
        graph.addImage("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
    for (size_t n = 0; n < symbols.size(); n++) {
        const bool coreFoundation = symbols[n].rfind("_CF", 0) == 0;
        graph.define(coreFoundation ? foundation : system, symbols[n], {Base + uint32_t(n) * 8});
    }
    graph.define(graphics, "_CGAffineTransformIdentity", {Base + Memory::PageSize});
    graph.define(graphics, "_CGRectNull", {Base + Memory::PageSize + 24});
}
void Services::dispatch(const CpuStop& stop, CpuState& state) {
    if (stop.reason != StopReason::SupervisorCall || stop.pc < Base || (stop.pc - Base) % 8 ||
        (stop.pc - Base) / 8 >= symbols.size() || stop.service != (stop.pc - Base) / 8 + 1 ||
        memory.fetch32(stop.pc) != (0xef000000 | stop.service) ||
        state.registers[15] != stop.pc + 4 || (state.cpsr & 0x23f) != 0x10)
        throw Error("HLE_TRAP", "unregistered or inconsistent guest service trap", stop.pc);
    uint32_t lr = state.registers[14], pc = lr & ~1u;
    bool thumb = lr & 1;
    if ((!thumb && pc % 4) || !memory.permits(pc, thumb ? 2 : 4, Execute))
        throw Error("HLE_RETURN", "invalid guest return address", lr);
    invoke(symbols[(stop.pc - Base) / 8], state);
    state.registers[15] = pc;
    state.cpsr = (state.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
}
void Services::invoke(const std::string& name, CpuState& state) {
    // Order matters only for diagnostics: every module reports "not mine" without
    // inventing a result, and anything unclaimed is a hard guest-visible failure.
    if (libc.invoke(name, state)) return;
    if (cf.invoke(name, state, memory)) return;
    throw Error("HLE_SYMBOL", "service has no implementation: " + name);
}
}  // namespace podnative::runtime
