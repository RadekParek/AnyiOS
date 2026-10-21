#include "coregraphics.h"

#include <chrono>

#include "abi.h"
#include "error.h"

namespace podnative::runtime {
namespace {
// Media time is monotonic from the first request, in seconds, like CACurrentMediaTime.
double mediaTime() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point epoch = Clock::now();
    return std::chrono::duration<double>(Clock::now() - epoch).count();
}
uint32_t createColorSpace(std::map<uint32_t, uint32_t>& spaces, uint32_t& next, const char* label) {
    if (spaces.size() >= 64) throw Error("CG_BUDGET", std::string("too many ") + label);
    const uint32_t handle = next;
    next += 16;
    spaces.emplace(handle, 1);
    return handle;
}
uint32_t retainColorSpace(std::map<uint32_t, uint32_t>& spaces, uint32_t handle,
                          const char* label) {
    auto it = spaces.find(handle);
    if (it == spaces.end()) throw Error("CG_COLORSPACE", std::string("unknown ") + label, handle);
    if (it->second == UINT32_MAX) throw Error("CG_RETAIN", "colour space retain overflow");
    return ++it->second;
}
}  // namespace

bool CoreGraphics::invoke(const std::string& symbol, CpuState& cpu, Memory& memory) {
    Arguments args(cpu, memory);
    if (symbol == "_CGAffineTransformMakeTranslation") {
        // CGAffineTransform is six words, so the caller passes a hidden result pointer
        // in r0 and the two floats follow it.
        Arguments inputs(cpu, memory, 1);
        // Read in order: the two reads advance one cursor.
        const float tx = inputs.f32();
        const float ty = inputs.f32();
        const Affine transform = Affine::translation(tx, ty);
        storeThroughPointer(memory, resultPointer(cpu), transform);
        return true;
    }
    if (symbol == "_CGAffineTransformTranslate" || symbol == "_CGAffineTransformScale") {
        Arguments inputs(cpu, memory, 1);
        const Affine base = inputs.aggregate<Affine>();
        const float first = inputs.f32(), second = inputs.f32();
        const Affine delta = symbol == "_CGAffineTransformTranslate"
                                 ? Affine::translation(first, second)
                                 : Affine::scale(first, second);
        storeThroughPointer(memory, resultPointer(cpu), base.concat(delta));
        return true;
    }
    if (symbol == "_CGRectApplyAffineTransform") {
        const Rect rect = args.aggregate<Rect>();
        const Affine transform = args.aggregate<Affine>();
        resultAggregate(cpu, transform.apply(rect));
        return true;
    }
    if (symbol == "_CGRectEqualToRect") {
        const Rect first = args.aggregate<Rect>();
        const Rect second = args.aggregate<Rect>();
        resultBool(cpu, first.x == second.x && first.y == second.y && first.width == second.width &&
                            first.height == second.height);
        return true;
    }
    if (symbol == "_CGRectGetMidX" || symbol == "_CGRectGetMidY") {
        const Rect rect = args.aggregate<Rect>();
        resultF32(cpu,
                  symbol == "_CGRectGetMidX" ? rect.x + rect.width / 2 : rect.y + rect.height / 2);
        return true;
    }
    if (symbol == "_CGRectInset") {
        const Rect rect = args.aggregate<Rect>();
        const float dx = args.f32(), dy = args.f32();
        resultAggregate(cpu,
                        Rect{rect.x + dx, rect.y + dy, rect.width - 2 * dx, rect.height - 2 * dy});
        return true;
    }
    if (symbol == "_CATransform3DMakeRotation") {
        // CATransform3D is sixteen floats, so the caller passes a hidden result pointer in
        // r0 and (angle, x, y, z) follow in r1-r3 and on the stack.
        Arguments inputs(cpu, memory, 1);
        const float radians = inputs.f32(), x = inputs.f32(), y = inputs.f32(), z = inputs.f32();
        storeThroughPointer(memory, resultPointer(cpu), CATransform3D::rotation(radians, x, y, z));
        return true;
    }
    if (symbol == "_CATransform3DScale") {
        Arguments inputs(cpu, memory, 1);
        const float x = inputs.f32(), y = inputs.f32(), z = inputs.f32();
        storeThroughPointer(memory, resultPointer(cpu), CATransform3D::scale(x, y, z));
        return true;
    }
    if (symbol == "_CACurrentMediaTime") {
        resultF64(cpu, mediaTime());
        return true;
    }
    if (symbol == "_CGColorSpaceCreateDeviceRGB") {
        resultWord(cpu, 0, createColorSpace(colorSpaces, next, "device RGB colour spaces"));
        return true;
    }
    if (symbol == "_CGColorSpaceCreateDeviceGray") {
        resultWord(cpu, 0, createColorSpace(colorSpaces, next, "device gray colour spaces"));
        return true;
    }
    if (symbol == "_CGColorSpaceRetain") {
        resultWord(cpu, 0, retainColorSpace(colorSpaces, cpu.registers[0], "colour space"));
        return true;
    }
    if (symbol == "_CGColorSpaceRelease") {
        auto it = colorSpaces.find(cpu.registers[0]);
        if (it == colorSpaces.end())
            throw Error("CG_COLORSPACE", "unknown colour space", cpu.registers[0]);
        if (!--it->second) colorSpaces.erase(it);
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
