#include "runtime/frameworks.h"

#include <cassert>
#include <cmath>
#include <iostream>

#include "runtime/error.h"
using namespace podnative::runtime;
template <class F>
void fails(const char* code, F f) {
    try {
        f();
        assert(false);
    } catch (const Error& e) {
        assert(e.code == code);
    }
}
int main() {
    Affine a = Affine::scale(2, 3).concat(Affine::translation(4, 5));
    auto point = a.apply(Point{1, 2});
    assert(point.x == 6 && point.y == 11);
    auto original = a.inverted().apply(point);
    assert(std::abs(original.x - 1) < 0.0001 && std::abs(original.y - 2) < 0.0001);
    auto r = Affine::rotation(3.14159265f / 2).apply(Rect{0, 0, 2, 3});
    assert(std::abs(r.width - 3) < 0.0001 && std::abs(r.height - 2) < 0.0001);
    assert(Affine::scale(0, 0).inverted().a == 0);
    Memory memory;
    memory.map({0x1000, 4096, uint8_t(Read | Write), "test bytes"});
    memory.write32(0x1000, 0x81);
    CoreFoundation cf;
    CpuState cpu;
    cpu.registers[1] = 0x1000;
    cpu.registers[2] = 8;
    assert(cf.invoke("_CFBitVectorCreate", cpu, memory));
    uint32_t immutable = cpu.registers[0];
    cpu.registers[1] = 0;
    cf.invoke("_CFBitVectorGetBitAtIndex", cpu, memory);
    assert(cpu.registers[0] == 1);  // Big-endian bit numbering in each byte.
    cpu.registers[0] = immutable;
    cpu.registers[1] = 1;
    cpu.registers[2] = 1;
    fails("CF_IMMUTABLE", [&] { cf.invoke("_CFBitVectorSetBitAtIndex", cpu, memory); });
    cpu.registers[0] = 0;
    cpu.registers[1] = 0;
    cpu.registers[2] = immutable;
    cf.invoke("_CFBitVectorCreateMutableCopy", cpu, memory);
    uint32_t copy = cpu.registers[0];
    cpu.registers[1] = 1;
    cpu.registers[2] = 1;
    cf.invoke("_CFBitVectorSetBitAtIndex", cpu, memory);
    cf.invoke("_CFBitVectorGetBitAtIndex", cpu, memory);
    assert(cpu.registers[0] == 1);
    cpu.registers[0] = immutable;
    cpu.registers[1] = 1;
    cf.invoke("_CFBitVectorGetBitAtIndex", cpu, memory);
    assert(cpu.registers[0] == 0);
    cpu.registers[0] = copy;
    cpu.registers[1] = 8;
    fails("CF_INDEX", [&] { cf.invoke("_CFBitVectorGetBitAtIndex", cpu, memory); });
    assert(!cf.invoke("_UIApplicationMain", cpu, memory));
    cpu.registers[0] = copy;
    cf.invoke("_CFRetain", cpu, memory);
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 2);
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 1);
    fails("CF_HANDLE", [&] { cf.invoke("_CFRelease", cpu, memory); });
    cpu.registers[0] = immutable;
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 0);
    cpu.registers[0] = 0;
    cpu.registers[1] = 0xffffffff;
    cpu.registers[2] = 8;
    fails("MEMORY_UNMAPPED", [&] { cf.invoke("_CFBitVectorCreate", cpu, memory); });
    std::cout
        << "CoreFoundation bit-vector ownership/ABI and CoreGraphics affine semantics passed\n";
}
