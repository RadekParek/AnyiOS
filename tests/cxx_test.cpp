#include "runtime/cxx.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/abi.h"
#include "runtime/error.h"
#include "runtime/exports.h"
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

    // Both libstdc++ and libc++ emit this guard from translation units that include
    // iostreams. Model its reference-counted lifetime, but do not claim formatted streams.
    check(cxx.isCxxSymbol("__ZNSt8ios_base4InitC1Ev") &&
              cxx.isCxxSymbol("_ZNSt8ios_base4InitC1Ev") &&
              cxx.isCxxSymbol("__ZNSt3__18ios_base4InitC1Ev") &&
              cxx.isCxxSymbol("_ZNSt3__18ios_base4InitC1Ev"),
          "libstdc++ and versioned libc++ Init symbols resolve with Mach-O spellings");
    call("__ZNSt8ios_base4InitC1Ev");
    check(cxx.iosInitCount() == 1, "ios_base::Init acquires the stream-lifetime guard");
    call("_ZNSt8ios_base4InitC2Ev");
    check(cxx.iosInitCount() == 2, "C1 and C2 aliases share the same Init lifetime");
    call("__ZNSt8ios_base4InitD1Ev");
    call("_ZNSt8ios_base4InitD2Ev");
    check(cxx.iosInitCount() == 0, "ios_base::Init destructors release their references");
    call("__ZNSt3__18ios_base4InitC1Ev");
    call("_ZNSt3__18ios_base4InitC2Ev");
    check(cxx.iosInitCount() == 2, "libc++ Init constructors share the same lifetime guard");
    call("__ZNSt3__18ios_base4InitD1Ev");
    call("_ZNSt3__18ios_base4InitD2Ev");
    check(cxx.iosInitCount() == 0, "libc++ Init destructors release their references");
    fault("CXX_IOS_INIT", [&] { call("_ZNSt8ios_base4InitD1Ev"); });

    // libc++ uses inline short-string storage and a tagged {capacity,size,pointer} long form.
    // The internal __init ABI must preserve both layouts so the guest's inline accessors work.
    const std::string libcxxInit =
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcj";
    const std::string libcxxInitSizeT =
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm";
    const std::string libcxxCopyCtor =
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_";
    const std::string libcxxDtor =
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev";
    check(cxx.isCxxSymbol(libcxxInit) && cxx.isCxxSymbol(libcxxInitSizeT) &&
              cxx.isCxxSymbol(libcxxCopyCtor) && cxx.isLibcxxSymbol(libcxxInitSizeT) &&
              cxx.isCxxSymbol("_ZNSsC1EPKcj"),
          "libc++ __init size_t and the libstdc++ std::string constructors resolve");
    const uint32_t shortObject = 0x2000, shortSource = 0x2100;
    const char shortText[] = "small";
    memory.write(shortSource, shortText, sizeof(shortText));
    const size_t heapBeforeShort = heap.used();
    call(libcxxInit, shortObject, shortSource, sizeof(shortText) - 1);
    const uint8_t shortTag = uint8_t(read(memory, shortObject, 1)[0]);
    check(shortTag == (sizeof(shortText) - 1) * 2,
          "libc++ stores the short-string length in the low tag byte");
    check(std::string(read(memory, shortObject + 1, sizeof(shortText)).data()) == shortText,
          "libc++ short strings keep bytes and the terminator inline");
    check(heap.used() == heapBeforeShort, "libc++ short strings do not allocate");
    const uint32_t shortCopy = 0x2030;
    check(call(libcxxCopyCtor, shortCopy, shortObject) == shortCopy &&
              uint8_t(read(memory, shortCopy, 1)[0]) == (sizeof(shortText) - 1) * 2 &&
              std::string(read(memory, shortCopy + 1, sizeof(shortText)).data()) == shortText,
          "libc++ copy construction preserves short-string inline storage and returns this");
    check(heap.used() == heapBeforeShort, "copying a short libc++ string does not allocate");
    call(libcxxDtor, shortCopy);
    const uint32_t sizeTObject = 0x2010, sizeTSource = 0x2110;
    put(memory, sizeTSource, "size_t ABI");
    call(libcxxInitSizeT, sizeTObject, sizeTSource, uint32_t(std::strlen("size_t ABI")));
    check(uint8_t(read(memory, sizeTObject, 1)[0]) == std::strlen("size_t ABI") * 2,
          "the exact libc++ PKcm size_t import initializes short-string storage");
    call(libcxxDtor, sizeTObject);

    const std::string libcxxInitReserved =
        "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcmm";
    const uint32_t reservedObject = 0x2050, reservedSource = 0x2150;
    put(memory, reservedSource, "room");
    check(cxx.isCxxSymbol(libcxxInitReserved),
          "the libc++ PKcmm reserve-capacity initializer is classified");
    call(libcxxInitReserved, reservedObject, reservedSource, 4, 12);
    const uint32_t reservedData = memory.read32(reservedObject + 8);
    check((memory.read32(reservedObject) & 1) && memory.read32(reservedObject + 4) == 4 &&
              (memory.read32(reservedObject) & ~1u) >= 13 &&
              std::string(read(memory, reservedData, 5).data()) == "room",
          "libc++ PKcmm honors a long reserved capacity while keeping the explicit size");
    call(libcxxDtor, reservedObject);

    const uint32_t longObject = 0x2020, longSource = 0x2120;
    const std::string longText = "libc++ string beyond SSO";
    memory.write(longSource, longText.data(), longText.size());
    call(libcxxInit, longObject, longSource, uint32_t(longText.size()));
    const uint32_t longTag = memory.read32(longObject);
    const uint32_t longData = memory.read32(longObject + 8);
    check((longTag & 1) && (longTag & ~1u) >= longText.size() + 1 &&
              memory.read32(longObject + 4) == longText.size() && heap.owns(longData),
          "libc++ long strings publish their tagged capacity, length and guest pointer");
    check(std::string(read(memory, longData, longText.size() + 1).data()) == longText,
          "libc++ long-string storage is copied and terminated in guest memory");
    const uint32_t longCopyObject = 0x2070;
    check(call(libcxxCopyCtor, longCopyObject, longObject) == longCopyObject,
          "libc++ long copy construction returns the destination object");
    const uint32_t longCopyData = memory.read32(longCopyObject + 8);
    check((memory.read32(longCopyObject) & 1) && longCopyData != longData &&
              std::string(read(memory, longCopyData, longText.size() + 1).data()) == longText,
          "libc++ long-string copy owns independent, terminated guest storage");
    const size_t heapBeforeLongDestroy = heap.used();
    call(libcxxDtor, longCopyObject);
    call(libcxxDtor, longObject);
    check(!heap.owns(longData) && !heap.owns(longCopyData) && heap.used() < heapBeforeLongDestroy,
          "libc++ long-string destruction releases both independent allocations");

    const uint32_t appendObject = 0x20a0, appendBase = 0x21a0, appendShort = 0x21b0,
                   appendLong = 0x21c0;
    put(memory, appendBase, "small");
    put(memory, appendShort, "!");
    const std::string appendTail = " plus a long tail";
    memory.write(appendLong, appendTail.data(), appendTail.size());
    call(libcxxInit, appendObject, appendBase, 5);
    check(cxx.isCxxSymbol(
              "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm"),
          "libc++ length-delimited append is classified");
    call("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm",
         appendObject, appendShort, 1);
    check(uint8_t(read(memory, appendObject, 1)[0]) == 12 &&
              std::string(read(memory, appendObject + 1, 7).data()) == "small!",
          "libc++ append updates a short string in place");
    call("__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm",
         appendObject, appendLong, uint32_t(appendTail.size()));
    const uint32_t firstAppendData = memory.read32(appendObject + 8);
    check((memory.read32(appendObject) & 1) &&
              std::string(read(memory, firstAppendData, 24).data()) == "small!" + appendTail,
          "libc++ append grows short-string storage without losing the existing prefix");
    call("_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm",
         appendObject, appendShort, 1);
    const uint32_t secondAppendData = memory.read32(appendObject + 8);
    check(secondAppendData != firstAppendData && !heap.owns(firstAppendData) &&
              std::string(read(memory, secondAppendData, 25).data()) == "small!" + appendTail + "!",
          "libc++ append grows a full long buffer and releases its old owned allocation");
    call(libcxxDtor, appendObject);
    check(!heap.owns(secondAppendData), "libc++ append allocation is released by destruction");

    // The old libstdc++ ABI stores one data pointer in the object, preceded by a
    // copy-on-write {length,capacity,refcount} header. Explicit lengths preserve embedded NULs.
    const uint32_t legacyObject = 0x2040, legacySource = 0x2140;
    const char legacyBytes[] = {'c', 'o', 'w', '\0', 's', 't', 'r'};
    memory.write(legacySource, legacyBytes, sizeof(legacyBytes));
    call("__ZNSsC1EPKcj", legacyObject, legacySource, sizeof(legacyBytes));
    const uint32_t legacyData = memory.read32(legacyObject);
    const uint32_t legacyHeader = legacyData - 12;
    check(memory.read32(legacyHeader) == sizeof(legacyBytes) &&
              memory.read32(legacyHeader + 4) == sizeof(legacyBytes) &&
              memory.read32(legacyHeader + 8) == 0,
          "libstdc++ initializes its COW header with length, capacity and one reference");
    std::vector<char> legacyCopy = read(memory, legacyData, sizeof(legacyBytes));
    const char legacyTerminator = read(memory, legacyData + sizeof(legacyBytes), 1)[0];
    check(std::memcmp(legacyCopy.data(), legacyBytes, sizeof(legacyBytes)) == 0 &&
              legacyTerminator == 0,
          "libstdc++ copies length-delimited bytes and appends a terminator");
    call("__ZNSsD1Ev", legacyObject);
    check(memory.read32(legacyObject) == 0 && !heap.owns(legacyHeader),
          "libstdc++ destruction releases the COW buffer and clears the object");

    const uint32_t cStringObject = 0x2060, cStringSource = 0x2160;
    put(memory, cStringSource, "null-terminated legacy string");
    call("_ZNSsC2EPKc", cStringObject, cStringSource);
    const uint32_t cStringData = memory.read32(cStringObject);
    check(memory.read32(cStringData - 12) == std::strlen("null-terminated legacy string") &&
              std::string(read(memory, cStringData, 30).data()) == "null-terminated legacy string",
          "libstdc++ null-terminated constructor computes the string length");
    call("_ZNSsD2Ev", cStringObject);
    const uint32_t allocatorObject = 0x2080, allocatorSource = 0x2180;
    put(memory, allocatorSource, "allocator ABI");
    const std::string allocatorCtor = "__ZNSsC1EPKcRKSaIcE";
    check(cxx.isCxxSymbol(allocatorCtor), "allocator-taking C-string constructor is classified");
    call(allocatorCtor, allocatorObject, allocatorSource, 0xdeadbeef);
    const uint32_t allocatorData = memory.read32(allocatorObject);
    check(std::string(read(memory, allocatorData, 14).data()) == "allocator ABI",
          "the allocator-taking constructor initializes the COW string object");
    call("__ZNSsD1Ev", allocatorObject);

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
    // The SjLj frame chain is real guest memory: each registration links the new frame to
    // the previous head through the frame's first word, which is what libgcc's unwinder
    // walks and what the personality reads the frame's LSDA from.
    {
        const uint32_t outer = 0x5000, inner = 0x5040;
        put(memory, 0x6000, "N2io11IOExceptionE");
        memory.write32(0x6014, 0x6000);      // type_info: vptr at +0, the name pointer at +4
        memory.write32(0x5000, 0xdeadbeef);  // stale contents must be replaced
        call("__Unwind_SjLj_Register", outer);
        check(memory.read32(outer) == 0, "the outermost frame links to the empty chain");
        call("__Unwind_SjLj_Register", inner);
        check(memory.read32(inner) == outer, "an inner frame links to the frame below it");
        call("__Unwind_SjLj_Unregister", inner);
        check(memory.read32(outer) == 0, "unregistering the inner frame keeps the chain");
        // A throw names the type it was given and says how deep the chain was.
        memory.write32(0x5004, 0);  // reset the outer link so the report says one frame
        call("__Unwind_SjLj_Register", inner);
        bool reported = false;
        try {
            call("___cxa_throw", 0x1800, 0x6010, 0);
        } catch (const Error& e) {
            const std::string what = e.what();
            reported = what.find("N2io11IOExceptionE") != std::string::npos &&
                       what.find("2 SjLj frame(s)") != std::string::npos;
        }
        check(reported, "a throw reports its type name and the SjLj chain depth");
        call("__Unwind_SjLj_Unregister", inner);
        call("__Unwind_SjLj_Unregister", outer);
    }
    // Mangled names alone only match exact types. Real C++ exceptions also match a base
    // class, which is why the ABI vtables above are guest data rather than missing call gates.
    // This mirrors io::IOException being caught through a standard exception base: a
    // derived type_info points to its direct base at +8, which can itself be __si RTTI.
    {
        ExportGraph graph;
        const size_t standard = graph.addImage("/usr/lib/libstdc++.6.dylib");
        cxx.installTypeInfoVtables(graph, standard);
        check(graph.resolve(standard, 0, "__ZTVN10__cxxabiv117__class_type_infoE") ==
                  CxxRuntime::ClassTypeInfoVtable,
              "class_type_info resolves to mapped ABI data, not a service gate");
        check(graph.resolve(standard, 0, "__ZTVN10__cxxabiv120__si_class_type_infoE") ==
                  CxxRuntime::SiClassTypeInfoVtable,
              "si_class_type_info resolves to its own vtable identity");

        put(memory, 0x6000, "N2io11IOExceptionE");
        put(memory, 0x6020, "St13runtime_error");
        put(memory, 0x6040, "St9exception");
        // Three target-layout type_info records: a simple-derived chain, then its base.
        memory.write32(0x6100, CxxRuntime::SiClassTypeInfoVtable + 8);
        memory.write32(0x6104, 0x6000);
        memory.write32(0x6108, 0x6120);
        memory.write32(0x6120, CxxRuntime::SiClassTypeInfoVtable + 8);
        memory.write32(0x6124, 0x6020);
        memory.write32(0x6128, 0x6140);
        memory.write32(0x6140, CxxRuntime::ClassTypeInfoVtable + 8);
        memory.write32(0x6144, 0x6040);

        // LSDA: LPStart omitted, absolute type pointers, one call site with action 1,
        // positive filter 1, and the backwards type table entry at TType - 4.
        const uint8_t lsda[] = {0xff, 0x00, 0x0a, 0xff, 0x02, 0x01, 0x01, 0x01, 0x00};
        memory.write(0x6200, lsda, sizeof(lsda));
        memory.write32(0x6209, 0x6140);  // catch std::exception
        memory.write32(0x5000, 0);
        call("__Unwind_SjLj_Register", 0x5000);
        memory.write32(0x5004, 1);       // call-site index
        memory.write32(0x501c, 0x6200);  // LSDA
        memory.write32(0x5020, 0x7100);  // saved r7
        memory.write32(0x5024, 0x7201);  // Thumb landing-pad resume address
        memory.write32(0x5028, 0x7300);  // saved sp
        call("___cxa_throw", 0x1800, 0x6100, 0);
        check(call.state.registers[15] == 0x7200 && (call.state.cpsr & 0x20),
              "an io::IOException-derived type resumes at the std::exception handler");
        check(memory.read32(0x5008) == 0x1800,
              "the catch landing pad receives the thrown object for a zero-offset base");
        call("___cxa_begin_catch", 0x1800);
        check(call.state.registers[0] == 0x1800,
              "__cxa_begin_catch returns the caught base subobject");
        call("___cxa_end_catch");
        call("__Unwind_SjLj_Unregister", 0x5000);
    }
    // GCC can encode a catch type as DW_EH_PE_indirect | pcrel | sdata4 (0x9b):
    // the reverse type-table entry is a signed displacement to a second pointer word.
    {
        const uint8_t indirectLsda[] = {0xff, 0x9b, 0x0a, 0xff, 0x02, 0x01, 0x01, 0x01, 0x00};
        memory.write(0x6300, indirectLsda, sizeof(indirectLsda));
        memory.write32(0x6110, 0x6140);  // indirect pointer to std::exception type_info
        const int32_t displacement = int32_t(0x6110) - int32_t(0x6309);
        memory.write32(0x6309, uint32_t(displacement));
        memory.write32(0x5000, 0);
        call("__Unwind_SjLj_Register", 0x5000);
        memory.write32(0x5004, 1);
        memory.write32(0x501c, 0x6300);
        memory.write32(0x5020, 0x7100);
        memory.write32(0x5024, 0x7201);
        memory.write32(0x5028, 0x7300);
        call("___cxa_throw", 0x1800, 0x6100, 0);
        check(call.state.registers[15] == 0x7200 && (call.state.cpsr & 0x20),
              "the indirect PC-relative LSDA type entry matches std::exception");
        call("___cxa_begin_catch", 0x1800);
        call("___cxa_end_catch");
        call("__Unwind_SjLj_Unregister", 0x5000);
    }
    // Multiple inheritance keeps an offset for a non-virtual base in the upper 24 bits of
    // base_info::offset_flags. The handler must receive the base-subobject pointer, not the
    // start of the derived object.
    {
        put(memory, 0x6060, "N6sample7DerivedE");
        put(memory, 0x6080, "N6sample4BaseE");
        memory.write32(0x6160, CxxRuntime::VmiClassTypeInfoVtable + 8);
        memory.write32(0x6164, 0x6060);
        memory.write32(0x6168, 0);  // __flags
        memory.write32(0x616c, 1);  // one base class
        memory.write32(0x6170, 0x6180);
        memory.write32(0x6174, (8 << 8) | 2);  // public base, eight bytes into the thrown object
        memory.write32(0x6180, CxxRuntime::ClassTypeInfoVtable + 8);
        memory.write32(0x6184, 0x6080);
        // Replace the sole catch type entry with sample::Base; the frame still has one site.
        memory.write32(0x6209, 0x6180);
        memory.write32(0x5000, 0);
        call("__Unwind_SjLj_Register", 0x5000);
        memory.write32(0x5004, 1);
        memory.write32(0x501c, 0x6200);
        memory.write32(0x5020, 0x7100);
        memory.write32(0x5024, 0x7201);
        memory.write32(0x5028, 0x7300);
        call("___cxa_throw", 0x1800, 0x6160, 0);
        check(memory.read32(0x5008) == 0x1808,
              "a multiple-inheritance catch receives the adjusted base pointer");
        call("___cxa_begin_catch", 0x1808);
        check(call.state.registers[0] == 0x1808,
              "__cxa_begin_catch preserves the adjusted base pointer");
        call("___cxa_end_catch");
        call("__Unwind_SjLj_Unregister", 0x5000);
    }
    // __dynamic_cast uses the complete-object pointer from the vtable header, then the same
    // public base offsets to perform a cross-cast from a secondary base back to the derived
    // object. It must not return the secondary-base pointer unchanged.
    {
        memory.write32(0x6308, uint32_t(-8));  // offset-to-top for the Base subobject vtable
        memory.write32(0x630c, 0x6160);        // dynamic type_info: sample::Derived
        memory.write32(0x6808, 0x6310);        // vptr in its Base subobject
        check(call("___dynamic_cast", 0x6808, 0x6180, 0x6160, 8) == 0x6800,
              "dynamic_cast<Base*, Derived*> recovers the complete object");
        check(call("___dynamic_cast", 0x6808, 0x6180, 0, 8) == 0x6800,
              "dynamic_cast<void*> returns the complete object");
        check(call("___dynamic_cast", 0x6808, 0x6180, 0x6140, 8) == 0,
              "an unrelated dynamic cast fails with null");
    }
    // The complete GCC SjLj frame is 44 bytes; 40 readable bytes at the end of this region
    // must not pass validation before the runtime reads its LSDA and jump-buffer fields.
    fault("CXX_UNWIND", [&] { call("__Unwind_SjLj_Register", 0x10fd8); });
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
