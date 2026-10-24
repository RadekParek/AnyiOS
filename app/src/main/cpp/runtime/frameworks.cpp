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
namespace {
constexpr size_t MaxCfObjectBytes = 8 * 1024 * 1024;
constexpr size_t MaxCfArrayElements = 1024 * 1024;
constexpr size_t MaxCfObjects = 4096;
constexpr uint32_t CfHandleBase = 0xe0000000;
constexpr uint32_t CfHandleLimit = 0xe1000000;
}  // namespace
CoreFoundation::Bits& CoreFoundation::get(uint32_t handle) {
    auto it = objects.find(handle);
    if (it == objects.end())
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", handle);
    return it->second;
}
uint32_t CoreFoundation::createPath(std::string path, uint32_t kind) {
    if (objects.size() >= MaxCfObjects || next == 0xfffffffc ||
        path.size() > MaxCfObjectBytes - allocated)
        throw Error("CF_BUDGET", "CoreFoundation session handle limit");
    const uint32_t handle = next;
    Bits entry;
    entry.references = 1;
    entry.payloadBytes = path.size();
    entry.path = std::move(path);
    entry.kind = kind;
    entry.isPath = true;
    objects.emplace(handle, std::move(entry));
    next += 4;
    allocated += objects.at(handle).payloadBytes;
    return handle;
}
uint32_t CoreFoundation::currentRunLoopObject() {
    if (currentRunLoop && objects.count(currentRunLoop)) return currentRunLoop;
    if (objects.size() >= MaxCfObjects || next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session handle limit");
    const uint32_t handle = next;
    Bits runLoop;
    runLoop.references = 1;
    runLoop.isRunLoop = true;
    objects.emplace(handle, std::move(runLoop));
    next += 4;
    currentRunLoop = handle;
    return handle;
}
uint32_t CoreFoundation::create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits) {
    if (bytes.size() > MaxCfObjectBytes - allocated || objects.size() >= MaxCfObjects ||
        next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session allocation limit");
    const uint32_t handle = next;
    Bits entry;
    entry.bytes = std::move(bytes);
    entry.count = bits;
    entry.references = 1;
    entry.payloadBytes = entry.bytes.size();
    entry.mutableBits = mutableBits;
    objects.emplace(handle, std::move(entry));
    next += 4;
    allocated += objects.at(handle).payloadBytes;
    return handle;
}
uint32_t CoreFoundation::createArray(uint32_t capacity, bool mutableArray, uint32_t callbacks) {
    if (capacity > MaxCfArrayElements)
        throw Error("CF_BUDGET", "CFArray capacity exceeds the session limit");
    if (callbacks && callbacks != typeArrayCallbacksAddress)
        throw Error("CF_ARRAY_CALLBACKS", "custom CFArray callbacks are not implemented",
                    callbacks);
    const size_t bytes = size_t(capacity) * sizeof(uint32_t);
    if (bytes > MaxCfObjectBytes - allocated || objects.size() >= MaxCfObjects ||
        next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session array allocation limit");
    const uint32_t handle = next;
    Bits entry;
    entry.references = 1;
    entry.payloadBytes = bytes;
    entry.capacity = capacity;
    entry.mutableBits = mutableArray;
    entry.isArray = true;
    entry.callbackAddress = callbacks;
    entry.retainsValues = callbacks == typeArrayCallbacksAddress && callbacks != 0;
    entry.elements.reserve(capacity);
    objects.emplace(handle, std::move(entry));
    next += 4;
    allocated += bytes;
    return handle;
}
void CoreFoundation::reserveArray(Bits& array, size_t capacity) {
    if (!array.isArray) throw Error("CF_ARRAY", "object is not a CFArray");
    if (capacity <= array.capacity) return;
    if (!array.mutableBits) throw Error("CF_IMMUTABLE", "cannot grow an immutable CFArray");
    if (capacity > MaxCfArrayElements)
        throw Error("CF_BUDGET", "CFArray capacity exceeds the session limit");
    const size_t doubled = array.capacity > MaxCfArrayElements / 2
                               ? MaxCfArrayElements
                               : std::max<size_t>(8, array.capacity * 2);
    const size_t newCapacity = std::max(capacity, doubled);
    const size_t bytes = newCapacity * sizeof(uint32_t);
    const size_t otherBytes = allocated - array.payloadBytes;
    if (bytes > MaxCfObjectBytes - otherBytes)
        throw Error("CF_BUDGET", "CoreFoundation session array allocation limit");
    array.elements.reserve(newCapacity);
    allocated = otherBytes + bytes;
    array.payloadBytes = bytes;
    array.capacity = newCapacity;
}
void CoreFoundation::setExternalObjectHooks(std::function<bool(uint32_t)> owns,
                                            std::function<void(uint32_t)> retain,
                                            std::function<void(uint32_t)> release,
                                            std::function<bool(uint32_t, uint32_t)> equal) {
    ownsExternalObject = std::move(owns);
    retainExternalObject = std::move(retain);
    releaseExternalObject = std::move(release);
    equalExternalObjects = std::move(equal);
}
void CoreFoundation::retainValue(uint32_t value) {
    if (!value) return;
    auto it = objects.find(value);
    if (it != objects.end()) {
        if (it->second.references == std::numeric_limits<uint32_t>::max())
            throw Error("CF_RETAIN", "reference count overflow");
        ++it->second.references;
        return;
    }
    if (value >= CfHandleBase && value < CfHandleLimit)
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", value);
    if (ownsExternalObject && ownsExternalObject(value)) {
        if (retainExternalObject) retainExternalObject(value);
        return;
    }
    throw Error("CF_ARRAY_VALUE", "CFType array value is not a live CF/Objective-C object", value);
}
void CoreFoundation::releaseHandle(uint32_t handle) {
    auto it = objects.find(handle);
    if (it == objects.end())
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", handle);
    if (it->second.references > 1) {
        --it->second.references;
        return;
    }
    if (!it->second.references) throw Error("CF_RETAIN", "reference count underflow", handle);
    const bool releasesElements = it->second.isArray && it->second.retainsValues;
    const bool wasRunLoop = it->second.isRunLoop;
    std::vector<uint32_t> elements = std::move(it->second.elements);
    allocated -= it->second.payloadBytes;
    objects.erase(it);
    if (wasRunLoop && currentRunLoop == handle) currentRunLoop = 0;
    if (releasesElements)
        for (uint32_t element : elements) releaseValue(element);
}
void CoreFoundation::releaseValue(uint32_t value) {
    if (!value) return;
    if (objects.count(value)) {
        releaseHandle(value);
        return;
    }
    if (value >= CfHandleBase && value < CfHandleLimit)
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", value);
    if (ownsExternalObject && ownsExternalObject(value)) {
        if (releaseExternalObject) releaseExternalObject(value);
        return;
    }
    throw Error("CF_ARRAY_VALUE", "CFType array value is not a live CF/Objective-C object", value);
}
bool CoreFoundation::equalValue(uint32_t a, uint32_t b, size_t depth) const {
    if (a == b) return true;
    if (depth > 32) return false;
    const auto left = objects.find(a), right = objects.find(b);
    if (left != objects.end() && right != objects.end()) {
        const Bits& x = left->second;
        const Bits& y = right->second;
        if (x.isArray || y.isArray) {
            if (!x.isArray || !y.isArray || x.elements.size() != y.elements.size()) return false;
            for (size_t i = 0; i < x.elements.size(); ++i)
                if (!equalValue(x.elements[i], y.elements[i], depth + 1)) return false;
            return true;
        }
        if (x.isData || x.isBitVector || y.isData || y.isBitVector)
            return x.isData == y.isData && x.isBitVector == y.isBitVector && x.count == y.count &&
                   x.bytes == y.bytes;
        if (x.isPath || y.isPath)
            return x.isPath == y.isPath && x.kind == y.kind && x.path == y.path;
        return false;
    }
    return ownsExternalObject && ownsExternalObject(a) && ownsExternalObject(b) &&
           equalExternalObjects && equalExternalObjects(a, b);
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
    auto arrayFor = [&](uint32_t handle) -> Bits& {
        Bits& array = get(handle);
        if (!array.isArray) throw Error("CF_ARRAY", "object is not a CFArray", handle);
        return array;
    };
    if (name == "_CFRunLoopGetCurrent" || name == "_CFRunLoopGetMain") {
        r[0] = currentRunLoopObject();
    } else if (name == "_CFAbsoluteTimeGetCurrent") {
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
        data.isData = true;
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
    } else if (name == "_CFArrayCreateMutable") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        const int32_t capacity = int32_t(r[1]);
        if (capacity < 0 || uint32_t(capacity) > MaxCfArrayElements)
            throw Error("CF_BUDGET", "invalid or excessive CFArray capacity");
        r[0] = createArray(uint32_t(capacity), true, r[2]);
    } else if (name == "_CFArrayCreate") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        const int32_t signedCount = int32_t(r[2]);
        if (signedCount < 0 || uint32_t(signedCount) > MaxCfArrayElements)
            throw Error("CF_BUDGET", "invalid or excessive CFArray element count");
        const uint32_t count = uint32_t(signedCount);
        std::vector<uint32_t> values(count);
        if (count) {
            if (!r[1]) throw Error("CF_ARRAY", "non-empty CFArray requires a values pointer");
            memory.read(r[1], values.data(), size_t(count) * sizeof(uint32_t));
        }
        const uint32_t handle = createArray(count, false, r[3]);
        Bits& array = get(handle);
        try {
            for (uint32_t value : values) {
                if (array.retainsValues) retainValue(value);
                array.elements.push_back(value);
            }
        } catch (...) {
            // releaseHandle unwinds exactly the values that were retained and appended.
            releaseHandle(handle);
            throw;
        }
        r[0] = handle;
    } else if (name == "_CFArrayCreateMutableCopy" || name == "_CFArrayCreateCopy") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        const bool mutableArray = name == "_CFArrayCreateMutableCopy";
        const uint32_t originalHandle = mutableArray ? r[2] : r[1];
        Bits& original = arrayFor(originalHandle);
        const int32_t requested = mutableArray ? int32_t(r[1]) : 0;
        if (requested < 0) throw Error("CF_CAPACITY", "negative CFArray capacity");
        const size_t capacity = std::max<size_t>(original.elements.size(), uint32_t(requested));
        const uint32_t handle =
            createArray(uint32_t(capacity), mutableArray, original.callbackAddress);
        Bits& copy = get(handle);
        try {
            for (uint32_t value : original.elements) {
                if (copy.retainsValues) retainValue(value);
                copy.elements.push_back(value);
            }
        } catch (...) {
            // releaseHandle owns rollback for the retained elements copied so far.
            releaseHandle(handle);
            throw;
        }
        r[0] = handle;
    } else if (name == "_CFArrayGetCount") {
        r[0] = uint32_t(arrayFor(r[0]).elements.size());
    } else if (name == "_CFArrayGetValueAtIndex") {
        Bits& array = arrayFor(r[0]);
        const int32_t index = int32_t(r[1]);
        if (index < 0 || size_t(index) >= array.elements.size())
            throw Error("CF_INDEX", "CFArray index out of bounds", r[1]);
        r[0] = array.elements[size_t(index)];
    } else if (name == "_CFArrayGetValues") {
        Bits& array = arrayFor(r[0]);
        // CFRange is passed by value: ARM32 places location and length in r1/r2, with the
        // output pointer in r3. Treating r1 as a guest address reads unrelated memory.
        const int32_t location = int32_t(r[1]), length = int32_t(r[2]);
        if (location < 0 || length < 0 || size_t(location) > array.elements.size() ||
            size_t(length) > array.elements.size() - size_t(location))
            throw Error("CF_ARRAY_RANGE", "CFArray range out of bounds", r[1]);
        const size_t outputBytes = size_t(length) * sizeof(uint32_t);
        if (outputBytes && !memory.permits(r[3], outputBytes, Write))
            throw Error("CF_ARRAY", "CFArray output buffer is not writable", r[3]);
        for (int32_t i = 0; i < length; ++i)
            memory.write32(r[3] + uint32_t(i) * 4, array.elements[size_t(location) + size_t(i)]);
        r[0] = 0;
    } else if (name == "_CFArrayAppendValue" || name == "_CFArrayInsertValueAtIndex" ||
               name == "_CFArraySetValueAtIndex") {
        Bits& array = arrayFor(r[0]);
        if (!array.mutableBits) throw Error("CF_IMMUTABLE", "cannot mutate an immutable CFArray");
        const bool append = name == "_CFArrayAppendValue";
        const int32_t index = append ? int32_t(array.elements.size()) : int32_t(r[1]);
        const size_t valueIndex = append ? 1 : 2;
        if (index < 0 || size_t(index) > array.elements.size() ||
            (name == "_CFArraySetValueAtIndex" && size_t(index) == array.elements.size()))
            throw Error("CF_INDEX", "CFArray index out of bounds", uint32_t(index));
        if (name == "_CFArraySetValueAtIndex") {
            const uint32_t oldValue = array.elements[size_t(index)];
            const uint32_t newValue = r[valueIndex];
            if (oldValue != newValue && array.retainsValues) retainValue(newValue);
            array.elements[size_t(index)] = newValue;
            if (oldValue != newValue && array.retainsValues) releaseValue(oldValue);
        } else {
            if (array.elements.size() >= MaxCfArrayElements)
                throw Error("CF_BUDGET", "CFArray element limit exceeded");
            reserveArray(array, array.elements.size() + 1);
            const uint32_t value = r[valueIndex];
            if (array.retainsValues) retainValue(value);
            array.elements.insert(array.elements.begin() + index, value);
        }
        r[0] = 0;
    } else if (name == "_CFArrayRemoveValueAtIndex" || name == "_CFArrayRemoveAllValues") {
        Bits& array = arrayFor(r[0]);
        if (!array.mutableBits) throw Error("CF_IMMUTABLE", "cannot mutate an immutable CFArray");
        if (name == "_CFArrayRemoveAllValues") {
            if (array.retainsValues)
                for (uint32_t value : array.elements) releaseValue(value);
            array.elements.clear();
        } else {
            const int32_t index = int32_t(r[1]);
            if (index < 0 || size_t(index) >= array.elements.size())
                throw Error("CF_INDEX", "CFArray index out of bounds", r[1]);
            const uint32_t value = array.elements[size_t(index)];
            array.elements.erase(array.elements.begin() + index);
            if (array.retainsValues) releaseValue(value);
        }
        r[0] = 0;
    } else if (name == "_CFArrayRetainValueCallBack") {
        retainValue(r[1]);
        r[0] = r[1];
    } else if (name == "_CFArrayReleaseValueCallBack") {
        releaseValue(r[1]);
        r[0] = 0;
    } else if (name == "_CFArrayEqualValueCallBack") {
        r[0] = equalValue(r[0], r[1]) ? 1 : 0;
    } else if (name == "_CFBitVectorCreate") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        if (r[2] > 8 * 1024 * 1024) throw Error("CF_BUDGET", "invalid or excessive bit count");
        std::vector<uint8_t> bytes((r[2] + 7) / 8);
        memory.read(r[1], bytes.data(), bytes.size());
        r[0] = create(std::move(bytes), r[2], false);
        get(r[0]).isBitVector = true;
    } else if (name == "_CFBitVectorCreateMutableCopy") {
        if (r[0]) throw Error("CF_ALLOCATOR", "custom allocators are not implemented");
        auto& original = get(r[2]);
        if (r[1] && r[1] < original.count)
            throw Error("CF_CAPACITY", "capacity is less than bit count");
        if (r[1] > 8 * 1024 * 1024) throw Error("CF_BUDGET", "excessive bit vector capacity");
        r[0] = create(original.bytes, original.count, true);
        get(r[0]).isBitVector = true;
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
        retainValue(r[0]);
    } else if (name == "_CFRelease") {
        releaseValue(r[0]);
    } else
        return false;  // Unknown services are never successful no-ops.
    return true;
}
}  // namespace podnative::runtime
