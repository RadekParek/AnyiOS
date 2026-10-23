#include "cxx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

#include "error.h"
#include "exports.h"
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
    "___dynamic_cast",
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
    "___floatdisf",
    "___floatundisf",
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
uint32_t CxxRuntime::allocate(uint32_t bytes, const char* what, const CpuState* caller) {
    if (!bytes) bytes = 1;
    const uint32_t returnAddress = caller ? caller->registers[14] : 0;
    noteLargeRequest(what, bytes, returnAddress);
    const uint32_t pointer = heap.allocate(bytes, 8);
    if (pointer) return pointer;
    std::string detail = std::string(what) + " could not be satisfied: the guest heap holds " +
                         std::to_string(heap.used()) + " of " + std::to_string(heap.capacity()) +
                         " bytes, its largest contiguous free block is " +
                         std::to_string(heap.largestFreeBlock()) + " bytes, and the request was " +
                         std::to_string(bytes) + " bytes";
    if (returnAddress) detail += " from guest return address 0x" + hex(returnAddress);
    if (caller) {
        const std::string trace = guestBacktrace(*caller);
        if (!trace.empty()) detail += "; guest call stack: " + trace;
    }
    throw Error("CXX_ALLOC", detail, bytes);
}
std::string CxxRuntime::guestBacktrace(const CpuState& cpu) const {
    // iOS ARM images keep a frame chain in r7: [r7] is the caller's r7, [r7+4] its return
    // address. Walking it reports the guest's own call stack, which is what turns "the heap
    // ran out" into "this guest code asked for it". Every word is checked against guest
    // permissions before it is read, and the walk stops at the first frame that does not
    // look like one, so a corrupt chain yields a short trace rather than a wrong one.
    std::string trace;
    uint32_t frame = cpu.registers[7];
    for (size_t depth = 0; depth < 16; depth++) {
        if (!frame || (frame & 3) || !memory.permits(frame, 8, Read)) break;
        const uint32_t caller = memory.read32(frame);
        const uint32_t returnAddress = memory.read32(frame + 4);
        if (returnAddress < 4 || !memory.permits(returnAddress - 4, 4, Execute)) break;
        char cell[16];
        std::snprintf(cell, sizeof(cell), "%08x", returnAddress);
        if (!trace.empty()) trace += ' ';
        trace += "0x" + std::string(cell);
        if (caller <= frame) break;  // unwinding moves up the stack
        frame = caller;
    }
    return trace;
}
void CxxRuntime::noteLargeRequest(const char* what, uint32_t bytes, uint32_t returnAddress) const {
    if (!verbose || bytes < GuestHeap::LargeRequestBytes) return;
    emit(std::string(what) + " asked for " + std::to_string(bytes) +
         " bytes from guest return address 0x" + hex(returnAddress));
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
    // The ARM backend calls these for an int64-to-float conversion; the result comes back
    // as single-precision bits in r0, while a double result uses r0/r1.
    if (name == "___floatdisf" || name == "___floatundisf") {
        const uint64_t bits = combine(r[0], r[1]);
        const bool unsignedSource = name == "___floatundisf";
        resultF32(cpu, unsignedSource ? static_cast<float>(bits)
                                      : static_cast<float>(static_cast<int64_t>(bits)));
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
    if (name == "__Unwind_SjLj_Resume") {
        // A cleanup landing pad (or __cxa_rethrow) calls this to keep unwinding. The frame
        // that is running has already stored -1 in its call site, so it reports "no action"
        // and the walk naturally continues outwards, exactly as libgcc relies on.
        if (!thrown.active) throw Error("CXX_UNWIND", "SjLj resume with no exception in flight");
        return unwindPhase2(cpu);
    }
    if (name == "___gxx_personality_sj0") {
        // The unwinder that invokes personalities is this runtime; a guest call means the
        // image expects a different unwinder contract.
        throw Error("CXX_UNWIND", "the guest called the SjLj personality routine directly");
    }
    if (name == "___objc_personality_v0") {
        throw Error("CXX_OBJC_UNWIND",
                    "the guest threw or resumed an Objective-C exception, which this runtime "
                    "does not unwind yet");
    }
    return false;
}
std::string CxxRuntime::hex(uint32_t value) const {
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}
std::string CxxRuntime::thrownTypeName() const {
    // The Itanium ABI puts the mangled name at type_info+4 (the vtable pointer is at +0).
    if (thrown.typeInfo && memory.permits(thrown.typeInfo + 4, 4, Read)) {
        const uint32_t nameAt = memory.read32(thrown.typeInfo + 4);
        if (nameAt && memory.permits(nameAt, 1, Read)) {
            const std::string mangled = readString(nameAt);
            if (!mangled.empty()) return "'" + mangled + "'";
        }
    }
    return "a C++ exception of unknown type";
}
std::string CxxRuntime::frameDescription(uint32_t frame) const {
    if (!memory.permits(frame, FrameLayout::size, Read)) return "0x" + hex(frame) + " (unreadable)";
    return "0x" + hex(frame) +
           " call_site=" + std::to_string(int32_t(memory.read32(frame + FrameLayout::callSite))) +
           " lsda=0x" + hex(memory.read32(frame + FrameLayout::lsda));
}
bool CxxRuntime::readUleb(uint32_t& cursor, uint32_t& value) const {
    value = 0;
    for (uint32_t shift = 0; shift < 35; shift += 7) {
        if (!memory.permits(cursor, 1, Read)) return false;
        uint8_t byte = 0;
        memory.read(cursor, &byte, 1);
        cursor++;
        value |= uint32_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}
bool CxxRuntime::readSleb(uint32_t& cursor, int32_t& value) const {
    int64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte = 0;
    do {
        if (!memory.permits(cursor, 1, Read) || shift >= 35) return false;
        memory.read(cursor, &byte, 1);
        cursor++;
        result |= int64_t(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if ((byte & 0x40) && shift < 64) result |= -(int64_t(1) << shift);
    value = int32_t(result);
    return true;
}
bool CxxRuntime::parseLsda(uint32_t address, LsdaHeader& header) const {
    uint32_t cursor = address;
    uint8_t byte = 0;
    if (!memory.permits(cursor, 4, Read)) return false;
    memory.read(cursor, &byte, 1);
    cursor++;
    if (byte != 0xff)
        throw Error("CXX_LSDA",
                    "landing-pad start encoding is not implemented (0x" + hex(byte) + ")", address);
    memory.read(cursor, &byte, 1);
    cursor++;
    header.typeEncoding = byte;
    if (byte != 0xff) {
        uint32_t offset = 0;
        if (!readUleb(cursor, offset))
            throw Error("CXX_LSDA", "type table offset is not readable", address);
        header.typeTable = cursor + offset;
    } else {
        header.typeTable = 0;
    }
    // The call-site encoding byte is present but unused: a SjLj table is indexed by the
    // frame's call-site number, not by an instruction address.
    cursor++;
    uint32_t length = 0;
    if (!readUleb(cursor, length))
        throw Error("CXX_LSDA", "call-site table is not readable", address);
    header.callSiteTable = cursor;
    header.actionTable = cursor + length;
    return true;
}
uint32_t CxxRuntime::typeEntry(const LsdaHeader& header, int32_t filter) const {
    if (!header.typeTable || filter <= 0) return 0;
    // GCC stores the type table backwards from @TType: entry i lives at TType - i * size.
    if (header.typeEncoding != 0x00 && header.typeEncoding != 0x03)
        throw Error("CXX_LSDA",
                    "type table encoding 0x" + hex(header.typeEncoding) + " is not implemented",
                    header.typeTable);
    const uint32_t at = header.typeTable - uint32_t(filter) * 4;
    if (!memory.permits(at, 4, Read))
        throw Error("CXX_LSDA", "type table entry is outside guest memory", at);
    return memory.read32(at);
}
void CxxRuntime::installTypeInfoVtables(ExportGraph& graph, size_t standardLibrary) {
    // Four distinct address points let the matcher recognize the three class RTTI layouts
    // (plus pointer RTTI) without needing to call the guest's imported libstdc++ vtable. The
    // tables are deliberately data-only: CxxRuntime performs the type walk itself; virtual
    // __do_catch/dynamic-cast callbacks into a guest ABI are not claimed here.
    if (!rttiVtablesMapped) {
        std::array<uint32_t, Memory::PageSize / sizeof(uint32_t)> page{};
        memory.map({RttiVtablePage, Memory::PageSize, Read, "C++ ABI type_info vtable identities"},
                   reinterpret_cast<const uint8_t*>(page.data()), sizeof(page));
        rttiVtablesMapped = true;
    }
    const std::pair<const char*, uint32_t> vtables[] = {
        {"__ZTVN10__cxxabiv117__class_type_infoE", ClassTypeInfoVtable},
        {"__ZTVN10__cxxabiv119__pointer_type_infoE", PointerTypeInfoVtable},
        {"__ZTVN10__cxxabiv120__si_class_type_infoE", SiClassTypeInfoVtable},
        {"__ZTVN10__cxxabiv121__vmi_class_type_infoE", VmiClassTypeInfoVtable},
    };
    for (const auto& [symbol, address] : vtables)
        if (!graph.strong(standardLibrary, symbol))
            graph.define(standardLibrary, symbol, {address});
}
bool CxxRuntime::typeMatches(uint32_t catchType, uint32_t thrownType,
                             int32_t& objectAdjustment) const {
    objectAdjustment = 0;
    if (!catchType) return true;  // a null type_info is `catch (...)`
    if (!thrownType) return false;

    const auto typeName = [this](uint32_t type) -> std::string {
        if (!type || !memory.permits(type + 4, 4, Read)) return {};
        const uint32_t name = memory.read32(type + 4);
        if (!name || !memory.permits(name, 1, Read)) return {};
        return readString(name);
    };
    const std::string catchName = typeName(catchType);
    const std::string thrownName = typeName(thrownType);
    if (catchType == thrownType || (!catchName.empty() && catchName == thrownName)) return true;

    enum class Kind { Unknown, Class, Pointer, Single, Multiple };
    const auto kindOf = [](uint32_t vtable) {
        // Mach-O binds a C++ vtable symbol at its start; ABI RTTI objects point at its
        // address point eight bytes in. Accepting the start too accommodates SDK linkers
        // that encode the address-point bias in the vtable symbol itself.
        if (vtable == ClassTypeInfoVtable || vtable == ClassTypeInfoVtable + 8) return Kind::Class;
        if (vtable == PointerTypeInfoVtable || vtable == PointerTypeInfoVtable + 8)
            return Kind::Pointer;
        if (vtable == SiClassTypeInfoVtable || vtable == SiClassTypeInfoVtable + 8)
            return Kind::Single;
        if (vtable == VmiClassTypeInfoVtable || vtable == VmiClassTypeInfoVtable + 8)
            return Kind::Multiple;
        return Kind::Unknown;
    };

    // A class catch matches its exact type or an accessible, unambiguous base of the
    // dynamic thrown type. The RTTI objects are target ABI structures: __si stores its one
    // base pointer at +8; __vmi stores flags/count at +8/+12 and {type, offset_flags}
    // pairs from +16. Non-virtual base offsets are the signed high 24 bits of offset_flags.
    // Virtual-base adjustment needs the guest object's vtable and is intentionally refused
    // rather than handing a catch block a pointer to the wrong subobject.
    std::vector<uint32_t> path;
    bool found = false, ambiguous = false;
    int64_t foundAdjustment = 0;
    const auto visit = [&](const auto& self, uint32_t derived, int64_t adjustment,
                           unsigned depth) -> void {
        if (!derived || depth > 64 || path.size() >= 64 || !memory.permits(derived, 8, Read) ||
            std::find(path.begin(), path.end(), derived) != path.end())
            return;
        const std::string derivedName = typeName(derived);
        if (!catchName.empty() && catchName == derivedName) {
            if (!found) {
                found = true;
                foundAdjustment = adjustment;
            } else if (foundAdjustment != adjustment) {
                ambiguous = true;
            }
            return;
        }
        path.push_back(derived);
        const Kind kind = kindOf(memory.read32(derived));
        if (kind == Kind::Single) {
            if (memory.permits(derived + 8, 4, Read))
                self(self, memory.read32(derived + 8), adjustment, depth + 1);
        } else if (kind == Kind::Multiple && memory.permits(derived + 8, 8, Read)) {
            const uint32_t count = memory.read32(derived + 12);
            if (count <= 64 && memory.permits(derived + 16, size_t(count) * 8, Read)) {
                for (uint32_t index = 0; index < count; ++index) {
                    const uint32_t entry = derived + 16 + index * 8;
                    const uint32_t baseType = memory.read32(entry);
                    const uint32_t offsetFlags = memory.read32(entry + 4);
                    if (!(offsetFlags & 2u) || (offsetFlags & 1u))
                        continue;  // private/virtual bases need ABI access or object-vtable data
                    const int32_t baseOffset = int32_t(offsetFlags) >> 8;
                    self(self, baseType, adjustment + baseOffset, depth + 1);
                }
            }
        }
        path.pop_back();
    };
    visit(visit, thrownType, 0, 0);
    if (!found || ambiguous || foundAdjustment < INT32_MIN || foundAdjustment > INT32_MAX)
        return false;
    objectAdjustment = int32_t(foundAdjustment);
    if (verbose && !catchName.empty() && catchName != thrownName)
        emit("cxx: caught " + thrownName + " as public base " + catchName + " with object offset " +
             std::to_string(objectAdjustment));
    return true;
}
CxxRuntime::Personality CxxRuntime::personalityFor(uint32_t frame, bool search, bool handlerFrame) {
    if (!memory.permits(frame, FrameLayout::size, Read))
        throw Error("CXX_UNWIND", "SjLj frame context is outside guest memory", frame);
    // Phase 2 shortcut from libstdc++: the frame that will handle keeps the decision phase 1
    // cached, so its `call_site` (which the landing pad itself rewrote) is never re-read.
    if (!search && handlerFrame) {
        pendingLandingPad = thrown.landingPad;
        pendingFilter = thrown.filter;
        return thrown.landingPad ? Personality::Install : Personality::Terminate;
    }
    const uint32_t lsda = memory.read32(frame + FrameLayout::lsda);
    if (!lsda) return Personality::Continue;
    LsdaHeader header;
    if (!parseLsda(lsda, header)) throw Error("CXX_LSDA", "LSDA is not readable", lsda);
    const int32_t callSite = int32_t(memory.read32(frame + FrameLayout::callSite));
    if (callSite < 0) return Personality::Continue;    // this call is outside every region
    if (callSite == 0) return Personality::Terminate;  // a must-not-throw region
    uint32_t cursor = header.callSiteTable, landingIndex = 0, action = 0;
    for (int32_t index = 0; index < callSite; index++) {
        if (!readUleb(cursor, landingIndex) || !readUleb(cursor, action))
            throw Error("CXX_LSDA",
                        "call-site table has no entry " + std::to_string(callSite) + " (LSDA 0x" +
                            hex(lsda) + ")",
                        lsda);
    }
    // A SjLj landing pad value is an index into the function's own dispatch switch; the
    // personality stores it minus one because zero means "terminate".
    const uint32_t landingPad = landingIndex + 1;
    bool sawCleanup = false, sawHandler = false;
    int32_t filter = 0;
    if (!action) {
        sawCleanup = true;
    } else {
        uint32_t record = header.actionTable + action - 1;
        for (int guard = 0; guard < 64; guard++) {
            int32_t recordFilter = 0, displacement = 0;
            if (!readSleb(record, recordFilter)) break;
            if (!readSleb(record, displacement)) break;
            if (recordFilter == 0) {
                sawCleanup = true;
            } else if (recordFilter > 0) {
                int32_t adjustment = 0;
                const uint32_t catchType = typeEntry(header, recordFilter);
                if (typeMatches(catchType, thrown.typeInfo, adjustment)) {
                    if (verbose) {
                        std::string catchName = "catch (...)";
                        if (catchType && memory.permits(catchType + 4, 4, Read)) {
                            const uint32_t name = memory.read32(catchType + 4);
                            if (name && memory.permits(name, 1, Read))
                                catchName = "'" + readString(name) + "'";
                        }
                        emit("cxx: filter " + std::to_string(recordFilter) + " catches " +
                             thrownTypeName() + " as " + catchName + " (object offset " +
                             std::to_string(adjustment) + ")");
                    }
                    const int64_t caughtObject = int64_t(thrown.object) + adjustment;
                    if (caughtObject < 0 || caughtObject > UINT32_MAX) continue;
                    thrown.caughtObject = uint32_t(caughtObject);
                    sawHandler = true;
                    filter = recordFilter;
                    break;
                }
            } else if (verbose) {
                // A negative filter is an exception specification, not a handler: it limits
                // what may leave the function. Nothing here claims it caught anything.
                emit("cxx: frame " + frameDescription(frame) +
                     " has an exception specification (filter " + std::to_string(recordFilter) +
                     ") that is not evaluated");
            }
            if (!displacement) break;
            record = record + uint32_t(displacement);
        }
    }
    if (search) {
        if (!sawHandler) return Personality::Continue;
        thrown.landingPad = landingPad;
        thrown.filter = filter;
        return Personality::HandlerFound;
    }
    if (!sawHandler && !sawCleanup) return Personality::Continue;
    pendingLandingPad = landingPad;
    pendingFilter = sawHandler ? filter : 0;
    return Personality::Install;
}
void CxxRuntime::rewindChainTo(uint32_t frame) {
    sjljHead = frame;
    std::vector<uint32_t> chain;
    for (uint32_t at = frame, guard = 0; at && guard < 4096; guard++) {
        chain.push_back(at);
        at = memory.permits(at, 4, Read) ? memory.read32(at) : 0;
    }
    // The chain runs from the innermost frame outwards; the bookkeeping list is ordered the
    // other way, innermost last, so a later unregister pops what it pushed.
    sjljFrames.assign(chain.rbegin(), chain.rend());
}
bool CxxRuntime::installFrame(uint32_t frame, CpuState& cpu) {
    if (!memory.permits(frame, FrameLayout::size, Read))
        throw Error("CXX_UNWIND", "SjLj frame context is outside guest memory", frame);
    const uint32_t savedFrame = memory.read32(frame + FrameLayout::jumpBuffer);
    const uint32_t resume = memory.read32(frame + FrameLayout::jumpBuffer + 4);
    const uint32_t savedStack = memory.read32(frame + FrameLayout::jumpBuffer + 8);
    if (!resume || !savedStack)
        throw Error("CXX_UNWIND", "frame has no saved resume context", frame);
    // What libgcc's `_Unwind_SetGR`/`_Unwind_SetIP` do before the longjmp: the landing pad
    // reads the exception from data[0], the filter value from data[1] and the dispatch index
    // from call_site.
    memory.write32(frame + FrameLayout::callSite, pendingLandingPad - 1);
    memory.write32(frame + FrameLayout::data, thrown.caughtObject);
    memory.write32(frame + FrameLayout::data + 4, uint32_t(pendingFilter));
    rewindChainTo(frame);
    // __builtin_longjmp restores the frame pointer from buffer[0], the stack from buffer[2]
    // and jumps to buffer[1]; nothing else in the register file survives an unwind.
    // This toolchain is Apple's ARM gcc, whose hard frame pointer is r7 (the guest's own
    // prologues use `add r7, sp, #12` and its epilogues `sub sp, r7, #n`), so the frame
    // pointer word restored by __builtin_longjmp belongs in r7. The guest's landing pads
    // depend on it: their epilogues address the stack through it.
    cpu.registers[7] = savedFrame;
    cpu.registers[13] = savedStack;
    cpu.registers[15] = resume & ~1u;
    if (resume & 1)
        cpu.cpsr |= 0x20;
    else
        cpu.cpsr &= ~0x20u;
    if (verbose)
        emit("cxx: resumed frame " + frameDescription(frame) + " at 0x" + hex(resume & ~1u) +
             " sp=0x" + hex(savedStack) + " landing pad " + std::to_string(pendingLandingPad) +
             " filter " + std::to_string(pendingFilter));
    return true;
}
bool CxxRuntime::unwindPhase2(CpuState& cpu) {
    uint32_t frame = sjljHead;
    for (size_t depth = 0; frame && depth < 4096; depth++) {
        const bool handlerFrame = frame == thrown.handlerFrame;
        const Personality result = personalityFor(frame, false, handlerFrame);
        if (result == Personality::Install) return installFrame(frame, cpu);
        if (result == Personality::Terminate)
            throw Error("CXX_TERMINATE",
                        "the exception reached a frame whose call site must not throw: " +
                            frameDescription(frame),
                        frame);
        frame = memory.permits(frame, 4, Read) ? memory.read32(frame) : 0;
    }
    throw Error("CXX_UNWIND_END", "the unwinder ran out of SjLj frames without a landing pad");
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
        r[0] = allocate(r[0], "operator new", &cpu);
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
        r[0] = allocate(bytes, "exception object", &cpu);
        zero(r[0], bytes);
        return true;
    }
    if (name == "___cxa_free_exception") {
        if (r[0] && heap.owns(r[0])) heap.release(r[0]);
        return true;
    }
    if (name == "___dynamic_cast") {
        // Itanium ABI: __dynamic_cast(src_ptr, src_type, dst_type, src2dst_offset). The
        // object vptr's two preceding words are offset-to-top and the dynamic type_info.
        // Public, non-virtual source/destination base adjustment shares the same bounded RTTI
        // walk as exception matching; virtual-base casts need the full ABI vtable search and
        // are refused as a null cast rather than returning an unadjusted pointer.
        const uint32_t source = r[0], sourceType = r[1], destinationType = r[2];
        if (!source) {
            r[0] = 0;
            return true;
        }
        if (!memory.permits(source, 4, Read))
            throw Error("CXX_DYNAMIC_CAST", "source object is not readable", source);
        const uint32_t vtable = memory.read32(source);
        if (vtable < 8 || !memory.permits(vtable - 8, 8, Read)) {
            r[0] = 0;
            return true;
        }
        const int32_t topOffset = int32_t(memory.read32(vtable - 8));
        const uint32_t dynamicType = memory.read32(vtable - 4);
        const int64_t top = int64_t(source) + topOffset;
        if (!sourceType || top < 0 || top > UINT32_MAX || !dynamicType) {
            r[0] = 0;
            return true;
        }
        int32_t sourceAdjustment = 0;
        if (!typeMatches(sourceType, dynamicType, sourceAdjustment) ||
            int64_t(top) + sourceAdjustment != source) {
            r[0] = 0;
            return true;
        }
        if (!destinationType) {
            r[0] = uint32_t(top);  // dynamic_cast<void*> returns the complete object pointer
            return true;
        }
        int32_t destinationAdjustment = 0;
        if (!typeMatches(destinationType, dynamicType, destinationAdjustment)) {
            r[0] = 0;
            return true;
        }
        const int64_t result = top + destinationAdjustment;
        r[0] = result >= 0 && result <= UINT32_MAX ? uint32_t(result) : 0;
        return true;
    }
    if (name == "___cxa_throw") {
        // __cxa_throw(object, type_info, destructor): raise, exactly as libstdc++ does, with
        // this runtime playing the part of libgcc's SjLj unwinder. Phase 1 searches the
        // guest's own frame chain for a landing pad that catches, phase 2 walks it again
        // running cleanups, and the frame that catches is installed by rewinding the guest.
        thrown = ThrowRecord{};
        thrown.active = true;
        thrown.object = r[0];
        thrown.caughtObject = r[0];
        thrown.typeInfo = r[1];
        thrown.destructor = r[2];
        if (verbose)
            emit("cxx: throw " + thrownTypeName() + " object=0x" + hex(r[0]) + " over " +
                 std::to_string(sjljFrames.size()) + " frame(s)");
        uint32_t frame = sjljHead;
        bool handlerFound = false;
        for (size_t depth = 0; frame && depth < 4096; depth++) {
            if (personalityFor(frame, true, false) == Personality::HandlerFound) {
                handlerFound = true;
                thrown.handlerFrame = frame;
                break;
            }
            frame = memory.permits(frame, 4, Read) ? memory.read32(frame) : 0;
        }
        if (!handlerFound) {
            const std::string type = thrownTypeName();
            thrown = ThrowRecord{};
            throw Error("CXX_UNCAUGHT",
                        "the guest threw " + type + " and no landing pad in " +
                            std::to_string(sjljFrames.size()) +
                            " SjLj frame(s) catches it, so std::terminate would run");
        }
        if (verbose)
            emit("cxx: handler in frame 0x" + hex(thrown.handlerFrame) + " landing pad " +
                 std::to_string(thrown.landingPad) + " filter " + std::to_string(thrown.filter));
        return unwindPhase2(cpu);
    }
    if (name == "___cxa_begin_catch") {
        if (!thrown.active)
            throw Error("CXX_CATCH", "the guest began a catch with no exception in flight");
        // The landing pad passes the word the unwinder placed in frame->data[0], which is the
        // thrown object itself; the catch block binds that pointer.
        if (r[0] != thrown.caughtObject)
            emit("cxx: __cxa_begin_catch was handed 0x" + hex(r[0]) +
                 " but the catch object is 0x" + hex(thrown.caughtObject));
        thrown.caught = true;
        r[0] = thrown.caughtObject;
        return true;
    }
    if (name == "___cxa_end_catch") {
        if (thrown.active && thrown.caught) {
            // libstdc++ would call the exception's destructor here. That destructor is guest
            // code and this runtime has no way to call into the guest from inside a service
            // yet, so the object is released without it and the omission is stated instead of
            // being hidden behind a success.
            if (thrown.destructor && verbose)
                emit("cxx: exception destructor 0x" + hex(thrown.destructor) +
                     " is not called (guest callbacks from a service are not implemented)");
            if (thrown.object && heap.owns(thrown.object)) heap.release(thrown.object);
            thrown = ThrowRecord{};
        }
        r[0] = 0;
        return true;
    }
    if (name == "___cxa_rethrow") {
        if (!thrown.active)
            throw Error("CXX_CATCH", "the guest rethrew with no exception in flight");
        // A rethrow continues from the frame that is handling, so that frame is skipped.
        if (sjljHead && memory.permits(sjljHead, 4, Read)) sjljHead = memory.read32(sjljHead);
        rewindChainTo(sjljHead);
        return unwindPhase2(cpu);
    }
    if (name == "___cxa_call_unexpected" || name == "___cxa_pure_virtual" ||
        name == "___cxa_bad_typeid") {
        throw Error("CXX_ABI", "the guest called " + name + ", which ends the process");
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
