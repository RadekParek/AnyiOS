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
                   [](int, const std::string&, bool) -> std::optional<uint32_t> { return {}; });
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_UNRESOLVED");
    }
    assert(memory.read32(0x1000) == 0x1234);
    plan.apply(memory, 0, [](int n, const std::string&, bool) -> std::optional<uint32_t> {
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
    weak.apply(memory, 0,
               [](int, const std::string&, bool) -> std::optional<uint32_t> { return {}; });
    assert(memory.read32(0x1000) == 0);
    auto atomic = plan;
    atomic.fixups.push_back({0x1004, false, false, false, 1, 0, "_missing"});
    memory.write32(0x1000, 77);
    try {
        atomic.apply(memory, 0, [](int, const std::string& name, bool) -> std::optional<uint32_t> {
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
                       [](int, const std::string&, bool) -> std::optional<uint32_t> { return 2; });
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_VALUE");
    }
    uint8_t weakStream[] = {0x48, 'x', 0, 0x70, 0, 0x90, 0};
    auto weakPlan = decodeDyld(weakStream, sizeof(weakStream), segments, false, false, true);
    assert(weakPlan.needsCoalescing && weakPlan.fixups.size() == 1);
    assert(weakPlan.strongDefinitions.size() == 1 && weakPlan.strongDefinitions[0] == "x");
    ExportGraph graph;
    size_t weakImage = graph.addImage("weak-image");
    try {
        weakPlan.applyWithGraph(memory, 0, graph, weakImage);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_WEAK_DEFINITION");
    }
    graph.define(weakImage, "x", {0x4321, false});
    weakPlan.applyWithGraph(memory, 0, graph, weakImage);
    assert(memory.read32(0x1000) == 0x4321);
    uint8_t weakOrdinal[] = {0x11, 0x00};
    try {
        decodeDyld(weakOrdinal, sizeof(weakOrdinal), segments, false, false, true);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_WEAK_OPCODE");
    }
    uint8_t signedBinding[] = {0x11, 0x40, 'x', 0, 0x60, 0x7f, 0x70, 0, 0x90, 0};
    auto negative = decodeDyld(signedBinding, sizeof(signedBinding), segments, false);
    assert(negative.fixups[0].addend == -1);
    negative.apply(memory, 0,
                   [](int, const std::string&, bool) -> std::optional<uint32_t> { return 2; });
    assert(memory.read32(0x1000) == 1);
    // Final stride/skip is not a fixup: old validation rejected valid streams.
    std::vector<DyldSegment> tiny = {{0x1000, 8, uint8_t(Read | Write)}};
    uint8_t terminalRebase[] = {0x11, 0x20, 4, 0x70, 8, 0};
    assert(decodeDyld(terminalRebase, sizeof(terminalRebase), tiny, true).fixups.size() == 1);
    uint8_t terminalBind[] = {0x11, 0x40, 'x', 0, 0x70, 4, 0xb3, 0};
    assert(decodeDyld(terminalBind, sizeof(terminalBind), tiny, false).fixups.size() == 1);
    std::vector<DyldSegment> twelve = {{0x1000, 12, uint8_t(Read | Write)}};
    uint8_t terminalRepeatedRebase[] = {0x11, 0x20, 0, 0x80, 2, 4, 0};
    assert(decodeDyld(terminalRepeatedRebase, sizeof(terminalRepeatedRebase), twelve, true)
               .fixups.size() == 2);
    uint8_t terminalRepeatedBind[] = {0x11, 0x40, 'x', 0, 0x70, 0, 0xc0, 2, 4, 0};
    assert(decodeDyld(terminalRepeatedBind, sizeof(terminalRepeatedBind), twelve, false)
               .fixups.size() == 2);
    uint8_t repeatRebase[] = {0x11, 0x20, 0, 0x80, 2, 4, 0};
    try {
        decodeDyld(repeatRebase, sizeof(repeatRebase), tiny, true);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_TARGET");
    }
    uint8_t badNextBind[] = {0x11, 0x40, 'x', 0, 0x70, 4, 0xb3, 0x90, 0};
    try {
        decodeDyld(badNextBind, sizeof(badNextBind), tiny, false);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_TARGET");
    }
    uint8_t arithmeticOverflow[] = {0x11, 0x20, 4,    0x30, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 1,    0};
    try {
        decodeDyld(arithmeticOverflow, sizeof(arithmeticOverflow), tiny, true);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "DYLD_ADDRESS");
    }
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
