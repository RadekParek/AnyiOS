#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace podnative::runtime {

// PVRTC1 formats used by the legacy IMG extension. ANGLE's Vulkan backend in the
// pinned build does not implement these native Vulkan formats, so the GLES bridge
// decodes them into RGBA before upload when a guest requests one.
bool isPvrtcV1Format(uint32_t format);
bool pvrtcV1ImageSize(uint32_t format, int width, int height, size_t& encodedSize,
                      std::string& error);
bool decodePvrtcV1(uint32_t format, int width, int height, const uint8_t* encoded,
                   size_t encodedSize, std::vector<uint8_t>& rgba, std::string& error);

}  // namespace podnative::runtime
