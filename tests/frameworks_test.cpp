#include "runtime/frameworks.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <map>

#include "runtime/error.h"
using namespace podnative::runtime;
template <class F>
void fails(const char* code, F f) {
    try {
        f();
        assert(false);
    } catch (const Error& e) {
        if (e.code != code) std::cerr << "Expected " << code << ", got " << e.what() << '\n';
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

    cpu = {};
    assert(cf.invoke("_CFRunLoopGetCurrent", cpu, memory));
    const uint32_t runLoop = cpu.registers[0];
    assert(runLoop && cf.liveObjects() == 1);
    assert(cf.invoke("_CFRunLoopGetMain", cpu, memory) && cpu.registers[0] == runLoop);
    cf.invoke("_CFRetain", cpu, memory);
    cf.invoke("_CFRelease", cpu, memory);
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 0);

    // CFArray callbacks retain/release real session-owned objects, while a NULL callback
    // descriptor remains a non-owning pointer array. Values use hooks just as Objective-C
    // Foundation objects do in Services.
    std::map<uint32_t, int> externalReferences;
    cf.setTypeArrayCallbacksAddress(0x3000);
    cf.setExternalObjectHooks([](uint32_t value) { return value == 0x7000 || value == 0x7004; },
                              [&](uint32_t value) { ++externalReferences[value]; },
                              [&](uint32_t value) { --externalReferences[value]; },
                              [](uint32_t left, uint32_t right) { return left == right; });
    cpu.registers[0] = 0;  // allocator argument is unused by the type callback
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayRetainValueCallBack", cpu, memory);
    assert(cpu.registers[0] == 0x7000 && externalReferences[0x7000] == 1);
    cpu.registers[0] = 0;
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayReleaseValueCallBack", cpu, memory);
    assert(externalReferences[0x7000] == 0);
    memory.write32(0x1000, 0x7000);
    memory.write32(0x1004, 0x7004);
    cpu.registers[0] = 0;       // default allocator
    cpu.registers[1] = 2;       // requested capacity, not an initial element count
    cpu.registers[2] = 0x3000;  // kCFTypeArrayCallBacks
    cf.invoke("_CFArrayCreateMutable", cpu, memory);
    const uint32_t array = cpu.registers[0];
    assert(array && externalReferences[0x7000] == 0 && externalReferences[0x7004] == 0);
    cpu.registers[0] = array;
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayAppendValue", cpu, memory);
    cpu.registers[0] = array;
    cpu.registers[1] = 0x7004;
    cf.invoke("_CFArrayAppendValue", cpu, memory);
    assert(externalReferences[0x7000] == 1 && externalReferences[0x7004] == 1);
    cpu.registers[0] = array;
    cf.invoke("_CFArrayGetCount", cpu, memory);
    assert(cpu.registers[0] == 2);
    cpu.registers[0] = array;
    cpu.registers[1] = 1;
    cf.invoke("_CFArrayGetValueAtIndex", cpu, memory);
    assert(cpu.registers[0] == 0x7004);
    cpu.registers[0] = array;
    cpu.registers[1] = 1;       // CFRange.location
    cpu.registers[2] = 1;       // CFRange.length
    cpu.registers[3] = 0x1020;  // output pointer follows the by-value range
    cf.invoke("_CFArrayGetValues", cpu, memory);
    assert(memory.read32(0x1020) == 0x7004);
    cpu.registers[0] = array;
    cpu.registers[1] = 2;
    cpu.registers[2] = 1;
    cpu.registers[3] = 0x1024;
    fails("CF_ARRAY_RANGE", [&] { cf.invoke("_CFArrayGetValues", cpu, memory); });
    cpu.registers[0] = array;
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayAppendValue", cpu, memory);
    assert(externalReferences[0x7000] == 2);
    cpu.registers[0] = array;
    cpu.registers[1] = 1;
    cpu.registers[2] = 0x7004;
    cf.invoke("_CFArrayInsertValueAtIndex", cpu, memory);
    assert(externalReferences[0x7004] == 2);
    cpu.registers[0] = array;
    cpu.registers[1] = 0x7004;
    cf.invoke("_CFArrayEqualValueCallBack", cpu, memory);
    assert(cpu.registers[0] == 0);
    cpu.registers[0] = 0x7000;
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayEqualValueCallBack", cpu, memory);
    assert(cpu.registers[0] == 1);
    cpu.registers[0] = array;
    cpu.registers[1] = 0;
    cpu.registers[2] = 0x7004;
    cf.invoke("_CFArraySetValueAtIndex", cpu, memory);
    assert(externalReferences[0x7000] == 1 && externalReferences[0x7004] == 3);
    cpu.registers[0] = array;
    cpu.registers[1] = 1;
    cf.invoke("_CFArrayRemoveValueAtIndex", cpu, memory);
    assert(externalReferences[0x7004] == 2);
    cpu.registers[0] = array;
    cf.invoke("_CFArrayRemoveAllValues", cpu, memory);
    assert(externalReferences[0x7000] == 0 && externalReferences[0x7004] == 0);
    cpu.registers[0] = array;
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 0);

    // The immutable constructor takes values/count/callbacks, while the two copy APIs have
    // different ABIs: CFArrayCreateCopy takes its source in r1; MutableCopy also takes capacity.
    cpu.registers[0] = 0;
    cpu.registers[1] = 0x1000;
    cpu.registers[2] = 2;
    cpu.registers[3] = 0x3000;
    cf.invoke("_CFArrayCreate", cpu, memory);
    const uint32_t immutableArray = cpu.registers[0];
    assert(immutableArray && externalReferences[0x7000] == 1 && externalReferences[0x7004] == 1);
    cpu.registers[0] = immutableArray;
    cpu.registers[1] = 0x7000;
    fails("CF_IMMUTABLE", [&] { cf.invoke("_CFArrayAppendValue", cpu, memory); });
    cpu.registers[0] = 0;
    cpu.registers[1] = immutableArray;
    cf.invoke("_CFArrayCreateCopy", cpu, memory);
    const uint32_t immutableCopy = cpu.registers[0];
    assert(immutableCopy && externalReferences[0x7000] == 2 && externalReferences[0x7004] == 2);
    cpu.registers[0] = 0;
    cpu.registers[1] = 4;
    cpu.registers[2] = immutableArray;
    cf.invoke("_CFArrayCreateMutableCopy", cpu, memory);
    const uint32_t mutableCopy = cpu.registers[0];
    assert(mutableCopy && externalReferences[0x7000] == 3 && externalReferences[0x7004] == 3);
    cpu.registers[0] = mutableCopy;
    cpu.registers[1] = 0x7000;
    cf.invoke("_CFArrayAppendValue", cpu, memory);
    cpu.registers[0] = mutableCopy;
    cf.invoke("_CFRelease", cpu, memory);
    cpu.registers[0] = immutableCopy;
    cf.invoke("_CFRelease", cpu, memory);
    cpu.registers[0] = immutableArray;
    cf.invoke("_CFRelease", cpu, memory);
    assert(externalReferences[0x7000] == 0 && externalReferences[0x7004] == 0);
    assert(cf.liveObjects() == 0);

    cpu.registers[0] = 0;
    cpu.registers[1] = 0;
    cpu.registers[2] = 0;
    cpu.registers[3] = 0;
    cf.invoke("_CFArrayCreateMutable", cpu, memory);
    const uint32_t nonOwningArray = cpu.registers[0];
    cpu.registers[0] = nonOwningArray;
    cpu.registers[1] = 0xdeadbeef;  // intentionally not a retained CFType value
    cf.invoke("_CFArrayAppendValue", cpu, memory);
    cpu.registers[0] = nonOwningArray;
    cpu.registers[1] = 0;
    cf.invoke("_CFArrayGetValueAtIndex", cpu, memory);
    assert(cpu.registers[0] == 0xdeadbeef);
    cpu.registers[0] = nonOwningArray;
    cf.invoke("_CFRelease", cpu, memory);
    assert(cf.liveObjects() == 0);

    // A failed retaining constructor rolls back exactly the elements it already retained.
    memory.write32(0x1000, 0x7000);
    memory.write32(0x1004, 0x7008);  // not a live object in the hook's ownership table
    const int referencesBeforeFailure = externalReferences[0x7000];
    const size_t objectsBeforeFailure = cf.liveObjects();
    cpu.registers[0] = 0;
    cpu.registers[1] = 0x1000;
    cpu.registers[2] = 2;
    cpu.registers[3] = 0x3000;
    fails("CF_ARRAY_VALUE", [&] { cf.invoke("_CFArrayCreate", cpu, memory); });
    assert(externalReferences[0x7000] == referencesBeforeFailure);
    assert(cf.liveObjects() == objectsBeforeFailure);

    cpu.registers[0] = 0;
    cpu.registers[1] = 0xffffffff;
    cpu.registers[2] = 8;
    fails("MEMORY_UNMAPPED", [&] { cf.invoke("_CFBitVectorCreate", cpu, memory); });
    std::cout
        << "CoreFoundation bit-vector ownership/ABI and CoreGraphics affine semantics passed\n";
}
