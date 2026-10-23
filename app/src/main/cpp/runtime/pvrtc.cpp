#include "pvrtc.h"

#include <algorithm>
#include <limits>

#include "PVRTDecompress.h"

namespace podnative::runtime {
namespace {
constexpr uint32_t PvrtcRgb4 = 0x8c00;
constexpr uint32_t PvrtcRgb2 = 0x8c01;
constexpr uint32_t PvrtcRgba4 = 0x8c02;
constexpr uint32_t PvrtcRgba2 = 0x8c03;
constexpr size_t MaxDecodedBytes = 64 * 1024 * 1024;

bool isTwoBit(uint32_t format) { return format == PvrtcRgb2 || format == PvrtcRgba2; }
bool hasAlpha(uint32_t format) { return format == PvrtcRgba4 || format == PvrtcRgba2; }
}  // namespace

bool isPvrtcV1Format(uint32_t format) { return format >= PvrtcRgb4 && format <= PvrtcRgba2; }

bool pvrtcV1ImageSize(uint32_t format, int width, int height, size_t& encodedSize,
                      std::string& error) {
    encodedSize = 0;
    error.clear();
    if (!isPvrtcV1Format(format)) {
        error = "format is not a PVRTC1 RGB/RGBA format";
        return false;
    }
    if (width <= 0 || height <= 0) {
        error = "PVRTC1 texture dimensions must be positive";
        return false;
    }

    const uint32_t wordWidth = isTwoBit(format) ? 8 : 4;
    constexpr uint32_t wordHeight = 4;
    const uint32_t minimumWidth = wordWidth * 2;
    const uint32_t minimumHeight = wordHeight * 2;
    // PVRTC mip levels smaller than the minimum compression footprint still carry a 2x2
    // word image. The reference decoder pads those levels internally and crops the output.
    // Larger dimensions must stay word-aligned because that decoder walks whole words.
    if ((uint32_t(width) >= minimumWidth && uint32_t(width) % wordWidth != 0) ||
        (uint32_t(height) >= minimumHeight && uint32_t(height) % wordHeight != 0)) {
        error = "PVRTC1 dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                " are not aligned to complete compression words";
        return false;
    }

    const uint64_t wordsWide =
        std::max<uint64_t>(2, (uint64_t(uint32_t(width)) + wordWidth - 1) / wordWidth);
    const uint64_t wordsHigh =
        std::max<uint64_t>(2, (uint64_t(uint32_t(height)) + wordHeight - 1) / wordHeight);
    const uint64_t bytes = wordsWide * wordsHigh * 8;
    const uint64_t decodedBytes = uint64_t(uint32_t(width)) * uint32_t(height) * 4;
    if (bytes > 64 * 1024 * 1024 || decodedBytes > MaxDecodedBytes ||
        bytes > std::numeric_limits<size_t>::max() ||
        decodedBytes > std::numeric_limits<size_t>::max()) {
        error = "PVRTC1 image exceeds the bounded software-decode budget";
        return false;
    }
    encodedSize = size_t(bytes);
    return true;
}

bool decodePvrtcV1(uint32_t format, int width, int height, const uint8_t* encoded,
                   size_t encodedSize, std::vector<uint8_t>& rgba, std::string& error) {
    size_t expectedSize = 0;
    if (!pvrtcV1ImageSize(format, width, height, expectedSize, error)) return false;
    if (!encoded) {
        error = "PVRTC1 compressed texture data is null";
        return false;
    }
    if (encodedSize != expectedSize) {
        error = "PVRTC1 compressed byte count does not match the texture dimensions";
        return false;
    }

    std::vector<uint8_t> decoded(size_t(width) * size_t(height) * 4);
    pvr::PVRTDecompressPVRTC(encoded, isTwoBit(format) ? 1u : 0u, uint32_t(width), uint32_t(height),
                             decoded.data());
    if (!hasAlpha(format))
        for (size_t offset = 3; offset < decoded.size(); offset += 4) decoded[offset] = 255;
    rgba.swap(decoded);
    error.clear();
    return true;
}

}  // namespace podnative::runtime
