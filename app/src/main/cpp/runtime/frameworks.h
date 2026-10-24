#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
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
    bool isRunLoopObject(uint32_t handle) const {
        const auto found = objects.find(handle);
        return found != objects.end() && found->second.isRunLoop;
    }
    // Guest sandbox roots that CFBundle/CFURL report. Empty until the launcher mounts.
    void setPaths(std::string bundle, std::string documents) {
        bundlePath = std::move(bundle);
        documentsPath = std::move(documents);
    }
    // Handle of CFBundleGetMainBundle(), created on first request.
    uint32_t mainBundle() { return bundle == 0 ? (bundle = createPath(bundlePath, 1)) : bundle; }
    // Address of the guest CFArrayCallBacks descriptor exported as
    // _kCFTypeArrayCallBacks. Zero callbacks remain valid for non-owning pointer arrays.
    void setTypeArrayCallbacksAddress(uint32_t address) { typeArrayCallbacksAddress = address; }
    // CFType callbacks must retain/release Objective-C objects when an array is toll-free
    // bridged. The hooks recognize only this runtime's live objects, not arbitrary pointers.
    void setExternalObjectHooks(std::function<bool(uint32_t)> owns,
                                std::function<void(uint32_t)> retain,
                                std::function<void(uint32_t)> release,
                                std::function<bool(uint32_t, uint32_t)> equal);

   private:
    struct Bits {
        std::vector<uint8_t> bytes;
        std::vector<uint32_t> elements;
        uint32_t count = 0, references = 0;
        uint32_t dataAddress = 0;  // guest pointer for CFDataGetBytePtr; never a host address
        size_t payloadBytes = 0, capacity = 0;
        uint32_t callbackAddress = 0;
        bool mutableBits = false, isArray = false, isData = false, isBitVector = false;
        bool retainsValues = false, isPath = false, isRunLoop = false;
        // CFURL/CFBundle carry a guest path plus a kind tag; bit vectors leave both empty.
        std::string path;
        uint32_t kind = 0;
    };
    std::map<uint32_t, Bits> objects;
    uint32_t next = 0xe0000000;
    uint32_t nextDataAddress = 0xe1000000;
    size_t allocated = 0;
    uint32_t bundle = 0, typeArrayCallbacksAddress = 0, currentRunLoop = 0;
    std::string bundlePath = "/var/mobile", documentsPath = "/var/mobile/Documents";
    std::function<bool(uint32_t)> ownsExternalObject;
    std::function<void(uint32_t)> retainExternalObject, releaseExternalObject;
    std::function<bool(uint32_t, uint32_t)> equalExternalObjects;
    Bits& get(uint32_t handle);
    uint32_t create(std::vector<uint8_t> bytes, uint32_t bits, bool mutableBits);
    uint32_t createArray(uint32_t capacity, bool mutableArray, uint32_t callbacks);
    void reserveArray(Bits& array, size_t capacity);
    void retainValue(uint32_t value);
    void releaseValue(uint32_t value);
    void releaseHandle(uint32_t handle);
    bool equalValue(uint32_t a, uint32_t b, size_t depth = 0) const;
    uint32_t createPath(std::string path, uint32_t kind = 1);
    uint32_t currentRunLoopObject();
};
}  // namespace podnative::runtime
