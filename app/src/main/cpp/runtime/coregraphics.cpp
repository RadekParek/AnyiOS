#include "coregraphics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

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
uint32_t createColorSpace(std::map<uint32_t, uint32_t>& spaces,
                          std::map<uint32_t, uint32_t>& components, uint32_t& next,
                          uint32_t componentCount, const char* label) {
    if (spaces.size() >= 64) throw Error("CG_BUDGET", std::string("too many ") + label);
    const uint32_t handle = next;
    next += 16;
    spaces.emplace(handle, 1);
    components.emplace(handle, componentCount);
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
constexpr uint64_t MaxBitmapBytes = 64ull * 1024 * 1024;
constexpr uint32_t MaxBitmapDimension = 32768;
uint64_t checkedBitmapSize(uint32_t bytesPerRow, uint32_t height) {
    const uint64_t bytes = uint64_t(bytesPerRow) * height;
    if (!bytes || bytes > MaxBitmapBytes)
        throw Error("CG_BITMAP_LIMIT", "bitmap storage exceeds the CoreGraphics budget",
                    uint32_t(std::min<uint64_t>(bytes, UINT32_MAX)));
    return bytes;
}
}  // namespace

bool CoreGraphics::invoke(const std::string& symbol, CpuState& cpu, Memory& memory) {
    Arguments args(cpu, memory);
    if (symbol == "_CGBitmapContextCreate") {
        const uint32_t data = args.pointer();
        const uint32_t width = args.word(), height = args.word(), bitsPerComponent = args.word();
        uint32_t bytesPerRow = args.word();
        const uint32_t colorSpace = args.pointer(), bitmapInfo = args.word();
        if (!width || !height || width > MaxBitmapDimension || height > MaxBitmapDimension ||
            (bitsPerComponent != 8 && bitsPerComponent != 16 && bitsPerComponent != 32))
            throw Error("CG_BITMAP", "bitmap context dimensions or component depth are invalid");
        if (contexts.size() >= 64)
            throw Error("CG_BUDGET", "too many live bitmap contexts");
        const auto color = colorSpaceComponents.find(colorSpace);
        if (color == colorSpaceComponents.end())
            throw Error("CG_COLORSPACE", "bitmap context received an unknown colour space",
                        colorSpace);
        const uint32_t alphaInfo = bitmapInfo & 0x1fu;
        if (alphaInfo > 7)
            throw Error("CG_BITMAP", "bitmap context has an unsupported alpha mode", alphaInfo);
        const uint32_t pixelComponents =
            alphaInfo == 7 ? 1 : color->second + (alphaInfo == 0 ? 0 : 1);
        const uint32_t bytesPerComponent = bitsPerComponent / 8;
        const uint64_t pixelBytes = uint64_t(pixelComponents) * bytesPerComponent;
        const uint64_t minimumRow = uint64_t(width) * pixelBytes;
        if (!pixelBytes || minimumRow > UINT32_MAX)
            throw Error("CG_BITMAP", "bitmap row size overflows the guest ABI");
        if (!bytesPerRow) bytesPerRow = uint32_t(minimumRow);
        if (bytesPerRow < minimumRow)
            throw Error("CG_BITMAP", "bitmap row is shorter than its pixels", bytesPerRow);
        const uint64_t totalBytes = checkedBitmapSize(bytesPerRow, height);
        uint32_t storage = data;
        bool ownsStorage = false;
        if (!storage) {
            if (!allocateGuest)
                throw Error("CG_BITMAP_MEMORY", "bitmap context has no guest allocator");
            storage = allocateGuest(uint32_t(totalBytes), 16);
            if (!storage)
                throw Error("CG_BITMAP_MEMORY", "guest heap cannot allocate bitmap storage",
                            uint32_t(totalBytes));
            ownsStorage = true;
            try {
                std::vector<uint8_t> zeros(4096, 0);
                uint64_t offset = 0;
                while (offset < totalBytes) {
                    const size_t chunk = size_t(std::min<uint64_t>(zeros.size(), totalBytes - offset));
                    memory.write(storage + uint32_t(offset), zeros.data(), chunk);
                    offset += chunk;
                }
            } catch (...) {
                if (releaseGuest) releaseGuest(storage);
                throw;
            }
        } else if (!memory.permits(storage, size_t(totalBytes), Read | Write)) {
            throw Error("CG_BITMAP_MEMORY", "provided bitmap storage is not guest-readable and writable",
                        storage);
        }
        try {
            retainColorSpace(colorSpaces, colorSpace, "bitmap colour space");
        } catch (...) {
            if (ownsStorage && releaseGuest) releaseGuest(storage);
            throw;
        }
        const uint32_t handle = nextContext;
        nextContext += 16;
        BitmapContext context;
        context.width = width;
        context.height = height;
        context.bytesPerComponent = bytesPerComponent;
        context.bytesPerPixel = uint32_t(pixelBytes);
        context.bytesPerRow = bytesPerRow;
        context.data = storage;
        context.colorSpace = colorSpace;
        context.bitmapInfo = bitmapInfo;
        context.ownsData = ownsStorage;
        contexts.emplace(handle, context);
        resultWord(cpu, 0, handle);
        return true;
    }
    if (symbol == "_CGBitmapContextCreateImage") {
        const uint32_t contextHandle = args.pointer();
        auto context = contexts.find(contextHandle);
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "image snapshot received an unknown bitmap context",
                        contextHandle);
        if (images.size() >= 64)
            throw Error("CG_BUDGET", "too many live bitmap images");
        const BitmapContext& source = context->second;
        const size_t bytes = size_t(checkedBitmapSize(source.bytesPerRow, source.height));
        Image image;
        image.width = source.width;
        image.height = source.height;
        image.bytesPerComponent = source.bytesPerComponent;
        image.bytesPerPixel = source.bytesPerPixel;
        image.bytesPerRow = source.bytesPerRow;
        image.bitmapInfo = source.bitmapInfo;
        image.colorSpace = source.colorSpace;
        image.pixels.resize(bytes);
        memory.read(source.data, image.pixels.data(), bytes);
        retainColorSpace(colorSpaces, source.colorSpace, "image colour space");
        const uint32_t handle = nextImage;
        nextImage += 16;
        images.emplace(handle, std::move(image));
        resultWord(cpu, 0, handle);
        return true;
    }
    if (symbol == "_CGContextRetain") {
        auto context = contexts.find(args.pointer());
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "unknown bitmap context", cpu.registers[0]);
        if (context->second.references == UINT32_MAX)
            throw Error("CG_RETAIN", "bitmap context retain overflow");
        resultWord(cpu, 0, ++context->second.references);
        return true;
    }
    if (symbol == "_CGContextRelease") {
        auto context = contexts.find(args.pointer());
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "unknown bitmap context", cpu.registers[0]);
        if (--context->second.references == 0) {
            if (context->second.ownsData && releaseGuest) releaseGuest(context->second.data);
            auto color = colorSpaces.find(context->second.colorSpace);
            if (color == colorSpaces.end())
                throw Error("CG_COLORSPACE", "bitmap context lost its colour space",
                            context->second.colorSpace);
            if (!--color->second) {
                colorSpaceComponents.erase(color->first);
                colorSpaces.erase(color);
            }
            contexts.erase(context);
        }
        return true;
    }
    if (symbol == "_CGContextTranslateCTM") {
        auto context = contexts.find(args.pointer());
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "translation received an unknown bitmap context",
                        cpu.registers[0]);
        context->second.transform =
            context->second.transform.concat(Affine::translation(args.f32(), args.f32()));
        return true;
    }
    if (symbol == "_CGContextClearRect") {
        auto context = contexts.find(args.pointer());
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "clear received an unknown bitmap context", cpu.registers[0]);
        const Rect rectangle = context->second.transform.apply(args.aggregate<Rect>());
        if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
            !std::isfinite(rectangle.width) || !std::isfinite(rectangle.height))
            throw Error("CG_RECT", "clear rectangle contains a non-finite coordinate");
        const double left = std::max(0.0, std::floor(double(rectangle.x)));
        const double top = std::max(0.0, std::floor(double(rectangle.y)));
        const double right = std::min(double(context->second.width),
                                      std::ceil(double(rectangle.x) + rectangle.width));
        const double bottom = std::min(double(context->second.height),
                                       std::ceil(double(rectangle.y) + rectangle.height));
        if (right <= left || bottom <= top) return true;
        const uint32_t x = uint32_t(left), y = uint32_t(top);
        const uint32_t columns = uint32_t(right - left);
        const size_t rowBytes = size_t(columns) * context->second.bytesPerPixel;
        std::vector<uint8_t> zeros(rowBytes, 0);
        for (uint32_t row = y; row < uint32_t(bottom); ++row) {
            const uint64_t offset = uint64_t(row) * context->second.bytesPerRow +
                                    uint64_t(x) * context->second.bytesPerPixel;
            memory.write(context->second.data + uint32_t(offset), zeros.data(), rowBytes);
        }
        return true;
    }
    if (symbol == "_CGContextDrawImage") {
        auto context = contexts.find(args.pointer());
        if (context == contexts.end())
            throw Error("CG_CONTEXT", "draw received an unknown bitmap context", cpu.registers[0]);
        Rect rectangle = context->second.transform.apply(args.aggregate<Rect>());
        const uint32_t imageHandle = args.pointer();
        auto image = images.find(imageHandle);
        if (image == images.end())
            throw Error("CG_IMAGE", "draw received an unknown bitmap image", imageHandle);
        if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
            !std::isfinite(rectangle.width) || !std::isfinite(rectangle.height))
            throw Error("CG_RECT", "draw rectangle contains a non-finite coordinate");
        if (!rectangle.width || !rectangle.height) return true;
        if (context->second.bytesPerPixel != image->second.bytesPerPixel ||
            context->second.bytesPerComponent != image->second.bytesPerComponent)
            throw Error("CG_IMAGE_FORMAT", "bitmap draw requires matching pixel layouts",
                        imageHandle);
        const double left = std::max(0.0, std::floor(double(rectangle.x)));
        const double top = std::max(0.0, std::floor(double(rectangle.y)));
        const double right = std::min(double(context->second.width),
                                      std::ceil(double(rectangle.x) + rectangle.width));
        const double bottom = std::min(double(context->second.height),
                                       std::ceil(double(rectangle.y) + rectangle.height));
        const uint32_t bpp = context->second.bytesPerPixel;
        const uint32_t x0 = uint32_t(left), y0 = uint32_t(top);
        const uint32_t x1 = uint32_t(std::max(left, right));
        const uint32_t y1 = uint32_t(std::max(top, bottom));
        for (uint32_t y = y0; y < y1; ++y) {
            uint32_t sourceY = uint32_t(std::min<double>(
                image->second.height - 1,
                std::max(0.0, (double(y) + 0.5 - rectangle.y) * image->second.height /
                                   rectangle.height)));
            if (rectangle.height < 0) sourceY = image->second.height - 1 - sourceY;
            for (uint32_t x = x0; x < x1; ++x) {
                uint32_t sourceX = uint32_t(std::min<double>(
                    image->second.width - 1,
                    std::max(0.0, (double(x) + 0.5 - rectangle.x) * image->second.width /
                                       rectangle.width)));
                if (rectangle.width < 0) sourceX = image->second.width - 1 - sourceX;
                const uint64_t sourceOffset = uint64_t(sourceY) * image->second.bytesPerRow +
                                              uint64_t(sourceX) * bpp;
                const uint64_t destinationOffset = uint64_t(y) * context->second.bytesPerRow +
                                                   uint64_t(x) * bpp;
                memory.write(context->second.data + uint32_t(destinationOffset),
                             image->second.pixels.data() + sourceOffset, bpp);
            }
        }
        return true;
    }
    if (symbol == "_CGImageGetWidth" || symbol == "_CGImageGetHeight") {
        const uint32_t handle = args.pointer();
        const auto image = images.find(handle);
        if (image == images.end())
            throw Error("CG_IMAGE", "dimension query received an unknown bitmap image", handle);
        resultWord(cpu, 0, symbol == "_CGImageGetWidth" ? image->second.width : image->second.height);
        return true;
    }
    if (symbol == "_CGImageRetain") {
        const uint32_t handle = args.pointer();
        auto image = images.find(handle);
        if (image == images.end()) throw Error("CG_IMAGE", "unknown bitmap image", handle);
        if (image->second.references == UINT32_MAX)
            throw Error("CG_RETAIN", "bitmap image retain overflow");
        resultWord(cpu, 0, ++image->second.references);
        return true;
    }
    if (symbol == "_CGImageRelease") {
        const uint32_t handle = args.pointer();
        auto image = images.find(handle);
        if (image == images.end()) throw Error("CG_IMAGE", "unknown bitmap image", handle);
        if (--image->second.references == 0) {
            auto color = colorSpaces.find(image->second.colorSpace);
            if (color == colorSpaces.end())
                throw Error("CG_COLORSPACE", "bitmap image lost its colour space",
                            image->second.colorSpace);
            if (!--color->second) {
                colorSpaceComponents.erase(color->first);
                colorSpaces.erase(color);
            }
            images.erase(image);
        }
        return true;
    }
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
        resultWord(cpu, 0, createColorSpace(colorSpaces, colorSpaceComponents, nextColorSpace, 3,
                                            "device RGB colour spaces"));
        return true;
    }
    if (symbol == "_CGColorSpaceCreateDeviceGray") {
        resultWord(cpu, 0, createColorSpace(colorSpaces, colorSpaceComponents, nextColorSpace, 1,
                                            "device gray colour spaces"));
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
        if (!--it->second) {
            colorSpaceComponents.erase(it->first);
            colorSpaces.erase(it);
        }
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
