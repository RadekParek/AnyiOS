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
CATransform3D concat(const CATransform3D& left, const CATransform3D& right) {
    CATransform3D result;
    for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row) {
            result.m[column * 4 + row] = 0;
            for (size_t k = 0; k < 4; ++k)
                result.m[column * 4 + row] += left.m[k * 4 + row] * right.m[column * 4 + k];
        }
    return result;
}
}  // namespace

bool CoreGraphics::invoke(const std::string& symbol, CpuState& cpu, Memory& memory) {
    Arguments args(cpu, memory);
    if (symbol == "_CGAffineTransformMakeTranslation" ||
        symbol == "_CGAffineTransformMakeRotation") {
        // CGAffineTransform is six words, so the caller passes a hidden result pointer in r0.
        Arguments inputs(cpu, memory, 1);
        Affine transform;
        if (symbol == "_CGAffineTransformMakeTranslation") {
            const float tx = inputs.f32(), ty = inputs.f32();
            transform = Affine::translation(tx, ty);
        } else {
            transform = Affine::rotation(inputs.f32());
        }
        storeThroughPointer(memory, resultPointer(cpu), transform);
        return true;
    }
    if (symbol == "_CGAffineTransformTranslate" || symbol == "_CGAffineTransformRotate" ||
        symbol == "_CGAffineTransformScale") {
        Arguments inputs(cpu, memory, 1);
        const Affine base = inputs.aggregate<Affine>();
        const float first = inputs.f32();
        Affine delta;
        if (symbol == "_CGAffineTransformRotate") {
            delta = Affine::rotation(first);
        } else {
            const float second = inputs.f32();
            delta = symbol == "_CGAffineTransformTranslate" ? Affine::translation(first, second)
                                                            : Affine::scale(first, second);
        }
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
    if (symbol == "_CGRectContainsPoint") {
        const Rect rect = args.aggregate<Rect>();
        const Point point = args.aggregate<Point>();
        resultBool(cpu, point.x >= rect.x && point.y >= rect.y && point.x <= rect.x + rect.width &&
                            point.y <= rect.y + rect.height);
        return true;
    }
    if (symbol == "_CGRectGetHeight" || symbol == "_CGRectGetWidth" || symbol == "_CGRectGetMinX" ||
        symbol == "_CGRectGetMinY" || symbol == "_CGRectGetMidX" || symbol == "_CGRectGetMidY") {
        const Rect rect = args.aggregate<Rect>();
        if (symbol == "_CGRectGetHeight")
            resultF32(cpu, rect.height);
        else if (symbol == "_CGRectGetWidth")
            resultF32(cpu, rect.width);
        else if (symbol == "_CGRectGetMinX")
            resultF32(cpu, rect.x);
        else if (symbol == "_CGRectGetMinY")
            resultF32(cpu, rect.y);
        else if (symbol == "_CGRectGetMidX")
            resultF32(cpu, rect.x + rect.width / 2);
        else
            resultF32(cpu, rect.y + rect.height / 2);
        return true;
    }
    if (symbol == "_CGRectOffset") {
        const Rect rect = args.aggregate<Rect>();
        const float dx = args.f32(), dy = args.f32();
        resultAggregate(cpu, Rect{rect.x + dx, rect.y + dy, rect.width, rect.height});
        return true;
    }
    if (symbol == "_CGRectInset") {
        const Rect rect = args.aggregate<Rect>();
        const float dx = args.f32(), dy = args.f32();
        resultAggregate(cpu,
                        Rect{rect.x + dx, rect.y + dy, rect.width - 2 * dx, rect.height - 2 * dy});
        return true;
    }
    if (symbol == "_CATransform3DMakeRotation" || symbol == "_CATransform3DMakeTranslation") {
        // CATransform3D is sixteen floats and is returned through the hidden pointer in r0.
        Arguments inputs(cpu, memory, 1);
        CATransform3D transform;
        if (symbol == "_CATransform3DMakeRotation") {
            const float radians = inputs.f32(), x = inputs.f32(), y = inputs.f32(),
                        z = inputs.f32();
            transform = CATransform3D::rotation(radians, x, y, z);
        } else {
            const float x = inputs.f32(), y = inputs.f32(), z = inputs.f32();
            transform.m[12] = x;
            transform.m[13] = y;
            transform.m[14] = z;
        }
        storeThroughPointer(memory, resultPointer(cpu), transform);
        return true;
    }
    if (symbol == "_CATransform3DRotate" || symbol == "_CATransform3DTranslate") {
        Arguments inputs(cpu, memory, 1);
        const CATransform3D base = inputs.aggregate<CATransform3D>();
        CATransform3D delta;
        if (symbol == "_CATransform3DRotate") {
            const float radians = inputs.f32(), x = inputs.f32(), y = inputs.f32(),
                        z = inputs.f32();
            delta = CATransform3D::rotation(radians, x, y, z);
        } else {
            const float x = inputs.f32(), y = inputs.f32(), z = inputs.f32();
            delta.m[12] = x;
            delta.m[13] = y;
            delta.m[14] = z;
        }
        storeThroughPointer(memory, resultPointer(cpu), concat(base, delta));
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
