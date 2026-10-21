#include "runtime/objc.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/error.h"
#include "runtime/exports.h"
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
struct Harness {
    Memory memory;
    ObjC objc{memory};
    std::vector<uint8_t> scratch = std::vector<uint8_t>(16384, 0);
    Harness() {
        objc.installGates();
        memory.map({0x1000, 16384, uint8_t(Read | Write), "test strings"}, scratch.data(),
                   scratch.size());
    }
    uint32_t text(const std::string& value) {
        static uint32_t next = 0x1000;
        const uint32_t address = next;
        memory.write(address, value.data(), value.size());
        const char terminator = 0;
        memory.write(address + uint32_t(value.size()), &terminator, 1);
        next += uint32_t(value.size()) + 1;
        return address;
    }
    uint32_t call(uint32_t gate, uint32_t self, uint32_t selector, uint32_t argument = 0) {
        CpuState state;
        state.registers[0] = self;
        state.registers[1] = selector;
        state.registers[2] = argument;
        assert(objc.dispatch(gate, state));
        return state.registers[0];
    }
    uint32_t gate(size_t index) const { return ObjC::GateBase + uint32_t(index) * 8; }
};
}  // namespace
int main() {
    Harness harness;
    auto& memory = harness.memory;
    ExportGraph graph;
    size_t foundation = graph.addImage("Foundation");
    harness.objc.defineSymbols(graph, foundation);
    assert(graph.resolve(0, -2, "_objc_msgSend") == ObjC::GateBase);
    assert(graph.resolve(0, -2, "_OBJC_CLASS_$_NSString").has_value());

    const uint32_t selString = harness.text("stringWithUTF8String:");
    const uint32_t getClass = harness.gate(1);
    const uint32_t registerName = harness.gate(2);
    const uint32_t msgSend = harness.gate(0);
    const uint32_t retain = harness.gate(3);
    const uint32_t release = harness.gate(4);

    const uint32_t nsstring = harness.call(getClass, harness.text("NSString"), 0);
    const uint32_t nsobject = harness.call(getClass, harness.text("NSObject"), 0);
    assert(nsstring && nsobject && nsstring != nsobject);
    assert(harness.call(getClass, harness.text("NoSuchClass"), 0) == 0);
    // A selector is just its interned guest string.
    const uint32_t lengthSelector = harness.text("length");
    assert(harness.call(registerName, lengthSelector, 0) == lengthSelector);

    const uint32_t hello = harness.call(msgSend, nsstring, selString, harness.text("AnyiOS"));
    assert(hello);
    assert(memory.read32(hello) == nsstring);  // isa
    assert(harness.call(msgSend, hello, harness.text("length")) == 6);
    const uint32_t utf8 = harness.call(msgSend, hello, harness.text("UTF8String"));
    assert(harness.call(msgSend, hello, harness.text("hash")) != 0);

    const uint32_t other = harness.call(msgSend, nsstring, selString, harness.text("AnyiOS"));
    const uint32_t different = harness.call(msgSend, nsstring, selString, harness.text("anyios"));
    assert(harness.call(msgSend, hello, harness.text("isEqualToString:"), other) == 1);
    assert(harness.call(msgSend, hello, harness.text("isEqualToString:"), different) == 0);
    const uint32_t expectedUtf8 =
        uint32_t('A') | (uint32_t('n') << 8) | (uint32_t('y') << 16) | (uint32_t('i') << 24);
    assert(memory.read32(utf8) == expectedUtf8);

    // Retain/release accounting is per object.
    assert(harness.call(retain, hello, 0) == hello);
    assert(harness.call(msgSend, hello, harness.text("retainCount")) == 2);
    assert(harness.call(release, hello, 0) == hello);
    assert(harness.call(msgSend, hello, harness.text("retainCount")) == 1);

    // An autorelease pool owns and releases anything autoreleased into it.
    const uint32_t poolClass = harness.call(getClass, harness.text("NSAutoreleasePool"), 0);
    const uint32_t pool = harness.call(
        msgSend, harness.call(msgSend, poolClass, harness.text("alloc")), harness.text("init"));
    const uint32_t object = harness.call(msgSend, nsobject, harness.text("alloc"));
    harness.call(msgSend, object, harness.text("autorelease"));
    harness.call(msgSend, pool, harness.text("drain"));
    // Draining the pool released the object, so a later message must not silently succeed.
    fault("OBJC_RECEIVER", [&] { harness.call(msgSend, object, harness.text("retain")); });
    const uint32_t stray = harness.call(msgSend, nsobject, harness.text("alloc"));
    fault("OBJC_POOL", [&] { harness.call(msgSend, stray, harness.text("autorelease")); });

    // Unimplemented selectors and bogus receivers must fail, never return a fake success.
    fault("OBJC_UNIMPLEMENTED",
          [&] { harness.call(msgSend, hello, harness.text("drawInRect:"), 0); });
    fault("OBJC_RECEIVER", [&] { harness.call(msgSend, 0x1234, harness.text("length")); });
    std::cout << "Objective-C classes, selectors, message dispatch, strings and pools passed\n";
}
