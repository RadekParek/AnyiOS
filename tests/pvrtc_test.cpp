#include "runtime/pvrtc.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace podnative::runtime;

namespace {
void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
void checkWhite(const std::vector<uint8_t>& rgba, const std::string& message) {
    check(!rgba.empty() && rgba.size() % 4 == 0, message + " produces RGBA pixels");
    for (uint8_t component : rgba) check(component == 255, message + " decodes to opaque white");
}
}  // namespace

int main() {
    size_t encodedSize = 0;
    std::string error;
    check(isPvrtcV1Format(0x8c00) && isPvrtcV1Format(0x8c03),
          "the four PVRTC1 RGB/RGBA formats are recognized");
    check(!isPvrtcV1Format(0x8c04), "PVRTC2 and unknown formats are not misclassified");
    check(pvrtcV1ImageSize(0x8c00, 1024, 1024, encodedSize, error) && encodedSize == 512 * 1024,
          "the observed 1024-square PVRTC1 4-bpp upload has the expected 512 KiB size");
    check(pvrtcV1ImageSize(0x8c01, 16, 8, encodedSize, error) && encodedSize == 32,
          "the minimum PVRTC1 2-bpp image uses four eight-byte words");

    std::vector<uint8_t> decoded;
    const std::vector<uint8_t> white4(32, 0xff);
    check(decodePvrtcV1(0x8c00, 8, 8, white4.data(), white4.size(), decoded, error),
          "PVRTC1 4-bpp RGB decodes: " + error);
    checkWhite(decoded, "PVRTC1 4-bpp RGB");

    const std::vector<uint8_t> black4(32, 0);
    check(decodePvrtcV1(0x8c00, 8, 8, black4.data(), black4.size(), decoded, error),
          "PVRTC1 4-bpp RGB black decodes: " + error);
    for (size_t pixel = 0; pixel < decoded.size(); pixel += 4)
        check(decoded[pixel] == 0 && decoded[pixel + 1] == 0 && decoded[pixel + 2] == 0 &&
                  decoded[pixel + 3] == 255,
              "RGB PVRTC has opaque alpha regardless of endpoint alpha bits");

    check(decodePvrtcV1(0x8c02, 8, 8, black4.data(), black4.size(), decoded, error),
          "PVRTC1 4-bpp RGBA decodes: " + error);
    for (uint8_t component : decoded)
        check(component == 0, "RGBA PVRTC preserves transparent black endpoint data");

    const std::vector<uint8_t> white2(32, 0xff);
    check(decodePvrtcV1(0x8c03, 16, 8, white2.data(), white2.size(), decoded, error),
          "PVRTC1 2-bpp RGBA decodes: " + error);
    checkWhite(decoded, "PVRTC1 2-bpp RGBA");

    const std::vector<uint8_t> wrongSize(31, 0xff);
    check(!decodePvrtcV1(0x8c00, 8, 8, wrongSize.data(), wrongSize.size(), decoded, error) &&
              error.find("byte count") != std::string::npos,
          "malformed PVRTC byte counts are rejected before decompression");
    check(pvrtcV1ImageSize(0x8c00, 4, 4, encodedSize, error) && encodedSize == 32,
          "small mip levels retain the PVRTC1 minimum 2x2 word footprint");
    check(decodePvrtcV1(0x8c00, 4, 4, white4.data(), white4.size(), decoded, error),
          "a sub-minimum PVRTC1 mip level is padded for decode and cropped: " + error);
    checkWhite(decoded, "sub-minimum PVRTC1 4-bpp mip level");
    check(!pvrtcV1ImageSize(0x8c00, 10, 8, encodedSize, error),
          "partial PVRTC compression words are rejected above the minimum footprint");
    check(!pvrtcV1ImageSize(0x8c00, 8192, 8192, encodedSize, error),
          "decoded texture allocations are bounded before allocation");

    std::cout
        << "PVRTC1 2/4-bpp decoding, RGB alpha, encoded-size validation and memory bounds passed\n";
}
