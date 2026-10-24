#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "frameworks.h"

namespace podnative::runtime {
// Session-owned CoreGraphics subset. Geometry uses the shared float structs; opaque
// objects are guest handles, and bitmap storage is allocated from the guest heap.
// Bitmap contexts own or borrow guest memory with explicit bounds, and image dimensions
// are answered only for image objects created by this model.
class CoreGraphics final {
   public:
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory);
    size_t liveColorSpaces() const { return colorSpaces.size(); }
    size_t liveBitmapContexts() const { return contexts.size(); }
    size_t liveImages() const { return images.size(); }
    void setHeapHooks(std::function<uint32_t(uint32_t, uint32_t)> allocate,
                      std::function<void(uint32_t)> release) {
        allocateGuest = std::move(allocate);
        releaseGuest = std::move(release);
    }

   private:
    struct BitmapContext {
        uint32_t references = 1;
        uint32_t width = 0, height = 0, bytesPerComponent = 0, bytesPerPixel = 0;
        uint32_t bytesPerRow = 0, data = 0, colorSpace = 0, bitmapInfo = 0;
        bool ownsData = false;
        Affine transform{};
    };
    struct Image {
        uint32_t references = 1;
        uint32_t width = 0, height = 0, bytesPerComponent = 0, bytesPerPixel = 0;
        uint32_t bytesPerRow = 0, bitmapInfo = 0, colorSpace = 0;
        std::vector<uint8_t> pixels;
    };
    std::map<uint32_t, uint32_t> colorSpaces;  // handle -> reference count
    std::map<uint32_t, uint32_t> colorSpaceComponents;
    std::map<uint32_t, BitmapContext> contexts;  // guest handle -> bitmap state
    std::map<uint32_t, Image> images;            // guest handle -> immutable pixel snapshot
    uint32_t nextColorSpace = 0xe4000000;
    uint32_t nextContext = 0xe4100000;
    uint32_t nextImage = 0xe4200000;
    std::function<uint32_t(uint32_t, uint32_t)> allocateGuest;
    std::function<void(uint32_t)> releaseGuest;
};
}  // namespace podnative::runtime
