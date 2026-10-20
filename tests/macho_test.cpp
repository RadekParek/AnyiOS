#include "macho.h"

#include <cassert>
#include <iostream>
#include <random>
#include <vector>
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b.at(at + i) = uint8_t(v >> (i * 8));
}
Bytes header(uint32_t commands = 0, uint32_t size = 0) {
    Bytes b(28 + size);
    put(b, 0, 0xfeedface);
    put(b, 4, 12);
    put(b, 8, 9);
    put(b, 12, 2);
    put(b, 16, commands);
    put(b, 20, size);
    return b;
}
std::string run(const Bytes& b) { return podnative::inspectMachO(b.data(), b.size()); }
void expect(const Bytes& b, const char* prefix) {
    auto s = run(b);
    if (s.find(prefix) != 0) {
        std::cerr << s << " expected " << prefix << '\n';
        std::abort();
    }
}
int main() {
    expect({}, "MACHO_TRUNCATED");
    expect(header(), "INSPECTED_ARM32");
    auto b = header();
    put(b, 4, 0x100000c);
    expect(b, "MACHO_CPU");
    b = header();
    put(b, 0, 0xcafebabe);
    expect(b, "MACHO_FORMAT");
    b = header();
    put(b, 12, 6);
    expect(b, "MACHO_TYPE");
    b = header(1, 8);
    put(b, 32, 0xffffffff);
    expect(b, "MACHO_COMMANDS");
    b = header(1, 8);
    put(b, 32, 4);
    expect(b, "MACHO_COMMANDS");
    b = header(1, 20);
    put(b, 28, 0x21);
    put(b, 32, 20);
    put(b, 44, 1);
    expect(b, "MACHO_ENCRYPTED");
    put(b, 44, 0);
    expect(b, "INSPECTED_ARM32");
    put(b, 36, 49);
    expect(b, "MACHO_ENCRYPTION");
    b = header(1, 56);
    put(b, 28, 1);
    put(b, 32, 56);
    put(b, 60, 0xfffffff0);
    expect(b, "MACHO_SEGMENT");
    b = header(1, 56);
    put(b, 28, 1);
    put(b, 32, 56);
    put(b, 76, 1);
    expect(b, "MACHO_SEGMENT");
    b = header(0, 4);
    expect(b, "MACHO_COMMANDS");
    b = header();
    put(b, 16, 0xffffffff);
    expect(b, "MACHO_COMMANDS");
    std::mt19937 random(42);
    for (int i = 0; i < 25000; i++) {
        Bytes bytes(random() % 512);
        for (auto& value : bytes) value = uint8_t(random());
        if (bytes.size() >= 28 && i % 2) {
            put(bytes, 0, 0xfeedface);
            put(bytes, 4, 12);
            put(bytes, 12, 2);
        }
        (void)run(bytes);
    }
    std::cout << "Mach-O regressions and 25,000 deterministic malformed inputs passed\n";
}
