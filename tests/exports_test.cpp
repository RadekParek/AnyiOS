#include "runtime/exports.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/dyld.h"
#include "runtime/error.h"
#include "runtime/services.h"
using namespace podnative::runtime;
namespace {
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        if (e.code == code) return;
        std::cerr << e.what() << " expected " << code << '\n';
        std::abort();
    }
    std::cerr << "Expected fault " << code << '\n';
    std::abort();
}
CpuState call(Services& services, const char* name, uint32_t a, uint32_t b = 0, uint32_t c = 0,
              uint32_t lr = 0x1000) {
    uint32_t gate = services.address(name);
    CpuState state;
    state.registers[0] = a;
    state.registers[1] = b;
    state.registers[2] = c;
    state.registers[14] = lr;
    state.registers[15] = gate + 4;
    CpuStop stop{
        StopReason::SupervisorCall, 1, gate, (gate - 0x6f000000U) / 8 + 1, 0, "CPU_SVC", ""};
    services.dispatch(stop, state);
    return state;
}
}  // namespace
int main() {
    Memory memory;
    Services services(memory);
    const uint32_t memcpyGate = services.address("_memcpy");
    assert(memory.fetch32(memcpyGate) == (0xef000000 | ((memcpyGate - 0x6f000000U) / 8 + 1)));
    assert(memory.fetch32(memcpyGate + 4) == 0xe12fff1e);
    assert(services.count() > 7);

    std::vector<uint8_t> text(4096, 0), data(4096, 0);
    memory.map({0x1000, 4096, uint8_t(Read | Execute), "guest return code"}, text.data(),
               text.size());
    memory.map({0x3000, 4096, uint8_t(Read | Write), "service data"}, data.data(), data.size());

    ExportGraph graph;
    services.install(graph);
    // libSystem, CoreFoundation, CoreGraphics, libstdc++.6 and libgcc_s.1 are installed
    // by the service layer before the main executable.
    size_t executable = graph.addImage("<executable>");
    assert(executable == 5);
    graph.setMainExecutable(executable);
    assert(!graph.resolve(executable, -1, "_memcpy"));
    graph.define(executable, "_local", {0x999, false});
    assert(graph.resolve(executable, -1, "_local") == 0x999u);
    graph.setMainExecutable(1);
    assert(graph.resolve(executable, -1, "_CFRelease"));
    graph.setMainExecutable(executable);
    assert(graph.resolve(executable, -2, "_memcpy") == memcpyGate);
    assert(!graph.resolve(executable, -2, "_missing"));
    assert(graph.strong(1, "_CFRelease"));
    assert(graph.resolve(executable, -2, "_malloc") == services.address("_malloc"));
    assert(graph.hasStrong("_CFRelease") && !graph.hasStrong("_missing"));
    // Each gate is defined under the library that exports it on iOS, so strict
    // ordinals from a real binary resolve instead of failing at bind time.
    graph.dependencies(executable,
                       {"/usr/lib/libSystem.B.dylib",
                        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                        "/usr/lib/libstdc++.6.dylib", "/usr/lib/libgcc_s.1.dylib"});
    assert(graph.resolve(executable, 4, "_Znwm") == services.address("_Znwm"));
    assert(graph.resolve(executable, 5, "___divsi3") == services.address("___divsi3"));
    assert(!graph.resolve(executable, 4, "___divsi3"));
    assert(graph.resolve(executable, 2, "_CFRelease") == services.address("_CFRelease"));
    assert(graph.resolve(executable, 3, "_CGColorSpaceCreateDeviceRGB") ==
           services.address("_CGColorSpaceCreateDeviceRGB"));
    fault("EXPORT_IMAGE", [&] { graph.addImage("<executable>"); });

    ExportGraph deps;
    size_t exe = deps.addImage("<exe>");
    size_t system = deps.addImage("/usr/lib/libSystem.B.dylib");
    deps.alias(system, "/usr/lib/libSystem.dylib");
    deps.dependencies(exe, {"/usr/lib/libSystem.dylib"});
    deps.define(system, "_puts", {0x1234, false});
    assert(deps.resolve(exe, 1, "_puts") == 0x1234u);
    assert(!deps.resolve(exe, -3, "_missing"));
    fault("EXPORT_ORDINAL", [&] { deps.resolve(exe, -4, "_puts"); });
    fault("EXPORT_DEPENDENCIES", [&] { deps.dependencies(exe, {}); });

    ExportGraph weakGraph;
    size_t requester = weakGraph.addImage("<exe>");
    size_t weakImage = weakGraph.addImage("weak-image");
    size_t strongImage = weakGraph.addImage("strong-image");
    weakGraph.define(weakImage, "_mostlyEqual", {0x1111, true});
    weakGraph.define(strongImage, "_mostlyEqual", {0x2222, false});
    assert(weakGraph.resolve(requester, -2, "_mostlyEqual") == 0x1111u);
    assert(weakGraph.resolve(requester, -3, "_mostlyEqual") == 0x2222u);
    assert(weakGraph.resolve(requester, -3, "_onlyWeak") == std::nullopt);

    Memory pages;
    std::vector<uint8_t> page(4096, 0);
    pages.map({0x2000, 4096, uint8_t(Read | Write), "fixups"}, page.data(), page.size());
    pages.write32(0x2008, 0xdeadbeef);
    pages.write32(0x200c, 0xdeadbeef);

    DyldPlan coalesced;
    coalesced.needsCoalescing = true;
    coalesced.fixups.push_back({0x2000, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.fixups.push_back({0x2004, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.fixups.push_back({0x2000, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.applyWithGraph(pages, 0, weakGraph, requester);
    assert(pages.read32(0x2000) == 0x2222 && pages.read32(0x2004) == 0x2222);
    assert(pages.read32(0x2008) == 0xdeadbeef);

    DyldPlan conflict;
    conflict.fixups.push_back({0x2008, true, false, false, 0, 0, ""});
    conflict.fixups.push_back({0x2008, false, false, false, -2, 0, "_mostlyEqual"});
    fault("DYLD_TARGET", [&] { conflict.applyWithGraph(pages, 0, weakGraph, requester); });
    DyldPlan addendConflict;
    addendConflict.needsCoalescing = true;
    addendConflict.fixups.push_back({0x2008, false, false, false, -3, 0, "_mostlyEqual"});
    addendConflict.fixups.push_back({0x2008, false, false, false, -3, 4, "_mostlyEqual"});
    fault("DYLD_TARGET", [&] { addendConflict.applyWithGraph(pages, 0, weakGraph, requester); });
    assert(pages.read32(0x2008) == 0xdeadbeef);

    DyldPlan missing;
    missing.fixups.push_back({0x200c, false, false, false, -2, 0, "_notImplemented"});
    fault("DYLD_UNRESOLVED", [&] { missing.applyWithGraph(pages, 0, weakGraph, requester); });
    assert(pages.read32(0x200c) == 0xdeadbeef);

    ExportGraph weakOnly;
    size_t onlyWeak = weakOnly.addImage("weak-only-image");
    weakOnly.define(onlyWeak, "_mostlyEqual", {0x1111, true});
    DyldPlan undeclaredStrong;
    undeclaredStrong.needsCoalescing = true;
    undeclaredStrong.strongDefinitions.push_back("_mostlyEqual");
    fault("DYLD_WEAK_DEFINITION",
          [&] { undeclaredStrong.applyWithGraph(pages, 0, weakOnly, onlyWeak); });

    DyldPlan absentWeak;
    absentWeak.fixups.push_back({0x200c, false, true, false, -3, 0, "_absentWeak"});
    absentWeak.applyWithGraph(pages, 0, weakGraph, requester);
    assert(pages.read32(0x200c) == 0);

    const char source[] = "hello service";
    memory.write(0x3000, source, sizeof(source));
    memory.write(0x3100, "hello", 6);
    memory.write(0x3200, "hellp", 6);
    auto state = call(services, "_strlen", 0x3000);
    assert(state.registers[0] == 13);
    state = call(services, "_strcmp", 0x3100, 0x3100);
    assert(state.registers[0] == 0);
    state = call(services, "_strcmp", 0x3100, 0x3200);
    assert(int32_t(state.registers[0]) < 0);
    state = call(services, "_memcpy", 0x3300, 0x3000, 14);
    assert(state.registers[0] == 0x3300);
    std::vector<uint8_t> copied(14);
    memory.read(0x3300, copied.data(), copied.size());
    assert(std::memcmp(copied.data(), source, 14) == 0);
    state = call(services, "_memcmp", 0x3300, 0x3000, 14);
    assert(state.registers[0] == 0);
    state = call(services, "_memset", 0x3400, 0x5a, 8);
    assert(state.registers[0] == 0x3400);
    memory.read(0x3400, copied.data(), 8);
    for (int i = 0; i < 8; i++) assert(copied[i] == 0x5a);
    state = call(services, "_bzero", 0x3400, 8);
    memory.read(0x3400, copied.data(), 8);
    for (int i = 0; i < 8; i++) assert(copied[i] == 0);
    fault("LIBC_OVERLAP", [&] { call(services, "_memcpy", 0x3100, 0x3102, 8); });
    fault("LIBC_LIMIT", [&] { call(services, "_memset", 0x3100, 1, 32 * 1024 * 1024); });

    state = call(services, "_memcpy", 0x3500, 0x3100, 4, 0x1001);
    assert(state.registers[15] == 0x1000 && (state.cpsr & 0x20));
    state = call(services, "_memcpy", 0x3500, 0x3100, 4, 0x1000);
    assert(state.registers[15] == 0x1000 && !(state.cpsr & 0x20));
    fault("HLE_RETURN", [&] { call(services, "_memcpy", 0x3500, 0x3100, 4, 0x9001); });

    CpuState forged;
    forged.registers[14] = 0x1000;
    forged.registers[15] = 0x1004;
    CpuStop intercepted{StopReason::SupervisorCall, 1, 0x1000, 1, 0, "CPU_SVC", ""};
    fault("HLE_TRAP", [&] { services.dispatch(intercepted, forged); });
    CpuStop wrongNumber{StopReason::SupervisorCall, 1, memcpyGate, 2, 0, "CPU_SVC", ""};
    fault("HLE_TRAP", [&] { services.dispatch(wrongNumber, forged); });
    CpuStop wrongMode = wrongNumber;
    wrongMode.service = 1;
    forged.cpsr = 0x30;
    fault("HLE_TRAP", [&] { services.dispatch(wrongMode, forged); });
    std::cout << "Export graph, weak coalescing, call gates and service dispatch tests passed\n";
}
