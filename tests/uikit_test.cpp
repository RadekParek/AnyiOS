#include "runtime/uikit.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/error.h"
#include "runtime/objc.h"
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
uint32_t nameAddress(Memory& memory, uint32_t base, const std::string& text) {
    // Fixed-size slots: two names sharing an offset would overwrite each other, which is how
    // a test can end up asserting against a class called "BadDelegsomethingElse".
    static uint32_t slot = 0x300;
    const uint32_t address = base + slot;
    slot += 64;
    memory.write(address, text.data(), text.size());
    const char terminator = 0;
    memory.write(address + uint32_t(text.size()), &terminator, 1);
    return address;
}
// Minimal modern-layout guest class with one instance method, so UIApplicationMain has a
// real delegate to instantiate: class object -> class_ro_t -> method list -> IMP.
void buildClass(Memory& memory, uint32_t base, const std::string& name, const std::string& method,
                uint32_t imp) {
    const uint32_t ro = base + 0x100, meta = base + 0x40, methods = base + 0x200;
    memory.write32(base, meta);     // isa
    memory.write32(base + 4, 0);    // superclass: NSObject binds here at load time
    memory.write32(base + 16, ro);  // class_data_bits_t
    memory.write32(meta, meta);     // the metaclass is its own isa
    memory.write32(meta + 4, 0);
    memory.write32(meta + 16, ro);
    memory.write32(ro, 0x10);    // flags
    memory.write32(ro + 4, 4);   // instanceStart
    memory.write32(ro + 8, 24);  // instanceSize
    memory.write32(ro + 16, nameAddress(memory, base, name));
    memory.write32(ro + 20, methods);  // baseMethods
    memory.write32(methods, 12);       // entsizeAndFlags
    memory.write32(methods + 4, 1);    // count
    memory.write32(methods + 8, nameAddress(memory, base, method));
    memory.write32(methods + 12, 0);  // types
    memory.write32(methods + 16, imp);
}
}  // namespace
int main() {
    Memory memory;
    ObjC objc(memory);
    objc.installGates();
    UIKit uikit(memory);
    uikit.installGates();
    std::vector<std::string> log;
    uikit.setLogSink([&](const std::string& line) { log.push_back(line); });
    objc.setLogSink([&](const std::string&) {});
    // Guest image: one page of class metadata, one page of executable guest code, and the
    // literal string the game passes as the delegate class name.
    std::vector<uint8_t> classes(4096, 0), code(4096, 0), literals(4096, 0);
    memory.map({0x300000, 4096, uint8_t(Read | Write), "guest classes"}, classes.data(),
               classes.size());
    memory.map({0x301000, 4096, uint8_t(Read | Execute), "guest code"}, code.data(), code.size());
    memory.map({0x302000, 4096, uint8_t(Read | Write), "guest literals"}, literals.data(),
               literals.size());
    buildClass(memory, 0x300000, "GameAppDelegate", "applicationDidFinishLaunching:",
               0x301001);  // Thumb
    objc.registerGuestClasses({0x300000});
    // @"GameAppDelegate" as a __CFConstantString literal.
    const std::string name = "GameAppDelegate";
    memory.write(0x302100, name.data(), name.size());
    const char terminator = 0;
    memory.write(0x302100 + uint32_t(name.size()), &terminator, 1);
    memory.write32(0x302080, 0);      // isa, bound by dyld in a real image
    memory.write32(0x302084, 0x7c8);  // 8-bit constant string
    memory.write32(0x302088, 0x302100);
    memory.write32(0x30208c, uint32_t(name.size()));
    // argv: {"Game", "/var/mobile/Game.app/Game"}.
    memory.write(0x302200, "Game", 5);
    memory.write(0x302220, "/var/mobile/Game.app/Game", 26);
    memory.write32(0x302180, 0x302200);
    memory.write32(0x302184, 0x302220);

    CpuState state;
    state.registers[0] = 2;          // argc
    state.registers[1] = 0x302180;   // argv
    state.registers[2] = 0;          // principal class name
    state.registers[3] = 0x302080;   // delegate class name literal
    state.registers[14] = 0x300fff;  // return address of UIApplicationMain (Thumb)
    assert(uikit.invoke("_UIApplicationMain", state, objc));
    // The delegate's own launch method is now running.
    assert(state.registers[15] == 0x301000);
    assert((state.cpsr & 0x20) != 0);
    assert(state.registers[1] != 0);  // _cmd
    assert(state.registers[0] != 0);  // the delegate instance
    // The guest returns the BOOL, landing on the lifecycle gate.
    state.registers[0] = 1;
    state.registers[15] = UIKit::LifecycleGate;
    assert(uikit.atLifecycleGate(state.registers[15]));
    uikit.resume(state, objc);
    // UIApplicationMain returns to its caller with the Thumb bit re-applied from the
    // original return address.
    assert(state.registers[15] == 0x300ffe);
    assert((state.cpsr & 0x20) != 0);
    const std::string report = uikit.describe();
    assert(report.find("GameAppDelegate") != std::string::npos);
    assert(report.find("no implemented sources") != std::string::npos);
    assert(!log.empty());
    // A delegate that implements neither launch selector is a named failure, not a silent
    // "started".
    {
        Memory second;
        ObjC other(second);
        other.installGates();
        UIKit next(second);
        next.installGates();
        other.setLogSink([](const std::string&) {});
        std::vector<uint8_t> page(4096, 0), text(4096, 0);
        second.map({0x300000, 4096, uint8_t(Read | Write), "classes"}, page.data(), page.size());
        second.map({0x301000, 4096, uint8_t(Read | Execute), "code"}, text.data(), text.size());
        second.map({0x302000, 4096, uint8_t(Read | Write), "literals"}, text.data(), text.size());
        buildClass(second, 0x300000, "BadDelegate", "somethingElse", 0x301001);
        other.registerGuestClasses({0x300000});
        second.write(0x302100, "BadDelegate", 12);
        second.write32(0x302080, 0);
        second.write32(0x302084, 0x7c8);
        second.write32(0x302088, 0x302100);
        second.write32(0x30208c, 11);
        CpuState bad;
        bad.registers[3] = 0x302080;
        bad.registers[14] = 0x300fff;
        fault("UIKIT_DELEGATE", [&] { next.invoke("_UIApplicationMain", bad, other); });
        assert(next.describe().find("not started") != std::string::npos);
    }
    // A nil delegate class is legal: UIKit would load the main NIB, and this runtime says so
    // instead of claiming the application started.
    {
        Memory third;
        ObjC none(third);
        none.installGates();
        UIKit bootstrap(third);
        bootstrap.installGates();
        none.setLogSink([](const std::string&) {});
        CpuState empty;
        empty.registers[0] = 0;  // argc: no argument vector to read
        empty.registers[1] = 0;
        empty.registers[14] = 0x300fff;
        assert(bootstrap.invoke("_UIApplicationMain", empty, none));
        const std::string described = bootstrap.describe();
        assert(described.find("NIB") != std::string::npos);
        // The UIKit entry points live in Services, so this module only claims its own.
        CpuState unused;
        assert(!bootstrap.invoke("_UIViewCreate", unused, none));
    }
    std::cout << "UIKit bootstrap (argv, delegate instantiation, launch callback, run-loop "
                 "outcome) passed\n";
}
