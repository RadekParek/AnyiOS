#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include "runtime/error.h"
#include "runtime/loader.h"
#include "runtime/memory.h"
using namespace podnative::runtime;
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b.at(at + i) = uint8_t(v >> (i * 8));
}
template <class F>
void fault(const char* code, F action) {
    try {
        action();
        std::cerr << "Expected fault " << code << '\n';
        std::abort();
    } catch (const Error& e) {
        if (e.code != code) {
            std::cerr << e.what() << " expected " << code << '\n';
            std::abort();
        }
    }
}
Bytes executable(bool thumb = false) {
    Bytes b(4096, 0);
    put(b, 0, 0xfeedface);
    put(b, 4, 12);
    put(b, 8, 9);
    put(b, 12, 2);
    put(b, 16, 2);
    put(b, 20, 80);
    put(b, 28, 1);
    put(b, 32, 56);
    std::memcpy(b.data() + 36, "__TEXT", 6);
    put(b, 52, 0x1000);
    put(b, 56, 4096);
    put(b, 60, 0);
    put(b, 64, 4096);
    put(b, 68, 5);
    put(b, 72, 5);
    put(b, 84, 0x80000028);
    put(b, 88, 24);
    put(b, 92, 0x100 + (thumb ? 1 : 0));
    put(b, 0x100, thumb ? 0xdf00202a : 0xe3a0002a);
    put(b, 0x104, 0xef000000);
    return b;
}
int main() {
    Memory m(3 * 4096);
    m.map({0x1000, 4096, uint8_t(Read | Write), "data1"});
    m.map({0x2000, 4096, uint8_t(Read | Write), "data2"});
    m.map({0x3000, 4096, uint8_t(Read | Execute), "code"});
    m.write32(0x1ffe, 0x12345678);
    assert(m.read32(0x1ffe) == 0x12345678);
    assert(m.read32(0x1000) == 0);
    fault("MEMORY_ACCESS", [&] { m.write32(0x3000, 0); });
    fault("MEMORY_ACCESS", [&] { m.fetch32(0x1000); });
    fault("CPU_ALIGNMENT", [&] { m.fetch32(0x3002); });
    fault("CPU_ALIGNMENT", [&] { m.fetch16(0x3001); });
    fault("MEMORY_UNMAPPED", [&] { m.read32(0); });
    fault("MEMORY_OVERFLOW", [&] { m.read32(0xffffffff); });
    fault("MEMORY_ALIGNMENT", [&] { m.map({1, 4096, Read, "bad"}); });
    fault("MEMORY_OVERLAP", [&] { m.map({0x1000, 4096, Read, "bad"}); });
    fault("MEMORY_BUDGET", [&] { m.map({0x4000, 4096, Read, "too much"}); });
    assert(m.allocated() == 12288);
    m.write32(0x2ffc, 0x11223344);
    const uint8_t input[8] = {};
    fault("MEMORY_ACCESS", [&] { m.write(0x2ffc, input, 8); });
    assert(m.read32(0x2ffc) ==
           0x11223344);  // rejected write was atomic across permission boundaries
    Memory top;
    top.map({0xfffff000, 4096, uint8_t(Read | Write), "last page"});
    top.write32(0xfffffffc, 0xabcdef01);
    assert(top.read32(0xfffffffc) == 0xabcdef01);
    assert(!top.permits(0xfffffffc, 8, Read));
    Memory init;
    uint8_t bytes[] = {1, 2, 3, 4};
    init.map({0x1000, 4096, uint8_t(Read | Execute), "initialized"}, bytes, 4);
    assert(init.fetch32(0x1000) == 0x04030201);
    assert(init.fetch16(0x1000) == 0x0201);
    auto b = executable();
    auto image = loadMachO(b.data(), b.size());
    assert(image.registers[15] == 0x1100 && image.registers[13] % 16 == 0);
    assert(image.memory->fetch32(0x1100) == 0xe3a0002a && !image.requiresDyld);
    assert(image.memory->read32(0x70000000) == 0);
    assert(!image.memory->permits(0x6ffff000, 4, Read));
    b = executable(true);
    image = loadMachO(b.data(), b.size());
    assert(image.cpsr & 0x20);
    assert(image.memory->fetch16(0x1100) == 0x202a);
    b = executable();
    put(b, 24, 0x200000);
    fault("LOAD_PIE_UNSUPPORTED", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 92, 0x5000);
    fault("LOAD_ENTRY", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 92, 0x102);
    fault("LOAD_ENTRY_ALIGNMENT", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 72, 7);
    fault("LOAD_SEGMENT_PROTECTION", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 56, 8192);
    image = loadMachO(b.data(), b.size());
    assert(image.memory->read32(0x2000) == 0);
    b = executable();
    fault("MEMORY_BUDGET", [&] { loadMachO(b.data(), b.size(), 4096); });
    b = executable();
    put(b, 84, 0xffffffff);
    fault("LOAD_COMMAND_UNSUPPORTED", [&] { loadMachO(b.data(), b.size()); });
    // ARM_THREAD_STATE PC/CPSR, exact bounds and duplicate entries.
    b = executable();
    b.resize(4096);
    put(b, 20, 140);
    put(b, 84, 5);
    put(b, 88, 84);
    put(b, 92, 1);
    put(b, 96, 17);
    std::fill(b.begin() + 100, b.begin() + 168, 0);
    put(b, 160, 0x1100);
    put(b, 164, 0x10);
    image = loadMachO(b.data(), b.size());
    assert(image.registers[15] == 0x1100);
    put(b, 164, 0x13);
    fault("LOAD_CPSR", [&] { loadMachO(b.data(), b.size()); });
    // Mutate valid headers and command bytes with a low allocation budget. Never execute fuzz data.
    std::mt19937 random(9032);
    for (int i = 0; i < 10000; i++) {
        b = executable();
        for (int j = 0; j < 4; j++) b[random() % 192] = uint8_t(random());
        try {
            (void)loadMachO(b.data(), b.size(), 2 * 1024 * 1024);
        } catch (const Error&) {
        }
    }
    std::cout << "Memory/loader regressions + 10,000 mutated Mach-O inputs passed\n";
}
