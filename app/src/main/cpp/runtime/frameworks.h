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
// QuartzCore CATransform3D: a column-major 4x4 matrix in the same layout Core Animation
// hands to the GPU, so a guest that composes transforms sees real values.
struct CATransform3D {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static CATransform3D rotation(float radians, float x, float y, float z);
    static CATransform3D scale(float x, float y, float z);
};
// Session-owned CoreFoundation subset, using opaque guest handles. Not a global
// host-pointer registry. Binding and Objective-C toll-free bridging remain separate.
class CoreFoundation final {
   public:
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory);
    size_t liveObjects() const { return objects.size(); }
    // Guest sandbox roots that CFBundle/CFURL report. Empty until the launcher mounts.
    void setPaths(std::string bundle, std::string documents) {
        bundlePath = std::move(bundle);
        documentsPath = std::move(documents);
    }
    // Handle of CFBundleGetMainBundle(), created on first request.
    uint32_t mainBundle() { return bundle == 0 ? (bundle = createPath("/var/mobile")) : bundle; }

   private:
    struct Bits {
        std::vector<uint8_t> bytes;
        uint32_t count, references;
        bool mutableBits;
        // CFURL/CFBundle carry a guest path plus a kind tag; bit vectors leave both empty.
        std::string path;
        uint32_t kind = 0;
    };
    std::map<uint32_t, Bits> objects;
    uint32_t next = 0xe0000000;
    size_t allocated = 0;
    uint32_t bundle = 0;
    std::string bundlePath = "/var/mobile", documentsPath = "/var/mobile/Documents";
    Bits& get(uint32_t handle);
    uint32_t create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits);
    uint32_t createPath(std::string path, uint32_t kind = 1);
};
}  // namespace podnative::runtime
