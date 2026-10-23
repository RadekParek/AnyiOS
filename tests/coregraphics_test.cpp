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
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetWidth");
    assert(near(call.resultFloat(), 30));
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetHeight");
    assert(near(call.resultFloat(), 40));
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetMinX");
    assert(near(call.resultFloat(), 10));
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.invoke("_CGRectGetMinY");
    assert(near(call.resultFloat(), 20));

    // CGRectContainsPoint receives the four-word rect in r0-r3 and CGPoint on the stack.
    call.state = {};
    call.state.registers[13] = Stack;
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.stackFloat(0, 15);
    call.stackFloat(1, 25);
    call.invoke("_CGRectContainsPoint");
    assert(call.state.registers[0] == 1);
    call.stackFloat(0, 50);
    call.invoke("_CGRectContainsPoint");
    assert(call.state.registers[0] == 0);

    // CGRectOffset translates the origin and preserves the extent.
    call.state = {};
    call.state.registers[13] = Stack;
    call.f32(0, 10);
    call.f32(1, 20);
    call.f32(2, 30);
    call.f32(3, 40);
    call.stackFloat(0, 5);
    call.stackFloat(1, -2);
    call.invoke("_CGRectOffset");
    Rect offset = call.resultRect();
    assert(near(offset.x, 15) && near(offset.y, 18) && near(offset.width, 30) &&
           near(offset.height, 40));

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

    call.state = {};
    call.state.registers[13] = Stack;
    call.word(0, 0x8000);
    call.word(1, floatBits(static_cast<float>(M_PI / 2)));
    call.invoke("_CGAffineTransformMakeRotation");
    Affine rotated = call.readAffine(0x8000);
    assert(near(rotated.a, 0) && near(rotated.b, 1) && near(rotated.c, -1) && near(rotated.d, 0));

    call.state = {};
    call.state.registers[13] = Stack;
    call.word(0, 0x8000);
    call.word(1, floatBits(1));
    call.word(2, floatBits(0));
    call.word(3, floatBits(0));
    call.stackFloat(0, 1);
    call.stackFloat(1, 0);
    call.stackFloat(2, 0);
    call.stackFloat(3, static_cast<float>(M_PI / 2));
    call.invoke("_CGAffineTransformRotate");
    rotated = call.readAffine(0x8000);
    assert(near(rotated.a, 0) && near(rotated.b, 1) && near(rotated.c, -1) && near(rotated.d, 0));

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

    // CATransform3D is sixteen floats returned through a hidden pointer.
    {
        CpuState rotate;
        rotate.registers[0] = 0x8000;  // hidden result pointer
        rotate.registers[1] = floatBits(static_cast<float>(M_PI / 2));
        rotate.registers[2] = floatBits(0);
        rotate.registers[3] = floatBits(0);
        rotate.registers[13] = Stack;
        memory.write32(Stack, floatBits(1));  // z axis, fifth argument on the stack
        assert(cg.invoke("_CATransform3DMakeRotation", rotate, memory));
        // 90 degrees about z maps x to y: m[0] ~ 0, m[1] ~ 1.
        assert(near(bitsFloat(memory.read32(0x8000)), 0));
        assert(near(bitsFloat(memory.read32(0x8004)), 1));
        assert(near(bitsFloat(memory.read32(0x803C)), 1));  // m[15] stays 1
        CpuState scale;
        scale.registers[0] = 0x8100;
        scale.registers[1] = floatBits(2);
        scale.registers[2] = floatBits(3);
        scale.registers[3] = floatBits(4);
        scale.registers[13] = Stack;
        assert(cg.invoke("_CATransform3DScale", scale, memory));
        assert(near(bitsFloat(memory.read32(0x8100)), 2));
        assert(near(bitsFloat(memory.read32(0x8114)), 3));
        assert(near(bitsFloat(memory.read32(0x8128)), 4));

        CpuState translate;
        translate.registers[0] = 0x8200;
        translate.registers[1] = floatBits(3);
        translate.registers[2] = floatBits(4);
        translate.registers[3] = floatBits(5);
        translate.registers[13] = Stack;
        assert(cg.invoke("_CATransform3DMakeTranslation", translate, memory));
        assert(near(bitsFloat(memory.read32(0x8230)), 3));
        assert(near(bitsFloat(memory.read32(0x8234)), 4));
        assert(near(bitsFloat(memory.read32(0x8238)), 5));

        CpuState moved3d;
        moved3d.registers[0] = 0x8300;
        moved3d.registers[1] = floatBits(1);
        moved3d.registers[2] = 0;
        moved3d.registers[3] = 0;
        moved3d.registers[13] = Stack;
        for (size_t n = 0; n < 13; ++n) memory.write32(Stack + uint32_t(n * 4), floatBits(0));
        memory.write32(Stack + 2 * 4, floatBits(1));
        memory.write32(Stack + 7 * 4, floatBits(1));
        memory.write32(Stack + 12 * 4, floatBits(1));
        memory.write32(Stack + 13 * 4, floatBits(7));
        memory.write32(Stack + 14 * 4, floatBits(8));
        memory.write32(Stack + 15 * 4, floatBits(9));
        assert(cg.invoke("_CATransform3DTranslate", moved3d, memory));
        assert(near(bitsFloat(memory.read32(0x8330)), 7));
        assert(near(bitsFloat(memory.read32(0x8334)), 8));
        assert(near(bitsFloat(memory.read32(0x8338)), 9));
    }

    // CFBundle/CFURL report the mounted sandbox path to the guest.
    {
        CoreFoundation cf;
        cf.setPaths("/var/mobile", "/var/mobile/Documents");
        CpuState state;
        assert(cf.invoke("_CFBundleGetMainBundle", state, memory));
        const uint32_t bundle = state.registers[0];
        assert(bundle != 0);
        CpuState resources;
        resources.registers[0] = bundle;
        assert(cf.invoke("_CFBundleCopyResourcesDirectoryURL", resources, memory));
        const uint32_t url = resources.registers[0];
        assert(url != 0);
        CpuState path;
        path.registers[0] = url;
        path.registers[1] = 0;
        path.registers[2] = 0x8200;
        path.registers[3] = 64;
        assert(cf.invoke("_CFURLGetFileSystemRepresentation", path, memory));
        assert(path.registers[0] == 1);
        char text[64] = {0};
        memory.read(0x8200, text, 12);
        assert(std::string(text) == "/var/mobile");
        // A buffer that is too small is reported as false, not overflowed.
        CpuState small;
        small.registers[0] = url;
        small.registers[2] = 0x8200;
        small.registers[3] = 4;
        assert(cf.invoke("_CFURLGetFileSystemRepresentation", small, memory));
        assert(small.registers[0] == 0);
        // CFData owns bytes in a guest mapping, so CFDataGetBytePtr never leaks a host address.
        const uint8_t sourceBytes[] = {0x41, 0x00, 0x42, 0xff};
        memory.write(0x8300, sourceBytes, sizeof(sourceBytes));
        CpuState createData;
        createData.registers[0] = 0;  // default allocator
        createData.registers[1] = 0x8300;
        createData.registers[2] = sizeof(sourceBytes);
        assert(cf.invoke("_CFDataCreate", createData, memory));
        const uint32_t dataHandle = createData.registers[0];
        CpuState dataLength;
        dataLength.registers[0] = dataHandle;
        assert(cf.invoke("_CFDataGetLength", dataLength, memory));
        assert(dataLength.registers[0] == sizeof(sourceBytes));
        CpuState bytePointer;
        bytePointer.registers[0] = dataHandle;
        assert(cf.invoke("_CFDataGetBytePtr", bytePointer, memory));
        uint8_t copiedBytes[sizeof(sourceBytes)] = {};
        memory.read(bytePointer.registers[0], copiedBytes, sizeof(copiedBytes));
        assert(std::memcmp(copiedBytes, sourceBytes, sizeof(sourceBytes)) == 0);
        CpuState absolute;
        assert(cf.invoke("_CFAbsoluteTimeGetCurrent", absolute, memory));
        const double cfTime =
            bitsDouble(uint64_t(absolute.registers[0]) | uint64_t(absolute.registers[1]) << 32);
        assert(cfTime > 700000000 && cfTime < 1000000000);
        CpuState encoding;
        encoding.registers[0] = 0x08000100;  // kCFStringEncodingUTF8
        assert(cf.invoke("_CFStringConvertEncodingToNSStringEncoding", encoding, memory));
        assert(encoding.registers[0] == 4);
        // A bit vector handle is not a URL.
        CpuState notUrl;
        notUrl.registers[0] = 0x7fffffff;
        notUrl.registers[2] = 0x8200;
        notUrl.registers[3] = 64;
        try {
            cf.invoke("_CFURLGetFileSystemRepresentation", notUrl, memory);
            assert(false);
        } catch (const Error& e) {
            assert(e.code == "CF_HANDLE");
        }
    }

    std::cout << "CoreGraphics geometry, media time, colour-space, CATransform3D and CFBundle "
                 "passed\n";
    return 0;
}
