#include "runtime/libc.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/error.h"
#include "runtime/heap.h"
using namespace podnative::runtime;
namespace {
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        if (e.code == code) return;
        std::cerr << e.what() << " expected " << code << '\n';
        std::abort();
    }
    std::cerr << "Expected fault " << code << '\n';
    std::abort();
}
struct Call {
    Memory& memory;
    Libc& libc;
    CpuState state;
    Call(Memory& m, Libc& l) : memory(m), libc(l) {}
    uint32_t operator()(const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
        state = {};
        state.registers[0] = a;
        state.registers[1] = b;
        state.registers[2] = c;
        if (!libc.invoke(name, state))
            throw Error("TEST_SYMBOL", std::string("unimplemented ") + name);
        return state.registers[0];
    }
    void doubles(double d0) {
        uint64_t bits;
        std::memcpy(&bits, &d0, sizeof(bits));
        state.extensionRegisters[0] = uint32_t(bits);
        state.extensionRegisters[1] = uint32_t(bits >> 32);
    }
    double d0() const {
        uint64_t bits =
            uint64_t(state.extensionRegisters[0]) | uint64_t(state.extensionRegisters[1]) << 32;
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    float s(size_t index) const {
        uint32_t bits = state.extensionRegisters[index];
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
};
}  // namespace
int main() {
    Memory memory;
    std::vector<uint8_t> page(16384, 0);
    memory.map({0x1000, 16384, uint8_t(Read | Write), "guest data"}, page.data(), page.size());
    Libc libc(memory);
    Call call(memory, libc);

    // Guest heap: real allocation, alignment, zeroing and reuse.
    const uint32_t block = call("_malloc", 100);
    assert(block >= 0x40000000 && block % 8 == 0);
    memory.write32(block, 0x12345678);
    const uint32_t second = call("_malloc", 32);
    assert(second > block && second % 8 == 0);
    const uint32_t zeroed = call("_calloc", 16, 4);
    for (int i = 0; i < 16; i++) assert(memory.read32(zeroed + uint32_t(i) * 4) == 0);
    call("_free", second);
    const uint32_t reused = call("_malloc", 16);
    assert(reused == second);
    call.state = {};
    call.state.registers[0] = 0x1111;
    fault("HEAP_FREE", [&] { libc.invoke("_free", call.state); });
    const uint32_t grown = call("_realloc", block, 4096);
    assert(grown && memory.read32(grown) == 0x12345678);
    fault("HEAP_REALLOC", [&] { call("_realloc", 0x2222, 8); });

    // Strings.
    const char text[] = "AnyiOS runtime";
    memory.write(0x1000, text, sizeof(text));
    memory.write(0x1100, "Any", 4);
    memory.write(0x1200, "runtime", 8);
    assert(call("_strlen", 0x1000) == 14);
    assert(call("_strcmp", 0x1000, 0x1000) == 0);
    assert(call("_strncmp", 0x1100, 0x1000, 3) == 0);
    memory.write(0x1300, "any", 4);
    assert(call("_strcasecmp", 0x1100, 0x1300) == 0);
    assert(int32_t(call("_strncmp", 0x1100, 0x1300, 3)) < 0);
    const uint32_t copy = call("_strdup", 0x1000);
    std::vector<char> duplicated(15);
    memory.read(copy, duplicated.data(), 15);
    assert(std::memcmp(duplicated.data(), text, sizeof(text)) == 0);
    assert(call("_strchr", 0x1000, 'S') == 0x1005);
    assert(call("_strchr", 0x1000, 'Z') == 0);
    assert(call("_strchr", 0x1000, 'r') == 0x1007);
    assert(call("_strrchr", 0x1000, 'r') == 0x1007);
    assert(call("_strstr", 0x1000, 0x1200) == 0x1007);
    memory.write(0x1700, "zzz", 4);
    assert(call("_strstr", 0x1000, 0x1700) == 0);
    memory.write(0x1400, "abc", 4);
    assert(call("_strcat", 0x1400, 0x1300) == 0x1400);
    std::vector<char> joined(7);
    memory.read(0x1400, joined.data(), 7);
    assert(std::strcmp(joined.data(), "abcany") == 0);
    memory.write(0x1500, "0123456789", 11);
    assert(call("_strncpy", 0x1600, 0x1500, 4) == 0x1600);
    assert(memory.read32(0x1600) == 0x33323130);

    // Integer and floating parsing with end pointers.
    memory.write(0x2000, "-1234xyz", 9);
    memory.write(0x2100, "0x1f", 5);
    memory.write(0x2200, "4294967296", 11);
    assert(call("_atoi", 0x2000) == uint32_t(-1234));
    assert(call("_strtol", 0x2000, 0x2900, 10) == uint32_t(-1234));
    assert(memory.read32(0x2900) == 0x2005);
    assert(call("_strtol", 0x2100, 0, 0) == 31);
    assert(call("_strtoul", 0x2100, 0, 16) == 31);
    call("_strtoll", 0x2200, 0x2904, 10);
    assert(call.state.registers[0] == 0 && call.state.registers[1] == 1);
    assert(call("_atoi", 0x2300) == 0);
    memory.write(0x2400, "2.5e2tail", 10);
    call("_strtod", 0x2400, 0x2908);
    assert(call.d0() == 250.0 && memory.read32(0x2908) == 0x2405);

    // Math through the VFP return registers.
    call.state = {};
    call.doubles(0.5);
    libc.invoke("_sin", call.state);
    assert(std::abs(call.d0() - std::sin(0.5)) < 1e-12);
    call.state = {};
    uint32_t bits;
    const float quarter = 0.25f;
    std::memcpy(&bits, &quarter, sizeof(bits));
    call.state.extensionRegisters[0] = bits;
    libc.invoke("_sinf", call.state);
    assert(std::abs(call.s(0) - std::sin(0.25f)) < 1e-6f);

    // Atomics operate on guest memory.
    memory.write32(0x3000, 5);
    assert(call("_OSAtomicCompareAndSwap32Barrier", 5, 9, 0x3000) == 1);
    assert(memory.read32(0x3000) == 9);
    assert(call("_OSAtomicCompareAndSwap32Barrier", 5, 1, 0x3000) == 0);
    assert(call("_OSAtomicAnd32OrigBarrier", 0xf, 0x3000) == 9);
    assert(memory.read32(0x3000) == 9);
    assert(call("_rand") != call("_rand"));

    // Unimplemented APIs stay unimplemented instead of returning invented success.
    CpuState unknown;
    assert(!libc.invoke("_UIApplicationMain", unknown));
    assert(!libc.invoke("_qsort", unknown));
    std::cout << "Guest heap, C string/number/math/atomic and OOM-free service tests passed\n";
}
