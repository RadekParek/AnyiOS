#include "frameworks.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
CoreFoundation::Bits& CoreFoundation::get(uint32_t handle) {
    auto it = objects.find(handle);
    if (it == objects.end())
        throw Error("CF_HANDLE", "unknown or released CoreFoundation object", handle);
    return it->second;
}
uint32_t CoreFoundation::create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits) {
    if (bytes.size() > 8 * 1024 * 1024 - allocated || objects.size() >= 4096 || next == 0xfffffffc)
        throw Error("CF_BUDGET", "CoreFoundation session allocation limit");
    uint32_t handle = next;
    next += 4;
    allocated += bytes.size();
    objects.emplace(handle, Bits{std::move(bytes), bits, 1, mutableBits});
    return handle;
}
bool CoreFoundation::invoke(const std::string& name, CpuState& cpu, Memory& memory) {
    auto& r = cpu.registers;
    if (name == "_CFBitVectorCreate") {
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
