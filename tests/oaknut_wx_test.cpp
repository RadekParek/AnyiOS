// Tests real kernel permissions of the narrowly patched allocator, not JIT execution.
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <oaknut/code_block.hpp>
#include <sstream>
#include <string>
std::string permissions(void* pointer) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range, mode;
        fields >> range >> mode;
        auto dash = range.find('-');
        if (dash == std::string::npos) continue;
        const auto low = std::stoull(range.substr(0, dash), nullptr, 16);
        const auto high = std::stoull(range.substr(dash + 1), nullptr, 16);
        if (address >= low && address < high) return mode;
    }
    throw std::runtime_error("Own allocator mapping not found in /proc/self/maps");
}
int main() {
    oaknut::CodeBlock block(16384);  // compatible with 4 KiB and 16 KiB host page configurations
    auto mode = permissions(block.ptr());
    assert(mode[0] == 'r' && mode[1] == 'w' && mode[2] != 'x');
    block.ptr()[0] = 0x12345678;
    block.protect();
    mode = permissions(block.ptr());
    assert(mode[0] == 'r' && mode[1] != 'w' && mode[2] == 'x');
    assert(block.ptr()[0] == 0x12345678);
    block.unprotect();
    mode = permissions(block.ptr());
    assert(mode[1] == 'w' && mode[2] != 'x');
    block.ptr()[0] = 0x87654321;
    block.protect();
    assert(block.ptr()[0] == 0x87654321);
    bool failed = false;
    try {
        oaknut::CodeBlock invalid(0);
    } catch (const std::system_error&) {
        failed = true;
    }
    assert(failed);  // MAP_FAILED must be a structured failure, never a usable pointer.
    std::cout << "Oaknut W^X permissions and MAP_FAILED regression tests passed\n";
}
