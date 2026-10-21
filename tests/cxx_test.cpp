#include "runtime/cxx.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/abi.h"
#include "runtime/error.h"
#include "runtime/format.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
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
void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
struct Call {
    Memory& memory;
    CxxRuntime& cxx;
    CpuState state;
    Call(Memory& m, CxxRuntime& c) : memory(m), cxx(c) {}
    uint32_t operator()(const std::string& name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                        uint32_t d = 0) {
        state = {};
        state.registers[0] = a;
        state.registers[1] = b;
        state.registers[2] = c;
        state.registers[3] = d;
        if (!cxx.invoke(name, state))
            throw Error("TEST_SYMBOL", std::string("unimplemented ") + name);
        return state.registers[0];
    }
    uint64_t wide(const std::string& name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                  uint32_t d = 0) {
        operator()(name, a, b, c, d);
        return uint64_t(state.registers[0]) | uint64_t(state.registers[1]) << 32;
    }
};
void put(Memory& memory, uint32_t address, const char* text) {
    memory.write(address, text, std::strlen(text) + 1);
}
std::vector<char> read(Memory& memory, uint32_t address, size_t size) {
    std::vector<char> bytes(size);
    memory.read(address, bytes.data(), size);
    return bytes;
}
}  // namespace
int main() {
    Memory memory;
    std::vector<uint8_t> page(65536, 0);
    memory.map({0x1000, 65536, uint8_t(Read | Write), "guest data"}, page.data(), page.size());
    GuestHeap heap(memory);
    CxxRuntime cxx(memory, heap);
    std::vector<std::string> log;
    cxx.setLogSink([&](const std::string& text) { log.push_back(text); });
    Call call(memory, cxx);

    // operator new/delete route through the guest heap, so guest pointers stay contestable.
    const uint32_t block = call("_Znwm", 64);
    check(block != 0 && heap.owns(block), "operator new must return a live guest block");
    const char* text = "guest c++";
    memory.write(block, text, std::strlen(text) + 1);
    call("_ZdlPv", block);
    check(!heap.owns(block), "operator delete must release the block");
    call("_ZdlPv", 0);  // deleting null is a no-op, not a fault.

    // A failed allocation is a named guest failure, never a silent null return.
    fault("CXX_ALLOC", [&] { call("_Znwm", 0x7fffffffu); });

    // Integer helpers match the ARM runtime routines, including the documented /0 result.
    check(call("___divsi3", uint32_t(-7), 2) == uint32_t(-3), "signed division truncates to zero");
    check(call("___udivsi3", 100, 7) == 14, "unsigned division");
    check(call("___modsi3", uint32_t(-7), 2) == uint32_t(-1), "signed remainder");
    check(call("___umodsi3", 100, 7) == 2, "unsigned remainder");
    check(call("___divsi3", 5, 0) == 0, "division by zero returns zero, like __aeabi_idiv0");
    check(log.size() == 1 && log[0].find("division by zero") != std::string::npos,
          "division by zero is reported");

    auto words = [](uint64_t value) {
        return std::pair<uint32_t, uint32_t>(uint32_t(value), uint32_t(value >> 32));
    };
    const uint64_t dividend = 0xfffffffe00000000ULL;
    auto a = words(dividend), b = words(3);
    check(call.wide("___udivdi3", a.first, a.second, b.first, b.second) == dividend / 3,
          "64-bit unsigned division");
    auto negative = words(uint64_t(int64_t(-1000)));
    check(call.wide("___divdi3", negative.first, negative.second, b.first, b.second) ==
              uint64_t(int64_t(-333)),
          "64-bit signed division");
    check(call.wide("___umoddi3", a.first, a.second, b.first, b.second) == dividend % 3,
          "64-bit unsigned remainder");
    check(
        call.wide("___divdi3", words(uint64_t(INT64_MIN)).first, words(uint64_t(INT64_MIN)).second,
                  uint32_t(-1), uint32_t(-1)) == uint64_t(INT64_MIN),
        "INT64_MIN/-1 does not trap");

    // long long to double and back through the r0:r1 pair.
    call("___floatundidf", words(1ULL << 40).first, words(1ULL << 40).second);
    check(bitsDouble(uint64_t(call.state.registers[0]) | uint64_t(call.state.registers[1]) << 32) ==
              1099511627776.0,
          "unsigned 64-bit to double");
    const uint64_t asDouble = doubleBits(-42.75);
    call("___fixdfdi", uint32_t(asDouble), uint32_t(asDouble >> 32));
    check((uint64_t(call.state.registers[0]) | uint64_t(call.state.registers[1]) << 32) ==
              uint64_t(int64_t(-42)),
          "double to signed 64-bit truncates toward zero");

    // Function-local static guards: acquire once, release, then acquire returns 0.
    check(call("___cxa_guard_acquire", 0x1100) == 1, "first guard acquire claims initialization");
    check(memory.read32(0x1100) == 0x00000100, "guard marks initialization in progress");
    fault("CXX_GUARD", [&] { call("___cxa_guard_acquire", 0x1100); });
    call("___cxa_guard_release", 0x1100);
    check(call("___cxa_guard_acquire", 0x1100) == 0, "released guard reports initialized");
    call("___cxa_guard_acquire", 0x1110);
    call("___cxa_guard_abort", 0x1110);
    check(memory.read32(0x1110) == 0, "aborted guard returns to uninitialized");

    // Exit handlers are recorded for the guest shutdown path.
    check(call("___cxa_atexit", 0x1200, 0x1300) == 0, "atexit registration succeeds");
    check(cxx.atExitCount() == 1, "atexit handler is tracked");
    fault("CXX_LIMIT", [&] {
        for (int n = 0; n < 5000; n++) call("___cxa_atexit", 0x1200, 0);
    });

    // Fortified copies enforce the object size the compiler recorded.
    put(memory, 0x2000, "source text");
    check(call("___strcpy_chk", 0x2100, 0x2000, 64) == 0x2100,
          "strcpy_chk returns the destination");
    check(std::string(read(memory, 0x2100, 16).data()) == "source text", "strcpy_chk copies");
    fault("LIBC_FORTIFY", [&] { call("___strcpy_chk", 0x2100, 0x2000, 4); });
    fault("LIBC_FORTIFY", [&] { call("___memcpy_chk", 0x2100, 0x2000, 32, 16); });
    check(call("___memcpy_chk", 0x2200, 0x2000, 4, 4) == 0x2200, "memcpy_chk copies in bounds");
    // The fifth word travels on the guest stack, exactly as AAPCS places it.
    put(memory, 0x2400, "value=%d");
    auto sprintfChk = [&](uint32_t capacity) {
        CpuState state{};
        state.registers[0] = 0x2300;
        state.registers[2] = capacity;
        state.registers[3] = 0x2400;
        state.registers[13] = 0x8000;
        memory.write32(0x8000, 17);
        if (!cxx.invoke("___sprintf_chk", state))
            throw Error("TEST_SYMBOL", "unimplemented ___sprintf_chk");
        return state.registers[0];
    };
    check(sprintfChk(64) == 8, "sprintf_chk formats");
    check(std::string(read(memory, 0x2300, 9).data()) == "value=17",
          "sprintf_chk writes the formatted text");
    fault("LIBC_FORTIFY", [&] { sprintfChk(4); });

    // Refusals are reported with the precise reason, never as a fake success.
    fault("GUEST_ASSERT", [&] {
        put(memory, 0x3000, "updateGame");
        put(memory, 0x3010, "Game.cpp");
        put(memory, 0x3020, "ready");
        call("___assert_rtn", 0x3000, 0x3010, 12, 0x3020);
    });
    fault("CXX_UNCAUGHT", [&] { call("___cxa_throw", 0x1800, 0, 0); });
    fault("CXX_UNWIND", [&] { call("___gxx_personality_sj0", 0, 0, 0, 0); });
    fault("CXX_TERMINATE", [&] { call("_ZSt9terminatev"); });
    fault("GUEST_STACK_GUARD", [&] { call("___stack_chk_fail"); });

    // Symbols are attributed to the library that exports them on iOS.
    check(cxx.isCxxSymbol("_Znwm") && !cxx.isCompilerHelper("_Znwm"), "libstdc++ symbol class");
    check(cxx.isCompilerHelper("___divsi3") && !cxx.isCxxSymbol("___divsi3"),
          "libgcc symbol class");
    check(!cxx.isCxxSymbol("_malloc") && !cxx.isCompilerHelper("_atexit"),
          "libSystem symbols are not claimed by the C++ layer");
    std::cout << "cxx_test: ok\n";
    return 0;
}
