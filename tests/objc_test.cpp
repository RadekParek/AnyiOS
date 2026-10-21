#include "runtime/objc.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/abi.h"
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

    // Foundation collections own their elements on the host, addressable only by guest
    // object pointers, and every index is bounds checked.
    const uint32_t arrayClass = harness.call(getClass, harness.text("NSMutableArray"), 0);
    const uint32_t array = harness.call(msgSend, arrayClass, harness.text("array"));
    assert(harness.call(msgSend, array, harness.text("count")) == 0);
    harness.call(msgSend, array, harness.text("addObject:"), hello);
    harness.call(msgSend, array, harness.text("addObject:"), nsobject);
    assert(harness.call(msgSend, array, harness.text("count")) == 2);
    assert(harness.call(msgSend, array, harness.text("objectAtIndex:"), 0) == hello);
    assert(harness.call(msgSend, array, harness.text("lastObject")) == nsobject);
    assert(harness.call(msgSend, array, harness.text("containsObject:"), hello) == 1);
    assert(harness.call(msgSend, array, harness.text("indexOfObject:"), nsobject) == 1);
    fault("OBJC_RANGE", [&] { harness.call(msgSend, array, harness.text("objectAtIndex:"), 2); });
    harness.call(msgSend, array, harness.text("removeObjectAtIndex:"), 0);
    assert(harness.call(msgSend, array, harness.text("count")) == 1);

    // Formatted Foundation strings use the same formatter as guest printf.
    const uint32_t formatted = [&] {
        CpuState state;
        state.registers[0] = harness.call(getClass, harness.text("NSString"), 0);
        state.registers[1] = harness.text("stringWithFormat:");
        state.registers[2] = harness.text("%@-%d-%s");
        state.registers[3] = hello;
        state.registers[13] = 0x3000;
        const uint64_t aligned = 0;
        (void)aligned;
        memory.write32(0x3000, 7);
        memory.write32(0x3004, harness.text("ok"));
        assert(harness.objc.dispatch(harness.gate(0), state));
        return state.registers[0];
    }();
    assert(harness.call(msgSend, formatted, harness.text("length")) > 0);

    // Dictionaries round-trip objects by key and expose their keys.
    const uint32_t dictionaryClass = harness.call(getClass, harness.text("NSMutableDictionary"), 0);
    const uint32_t dictionary = harness.call(msgSend, dictionaryClass, harness.text("dictionary"));
    {
        CpuState state;
        state.registers[0] = dictionary;
        state.registers[1] = harness.text("setObject:forKey:");
        state.registers[2] = hello;
        state.registers[3] = nsobject;
        assert(harness.objc.dispatch(harness.gate(0), state));
    }
    assert(harness.call(msgSend, dictionary, harness.text("count")) == 1);
    assert(harness.call(msgSend, dictionary, harness.text("objectForKey:"), nsobject) == hello);
    assert(harness.call(msgSend, dictionary, harness.text("objectForKey:"), hello) == 0);
    const uint32_t keys = harness.call(msgSend, dictionary, harness.text("allKeys"));
    assert(harness.call(msgSend, keys, harness.text("count")) == 1);

    // NSData is a real byte buffer in guest memory.
    const uint32_t dataClass = harness.call(getClass, harness.text("NSData"), 0);
    {
        CpuState state;
        state.registers[0] = dataClass;
        state.registers[1] = harness.text("dataWithBytes:length:");
        state.registers[2] = harness.text("ABCD");
        state.registers[3] = 4;
        assert(harness.objc.dispatch(harness.gate(0), state));
        const uint32_t data = state.registers[0];
        assert(harness.call(msgSend, data, harness.text("length")) == 4);
        const uint32_t bytes = harness.call(msgSend, data, harness.text("bytes"));
        assert(memory.read32(bytes) == 0x44434241);
    }

    // NSNumber keeps integer and floating-point payloads separately.
    const uint32_t numberClass = harness.call(getClass, harness.text("NSNumber"), 0);
    const uint32_t number = harness.call(msgSend, numberClass, harness.text("numberWithInt:"), 41);
    assert(harness.call(msgSend, number, harness.text("intValue")) == 41);
    {
        // Doubles come back in r0:r1 under the iOS ARMv7 base ABI.
        CpuState state;
        state.registers[0] = number;
        state.registers[1] = harness.text("doubleValue");
        assert(harness.objc.dispatch(harness.gate(0), state));
        const double value =
            bitsDouble(uint64_t(state.registers[0]) | uint64_t(state.registers[1]) << 32);
        assert(value == 41.0);
    }

    // Unimplemented selectors and bogus receivers must fail, never return a fake success.
    fault("OBJC_UNIMPLEMENTED",
          [&] { harness.call(msgSend, hello, harness.text("drawInRect:"), 0); });
    fault("OBJC_RECEIVER", [&] { harness.call(msgSend, 0x1234, harness.text("length")); });
    std::cout << "Objective-C classes, collections, formatting, dispatch, strings and pools "
                 "passed\n";
}
