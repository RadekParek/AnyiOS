#include "cxx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "error.h"
#include "format.h"

namespace podnative::runtime {
namespace {
constexpr size_t CopyLimit = 16 * 1024 * 1024;
// Mach-O prefixes every C symbol with an underscore, and an Itanium mangled name already
// starts with `_Z`, so the guest's import is `__Znwm`, not `_Znwm`. Both spellings are
// accepted so a hand-written test and a real image resolve to the same implementation.
const char* const CxxSymbols[] = {
    "__Znwm",
    "__Znam",
    "__ZdlPv",
    "__ZdaPv",
    "__ZSt9terminatev",
    "_Znwm",
    "_Znam",
    "_ZdlPv",
    "_ZdaPv",
    "_ZSt9terminatev",
    "___cxa_allocate_exception",
    "___cxa_free_exception",
    "___cxa_throw",
    "___cxa_begin_catch",
    "___cxa_end_catch",
    "___cxa_rethrow",
    "___cxa_call_unexpected",
    "___cxa_bad_typeid",
    "___cxa_pure_virtual",
    "___cxa_atexit",
    "___cxa_guard_acquire",
    "___cxa_guard_release",
    "___cxa_guard_abort",
    "___objc_personality_v0",
};
const char* const CompilerSymbols[] = {
    "___divsi3",
    "___udivsi3",
    "___modsi3",
    "___umodsi3",
    "___divdi3",
    "___udivdi3",
    "___moddi3",
    "___umoddi3",
    "___floatundidf",
    "___floatdidf",
    "___fixdfdi",
    "___fixunsdfdi",
    "___gxx_personality_sj0",
    "__Unwind_SjLj_Register",
    "__Unwind_SjLj_Unregister",
    "__Unwind_SjLj_Resume",
};
template <class Range>
bool listed(const Range& range, const std::string& name) {
    for (const char* candidate : range)
        if (name == candidate) return true;
    return false;
}
uint64_t combine(uint32_t low, uint32_t high) { return uint64_t(low) | uint64_t(high) << 32; }
void split(uint64_t value, CpuState& cpu) {
    cpu.registers[0] = uint32_t(value & 0xffffffffu);
    cpu.registers[1] = uint32_t(value >> 32);
}
int32_t signedDivide(int32_t a, int32_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
    return a / b;
}
int64_t signedDivide64(int64_t a, int64_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return a;
    return a / b;
}
}  // namespace
bool CxxRuntime::isCxxSymbol(const std::string& symbol) const { return listed(CxxSymbols, symbol); }
bool CxxRuntime::isCompilerHelper(const std::string& symbol) const {
    return listed(CompilerSymbols, symbol);
}
void CxxRuntime::setLogSink(std::function<void(const std::string&)> sink) {
    logSink = std::move(sink);
}
void CxxRuntime::emit(const std::string& text) const {
    if (logSink) logSink(text);
}
std::string CxxRuntime::readString(uint32_t address) const {
    return readGuestString(memory, address);
}
void CxxRuntime::zero(uint32_t address, size_t bytes) {
    std::vector<uint8_t> zeros(std::min<size_t>(bytes, 64 * 1024), 0);
    size_t done = 0;
    while (done < bytes) {
        const size_t chunk = std::min(zeros.size(), bytes - done);
        memory.write(address + uint32_t(done), zeros.data(), chunk);
        done += chunk;
    }
}
uint32_t CxxRuntime::allocate(uint32_t bytes, const char* what) {
    if (!bytes) bytes = 1;
    const uint32_t pointer = heap.allocate(bytes, 8);
    if (!pointer) throw Error("CXX_ALLOC", std::string(what) + " could not be satisfied");
    return pointer;
}
uint32_t CxxRuntime::errnoAddress() {
    if (!errnoCell) {
        errnoCell = allocate(4, "guest errno");
        zero(errnoCell, 4);
    }
    return errnoCell;
}
size_t CxxRuntime::runAtExit() {
    // Run in reverse registration order, as __cxa_finalize does.
    size_t ran = 0;
    while (!atExit.empty()) {
        const ExitHandler handler = atExit.back();
        atExit.pop_back();
        emit("__cxa_atexit handler 0x" + std::to_string(handler.function) +
             " runs only once a guest exit path is implemented");
        ran++;
    }
    return ran;
}
bool CxxRuntime::invoke(const std::string& symbol, CpuState& cpu) {
    if (compilerHelper(symbol, cpu)) return true;
    if (fortified(symbol, cpu)) return true;
    return abi(symbol, cpu);
}
bool CxxRuntime::compilerHelper(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    if (name == "___divsi3" || name == "___udivsi3" || name == "___modsi3" ||
        name == "___umodsi3") {
        const uint32_t a = r[0], b = r[1];
        if (!b) {
            emit(
                "guest integer division by zero returns 0, matching __aeabi_idiv0 without a "
                "host trap");
            r[0] = 0;
            return true;
        }
        if (name == "___divsi3") r[0] = uint32_t(signedDivide(int32_t(a), int32_t(b)));
        if (name == "___udivsi3") r[0] = a / b;
        if (name == "___modsi3") r[0] = uint32_t(int32_t(a) % int32_t(b));
        if (name == "___umodsi3") r[0] = a % b;
        return true;
    }
    if (name == "___divdi3" || name == "___udivdi3" || name == "___moddi3" ||
        name == "___umoddi3") {
        const uint64_t a = combine(r[0], r[1]), b = combine(r[2], r[3]);
        if (!b) {
            emit("guest 64-bit division by zero returns 0");
            split(0, cpu);
            return true;
        }
        if (name == "___divdi3") split(uint64_t(signedDivide64(int64_t(a), int64_t(b))), cpu);
        if (name == "___udivdi3") split(a / b, cpu);
        if (name == "___moddi3") split(uint64_t(int64_t(a) % int64_t(b)), cpu);
        if (name == "___umoddi3") split(a % b, cpu);
        return true;
    }
    if (name == "___floatundidf" || name == "___floatdidf") {
        const uint64_t bits = combine(r[0], r[1]);
        const double value = name == "___floatundidf" ? static_cast<double>(bits)
                                                      : static_cast<double>(int64_t(bits));
        split(doubleBits(value), cpu);
        return true;
    }
    if (name == "___fixdfdi" || name == "___fixunsdfdi") {
        const double value = bitsDouble(combine(r[0], r[1]));
        const bool unsignedResult = name == "___fixunsdfdi";
        if (std::isnan(value)) {
            split(0, cpu);
            return true;
        }
        const double limit = unsignedResult ? 18446744073709551615.0 : 9223372036854775807.0;
        const double clamped =
            std::min(std::max(value, unsignedResult ? 0.0 : -9223372036854775808.0), limit);
        split(unsignedResult ? uint64_t(clamped) : uint64_t(int64_t(clamped)), cpu);
        return true;
    }
    if (name == "__Unwind_SjLj_Register" || name == "__Unwind_SjLj_Unregister") {
        // Setjmp/longjmp unwinding tables. Registering and unregistering a frame is pure
        // bookkeeping around a `__builtin_setjmp` the guest already performed; refusing them
        // rejected every GCC-era Objective-C method that contains a @try, including normal
        // code paths that never throw. The frames are recorded so a resume can name the
        // innermost one instead of guessing.
        const uint32_t context = r[0];
        if (name == "__Unwind_SjLj_Register") {
            if (!context || !memory.permits(context, 40, Read))
                throw Error("CXX_UNWIND", "SjLj register without a valid function context",
                            context);
            if (sjljFrames.size() >= 4096) throw Error("CXX_LIMIT", "too many SjLj frames");
            // The frame context is a real linked list in guest memory: libgcc's unwinder
            // walks `prev`, and the personality reads the frame's LSDA out of it. Keeping
            // the host-side list in step is bookkeeping; `prev` is the contract.
            memory.write32(context, sjljHead);
            sjljHead = context;
            sjljFrames.push_back(context);
        } else if (sjljHead == context) {
            sjljHead = memory.permits(context, 4, Read) ? memory.read32(context) : 0;
            auto found = std::find(sjljFrames.rbegin(), sjljFrames.rend(), context);
            if (found != sjljFrames.rend()) sjljFrames.erase(std::next(found).base());
        } else {
            // Unregistering an inner frame first (an early return that skipped an
            // unregister) must not drop the whole chain: splice it out of the list.
            uint32_t previous = sjljHead;
            for (size_t guard = 0; previous && guard < 4096; guard++) {
                const uint32_t next =
                    memory.permits(previous, 4, Read) ? memory.read32(previous) : 0;
                if (next == context) {
                    memory.write32(previous, memory.read32(context));
                    break;
                }
                previous = next;
            }
            auto found = std::find(sjljFrames.rbegin(), sjljFrames.rend(), context);
            if (found != sjljFrames.rend()) sjljFrames.erase(std::next(found).base());
        }
        r[0] = 0;
        return true;
    }
    if (name == "__Unwind_SjLj_Resume" || name == "___gxx_personality_sj0" ||
        name == "___objc_personality_v0") {
        throw Error("CXX_UNWIND", "guest threw a C++ exception: " + name +
                                      " requires SjLj resumption, which is not emulated yet "
                                      "(frames registered: " +
                                      std::to_string(sjljFrames.size()) + ")");
    }
    return false;
}
bool CxxRuntime::fortified(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    const bool memoryCopy =
        name == "___memcpy_chk" || name == "___memmove_chk" || name == "___memset_chk";
    const bool stringCopy =
        name == "___strcpy_chk" || name == "___strcat_chk" || name == "___strncpy_chk";
    if (!memoryCopy && !stringCopy && name != "___sprintf_chk") return false;
    if (name == "___sprintf_chk") {
        const uint32_t destination = r[0], capacity = r[2], format = r[3];
        if (capacity > CopyLimit) throw Error("LIBC_LIMIT", "__sprintf_chk capacity is unbounded");
        Arguments args(cpu, memory, 4);
        const std::string text = formatGuest(readString(format), args);
        if (text.size() + 1 > capacity)
            throw Error("LIBC_FORTIFY",
                        "__sprintf_chk format result exceeds the destination object size",
                        destination);
        if (!memory.permits(destination, capacity, Write))
            throw Error("LIBC_BUFFER", "__sprintf_chk destination is not writable", destination);
        if (!text.empty()) memory.write(destination, text.data(), text.size());
        const char terminator = 0;
        memory.write(destination + uint32_t(text.size()), &terminator, 1);
        r[0] = uint32_t(text.size());
        return true;
    }
    const uint32_t destination = r[0], source = r[1], count = r[2],
                   capacity = memoryCopy || name == "___strncpy_chk" ? r[3] : r[2];
    if (memoryCopy) {
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "fortified copy exceeds call budget");
        if (count > capacity)
            throw Error("LIBC_FORTIFY", std::string(name) + " exceeds the destination object size",
                        destination);
        if (!memory.permits(destination, count, Write))
            throw Error("LIBC_BUFFER", "fortified copy destination is not writable", destination);
        if (name == "___memset_chk") {
            std::vector<uint8_t> bytes(count, uint8_t(source));
            memory.write(destination, bytes.data(), count);
        } else {
            if (name == "___memcpy_chk" && count &&
                uint64_t(destination) < uint64_t(source) + count &&
                uint64_t(source) < uint64_t(destination) + count)
                throw Error("LIBC_OVERLAP", "overlapping __memcpy_chk is undefined");
            std::vector<uint8_t> bytes(count);
            memory.read(source, bytes.data(), count);
            memory.write(destination, bytes.data(), count);
        }
        r[0] = destination;
        return true;
    }
    const std::string text = readString(source);
    size_t needed = text.size() + 1;
    if (name == "___strcat_chk") needed += readString(destination).size();
    if (name == "___strncpy_chk") needed = std::min<size_t>(text.size(), count) + 1;
    if (needed > capacity)
        throw Error("LIBC_FORTIFY", std::string(name) + " exceeds the destination object size",
                    destination);
    if (!memory.permits(destination, capacity, Write))
        throw Error("LIBC_BUFFER", "fortified string destination is not writable", destination);
    std::string result = name == "___strcat_chk" ? readString(destination) : std::string();
    if (name == "___strncpy_chk") {
        result = text.substr(0, std::min<size_t>(text.size(), count));
        memory.write(destination, result.data(), result.size());
        for (size_t offset = result.size(); offset < count; offset++) {
            const char terminator = 0;
            memory.write(destination + uint32_t(offset), &terminator, 1);
        }
    } else {
        result += text;
        memory.write(destination, result.data(), result.size());
        const char terminator = 0;
        memory.write(destination + uint32_t(result.size()), &terminator, 1);
    }
    r[0] = destination;
    return true;
}
bool CxxRuntime::abi(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    if (name == "__Znwm" || name == "_Znwm" || name == "__Znam" || name == "_Znam") {
        r[0] = allocate(r[0], "operator new");
        return true;
    }
    if (name == "__ZdlPv" || name == "_ZdlPv" || name == "__ZdaPv" || name == "_ZdaPv") {
        if (r[0] && heap.owns(r[0])) heap.release(r[0]);
        return true;
    }
    if (name == "__ZSt9terminatev" || name == "_ZSt9terminatev") {
        throw Error("CXX_TERMINATE", "the guest called std::terminate");
    }
    if (name == "___cxa_allocate_exception") {
        const uint32_t bytes = std::max<uint32_t>(r[0], 8);
        r[0] = allocate(bytes, "exception object");
        zero(r[0], bytes);
        return true;
    }
    if (name == "___cxa_free_exception") {
        if (r[0] && heap.owns(r[0])) heap.release(r[0]);
        return true;
    }
    if (name == "___cxa_throw") {
        // __cxa_throw(object, type_info, destructor). The type name lives at type_info+4 in
        // the Itanium ABI, so the report can say *what* was thrown instead of only that
        // something was.
        std::string what = "an unnamed C++ type";
        if (r[1] && memory.permits(r[1] + 4, 4, Read)) {
            const uint32_t nameAt = memory.read32(r[1] + 4);
            if (nameAt && memory.permits(nameAt, 1, Read)) {
                const std::string mangled = readGuestString(memory, nameAt, 512);
                if (!mangled.empty()) what = "'" + mangled + "'";
            }
        }
        // The frames are the guest's own SjLj chain; reporting its depth and the innermost
        // LSDA says exactly how far the unwinder would have had to walk, which is what the
        // next piece of work needs. Nothing is presented as caught.
        const size_t depth = sjljFrames.size();
        throw Error("CXX_UNCAUGHT", "the guest threw a C++ exception of type " + what + " with " +
                                        std::to_string(depth) +
                                        " SjLj frame(s) registered; C++ unwinding is not "
                                        "implemented, so the guest's handler cannot run");
    }
    if (name == "___cxa_begin_catch" || name == "___cxa_end_catch" || name == "___cxa_rethrow" ||
        name == "___cxa_call_unexpected" || name == "___cxa_pure_virtual" ||
        name == "___cxa_bad_typeid") {
        throw Error("CXX_UNWIND",
                    "the guest entered " + name +
                        ", which requires C++ exception handling that is not implemented");
    }
    if (name == "___cxa_atexit" || name == "_atexit") {
        if (atExit.size() >= 4096) throw Error("CXX_LIMIT", "too many exit handlers");
        atExit.push_back({r[0], name == "___cxa_atexit" ? r[1] : 0});
        r[0] = 0;
        return true;
    }
    if (name == "___cxa_guard_acquire") {
        uint8_t state[2] = {0, 0};
        memory.read(r[0], state, sizeof(state));
        if (state[0]) {
            r[0] = 0;
            return true;
        }
        if (state[1]) throw Error("CXX_GUARD", "recursive function-local static initialization");
        state[1] = 1;
        memory.write(r[0], state, sizeof(state));
        r[0] = 1;
        return true;
    }
    if (name == "___cxa_guard_release" || name == "___cxa_guard_abort") {
        const uint8_t state[2] = {name == "___cxa_guard_release" ? uint8_t(1) : uint8_t(0),
                                  uint8_t(0)};
        memory.write(r[0], state, sizeof(state));
        return true;
    }
    if (name == "___assert_rtn") {
        const std::string function = readString(r[0]), file = readString(r[1]),
                          expression = readString(r[3]);
        throw Error("GUEST_ASSERT", expression + " failed in " + function + " (" + file + ":" +
                                        std::to_string(r[2]) + ")");
    }
    if (name == "___stack_chk_fail") {
        throw Error("GUEST_STACK_GUARD", "guest stack canary check failed");
    }
    return false;
}
}  // namespace podnative::runtime
