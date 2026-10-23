#include "frameworks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "abi.h"
#include "error.h"

namespace podnative::runtime {
Point Affine::apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
Affine Affine::concat(const Affine& n) const {
    // Row-vector CoreGraphics order: apply this transform, then n.
    return {a * n.a + b * n.c, a * n.b + b * n.d,          c * n.a + d * n.c,
            c * n.b + d * n.d, tx * n.a + ty * n.c + n.tx, tx * n.b + ty * n.d + n.ty};
}
Affine Affine::inverted() const {
    float determinant = a * d - b * c;
    // CoreGraphics returns the original transform when it cannot be inverted.
    if (determinant == 0) return *this;
    return {d / determinant,
            -b / determinant,
            -c / determinant,
            a / determinant,
            (c * ty - d * tx) / determinant,
            (b * tx - a * ty) / determinant};
}
Rect Affine::apply(Rect r) const {
    Point p[] = {apply(Point{r.x, r.y}), apply(Point{r.x + r.width, r.y}),
                 apply(Point{r.x, r.y + r.height}), apply(Point{r.x + r.width, r.y + r.height})};
    float left = p[0].x, right = p[0].x, top = p[0].y, bottom = p[0].y;
    for (auto q : p) {
        left = std::min(left, q.x);
        right = std::max(right, q.x);
        top = std::min(top, q.y);
        bottom = std::max(bottom, q.y);
    }
    return {left, top, right - left, bottom - top};
}
Affine Affine::rotation(float r) {
    return {std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0};
}
Affine Affine::translation(float x, float y) { return {1, 0, 0, 1, x, y}; }
Affine Affine::scale(float x, float y) { return {x, 0, 0, y, 0, 0}; }
CATransform3D CATransform3D::rotation(float radians, float x, float y, float z) {
    // Normalise the axis; a zero axis is the identity rotation, as Core Animation does.
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length == 0) return {};
    const float nx = x / length, ny = y / length, nz = z / length;
    const float c = std::cos(radians), s = std::sin(radians), t = 1 - c;
    CATransform3D result;
    result.m[0] = t * nx * nx + c;
    result.m[1] = t * nx * ny + s * nz;
    result.m[2] = t * nx * nz - s * ny;
    result.m[4] = t * nx * ny - s * nz;
    result.m[5] = t * ny * ny + c;
    result.m[6] = t * ny * nz + s * nx;
    result.m[8] = t * nx * nz + s * ny;
    result.m[9] = t * ny * nz - s * nx;
    result.m[10] = t * nz * nz + c;
    return result;
}
CATransform3D CATransform3D::scale(float x, float y, float z) {
    CATransform3D result;
    result.m[0] = x;
    result.m[5] = y;
    result.m[10] = z;
    return result;
}
CoreFoundation::Bits& CoreFoundation::get(uint32_t handle) {
    auto it = objects.find(handle);
    if (it == objects.end())
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", handle);
    return it->second;
}
uint32_t CoreFoundation::createPath(std::string path, uint32_t kind) {
    if (objects.size() >= 4096 || next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session handle limit");
    const uint32_t handle = next;
    next += 4;
    Bits entry;
    entry.references = 1;
    entry.path = std::move(path);
    entry.kind = kind;
    objects.emplace(handle, std::move(entry));
    return handle;
}
uint32_t CoreFoundation::create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits) {
    if (bytes.size() > 8 * 1024 * 1024 - allocated || objects.size() >= 4096 || next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session allocation limit");
    uint32_t handle = next;
    next += 4;
    allocated += bytes.size();
    Bits entry;
    entry.bytes = std::move(bytes);
    entry.count = bits;
    entry.references = 1;
    entry.mutableBits = mutableBits;
    objects.emplace(handle, std::move(entry));
    return handle;
}
namespace {
double cfAbsoluteTime() {
    constexpr double UnixToCfEpoch = 978307200.0;
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
               .count() -
           UnixToCfEpoch;
}
}  // namespace
bool CoreFoundation::invoke(const std::string& name, CpuState& cpu, Memory& memory) {
    auto& r = cpu.registers;
    if (name == "_CFAbsoluteTimeGetCurrent") {
        resultF64(cpu, cfAbsoluteTime());
    } else if (name == "_CFStringConvertEncodingToNSStringEncoding") {
        // Common encodings from CoreFoundation and NSString's public encoding constants.
        switch (r[0]) {
            case 0x0000:
                r[0] = 30;
                break;  // MacRoman
            case 0x0201:
                r[0] = 5;
                break;  // ISO Latin-1
            case 0x0600:
                r[0] = 1;
                break;  // ASCII
            case 0x0100:
                r[0] = 10;
                break;  // UTF-16
            case 0x08000100:
                r[0] = 4;
                break;  // UTF-8
            case 0x10000100:
                r[0] = 0x90000100;
                break;  // UTF-16BE
            case 0x14000100:
                r[0] = 0x94000100;
                break;  // UTF-16LE
            default:
                r[0] = 0xffffffffu;
                break;  // kCFStringEncodingInvalidId
        }
    } else if (name == "_CFDataCreate") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        const int32_t length = int32_t(r[2]);
        if (length < 0 || uint32_t(length) > 8 * 1024 * 1024)
            throw Error("CF_BUDGET", "invalid or excessive CFData length");
        std::vector<uint8_t> bytes(static_cast<size_t>(length), 0);
        if (length) memory.read(r[1], bytes.data(), bytes.size());
        const uint32_t handle = create(std::move(bytes), uint32_t(length), false);
        Bits& data = get(handle);
        if (!data.bytes.empty()) {
            const size_t pageSize = Memory::PageSize;
            const size_t mapped = (data.bytes.size() + pageSize - 1) & ~(pageSize - 1);
            constexpr uint32_t DataLimit = 0xef000000;
            if (mapped > DataLimit - nextDataAddress)
                throw Error("CF_BUDGET", "CFData guest address space is exhausted");
            data.dataAddress = nextDataAddress;
            memory.map({nextDataAddress, uint32_t(mapped), uint8_t(Read | Write),
                        "CoreFoundation data bytes"});
            memory.write(nextDataAddress, data.bytes.data(), data.bytes.size());
            nextDataAddress += uint32_t(mapped);
        }
        r[0] = handle;
    } else if (name == "_CFDataGetBytePtr") {
        r[0] = get(r[0]).dataAddress;
    } else if (name == "_CFDataGetLength") {
        const Bits& data = get(r[0]);
        if (data.bytes.size() > size_t(std::numeric_limits<int32_t>::max()))
            throw Error("CF_BUDGET", "CFData length overflows CFIndex");
        r[0] = uint32_t(data.bytes.size());
    } else if (name == "_CFBitVectorCreate") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        if (r[2] > 8 * 1024 * 1024) throw Error("CF_BUDGET", "invalid or excessive bit count");
        std::vector<uint8_t> bytes((r[2] + 7) / 8);
        memory.read(r[1], bytes.data(), bytes.size());
        r[0] = create(std::move(bytes), r[2], false);
    } else if (name == "_CFBitVectorCreateMutableCopy") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        auto& original = get(r[2]);
        if (r[1] && r[1] < original.count)
            throw Error("CF_CAPACITY", "capacity is less than bit count");
        if (r[1] > 8 * 1024 * 1024) throw Error("CF_BUDGET", "excessive bit vector capacity");
        r[0] = create(original.bytes, original.count, true);
    } else if (name == "_CFBitVectorGetBitAtIndex" || name == "_CFBitVectorSetBitAtIndex") {
        auto& bits = get(r[0]);
        if (r[1] >= bits.count) throw Error("CF_INDEX", "bit vector index out of bounds");
        uint8_t mask = uint8_t(0x80u >> (r[1] % 8));
        if (name == "_CFBitVectorGetBitAtIndex")
            r[0] = (bits.bytes[r[1] / 8] & mask) ? 1 : 0;
        else {
            if (!bits.mutableBits)
                throw Error("CF_IMMUTABLE", "cannot mutate an immutable bit vector");
            if (r[2] > 1) throw Error("CF_BIT", "bit value must be zero or one");
            if (r[2])
                bits.bytes[r[1] / 8] |= mask;
            else
                bits.bytes[r[1] / 8] &= uint8_t(~mask);
        }
    } else if (name == "_CFBundleGetMainBundle") {
        r[0] = mainBundle();
    } else if (name == "_CFBundleCopyResourcesDirectoryURL") {
        // The resources of a mounted bundle live at the bundle root.
        if (bundlePath.empty()) throw Error("CF_BUNDLE", "no bundle is mounted");
        r[0] = createPath(bundlePath, 2);
    } else if (name == "_CFBundleCopyBundleURL") {
        if (bundlePath.empty()) throw Error("CF_BUNDLE", "no bundle is mounted");
        r[0] = createPath(bundlePath, 2);
    } else if (name == "_CFURLGetFileSystemRepresentation") {
        auto& url = get(r[0]);
        if (url.kind == 0) throw Error("CF_URL", "object is not a URL", r[0]);
        const uint32_t buffer = r[2], capacity = r[3];
        if (!buffer) {
            r[0] = 1;
            return true;
        }
        if (capacity < url.path.size() + 1) {
            r[0] = 0;  // Documented behaviour: false when the buffer is too small.
            return true;
        }
        if (!memory.permits(buffer, url.path.size() + 1, Write))
            throw Error("CF_URL", "destination buffer is not writable", buffer);
        memory.write(buffer, url.path.c_str(), url.path.size() + 1);
        r[0] = 1;
    } else if (name == "_CFURLCopyFileSystemPath") {
        auto& url = get(r[0]);
        if (url.kind == 0) throw Error("CF_URL", "object is not a URL", r[0]);
        r[0] = createPath(url.path, 3);
    } else if (name == "_CFRetain") {
        auto& bits = get(r[0]);
        if (bits.references == std::numeric_limits<uint32_t>::max())
            throw Error("CF_RETAIN", "reference count overflow");
        bits.references++;
    } else if (name == "_CFRelease") {
        auto& bits = get(r[0]);
        if (!--bits.references) {
            allocated -= bits.bytes.size();
            objects.erase(r[0]);
        }
    } else
        return false;  // Unknown services are never successful no-ops.
    return true;
}
}  // namespace podnative::runtime
