#include "cpu.h"

#include <dynarmic/interface/A32/a32.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <limits>
#include <optional>

#include "error.h"
namespace podnative::runtime {
class Cpu::Impl final : public Dynarmic::A32::UserCallbacks {
   public:
    Memory& memory;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::optional<CpuStop> stopped;
    bool poisoned = false;
    uint32_t instructionPC = 0;
    // Batch execution keeps an exact instruction count through Dynarmic's own cycle
    // accounting: the JIT reports every tick it consumed, so the number in the report is
    // still a count of retired instructions, not an estimate.
    uint64_t ticksRemaining = 0, ticksUsed = 0;
    bool precise = false;
    uint64_t observedMemoryRevision = 0;
    // A ring of the PCs that were about to execute, newest at pcCursor-1. Fixed size, one
    // store per instruction, so it stays affordable in a profile build too.
    static constexpr size_t PcHistory = 256;
    std::array<uint32_t, PcHistory> pcs{};
    size_t pcCursor = 0;
    void record(uint32_t pc) {
        pcs[pcCursor % PcHistory] = pc;
        pcCursor++;
    }
    // A fault inside translated code cannot be reported as a C++ exception: generated code has
    // no unwind tables. This handler therefore prints the guest PCs that led to the fault and
    // the host address that faulted, then lets the default action happen. Without it a run
    // ends with nothing but a host RIP, which says nothing about the guest.
    static Impl* active;
    static void reportCrash(int signal, siginfo_t* info, void* context) {
        (void)context;
        Impl* impl = active;
        char line[512];
        size_t used = 0;
        auto append = [&](const char* text) {
            while (*text && used + 1 < sizeof(line)) line[used++] = *text++;
        };
        auto appendHex = [&](uint32_t value) {
            static const char digits[] = "0123456789abcdef";
            append("0x");
            for (int shift = 28; shift >= 0; shift -= 4)
                if (used + 1 < sizeof(line)) line[used++] = digits[(value >> shift) & 0xf];
        };
        append(signal == SIGBUS ? "HOST_FAULT: SIGBUS at " : "HOST_FAULT: SIGSEGV at ");
        appendHex(uint32_t(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr)));
        if (impl) {
            const auto& registers = impl->jit->Regs();
            const char* names[] = {"\nHOST_FAULT_GUEST_REGS: r0=", " r1=", " r2=", " r3="};
            for (size_t index = 0; index < 4; index++) {
                append(names[index]);
                appendHex(registers[index]);
            }
            append(" sp=");
            appendHex(registers[13]);
            append(" lr=");
            appendHex(registers[14]);
            append(" pc=");
            appendHex(registers[15]);
        }
        append("\nHOST_FAULT_GUEST_PCS:");
        if (impl) {
            const size_t count = std::min(impl->pcCursor, size_t(PcHistory));
            for (size_t i = 0; i < count && used + 8 < sizeof(line); i++) {
                append(" ");
                appendHex(impl->pcs[(impl->pcCursor - count + i) % PcHistory]);
            }
        }
        append("\n");
        const ssize_t ignored = ::write(2, line, used);
        (void)ignored;
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
    struct sigaction previousSegv{}, previousBus{};
    void installCrashReporter() {
        active = this;
        struct sigaction action{};
        action.sa_sigaction = &Impl::reportCrash;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previousSegv);
        sigaction(SIGBUS, &action, &previousBus);
    }
    explicit Impl(Memory& memory, IsaArch arch) : memory(memory) {
        Dynarmic::A32::UserConfig config{};
        config.callbacks = this;
        // iOS "armv6" devices are ARM11 cores, i.e. ARMv6K. Dynarmic has no plain v6
        // selector, and v6K is the superset those binaries were built against.
        config.arch_version = arch == IsaArch::ArmV6 ? Dynarmic::A32::ArchVersion::v6K
                                                     : Dynarmic::A32::ArchVersion::v7;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = 16 * 1024 * 1024;
        config.unsafe_optimizations = false;
        jit = std::make_unique<Dynarmic::A32::Jit>(config);
        installCrashReporter();
    }
    ~Impl() {
        sigaction(SIGSEGV, &previousSegv, nullptr);
        sigaction(SIGBUS, &previousBus, nullptr);
        if (active == this) active = nullptr;
    }
    void fault(const std::string& code, const std::string& detail, uint32_t address = 0) {
        if (!stopped)
            stopped = CpuStop{StopReason::Fault, 0, instructionPC, 0, address, code, detail};
        if (jit) jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }
    // Exceptions never unwind through generated machine code.
    template <class T>
    T read(uint32_t address) {
        if (stopped) return 0;
        uint8_t bytes[sizeof(T)]{};
        try {
            memory.read(address, bytes, sizeof(T));
        } catch (const Error& e) {
            fault(e.code, e.what(), e.address);
            return 0;
        }
        T value = 0;
        for (size_t i = 0; i < sizeof(T); i++) value |= T(bytes[i]) << (i * 8);
        return value;
    }
    template <class T>
    void write(uint32_t address, T value) {
        if (stopped) return;
        uint8_t bytes[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); i++) bytes[i] = uint8_t(value >> (i * 8));
        try {
            // Only a store into an execute-mapped page can make translated code stale. Data
            // stores are the overwhelming majority, and invalidating a range costs far more
            // than the store itself: doing it for every byte written made the guest run at a
            // fraction of the speed it needs.
            const bool executable = memory.permits(address, sizeof(T), Execute);
            memory.write(address, bytes, sizeof(T));
            if (executable) jit->InvalidateCacheRange(address, sizeof(T));
            observedMemoryRevision = memory.revision();
        } catch (const Error& e) {
            fault(e.code, e.what(), e.address);
        }
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        try {
            return memory.fetch32(address);
        } catch (const Error& e) {
            fault(e.code, e.what(), e.address);
            return std::nullopt;
        }
    }
    uint8_t MemoryRead8(uint32_t a) override { return read<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return read<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return read<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return read<uint64_t>(a); }
    void MemoryWrite8(uint32_t a, uint8_t v) override { write(a, v); }
    void MemoryWrite16(uint32_t a, uint16_t v) override { write(a, v); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { write(a, v); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { write(a, v); }
    template <class T>
    bool exclusive(uint32_t address, T, T) {
        fault("CPU_EXCLUSIVE_UNSUPPORTED", "exclusive-memory semantics not yet validated", address);
        return false;
    }
    bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override {
        return exclusive(a, v, e);
    }
    bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override {
        return exclusive(a, v, e);
    }
    void InterpreterFallback(uint32_t pc, size_t) override {
        fault("CPU_INSTRUCTION_UNSUPPORTED", "Dynarmic requested interpreter fallback", pc);
    }
    void CallSVC(uint32_t service) override {
        // In a batched run the block entry is not the SVC's address, so the trapping
        // instruction is found from the register file: Dynarmic has already advanced the PC
        // past the SVC, and the encoding at the candidate address says whether it really is
        // the trap (ARM `svc #imm24`, Thumb `svc #imm8`). A candidate that does not encode
        // this service is rejected, so a wrong guess names itself instead of silently
        // mis-dispatching.
        uint32_t pc = instructionPC;
        if (!precise) {
            const uint32_t after = jit->Regs()[15];
            const std::array<uint32_t, 2> candidates{after - 4, after - 2};
            for (uint32_t candidate : candidates) {
                try {
                    const uint32_t word = memory.fetch32(candidate & ~1u);
                    const bool arm =
                        (word & 0xff000000u) == 0xef000000u && (word & 0xffffffu) == service;
                    const bool thumb =
                        (word & 0xffff0000u) == 0xdf000000u && (word & 0xffu) == service;
                    if (arm || thumb) {
                        pc = candidate;
                        break;
                    }
                } catch (const Error&) {
                }
            }
        }
        if (pc && jit) instructionPC = pc;
        if (!stopped)
            stopped = CpuStop{StopReason::SupervisorCall,
                              0,
                              pc,
                              service,
                              0,
                              "CPU_SVC",
                              "service requires a guest ABI handler; not emulated as success"};
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        fault("CPU_EXCEPTION", "Dynarmic exception " + std::to_string(static_cast<int>(exception)),
              pc);
    }
    void AddTicks(uint64_t ticks) override {
        ticksUsed += ticks;
        ticksRemaining = ticks >= ticksRemaining ? 0 : ticksRemaining - ticks;
    }
    uint64_t GetTicksRemaining() override { return ticksRemaining ? ticksRemaining : 1; }
};
Cpu::Impl* Cpu::Impl::active = nullptr;
Cpu::Cpu(Memory& memory, IsaArch arch) : impl_(std::make_unique<Impl>(memory, arch)) {}
Cpu::~Cpu() = default;
void Cpu::setPrecise(bool on) { impl_->precise = on; }
void Cpu::setState(const CpuState& state) {
    if ((state.cpsr & 0x1f) != 0x10 || (state.cpsr & 0x200))
        throw Error("CPU_STATE", "only little-endian ARM user mode is supported",
                    state.registers[15]);
    const bool thumb = state.cpsr & 0x20;
    if (state.registers[15] % (thumb ? 2 : 4))
        throw Error("CPU_ALIGNMENT", "invalid initial PC", state.registers[15]);
    impl_->jit->Reset();
    impl_->jit->ClearCache();
    impl_->jit->Regs() = state.registers;
    impl_->jit->ExtRegs() = state.extensionRegisters;
    impl_->jit->SetCpsr(state.cpsr);
    impl_->jit->SetFpscr(state.fpscr);
    impl_->stopped.reset();
    impl_->poisoned = false;
    impl_->observedMemoryRevision = impl_->memory.revision();
}
CpuState Cpu::state() const {
    return {impl_->jit->Regs(), impl_->jit->ExtRegs(), impl_->jit->Cpsr(), impl_->jit->Fpscr()};
}
std::vector<uint32_t> Cpu::recentPcs(size_t count) const {
    std::vector<uint32_t> result;
    const size_t available = std::min(count, std::min(impl_->pcCursor, size_t(Impl::PcHistory)));
    for (size_t i = available; i > 0; i--)
        result.push_back(impl_->pcs[(impl_->pcCursor - i) % Impl::PcHistory]);
    return result;
}
CpuStop Cpu::run(uint64_t budget) {
    if (impl_->poisoned)
        throw Error("CPU_FAULTED", "reset CPU state before continuing after a fault");
    if (budget == 0)
        return {StopReason::Budget,
                0,
                impl_->jit->Regs()[15],
                0,
                0,
                "CPU_BUDGET",
                "an empty slice retires nothing"};
    impl_->stopped.reset();
    impl_->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    // External host writes between slices must never leave translated instructions stale.
    if (impl_->observedMemoryRevision != impl_->memory.revision()) {
        impl_->jit->ClearCache();
        impl_->observedMemoryRevision = impl_->memory.revision();
    }
    impl_->ticksUsed = 0;
    impl_->ticksRemaining = budget;
    // Two execution paths, one instruction count.
    //
    // Batched (the default) enters translated code once per basic block and stops when the
    // block retires, when the guest traps into a service, or when the slice's ticks run out.
    // Precise ("Verbose logging") single-steps so every recorded PC is the instruction that
    // actually ran, which is what makes a deep trace or a fault report exact. Both report the
    // number of instructions Dynarmic retired; neither estimates it.
    if (!impl_->precise) {
        while (impl_->ticksRemaining > 0 && budget > impl_->ticksUsed) {
            impl_->instructionPC = impl_->jit->Regs()[15];
            impl_->record(impl_->instructionPC);
            Dynarmic::HaltReason flags;
            try {
                flags = impl_->jit->Run();
            } catch (const std::exception& e) {
                impl_->poisoned = true;
                return {StopReason::Fault,    impl_->ticksUsed, impl_->instructionPC, 0, 0,
                        "CPU_HOST_EXECUTION", e.what()};
            }
            if (impl_->stopped) {
                auto result = *impl_->stopped;
                result.attemptedInstructions = impl_->ticksUsed;
                impl_->poisoned = result.reason == StopReason::Fault;
                return result;
            }
            if (Dynarmic::Has(flags, Dynarmic::HaltReason::MemoryAbort)) {
                impl_->poisoned = true;
                return {StopReason::Fault,
                        impl_->ticksUsed,
                        impl_->instructionPC,
                        0,
                        0,
                        "CPU_MEMORY_ABORT",
                        "unreported upstream memory abort"};
            }
            if (Dynarmic::Has(flags, Dynarmic::HaltReason::CacheInvalidation))
                impl_->jit->ClearHalt(Dynarmic::HaltReason::CacheInvalidation);
        }
        return {StopReason::Budget,
                impl_->ticksUsed,
                impl_->jit->Regs()[15],
                0,
                0,
                "CPU_BUDGET",
                "instruction budget exhausted"};
    }
    for (uint64_t i = 0; i < budget; i++) {
        impl_->instructionPC = impl_->jit->Regs()[15];
        impl_->record(impl_->instructionPC);
        Dynarmic::HaltReason flags;
        try {
            flags = impl_->jit->Step();
        } catch (const std::exception& e) {
            impl_->poisoned = true;
            return {StopReason::Fault,    i + 1,   impl_->instructionPC, 0, 0,
                    "CPU_HOST_EXECUTION", e.what()};
        }
        if (impl_->stopped) {
            auto result = *impl_->stopped;
            result.attemptedInstructions = i + 1;
            impl_->poisoned = result.reason == StopReason::Fault;
            return result;
        }
        if (Dynarmic::Has(flags, Dynarmic::HaltReason::MemoryAbort)) {
            impl_->poisoned = true;
            return {StopReason::Fault,
                    i + 1,
                    impl_->instructionPC,
                    0,
                    0,
                    "CPU_MEMORY_ABORT",
                    "unreported upstream memory abort"};
        }
    }
    return {StopReason::Budget,
            budget,
            impl_->jit->Regs()[15],
            0,
            0,
            "CPU_BUDGET",
            "instruction budget exhausted"};
}
}  // namespace podnative::runtime
