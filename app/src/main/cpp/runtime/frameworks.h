#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"

namespace podnative::runtime {
struct Point {
    float x, y;
};
struct Rect {
    float x, y, width, height;
};
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Point apply(Point p) const;
    Affine concat(const Affine& after) const;
    Affine inverted() const;
    Rect apply(Rect rectangle) const;
    static Affine rotation(float radians);
    static Affine translation(float x, float y);
    static Affine scale(float x, float y);
};
// Session-owned CoreFoundation subset, using opaque guest handles. Not a global
// host-pointer registry. Binding and Objective-C toll-free bridging remain separate.
class CoreFoundation final {
   public:
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory);
    size_t liveObjects() const { return objects.size(); }

   private:
    struct Bits {
        std::vector<uint8_t> bytes;
        uint32_t count, references;
        bool mutableBits;
    };
    std::map<uint32_t, Bits> objects;
    uint32_t next = 0xe0000000;
    size_t allocated = 0;
    Bits& get(uint32_t handle);
    uint32_t create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits);
};
}  // namespace podnative::runtime
