#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "memory.h"
namespace podnative::runtime {
// Instruction-set generation the guest was built for. iOS ARMv6 titles (iPod touch 2,
// iPhone 3G/3GS era) must not see ARMv7-only encodings, and ARMv7 titles must not be
// silently downgraded.
enum class IsaArch { ArmV6, ArmV7 };
struct CpuState {
    std::array<uint32_t, 16> registers{};
    std::array<uint32_t, 64> extensionRegisters{};
    uint32_t cpsr = 0x10;
    uint32_t fpscr = 0;
};
enum class StopReason { Budget, SupervisorCall, Fault };
struct CpuStop {
    StopReason reason;
    uint64_t attemptedInstructions;
    uint32_t pc;
    uint32_t service = 0;
    uint32_t address = 0;
    std::string code;
    std::string detail;
};
// Proven upstream JIT behind a PodNative-owned boundary. No graphics dependency.
// Strict single-step slices are an initial correctness path, not a performance claim.
class Cpu final {
   public:
    explicit Cpu(Memory& memory, IsaArch arch = IsaArch::ArmV7);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    void setState(const CpuState& state);
    // Single-step instead of running translated blocks. "Verbose logging" asks for this: it
    // trades throughput for an exact PC for every instruction, which is what a deep trace and
    // a fault report need.
    void setPrecise(bool on);
    CpuState state() const;
    CpuStop run(uint64_t maxInstructions);
    // The instructions retired immediately before the current PC, newest last. A guest that
    // jumps to an address it read out of corrupted memory cannot be explained from the fault
    // registers alone; the instructions that ran just before it can.
    std::vector<uint32_t> recentPcs(size_t count) const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace podnative::runtime
