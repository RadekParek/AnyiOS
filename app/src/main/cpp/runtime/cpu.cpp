#include "cpu.h"

#include <dynarmic/interface/A32/a32.h>

#include <algorithm>
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
    uint64_t observedMemoryRevision = 0;
    explicit Impl(Memory& memory) : memory(memory) {
        Dynarmic::A32::UserConfig config{};
        config.callbacks = this;
        config.arch_version = Dynarmic::A32::ArchVersion::v7;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = 16 * 1024 * 1024;
        config.unsafe_optimizations = false;
        jit = std::make_unique<Dynarmic::A32::Jit>(config);
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
            memory.write(address, bytes, sizeof(T));
            jit->InvalidateCacheRange(address, sizeof(T));
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
        if (!stopped)
            stopped = CpuStop{StopReason::SupervisorCall,
                              0,
                              instructionPC,
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
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1; }
};
Cpu::Cpu(Memory& memory) : impl_(std::make_unique<Impl>(memory)) {}
Cpu::~Cpu() = default;
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
CpuStop Cpu::run(uint64_t budget) {
    if (impl_->poisoned)
        throw Error("CPU_FAULTED", "reset CPU state before continuing after a fault");
    if (budget > 10000000)
        throw Error("CPU_BUDGET", "single slice exceeds 10 million instructions");
    impl_->stopped.reset();
    impl_->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    // External host writes between slices must never leave translated instructions stale.
    if (impl_->observedMemoryRevision != impl_->memory.revision()) {
        impl_->jit->ClearCache();
        impl_->observedMemoryRevision = impl_->memory.revision();
    }
    for (uint64_t i = 0; i < budget; i++) {
        impl_->instructionPC = impl_->jit->Regs()[15];
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
