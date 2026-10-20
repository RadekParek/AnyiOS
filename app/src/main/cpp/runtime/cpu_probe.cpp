#include "cpu_probe.h"

#include <vector>

#include "cpu.h"
#include "error.h"

namespace podnative::runtime {
std::string cpuProbe() {
    try {
        Memory memory;
        std::vector<uint8_t> code(4096);
        auto put = [&](size_t offset, uint32_t value) {
            for (size_t n = 0; n < 4; n++) code.at(offset + n) = uint8_t(value >> (n * 8));
        };
        put(0, 0xe3a0002a);    // ARM mov r0,#42
        put(4, 0xef000007);    // svc #7
        put(128, 0x30042003);  // Thumb movs r0,#3; adds r0,#4
        put(132, 0xe7fedf09);  // svc #9; b .
        memory.map({0x1000, 4096, uint8_t(Read | Execute), "PodNative original CPU probe"},
                   code.data(), code.size());
        Cpu cpu(memory);
        CpuState state;
        state.registers[15] = 0x1000;
        cpu.setState(state);
        auto arm = cpu.run(8);
        if (arm.reason != StopReason::SupervisorCall || arm.service != 7 ||
            cpu.state().registers[0] != 42)
            throw Error("CPU_PROBE_ARM", "unexpected ARM result: " + arm.code + " " + arm.detail);
        state = {};
        state.registers[15] = 0x1080;
        state.cpsr = 0x30;
        cpu.setState(state);
        auto thumb = cpu.run(8);
        if (thumb.reason != StopReason::SupervisorCall || thumb.service != 9 ||
            cpu.state().registers[0] != 7)
            throw Error("CPU_PROBE_THUMB",
                        "unexpected Thumb result: " + thumb.code + " " + thumb.detail);
        return "CPU_SELF_TEST_PASS: actual Dynarmic ARM r0=42/SVC7 and Thumb r0=7/SVC9. Fixed original programs only; no game execution or framework compatibility claim.";
    } catch (const std::exception& e) {
        return "CPU_SELF_TEST_FAIL: " + std::string(e.what());
    }
}
}  // namespace podnative::runtime
