#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace podnative::runtime {
class Error final : public std::runtime_error {
   public:
    const std::string code;
    const uint32_t address;
    Error(std::string code, std::string detail, uint32_t address = 0)
        : std::runtime_error(code + ": " + detail), code(std::move(code)), address(address) {}
};
}  // namespace podnative::runtime
