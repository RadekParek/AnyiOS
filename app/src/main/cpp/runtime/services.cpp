#include "services.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "error.h"

namespace podnative::runtime {
Services::Services(Memory& m)
    : memory(m),
      symbols{"_memcpy",
              "_memmove",
              "_memset",
              "_memcmp",
              "_strlen",
              "_strcmp",
              "_bzero",
              "_CFBitVectorCreate",
              "_CFBitVectorCreateMutableCopy",
              "_CFBitVectorGetBitAtIndex",
              "_CFBitVectorSetBitAtIndex",
              "_CFRetain",
              "_CFRelease"} {
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
    auto foundation =
        graph.addImage("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
    auto graphics =
        graph.addImage("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
    for (size_t n = 0; n < symbols.size(); n++)
        graph.define(n < 7 ? system : foundation, symbols[n], {Base + uint32_t(n) * 8});
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
    auto& r = state.registers;
    if (cf.invoke(name, state, memory)) return;
    constexpr size_t Limit = 16 * 1024 * 1024;
    if (name == "_strlen" || name == "_strcmp") {
        for (uint32_t n = 0; n < 1024 * 1024; n++) {
            uint8_t a, b = 0;
            if (uint64_t(r[0]) + n > UINT32_MAX ||
                (name == "_strcmp" && uint64_t(r[1]) + n > UINT32_MAX))
                throw Error("HLE_RANGE", "string address overflow");
            memory.read(r[0] + n, &a, 1);
            if (name == "_strcmp") memory.read(r[1] + n, &b, 1);
            if (name == "_strcmp" && a != b) {
                r[0] = uint32_t(int(a) - int(b));
                return;
            }
            if (!a) {
                r[0] = name == "_strlen" ? n : 0;
                return;
            }
        }
        throw Error("HLE_LIMIT", "unterminated string exceeds scan budget");
    }
    size_t count = name == "_bzero" ? r[1] : r[2];
    if (count > Limit) throw Error("HLE_LIMIT", "memory operation exceeds session call budget");
    if (name == "_memcmp") {
        std::vector<uint8_t> a(count), b(count);
        memory.read(r[0], a.data(), count);
        memory.read(r[1], b.data(), count);
        int result = 0;
        for (size_t n = 0; n < count; n++)
            if (a[n] != b[n]) {
                result = int(a[n]) - int(b[n]);
                break;
            }
        r[0] = uint32_t(result);
        return;
    }
    if (!memory.permits(r[0], count, Write) && count)
        throw Error("HLE_RANGE", "memory destination is not writable", r[0]);
    std::vector<uint8_t> bytes(count);
    if (name == "_memcpy" || name == "_memmove") {
        if (name == "_memcpy" && count && uint64_t(r[0]) < uint64_t(r[1]) + count &&
            uint64_t(r[1]) < uint64_t(r[0]) + count)
            throw Error("HLE_OVERLAP", "overlapping memcpy is undefined; use memmove");
        memory.read(r[1], bytes.data(), count);
    } else if (name == "_memset")
        std::fill(bytes.begin(), bytes.end(), uint8_t(r[1]));
    else if (name != "_bzero")
        throw Error("HLE_SYMBOL", "unimplemented service");
    memory.write(r[0], bytes.data(), count);
    // memcpy/memmove/memset return the original destination; bzero is void.
}
}  // namespace podnative::runtime
