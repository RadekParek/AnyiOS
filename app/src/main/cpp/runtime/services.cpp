#include "services.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "error.h"

namespace podnative::runtime {
Services::Services(Memory& m)
    : memory(m),
      libc(m),
      objc(m),
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
    objc.installGates();
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
    objc.defineSymbols(graph, foundation);
    graph.define(graphics, "_CGAffineTransformIdentity", {Base + Memory::PageSize});
    graph.define(graphics, "_CGRectNull", {Base + Memory::PageSize + 24});
}
void Services::mapTrapPage(size_t index) const {
    if (index >= 32) throw Error("HLE_TRAP_LIMIT", "too many unimplemented imports");
    std::vector<uint8_t> code(Memory::PageSize, 0);
    for (size_t n = 0; n < TrapsPerPage; n++)
        for (unsigned byte = 0; byte < 8; byte++) {
            const uint32_t word = byte < 4 ? 0xef000000u : 0xe12fff1eu;
            code[n * 8 + byte] = uint8_t(word >> ((byte % 4) * 8));
        }
    const uint32_t address = TrapBase + uint32_t(index * Memory::PageSize);
    memory.map(
        {address, Memory::PageSize, uint8_t(Read | Execute), "AnyiOS unimplemented-import traps"},
        code.data(), code.size());
}
uint32_t Services::trapFor(const std::string& symbol) {
    auto existing = trapIndex.find(symbol);
    if (existing != trapIndex.end()) return existing->second;
    if (traps.size() >= 32 * TrapsPerPage) throw Error("HLE_TRAP_LIMIT", "too many imports");
    const size_t index = traps.size();
    if (index % TrapsPerPage == 0) mapTrapPage(index / TrapsPerPage);
    traps.push_back(symbol);
    const uint32_t address = TrapBase + uint32_t(index * 8);
    trapIndex.emplace(symbol, address);
    return address;
}
void Services::dispatch(const CpuStop& stop, CpuState& state) {
    if (stop.reason != StopReason::SupervisorCall) throw Error("HLE_TRAP", "not a service stop");
    if ((state.cpsr & 0x23f) != 0x10)
        throw Error("HLE_TRAP", "service call left ARM user mode", stop.pc);
    if (memory.fetch32(stop.pc) != (0xef000000 | stop.service) ||
        state.registers[15] != stop.pc + 4)
        throw Error("HLE_TRAP", "unregistered or inconsistent guest service trap", stop.pc);
    const uint32_t lr = state.registers[14], returnAddress = lr & ~1u;
    const bool thumb = lr & 1;
    if ((!thumb && returnAddress % 4) || !memory.permits(returnAddress, thumb ? 2 : 4, Execute))
        throw Error("HLE_RETURN", "invalid guest return address", lr);
    if (stop.pc >= TrapBase && (stop.pc - TrapBase) % 8 == 0 &&
        (stop.pc - TrapBase) / 8 < traps.size())
        throw Error("HLE_UNIMPLEMENTED",
                    "guest called unimplemented import " + traps[(stop.pc - TrapBase) / 8],
                    stop.pc);
    if (objc.dispatch(stop.pc, state)) {
        state.registers[15] = returnAddress;
        state.cpsr = (state.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
        return;
    }
    if (stop.pc < Base || (stop.pc - Base) % 8 || (stop.pc - Base) / 8 >= symbols.size() ||
        stop.service != (stop.pc - Base) / 8 + 1)
        throw Error("HLE_TRAP", "unregistered guest service trap", stop.pc);
    invoke(symbols[(stop.pc - Base) / 8], state);
    state.registers[15] = returnAddress;
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
