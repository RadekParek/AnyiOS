#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cpu.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// C++ and compiler-runtime support that an iOS ARMv7 image links against:
// libstdc++.6.dylib (operator new/delete, the Itanium C++ ABI entry points) and
// libgcc_s.1.dylib (the integer/64-bit helper routines the ARM backend emits).
//
// Every entry point either performs the operation for real or raises a named guest
// failure. Nothing here reports success for work it did not do: exception unwinding,
// for example, is refused with CXX_UNWIND because SjLj registration is not emulated.
class CxxRuntime final {
   public:
    CxxRuntime(Memory& memory, GuestHeap& heap)
        : memory(memory), heap(heap), logSink([](const std::string&) {}) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    void setLogSink(std::function<void(const std::string&)> sink);
    // Extra tracing for the "Verbose logging" setting: every unwind decision, every
    // exception and every skipped runtime step. Off by default, and never on the hot path.
    void setVerbose(bool on) { verbose = on; }
    bool isVerbose() const { return verbose; }
    // Registered __cxa_atexit/_atexit handlers, run once at guest exit.
    size_t atExitCount() const { return atExit.size(); }
    size_t runAtExit();
    bool isCxxSymbol(const std::string& symbol) const;
    bool isCompilerHelper(const std::string& symbol) const;

   private:
    Memory& memory;
    GuestHeap& heap;
    std::function<void(const std::string&)> logSink;
    struct ExitHandler {
        uint32_t function, argument;
    };
    std::vector<ExitHandler> atExit;
    // Stack of live `__Unwind_SjLj_Register` frames (innermost last).
    std::vector<uint32_t> sjljFrames;
    // Head of the guest's SjLj frame chain (each frame stores its `prev` in guest memory).
    uint32_t sjljHead = 0;
    uint32_t errnoCell = 0;

    // ---- SjLj unwinding -------------------------------------------------------------
    // libgcc's SjLj unwinder works on `struct SjLj_Function_Context` blocks that the guest
    // itself lays out and links through `prev`. The layout below is the one libgcc and this
    // 2009 toolchain agree on; it is verified against the guest's own frames, not assumed.
    struct FrameLayout {
        static constexpr uint32_t prev = 0;
        static constexpr uint32_t callSite = 4;
        static constexpr uint32_t data = 8;  // four words
        static constexpr uint32_t personality = 24;
        static constexpr uint32_t lsda = 28;
        static constexpr uint32_t jumpBuffer = 32;  // {frame pointer, resume label, stack}
        static constexpr uint32_t size = 44;
    };
    // Parsed LSDA header. The call-site table of a SjLj image is a sequence of uleb128
    // (landing-pad index, action index) pairs indexed by `fc->call_site`, not the DWARF
    // address-range table: libstdc++'s `_GLIBCXX_SJLJ_EXCEPTIONS` branch reads it that way.
    struct LsdaHeader {
        uint32_t callSiteTable = 0, actionTable = 0, typeTable = 0;
        uint8_t typeEncoding = 0xff;
    };
    enum class Personality { Continue, HandlerFound, Install, Terminate };
    // One exception in flight, from `__cxa_throw` to `__cxa_end_catch`.
    struct ThrowRecord {
        bool active = false, caught = false;
        uint32_t object = 0, typeInfo = 0, destructor = 0;
        uint32_t handlerFrame = 0, landingPad = 0;
        int32_t filter = 0;
    };
    ThrowRecord thrown;
    // Landing-pad decision for the frame the personality last accepted.
    uint32_t pendingLandingPad = 0;
    int32_t pendingFilter = 0;
    bool readUleb(uint32_t& cursor, uint32_t& value) const;
    bool readSleb(uint32_t& cursor, int32_t& value) const;
    bool parseLsda(uint32_t address, LsdaHeader& header) const;
    uint32_t typeEntry(const LsdaHeader& header, int32_t filter) const;
    bool typeMatches(uint32_t catchType, uint32_t thrownType) const;
    Personality personalityFor(uint32_t frame, bool search, bool handlerFrame);
    bool installFrame(uint32_t frame, CpuState& cpu);
    void rewindChainTo(uint32_t frame);
    bool unwindPhase2(CpuState& cpu);
    std::string frameDescription(uint32_t frame) const;
    std::string thrownTypeName() const;
    std::string hex(uint32_t value) const;
    bool verbose = false;
    uint32_t allocate(uint32_t bytes, const char* what);
    uint32_t errnoAddress();
    void zero(uint32_t address, size_t bytes);
    std::string readString(uint32_t address) const;
    void emit(const std::string& text) const;
    bool compilerHelper(const std::string& name, CpuState& cpu);
    bool fortified(const std::string& name, CpuState& cpu);
    bool abi(const std::string& name, CpuState& cpu);
};
}  // namespace podnative::runtime
