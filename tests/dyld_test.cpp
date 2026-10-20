#include "runtime/dyld.h"

#include <cassert>
#include <iostream>
#include <random>

#include "runtime/error.h"
using namespace podnative::runtime;
int main() {
    std::vector<DyldSegment> segments = {{0x1000, 4096, uint8_t(Read | Write)}};
    uint8_t binding[] = {0x11, 0x40, '_', 't', 'e', 's', 't', 0, 0x70, 0, 0x90, 0};
    auto plan = decodeDyld(binding, sizeof(binding), segments, false);
    assert(plan.fixups.size() == 1 && plan.fixups[0].symbol == "_test" &&
           plan.fixups[0].address == 0x1000);
    Memory memory;
    memory.map({0x1000, 4096, uint8_t(Read | Write), "fixups"});
    memory.write32(0x1000, 0x1234);
    try {
        plan.apply(memory, 0,
                   [](int, const std::string&) -> std::optional<uint32_t> { return {}; });
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_UNRESOLVED");
    }
    assert(memory.read32(0x1000) == 0x1234);
    plan.apply(memory, 0, [](int n, const std::string&) -> std::optional<uint32_t> {
        assert(n == 1);
        return 0x2000;
    });
    assert(memory.read32(0x1000) == 0x2000);
    uint8_t rebase[] = {0x11, 0x20, 0, 0x51, 0};
    auto rebases = decodeDyld(rebase, sizeof(rebase), segments, true);
    rebases.apply(memory, 0x100, {});
    assert(memory.read32(0x1000) == 0x2100);
    auto weak = plan;
    weak.fixups[0].weak = true;
    weak.apply(memory, 0, [](int, const std::string&) -> std::optional<uint32_t> { return {}; });
    assert(memory.read32(0x1000) == 0);
    auto atomic = plan;
    atomic.fixups.push_back({0x1004, false, false, false, 1, 0, "_missing"});
    memory.write32(0x1000, 77);
    try {
        atomic.apply(memory, 0, [](int, const std::string& name) -> std::optional<uint32_t> {
            if (name == "_test") return 999;
            return {};
        });
        assert(false);
    } catch (const Error&) {
    }
    assert(memory.read32(0x1000) == 77);
    uint8_t lazy[] = {0x11, 0x40, 'x', 0, 0x70, 4, 0x90, 0, 0x11, 0x40, 'y', 0, 0x70, 8, 0x90, 0};
    assert(decodeDyld(lazy, sizeof(lazy), segments, false, true).fixups.size() == 2);
    auto overflow = plan;
    overflow.fixups[0].addend = INT64_MAX;
    try {
        overflow.apply(memory, 0,
                       [](int, const std::string&) -> std::optional<uint32_t> { return 2; });
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_VALUE");
    }
    auto coalescing = decodeDyld(binding, sizeof(binding), segments, false, false, true);
    try {
        coalescing.apply(memory, 0, {});
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_COALESCING_UNIMPLEMENTED");
    }
    uint8_t signedBinding[] = {0x11, 0x40, 'x', 0, 0x60, 0x7f, 0x70, 0, 0x90, 0};
    auto negative = decodeDyld(signedBinding, sizeof(signedBinding), segments, false);
    assert(negative.fixups[0].addend == -1);
    negative.apply(memory, 0, [](int, const std::string&) -> std::optional<uint32_t> { return 2; });
    assert(memory.read32(0x1000) == 1);
    std::mt19937 random(197);
    for (int i = 0; i < 10000; i++) {
        std::vector<uint8_t> fuzz(binding, binding + sizeof(binding));
        for (int j = 0; j < 3; j++) fuzz[random() % fuzz.size()] = uint8_t(random());
        try {
            decodeDyld(fuzz.data(), fuzz.size(), segments, i % 2, i % 3 == 0);
        } catch (const Error&) {
        }
    }
    std::cout
        << "Dyld pointer rebase/bind, lazy reset, weak null, transactional unresolved failure + 10,000 mutations passed\n";
}
