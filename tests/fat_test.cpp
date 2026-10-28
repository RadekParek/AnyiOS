#include <cassert>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "macho_container.h"
#include "runtime/error.h"
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, size_t p, uint64_t v, size_t n = 4, bool le = false) {
    for (size_t i = 0; i < n; i++) b.at(p + (le ? i : n - 1 - i)) = uint8_t(v >> (i * 8));
}
Bytes fat(bool wide = false, bool le = false) {
    Bytes b(4096);
    put(b, 0, wide ? 0xcafebabf : 0xcafebabe, 4, le);
    put(b, 4, 2, 4, le);
    size_t stride = wide ? 32 : 20;
    for (int i = 0; i < 2; i++) {
        size_t p = 8 + i * stride;
        put(b, p, i ? 12 : 0x100000c, 4, le);
        put(b, p + 4, i ? 9 : 0, 4, le);
        put(b, p + 8, i ? 2048 : 1024, wide ? 8 : 4, le);
        put(b, p + (wide ? 16 : 12), 512, wide ? 8 : 4, le);
        put(b, p + (wide ? 24 : 16), 9, 4, le);
    }
    return b;
}
template <class F>
void fault(const char* code, F f, const char* detail = nullptr) {
    bool caught = false;
    try {
        f();
    } catch (const podnative::runtime::Error& e) {
        caught = e.code == code &&
                 (detail == nullptr || std::string(e.what()).find(detail) != std::string::npos);
        if (!caught) std::cerr << e.what() << '\n';
    }
    assert(caught);
}
int main() {
    for (bool wide : {false, true})
        for (bool le : {false, true}) {
            auto b = fat(wide, le);
            auto s = podnative::selectArm32Slice(b.data(), 72, b.size());
            assert(s.universal && s.offset == 2048 && s.size == 512 && s.subtype == 9);
            assert(s.containsArm64 && !s.containsArm64_32);
        }
    auto arm64_32 = fat();
    put(arm64_32, 8, 0x0200000c);
    auto arm64_32_slice = podnative::selectArm32Slice(arm64_32.data(), 72, arm64_32.size());
    assert(!arm64_32_slice.containsArm64 && arm64_32_slice.containsArm64_32);

    auto arm32_variants = fat();
    put(arm32_variants, 8, 12);
    put(arm32_variants, 12, 6);
    put(arm32_variants, 32, 10);
    auto preferred = podnative::selectArm32Slice(arm32_variants.data(), 72, arm32_variants.size());
    assert(preferred.universal && preferred.offset == 2048 && preferred.subtype == 10);
    assert(!preferred.containsArm64);

    // v5TEJ is a fallback; an existing generic/v6/v7 slice still wins.
    for (uint32_t better : {0u, 6u, 9u, 10u, 11u}) {
        auto both = fat();
        put(both, 8, 12);
        put(both, 12, 7);
        put(both, 32, better);
        auto selected = podnative::selectArm32Slice(both.data(), 72, both.size());
        assert(selected.offset == 2048 && selected.subtype == better);
        put(both, 12, better);
        put(both, 32, 7);
        selected = podnative::selectArm32Slice(both.data(), 72, both.size());
        assert(selected.offset == 1024 && selected.subtype == better);
    }
    auto onlyV5 = fat();
    put(onlyV5, 32, 7);
    auto v5Slice = podnative::selectArm32Slice(onlyV5.data(), 72, onlyV5.size());
    assert(v5Slice.offset == 2048 && v5Slice.subtype == 7);

    auto b = fat();
    put(b, 28, 0x100000c);
    fault(
        "FAT_NO_ARM32", [&] { podnative::selectArm32Slice(b.data(), 64, b.size()); },
        "contains ARM64 but no supported ARM32 slice");
    b = fat();
    put(b, 36, 1024);
    fault("FAT_SLICE_OVERLAP", [&] { podnative::selectArm32Slice(b.data(), 64, b.size()); });
    b = fat();
    put(b, 36, 4096);
    fault("FAT_SLICE_BOUNDS", [&] { podnative::selectArm32Slice(b.data(), 64, b.size()); });
    b = fat();
    put(b, 44, 32);
    fault("FAT_ALIGNMENT", [&] { podnative::selectArm32Slice(b.data(), 64, b.size()); });
    b = fat();
    put(b, 4, 65);
    fault("FAT_ARCH_COUNT", [&] { podnative::selectArm32Slice(b.data(), 64, b.size()); });
    b = fat();
    fault("FAT_TABLE_TRUNCATED", [&] { podnative::selectArm32Slice(b.data(), 12, b.size()); });
    b = fat(true);
    put(b, 36, 1);
    fault("FAT_RESERVED", [&] { podnative::selectArm32Slice(b.data(), 72, b.size()); });
    b.assign(28, 0);
    put(b, 0, 0xfeedface, 4, true);
    put(b, 4, 12, 4, true);
    auto s = podnative::selectArm32Slice(b.data(), 28, 128);
    assert(!s.universal && s.size == 128);
    put(b, 8, 10, 4, true);
    s = podnative::selectArm32Slice(b.data(), 28, 128);
    assert(!s.universal && s.subtype == 10);
    put(b, 8, 7, 4, true);
    s = podnative::selectArm32Slice(b.data(), 28, 128);
    assert(!s.universal && s.subtype == 7);
    put(b, 8, 5, 4, true);
    fault("MACHO_CPU_SUBTYPE", [&] { podnative::selectArm32Slice(b.data(), 28, 128); });
    put(b, 8, 0x80000009, 4, true);
    fault("MACHO_CPU_SUBTYPE", [&] { podnative::selectArm32Slice(b.data(), 28, 128); });
    b = fat();
    put(b, 32, 0x80000009);
    fault("FAT_NO_ARM32", [&] { podnative::selectArm32Slice(b.data(), 72, b.size()); });
    std::mt19937 rng(107);
    for (int i = 0; i < 20000; i++) {
        b = fat(i % 2, i % 3 == 0);
        for (int j = 0; j < 3; j++) b[rng() % 72] = uint8_t(rng());
        try {
            podnative::selectArm32Slice(b.data(), 72, b.size());
        } catch (const podnative::runtime::Error&) {
        }
    }
    std::cout
        << "Universal ARM32 selection, FAT64/endian/bounds regressions and 20,000 mutations passed\n";
}
