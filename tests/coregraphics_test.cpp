#include "runtime/coregraphics.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "runtime/abi.h"
#include "runtime/error.h"
using namespace podnative::runtime;
namespace {
constexpr uint32_t Stack = 0x8800;  // inside the mapped scratch page
struct Call {
    Memory& memory;
    CoreGraphics& cg;
    CpuState state;
    Call(Memory& m, CoreGraphics& g) : memory(m), cg(g) {
        state.registers[13] = Stack;
        state.cpsr = 0x10;
    }
    void word(size_t index, uint32_t value) { state.registers[index] = value; }
    void f32(size_t index, float value) { state.registers[index] = floatBits(value); }
    void stackFloat(size_t slot, float value) {
        memory.write32(Stack + uint32_t(slot * 4), floatBits(value));
    }
    void stackWord(size_t slot, uint32_t value) {
        memory.write32(Stack + uint32_t(slot * 4), value);
    }
    void invoke(const char* name) {
        if (!cg.invoke(name, state, memory))
            throw Error("TEST_SYMBOL", std::string("unimplemented ") + name);
    }
    float resultFloat() const { return bitsFloat(state.registers[0]); }
    Rect resultRect() const {
        Rect rect{};
        auto* bytes = reinterpret_cast<uint8_t*>(&rect);
        for (size_t index = 0; index < 4; index++)
            std::memcpy(bytes + index * 4, &state.registers[index], 4);
        return rect;
    }
    Affine readAffine(uint32_t address) const {
        Affine value{};
        auto* bytes = reinterpret_cast<uint8_t*>(&value);
        for (size_t index = 0; index < 6; index++) {
            const uint32_t part = memory.read32(address + uint32_t(index * 4));
            std::memcpy(bytes + index * 4, &part, 4);
        }
        return value;
    }
};
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}  // namespace
int main() {
    Memory memory;
    memory.map({0x8000, Memory::PageSize, Read | Write, "cg scratch"});
    CoreGraphics cg;
    Call call(memory, cg);

    // CGRect is four words and travels in r0-r3 in both directions.
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetMidX");
    assert(near(call.resultFloat(), 25));
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetMidY");
    assert(near(call.resultFloat(), 40));

    call.state = {};
    call.state.registers[13] = Stack;
    call.f32(0, 0);
    call.f32(1, 0);
    call.f32(2, 100);
    call.f32(3, 50);
    call.stackFloat(0, 5);
    call.stackFloat(1, 10);
    call.invoke("_CGRectInset");
    Rect inset = call.resultRect();
    assert(near(inset.x, 5) && near(inset.y, 10) && near(inset.width, 90) &&
           near(inset.height, 30));

    // Equality compares two four-word rectangles: r0-r3 and the first four stack slots.
    call.state = {};
    call.state.registers[13] = Stack;
    call.f32(0, 1);
    call.f32(1, 2);
    call.f32(2, 3);
    call.f32(3, 4);
    call.stackFloat(0, 1);
    call.stackFloat(1, 2);
    call.stackFloat(2, 3);
    call.stackFloat(3, 4);
    call.invoke("_CGRectEqualToRect");
    assert(call.state.registers[0] == 1);
    call.stackFloat(3, 5);
    call.invoke("_CGRectEqualToRect");
    assert(call.state.registers[0] == 0);

    // CGAffineTransform is six words, so the caller supplies a hidden result pointer.
    call.state = {};
    call.state.registers[13] = Stack;
    call.word(0, 0x8000);
    call.word(1, floatBits(3));
    call.word(2, floatBits(4));
    call.invoke("_CGAffineTransformMakeTranslation");
    Affine translated = call.readAffine(0x8000);
    assert(near(translated.a, 1) && near(translated.d, 1) && near(translated.tx, 3) &&
           near(translated.ty, 4));

    // Translate by value: base transform in r1-r3 plus stack, then the two floats.
    call.state = {};
    call.state.registers[13] = Stack;
    call.word(0, 0x8000);
    call.word(1, floatBits(2));  // a
    call.word(2, floatBits(0));  // b
    call.word(3, floatBits(0));  // c
    call.stackFloat(0, 2);       // d
    call.stackFloat(1, 0);       // tx
    call.stackFloat(2, 0);       // ty
    call.stackFloat(3, 5);       // translate x
    call.stackFloat(4, 6);       // translate y
    call.invoke("_CGAffineTransformTranslate");
    Affine moved = call.readAffine(0x8000);
    assert(near(moved.a, 2) && near(moved.d, 2) && near(moved.tx, 5) && near(moved.ty, 6));

    // Applying a transform to a rectangle returns its bounding box in r0-r3.
    call.state = {};
    call.state.registers[13] = Stack;
    call.f32(0, 0);
    call.f32(1, 0);
    call.f32(2, 10);
    call.f32(3, 10);
    // Identity transform by value: 6 words on the stack.
    call.stackFloat(0, 1);
    call.stackFloat(1, 0);
    call.stackFloat(2, 0);
    call.stackFloat(3, 1);
    call.stackFloat(4, 5);
    call.stackFloat(5, 5);
    call.invoke("_CGRectApplyAffineTransform");
    Rect applied = call.resultRect();
    assert(near(applied.x, 5) && near(applied.y, 5) && near(applied.width, 10) &&
           near(applied.height, 10));

    // Media time is monotonic in seconds and returns a double in r0:r1.
    call.state = {};
    call.state.registers[13] = Stack;
    call.invoke("_CACurrentMediaTime");
    const double first =
        bitsDouble(uint64_t(call.state.registers[0]) | uint64_t(call.state.registers[1]) << 32);
    call.invoke("_CACurrentMediaTime");
    const double second =
        bitsDouble(uint64_t(call.state.registers[0]) | uint64_t(call.state.registers[1]) << 32);
    assert(first >= 0 && second >= first);

    // Colour spaces are owned handles with real reference counting.
    call.state = {};
    call.invoke("_CGColorSpaceCreateDeviceRGB");
    const uint32_t rgb = call.state.registers[0];
    call.invoke("_CGColorSpaceCreateDeviceGray");
    const uint32_t gray = call.state.registers[0];
    assert(rgb && gray && rgb != gray && cg.liveColorSpaces() == 2);
    call.state = {};
    call.word(0, rgb);
    call.invoke("_CGColorSpaceRetain");
    assert(call.state.registers[0] == 2);
    call.state = {};
    call.word(0, rgb);
    call.invoke("_CGColorSpaceRelease");
    call.invoke("_CGColorSpaceRelease");
    assert(cg.liveColorSpaces() == 1);
    call.state = {};
    call.word(0, rgb);
    try {
        call.invoke("_CGColorSpaceRelease");
        std::cerr << "released colour space was accepted\n";
        return 1;
    } catch (const Error& e) {
        assert(e.code == "CG_COLORSPACE");
    }

    // Unknown entry points report "not mine" so dispatch can keep looking.
    CpuState unknown;
    unknown.registers[13] = Stack;
    assert(!cg.invoke("_CGBitmapContextCreate", unknown, memory));

    std::cout << "CoreGraphics geometry, media time and colour-space lifetime passed\n";
    return 0;
}
