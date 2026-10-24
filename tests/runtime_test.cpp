#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include "runtime/error.h"
#include "runtime/launch.h"
#include "runtime/loader.h"
#include "runtime/memory.h"
#include "runtime/objc.h"
#include "runtime/posix.h"
#include "runtime/services.h"
using namespace podnative::runtime;
using Bytes = std::vector<uint8_t>;
void put(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b.at(at + i) = uint8_t(v >> (i * 8));
}
template <class F>
void fault(const char* code, F action) {
    try {
        action();
        std::cerr << "Expected fault " << code << '\n';
        std::abort();
    } catch (const Error& e) {
        if (e.code != code) {
            std::cerr << e.what() << " expected " << code << '\n';
            std::abort();
        }
    }
}
Bytes executable(bool thumb = false) {
    Bytes b(4096, 0);
    put(b, 0, 0xfeedface);
    put(b, 4, 12);
    put(b, 8, 9);
    put(b, 12, 2);
    put(b, 16, 2);
    put(b, 20, 80);
    put(b, 28, 1);
    put(b, 32, 56);
    std::memcpy(b.data() + 36, "__TEXT", 6);
    put(b, 52, 0x1000);
    put(b, 56, 4096);
    put(b, 60, 0);
    put(b, 64, 4096);
    put(b, 68, 5);
    put(b, 72, 5);
    put(b, 84, 0x80000028);
    put(b, 88, 24);
    put(b, 92, 0x100 + (thumb ? 1 : 0));
    put(b, 0x100, thumb ? 0xdf00202a : 0xe3a0002a);
    put(b, 0x104, 0xef000000);
    return b;
}
void appendUleb(Bytes& bytes, uint32_t value) {
    do {
        uint8_t byte = uint8_t(value & 0x7f);
        value >>= 7;
        if (value) byte |= 0x80;
        bytes.push_back(byte);
    } while (value);
}
Bytes combinedFixupBudgetFixture() {
    // Both valid dyld streams stay below the per-stream limit; together they exceed the
    // image-wide budget. Rebase and bind the same mapped slots so the test covers aggregate
    // input work, not a malformed target or a target-count-only check.
    constexpr uint32_t Count = 500001;
    constexpr uint32_t RebaseOffset = 0x4f0000, BindOffset = 0x4f0100;
    constexpr uint32_t FileBytes = 0x500000;
    Bytes b = executable();
    b.resize(FileBytes, 0);
    put(b, 16, 3);
    put(b, 20, 128);
    put(b, 56, FileBytes);  // __TEXT vm size
    put(b, 64, FileBytes);  // and file size
    put(b, 68, 7);          // maxprot permits dyld writes
    put(b, 72, 7);
    put(b, 84, 0x80000022);  // LC_DYLD_INFO_ONLY
    put(b, 88, 48);
    for (size_t at = 92; at < 132; at += 4) put(b, at, 0);
    Bytes rebase{0x20, 0x00, 0x80};  // segment 0, offset 0, rebase count with skip
    appendUleb(rebase, Count);
    rebase.push_back(0);  // skip zero bytes between pointer slots
    rebase.push_back(0);  // DONE
    Bytes bind{0x40};     // SET_SYMBOL_TRAILING_FLAGS_IMM, regular strong import
    const std::string symbol = "_test";
    bind.reserve(32);
    for (char value : symbol) bind.push_back(uint8_t(value));
    bind.push_back(0);
    bind.push_back(0x70);  // segment 0, offset 0
    bind.push_back(0);
    bind.push_back(0xc0);  // bind count with skip
    appendUleb(bind, Count);
    bind.push_back(0);
    bind.push_back(0);  // DONE
    put(b, 92, RebaseOffset);
    put(b, 96, uint32_t(rebase.size()));
    put(b, 100, BindOffset);
    put(b, 104, uint32_t(bind.size()));
    std::copy(rebase.begin(), rebase.end(), b.begin() + RebaseOffset);
    std::copy(bind.begin(), bind.end(), b.begin() + BindOffset);
    put(b, 132, 0x80000028);  // LC_MAIN
    put(b, 136, 24);
    put(b, 140, 0x100);
    put(b, 148, 0);
    return b;
}
Bytes executableWithRebasedObjectiveC(bool rebaseClassList = true) {
    Bytes b(8192, 0);
    put(b, 0, 0xfeedface);
    put(b, 4, 12);
    put(b, 8, 9);
    put(b, 12, 2);
    put(b, 16, 4);
    put(b, 20, 252);
    put(b, 28, 1);
    put(b, 32, 56);
    std::memcpy(b.data() + 36, "__TEXT", 6);
    put(b, 52, 0x1000);
    put(b, 56, 4096);
    put(b, 60, 0);
    put(b, 64, 4096);
    put(b, 68, 5);
    put(b, 72, 5);
    put(b, 84, 1);
    put(b, 88, 124);
    std::memcpy(b.data() + 92, "__DATA", 6);
    put(b, 108, 0x2000);
    put(b, 112, 4096);
    put(b, 116, 4096);
    put(b, 120, 4096);
    put(b, 124, 7);
    put(b, 128, 3);
    put(b, 132, 1);
    std::memcpy(b.data() + 140, "__objc_classlist", 16);
    std::memcpy(b.data() + 156, "__DATA", 6);
    put(b, 172, 0x2000);
    put(b, 176, 4);
    put(b, 180, 4096);
    put(b, 184, 2);
    put(b, 208, 0x80000022);
    put(b, 212, 48);
    put(b, 216, rebaseClassList ? 0x300 : 0);
    put(b, 220, rebaseClassList ? 4 : 0);
    put(b, 256, 0x80000028);
    put(b, 260, 24);
    put(b, 264, 0x400);
    put(b, 4096, rebaseClassList ? 0x3000 : 0x4000);
    if (rebaseClassList) {
        // The list slot initially points outside mapped memory; the rebase fixes it to 0x4000.
        const uint8_t rebase[] = {0x21, 0x00, 0x51, 0x00};
        std::copy(std::begin(rebase), std::end(rebase), b.begin() + 0x300);
    }
    put(b, 0x400, 0xe3a0002a);
    put(b, 0x404, 0xef000000);
    return b;
}
Bytes readOnlyFixupFixture(uint32_t maximumProtection = uint32_t(Read | Write)) {
    Bytes b = executable();
    b.resize(8192, 0);
    put(b, 16, 4);
    put(b, 20, 184);
    // A final-read-only __DATA_CONST-like segment whose maxprot permits dyld's rebase.
    put(b, 84, 1);
    put(b, 88, 56);
    std::memcpy(b.data() + 92, "__DATA_CONST", 12);
    put(b, 108, 0x2000);
    put(b, 112, 4096);
    put(b, 116, 4096);
    put(b, 120, 4096);
    put(b, 124, maximumProtection);
    put(b, 128, Read);
    put(b, 132, 0);
    put(b, 140, 0x80000022);  // LC_DYLD_INFO_ONLY
    put(b, 144, 48);
    for (size_t at = 148; at < 188; at += 4) put(b, at, 0);
    const uint8_t rebase[] = {0x21, 0, 0x51, 0};  // segment 1, offset zero, rebase once
    put(b, 148, 0x300);
    put(b, 152, sizeof(rebase));
    std::copy(rebase, rebase + sizeof(rebase), b.begin() + 0x300);
    put(b, 188, 0x80000028);  // LC_MAIN
    put(b, 192, 24);
    put(b, 196, 0x100);
    put(b, 204, 0);
    put(b, 4096, 0x1234);
    return b;
}
Bytes executableWithInitializer() {
    Bytes b = executable();
    // Grow the __TEXT segment command by one section and move LC_MAIN after it.
    put(b, 20, 148);
    put(b, 32, 124);
    put(b, 76, 1);  // one section
    std::memcpy(b.data() + 84, "__mod_init_func", 15);
    std::memcpy(b.data() + 100, "__TEXT", 6);
    put(b, 116, 0x1f00);  // section vm address
    put(b, 120, 4);       // one function pointer
    put(b, 124, 0xf00);   // file offset inside __TEXT
    put(b, 128, 2);       // four-byte alignment
    put(b, 132, 0);       // no section relocations
    put(b, 136, 0);
    put(b, 140, 9);  // S_MOD_INIT_FUNC_POINTERS
    put(b, 144, 0);
    put(b, 148, 0);
    put(b, 152, 0x80000028);
    put(b, 156, 24);
    put(b, 160, 0x100);
    put(b, 168, 0);
    put(b, 0xf00, 0x1100);
    return b;
}
int main() {
    Memory m(3 * 4096);
    m.map({0x1000, 4096, uint8_t(Read | Write), "data1"});
    m.map({0x2000, 4096, uint8_t(Read | Write), "data2"});
    m.map({0x3000, 4096, uint8_t(Read | Execute), "code"});
    Posix threadReport(m);
    CpuState threadState{};
    threadState.registers[15] = 0x74da8;
    threadReport.adoptMainThread(threadState);
    assert(!threadReport.mainRunLoopWaiting());
    threadReport.blockInRunLoop();
    assert(threadReport.mainRunLoopWaiting());
    const std::string threadSummary = threadReport.describe();
    assert(threadSummary.find("pc=0x74da8") != std::string::npos);
    assert(threadSummary.find("pc=0x478632") == std::string::npos);
    assert(threadSummary.find("parked in the UIKit run loop") != std::string::npos);
    // Objective-C guest logs must always use Services' capture sink. Turning verbose tracing
    // on or off must not redirect them to the host process's stdout.
    {
        Memory consoleMemory;
        consoleMemory.map({0x1000, 4096, uint8_t(Read | Execute), "console return"});
        consoleMemory.map({0x2000, 4096, uint8_t(Read | Write), "console strings"});
        Services consoleServices(consoleMemory);
        std::vector<std::string> captured;
        consoleServices.setLogSink([&](const std::string& line) { captured.push_back(line); });
        const char format[] = "console log %d";
        consoleMemory.write(0x2000, format, sizeof(format));
        CpuState consoleState{};
        consoleState.cpsr = 0x10;
        consoleState.registers[0] = 0x2000;
        consoleState.registers[1] = 7;
        consoleState.registers[14] = 0x1005;
        ObjC gateLookup(consoleMemory);
        const uint32_t gate = gateLookup.gateAddress("_NSLog");
        const CpuStop consoleStop{StopReason::SupervisorCall, 0, gate, 0, 0, {}, {}};
        consoleServices.setVerbose(false);
        consoleState.registers[15] = gate + 4;
        consoleServices.dispatch(consoleStop, consoleState);
        assert(captured.size() == 1 && captured.back() == "console log 7");
        consoleServices.setVerbose(true);
        consoleState.cpsr = 0x10;
        consoleState.registers[15] = gate + 4;
        consoleServices.dispatch(consoleStop, consoleState);
        assert(captured.size() == 2 && captured.back() == "console log 7");
        assert(consoleServices.objectiveCTrace().size() == 1);
        consoleServices.setVerbose(false);
        consoleState.cpsr = 0x10;
        consoleState.registers[15] = gate + 4;
        consoleServices.dispatch(consoleStop, consoleState);
        assert(captured.size() == 3 && consoleServices.logSize() == 3);
    }
    m.write32(0x1ffe, 0x12345678);
    assert(m.read32(0x1ffe) == 0x12345678);
    assert(m.read32(0x1000) == 0);
    fault("MEMORY_ACCESS", [&] { m.write32(0x3000, 0); });
    fault("MEMORY_ACCESS", [&] { m.fetch32(0x1000); });
    fault("CPU_ALIGNMENT", [&] { m.fetch32(0x3002); });
    fault("CPU_ALIGNMENT", [&] { m.fetch16(0x3001); });
    // A null access is named as a null dereference: "unmapped at 0x0" leaves the reader
    // guessing whether a missing object, a nil message result or a bad index caused it.
    try {
        m.read32(0);
        assert(false);
    } catch (const Error& e) {
        assert(e.code == "MEMORY_NULL" && e.address == 0 &&
               std::string(e.what()).find("null pointer") != std::string::npos);
    }
    fault("MEMORY_OVERFLOW", [&] { m.read32(0xffffffff); });
    fault("MEMORY_ALIGNMENT", [&] { m.map({1, 4096, Read, "bad"}); });
    fault("MEMORY_OVERLAP", [&] { m.map({0x1000, 4096, Read, "bad"}); });
    fault("MEMORY_BUDGET", [&] { m.map({0x4000, 4096, Read, "too much"}); });
    assert(m.allocated() == 12288);
    m.write32(0x2ffc, 0x11223344);
    const uint8_t input[8] = {};
    fault("MEMORY_ACCESS", [&] { m.write(0x2ffc, input, 8); });
    assert(m.read32(0x2ffc) ==
           0x11223344);  // rejected write was atomic across permission boundaries
    Memory top;
    top.map({0xfffff000, 4096, uint8_t(Read | Write), "last page"});
    top.write32(0xfffffffc, 0xabcdef01);
    assert(top.read32(0xfffffffc) == 0xabcdef01);
    assert(!top.permits(0xfffffffc, 8, Read));
    Memory init;
    uint8_t bytes[] = {1, 2, 3, 4};
    init.map({0x1000, 4096, uint8_t(Read | Execute), "initialized"}, bytes, 4);
    assert(init.fetch32(0x1000) == 0x04030201);
    assert(init.fetch16(0x1000) == 0x0201);
    auto b = executable();
    auto image = loadMachO(b.data(), b.size());
    assert(image.registers[15] == 0x1100 && image.registers[13] % 16 == 0);
    assert(image.memory->fetch32(0x1100) == 0xe3a0002a && !image.requiresDyld);
    assert(image.memory->read32(0x70000000) == 0);
    assert(!image.memory->permits(0x6ffff000, 4, Read));
    b = readOnlyFixupFixture();
    image = loadMachO(b.data(), b.size());
    assert(image.dyld.fixups.size() == 1);
    assert(!image.memory->permits(0x2000, 4, Write));
    assert(image.memory->permitsLoader(0x2000, 4, Write));
    image.dyld.apply(*image.memory, 0x100, {});
    assert(image.memory->read32(0x2000) == 0x1334);
    assert(!image.memory->permits(0x2000, 4, Write));
    b = executableWithRebasedObjectiveC();
    image = loadMachO(b.data(), b.size());
    assert(image.objcClassListSections.size() == 1);
    fault("LOAD_OBJECTIVE_C", [&] { (void)image.objcClassAddresses(); });
    image.memory->map({0x4000, 4096, uint8_t(Read | Write), "rebased guest class"});
    image.memory->map({0x5000, 4096, uint8_t(Read | Write), "rebased class metadata"});
    image.memory->write32(0x4000, 0);       // isa
    image.memory->write32(0x4004, 0);       // superclass
    image.memory->write32(0x4010, 0x4000);  // class_data_bits_t before its rebase
    image.memory->write32(0x5008, 210);     // byte-sized modern class_ro_t instanceSize
    image.memory->write32(0x5010, 0x5100);
    const char appController[] = "AppController";
    image.memory->write(0x5100, appController, sizeof(appController));
    image.dyld.fixups.push_back({0x4010, true, false, false, 0, 0, {}});
    image.dyld.apply(*image.memory, 0x1000, {});
    assert(image.memory->read32(0x2000) == 0x4000);
    assert(image.memory->read32(0x4010) == 0x5000);
    ObjC guestObjC(*image.memory);
    guestObjC.registerGuestClasses(image.objcClassAddresses());
    assert(guestObjC.guestClassSummary().find("AppController") != std::string::npos);
    const uint32_t guestInstance = guestObjC.allocateGuestInstance(0x4000);
    assert(guestInstance && image.memory->read32(guestInstance) == 0x4000);

    // With a readable class-list entry but an unfixed class_data_bits_t, the old ordering
    // fell back to the legacy layout and reported the garbage vtable word as AppController's
    // implausible instance size. Once dyld rebases the class-data pointer, the same class is
    // correctly read as modern class_ro_t metadata.
    b = executableWithRebasedObjectiveC(false);
    image = loadMachO(b.data(), b.size());
    image.memory->map({0x4000, 4096, uint8_t(Read | Write), "unfixed Objective-C class"});
    image.memory->map({0x5000, 4096, uint8_t(Read | Write), "fixed Objective-C class data"});
    image.memory->write32(0x4008, 0x5100);      // legacy fallback looks like AppController
    image.memory->write32(0x4010, 0x4000);      // pre-dyld class_data_bits_t is not class_ro_t
    image.memory->write32(0x4014, 0x80000000);  // legacy fallback sees an implausible size
    image.memory->write32(0x5008, 210);
    image.memory->write32(0x5010, 0x5100);
    image.memory->write(0x5100, appController, sizeof(appController));
    ObjC orderedObjC(*image.memory);
    try {
        orderedObjC.registerGuestClasses(image.objcClassAddresses());
        std::cerr << "pre-dyld class data was accepted as Objective-C metadata\n";
        std::abort();
    } catch (const Error& e) {
        assert(e.code == "OBJC_CLASS_FORMAT");
        assert(std::string(e.what()).find("AppController") != std::string::npos);
    }
    image.dyld.fixups.push_back({0x4010, true, false, false, 0, 0, {}});
    image.dyld.apply(*image.memory, 0x1000, {});
    orderedObjC.registerGuestClasses(image.objcClassAddresses());
    assert(orderedObjC.guestClassSummary().find("AppController") != std::string::npos);
    b = readOnlyFixupFixture(uint32_t(Read));
    fault("DYLD_TARGET", [&] { loadMachO(b.data(), b.size()); });
    b = executableWithInitializer();
    image = loadMachO(b.data(), b.size());
    assert(image.initializerSections.size() == 1);
    assert(image.initializerSections[0].first == 0x1f00);
    assert(image.initializerSections[0].second == 1);
    assert(image.memory->read32(0x1f00) == 0x1100);
    b = executableWithInitializer();
    put(b, 120, 3);
    fault("LOAD_INITIALIZER", [&] { loadMachO(b.data(), b.size()); });
    b = executable(true);
    image = loadMachO(b.data(), b.size());
    assert(image.cpsr & 0x20);
    assert(image.memory->fetch16(0x1100) == 0x202a);
    // PIE is accepted: mapping happens at the un-slid vmaddrs and every rebase is
    // applied before execution, so a slide of zero is exactly what the fixups describe.
    b = executable();
    put(b, 24, 0x200000);
    image = loadMachO(b.data(), b.size());
    assert(image.registers[15] == 0x1100);
    // ARMv6 binaries select v6 semantics instead of being rejected or run as ARMv7.
    b = executable();
    put(b, 8, 6);
    image = loadMachO(b.data(), b.size());
    assert(image.cpuSubtype == 6 && image.arch == IsaArch::ArmV6);
    assert(image.summary().find("ARMv6") != std::string::npos);
    b = executable();
    put(b, 8, 12);
    try {
        loadMachO(b.data(), b.size());
        std::cerr << "unsupported ARM subtype was accepted\n";
        std::abort();
    } catch (const Error& e) {
        assert(e.code == "LOAD_FORMAT" &&
               std::string(e.what()).find("MACHO_CPU_SUBTYPE") != std::string::npos);
    }
    b = executable();
    put(b, 92, 0x5000);
    fault("LOAD_ENTRY", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 92, 0x102);
    fault("LOAD_ENTRY_ALIGNMENT", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 72, 7);
    fault("LOAD_SEGMENT_PROTECTION", [&] { loadMachO(b.data(), b.size()); });
    b = executable();
    put(b, 56, 8192);
    image = loadMachO(b.data(), b.size());
    assert(image.memory->read32(0x2000) == 0);
    b = executable();
    fault("MEMORY_BUDGET", [&] { loadMachO(b.data(), b.size(), 4096); });
    b = executable();
    put(b, 84, 0xffffffff);
    fault("LOAD_COMMAND_UNSUPPORTED", [&] { loadMachO(b.data(), b.size()); });
    // ARM_THREAD_STATE PC/CPSR, exact bounds and duplicate entries.
    b = executable();
    b.resize(4096);
    put(b, 20, 140);
    put(b, 84, 5);
    put(b, 88, 84);
    put(b, 92, 1);
    put(b, 96, 17);
    std::fill(b.begin() + 100, b.begin() + 168, 0);
    put(b, 160, 0x1100);
    put(b, 164, 0x10);
    image = loadMachO(b.data(), b.size());
    assert(image.registers[15] == 0x1100);
    put(b, 164, 0x13);
    fault("LOAD_CPSR", [&] { loadMachO(b.data(), b.size()); });
    // A zero-backed guard is still reserved and must not be used for the stack.
    b = executable();
    put(b, 16, 3);
    put(b, 20, 136);
    put(b, 108, 1);
    put(b, 112, 56);
    put(b, 132, 0x70000000);
    put(b, 136, 4096);
    fault("LOAD_STACK_OVERLAP", [&] { loadMachO(b.data(), b.size()); });
    // LC_DYSYMTAB may precede LC_SYMTAB; validate all spans and linkedit ranges.
    auto dynamic = [] {
        auto fixture = executable();
        put(fixture, 16, 4);
        put(fixture, 20, 184);
        put(fixture, 108, 0xb);
        put(fixture, 112, 80);
        put(fixture, 188, 2);
        put(fixture, 192, 24);
        put(fixture, 196, 0x300);
        put(fixture, 200, 3);
        put(fixture, 204, 0x340);
        put(fixture, 208, 1);
        put(fixture, 116, 0);
        put(fixture, 120, 1);
        put(fixture, 124, 1);
        put(fixture, 128, 1);
        put(fixture, 132, 2);
        put(fixture, 136, 1);
        put(fixture, 164, 0x350);
        put(fixture, 168, 3);
        put(fixture, 0x350, 2);
        put(fixture, 0x354, 0x80000000);
        put(fixture, 0x358, 0xc0000000);
        return fixture;
    };
    auto boundImports = [] {
        auto fixture = executable();
        put(fixture, 16, 3);
        put(fixture, 20, 128);
        put(fixture, 68, 7);
        put(fixture, 72, 7);
        put(fixture, 84, 0x80000022);  // LC_DYLD_INFO_ONLY
        put(fixture, 88, 48);
        for (size_t offset = 92; offset < 132; offset += 4) put(fixture, offset, 0);

        std::vector<uint8_t> stream{0x3e, 0x40};  // flat lookup, then symbol
        const std::string missing = "_missing_api";
        stream.insert(stream.end(), missing.begin(), missing.end());
        stream.push_back(0);
        stream.push_back(0x70);  // segment 0, offset 0x200
        stream.push_back(0x80);
        stream.push_back(0x04);
        stream.push_back(0x90);
        stream.push_back(0x90);  // two references to the same unresolved import
        stream.push_back(0x40);
        const std::string implemented = "__ZTVN10__cxxabiv117__class_type_infoE";
        stream.insert(stream.end(), implemented.begin(), implemented.end());
        stream.push_back(0);
        stream.push_back(0x90);
        stream.push_back(0x90);  // two resolved references should not appear in the report
        stream.push_back(0);
        put(fixture, 100, 0x300);  // bind_off
        put(fixture, 104, uint32_t(stream.size()));
        put(fixture, 132, 0x80000028);  // LC_MAIN
        put(fixture, 136, 24);
        put(fixture, 140, 0x100);
        put(fixture, 148, 0);
        std::copy(stream.begin(), stream.end(), fixture.begin() + 0x300);
        return fixture;
    };
    b = combinedFixupBudgetFixture();
    fault("DYLD_LIMIT", [&] { loadMachO(b.data(), b.size()); });
    b = dynamic();
    image = loadMachO(b.data(), b.size());
    assert(image.requiresDyld);
    // Without an integrated CPU the report must still name the audit, never claim execution.
    // A dynamic image with no bind fixups still reports the missing bootstrap, and the
    // plain executable reports the un-integrated CPU; neither may claim execution.
    assert(attemptLaunch(b.data(), b.size()).find("GUEST_ABI_UNIMPLEMENTED") != std::string::npos);
    assert(attemptLaunch(b.data(), b.size()).find("DYLD_EXPORT_AUDIT") != std::string::npos);
    b = boundImports();
    image = loadMachO(b.data(), b.size());
    assert(image.dyld.fixups.size() == 4);
    const std::string importReport = attemptLaunch(b.data(), b.size());
    assert(importReport.find("CPU_NOT_INTEGRATED") != std::string::npos);
    assert(importReport.find("registered service gate names=") != std::string::npos);
    assert(importReport.find("unresolved required imports=2 references across 1 unique "
                             "symbol/ordinal pairs") != std::string::npos);
    assert(
        importReport.find("MISSING_REQUIRED_IMPORTS:\n  _missing_api (2 references; ordinal=-2)") !=
        std::string::npos);
    assert(importReport.find("_missing_api") == importReport.rfind("_missing_api"));
    assert(importReport.find("__ZTVN10__cxxabiv117__class_type_infoE") == std::string::npos);
    assert(importReport.find("required ordinal=") == std::string::npos);
    assert(importReport.find("0 pointer rebases, 4 pointer bindings") != std::string::npos);
    assert(importReport.find("implemented APIs") == std::string::npos);
    b = executable();
    assert(attemptLaunch(b.data(), b.size()).find("GUEST_ABI_UNIMPLEMENTED") != std::string::npos);
    std::atomic<bool> cancelledBeforeLoad{true};
    LaunchOptions cancelledOptions;
    cancelledOptions.cancelRequested = &cancelledBeforeLoad;
    assert(attemptLaunch(nullptr, 0, "", "", cancelledOptions).find("LAUNCH_CANCELLED") !=
           std::string::npos);
    b = dynamic();
    put(b, 120, 4);
    fault("LOAD_DYSYMTAB", [&] { loadMachO(b.data(), b.size()); });
    b = dynamic();
    put(b, 124, 0);
    fault("LOAD_DYSYMTAB", [&] { loadMachO(b.data(), b.size()); });
    b = dynamic();
    put(b, 0x350, 3);
    fault("LOAD_DYSYMTAB", [&] { loadMachO(b.data(), b.size()); });
    b = dynamic();
    put(b, 0x354, 0x80000001);
    fault("LOAD_DYSYMTAB", [&] { loadMachO(b.data(), b.size()); });
    for (size_t table = 0; table < 6; table++) {
        b = dynamic();
        put(b, 140 + table * 8, 4092);
        put(b, 144 + table * 8, 2);
        fault("LOAD_BOUNDS", [&] { loadMachO(b.data(), b.size()); });
    }
    b = dynamic();
    put(b, 112, 76);
    put(b, 184, 0);
    put(b, 188, 0);  // malformed command chain
    assert(attemptLaunch(b.data(), b.size()).find("LAUNCH_BLOCKED:") == 0);
    // Legacy indirect pointers (LC_DYSYMTAB), without any LC_DYLD_INFO stream.
    b = executable();
    put(b, 16, 5);
    put(b, 20, 284);
    put(b, 32, 124);
    put(b, 68, 7);
    put(b, 72, 7);
    put(b, 76, 1);
    std::fill(b.begin() + 84, b.begin() + 312, 0);
    put(b, 116, 0x1300);
    put(b, 120, 4);
    put(b, 124, 0x300);
    put(b, 140, 6);
    put(b, 152, 0x80000028);
    put(b, 156, 24);
    put(b, 160, 0x200);
    put(b, 176, 2);
    put(b, 180, 24);
    put(b, 184, 0x400);
    put(b, 188, 1);
    put(b, 192, 0x420);
    put(b, 196, 7);
    put(b, 200, 0xb);
    put(b, 204, 80);
    put(b, 228, 1);
    put(b, 256, 0x430);
    put(b, 260, 1);
    put(b, 280, 0xc);
    put(b, 284, 32);
    put(b, 288, 24);
    std::memcpy(b.data() + 304, "libX", 5);
    put(b, 0x400, 1);
    b[0x404] = 1;
    b[0x407] = 1;
    std::memcpy(b.data() + 0x421, "_test", 6);
    image = loadMachO(b.data(), b.size());
    assert(image.dyld.fixups.size() == 1 && image.dyld.fixups[0].symbol == "_test" &&
           image.dyld.fixups[0].ordinal == 1);
    image.dyld.apply(
        *image.memory, 0,
        [](int, const std::string&, bool) -> std::optional<uint32_t> { return 0x4455; });
    assert(image.memory->read32(0x1300) == 0x4455);
    put(b, 144, 1);
    fault("DYLD_INDIRECT_SECTION", [&] { loadMachO(b.data(), b.size()); });
    // Classic symbol stubs: each is rewritten to "ldr r12, [pc] / bx r12 / .word target" so
    // no uninitialised lazy pointer is ever executed. Resolved symbols jump to the real
    // gate, missing ones to the named trap, and unresolved weak ones to null.
    {
        LoadedImage stubImage;
        stubImage.memory = std::make_unique<Memory>();
        const uint32_t base = 0x5000;
        stubImage.memory->map({base, 4096, uint8_t(Read | Execute), "stub section"});
        stubImage.stubs = {{base, 16, "_resolved", 1, false},
                           {base + 16, 16, "_missing", 1, false},
                           {base + 32, 16, "_optional", 1, true}};
        ExportGraph stubGraph;
        size_t system = stubGraph.addImage("libSystem.B.dylib");
        size_t exe = stubGraph.addImage("<exe>");
        stubGraph.dependencies(exe, {"libSystem.B.dylib"});
        stubGraph.define(system, "_resolved", {0x6f000008, false});
        bindLegacyStubs(stubImage, stubGraph, exe, [](const std::string& name) {
            return name == "_missing" ? 0x6f100000u : 0xdeadbeefu;
        });
        auto readWord = [&](uint32_t address) { return stubImage.memory->fetch32(address); };
        assert(readWord(base) == 0xe59fc000);
        assert(readWord(base + 4) == 0xe12fff1c);
        assert(readWord(base + 8) == 0x6f000008);
        assert(readWord(base + 12) == 0xe1a00000);
        assert(readWord(base + 16 + 8) == 0x6f100000);
        assert(readWord(base + 32 + 8) == 0);
        // A stub section that is not executable code is refused instead of patched blindly.
        stubImage.memory->map({0x6000, 4096, uint8_t(Read | Write), "data not code"});
        stubImage.stubs = {{0x6000, 16, "_resolved", 1, false}};
        fault("DYLD_STUB", [&] { bindLegacyStubs(stubImage, stubGraph, exe, {}); });
        fault("DYLD_STUB", [&] {
            LoadedImage unmapped;
            unmapped.memory = std::make_unique<Memory>();
            unmapped.stubs = {{0x9000, 16, "_resolved", 1, false}};
            bindLegacyStubs(unmapped, stubGraph, exe, {});
        });
    }
    // Classic external relocations: an image linked before LC_DYLD_INFO records its
    // Objective-C class references here, and dyld applies them as bindings. Ignoring the
    // table left every superclass and class-ref slot at zero.
    auto relocated = [] {
        auto fixture = executable();
        put(fixture, 16, 4);    // ncmds
        put(fixture, 20, 184);  // sizeofcmds
        put(fixture, 68, 7);    // maxprot: writable so a relocation target exists
        put(fixture, 72, 7);    // initprot
        put(fixture, 108, 0xb);
        put(fixture, 112, 80);
        put(fixture, 172, 0x360);  // extreloff
        put(fixture, 176, 1);      // nextrel
        put(fixture, 188, 2);      // LC_SYMTAB
        put(fixture, 192, 24);
        put(fixture, 196, 0x300);  // symoff
        put(fixture, 200, 1);      // nsyms
        put(fixture, 204, 0x340);  // stroff
        put(fixture, 208, 16);     // strsize
        put(fixture, 0x300, 1);    // n_strx: "_UIView"
        fixture[0x304] = 0x01;     // N_UNDF | N_EXT
        fixture[0x305] = 0;
        fixture[0x306] = 0;
        fixture[0x307] = 0xff;  // EXECUTABLE_ORDINAL: a self-image reference
        put(fixture, 0x308, 0);
        std::memcpy(fixture.data() + 0x340, "\0_UIView\0", 9);
        put(fixture, 0x360, 0x1f00);                   // r_address
        put(fixture, 0x364, (2u << 25) | (1u << 27));  // r_length=4 bytes, external
        return fixture;
    };
    b = relocated();
    image = loadMachO(b.data(), b.size());
    assert(image.dyld.fixups.size() == 1);
    assert(image.dyld.fixups[0].address == 0x1f00);
    assert(!image.dyld.fixups[0].rebase);
    assert(image.dyld.fixups[0].symbol == "_UIView");
    assert(image.dyld.fixups[0].ordinal == -1);
    // An ordinal past the dependency list is refused instead of being guessed.
    b = relocated();
    b[0x307] = 0x05;
    fault("DYLD_ORDINAL", [&] { loadMachO(b.data(), b.size()); });
    // Only four-byte absolute pointer relocations are pointer fixups.
    b = relocated();
    put(b, 0x364, (1u << 25) | (1u << 27));  // two-byte relocation
    fault("DYLD_RELOCATION", [&] { loadMachO(b.data(), b.size()); });
    b = relocated();
    put(b, 0x360, 0xff8);  // target outside the mapped segment
    fault("DYLD_RELOCATION", [&] { loadMachO(b.data(), b.size()); });

    // A stop report says which guest function was running, so the loader keeps the image's
    // defined code symbols. Local compiler labels are not functions and are dropped.
    {
        auto symbolImage = executable();
        put(symbolImage, 16, 3);
        put(symbolImage, 20, 104);
        put(symbolImage, 108, 2);  // LC_SYMTAB
        put(symbolImage, 112, 24);
        put(symbolImage, 116, 0x800);  // symoff
        put(symbolImage, 120, 4);      // nsyms: two functions, one label, one thumb function
        put(symbolImage, 124, 0x880);  // stroff
        put(symbolImage, 128, 64);
        const char* strings = "\0_main\0-[View draw]\0ltmp0\0_thumb_fn\0";
        std::memcpy(symbolImage.data() + 0x880, strings, 36);
        size_t record = 0x800;
        put(symbolImage, record, 1);
        symbolImage[record + 4] = 0x0f;  // N_SECT | N_EXT
        symbolImage[record + 5] = 1;
        put(symbolImage, record + 8, 0x1100);
        record += 12;
        put(symbolImage, record, 7);
        symbolImage[record + 4] = 0x0f;
        symbolImage[record + 5] = 1;
        put(symbolImage, record + 8, 0x1200);
        record += 12;
        put(symbolImage, record, 20);  // "ltmp0": a branch target, not a function
        symbolImage[record + 4] = 0x0f;
        symbolImage[record + 5] = 1;
        put(symbolImage, record + 8, 0x1300);
        record += 12;
        put(symbolImage, record, 26);  // "_thumb_fn", stored with its interworking bit set
        symbolImage[record + 4] = 0x0f;
        symbolImage[record + 5] = 1;
        put(symbolImage, record + 8, 0x1401);
        auto named = loadMachO(symbolImage.data(), symbolImage.size());
        assert(named.symbols.size() == 3);
        assert(named.nearestSymbol(0x1150) == "main");
        assert(named.nearestSymbol(0x1204) == "-[View draw]");
        assert(named.nearestSymbol(0x1402) == "thumb_fn");
        // Before the first symbol, or implausibly far past the last one, no name is invented.
        assert(named.nearestSymbol(0x1000).empty());
        assert(named.nearestSymbol(0x200000).empty());
        auto stripped = executable();
        assert(loadMachO(stripped.data(), stripped.size()).nearestSymbol(0x1000).empty());
    }
    // Mutate valid headers and command bytes with a low allocation budget. Never execute fuzz data.
    std::mt19937 random(9032);
    for (int i = 0; i < 10000; i++) {
        b = executable();
        for (int j = 0; j < 4; j++) b[random() % 192] = uint8_t(random());
        try {
            (void)loadMachO(b.data(), b.size(), 2 * 1024 * 1024);
        } catch (const Error&) {
        }
    }
    std::cout << "Memory/loader regressions + 10,000 mutated Mach-O inputs passed\n";
}
