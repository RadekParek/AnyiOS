#include "runtime/cpu.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/error.h"
using namespace podnative::runtime;
void put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; i++) bytes.at(offset + i) = uint8_t(value >> (i * 8));
}
CpuState initial(bool thumb = false) {
    CpuState state;
    state.registers[15] = 0x1000;
    state.cpsr = thumb ? 0x30 : 0x10;
    return state;
}
int main() {
    Memory memory;
    std::vector<uint8_t> code(4096);
    // ARM: mov r0,#42; add r1,r0,#8; str r1,[r2]; ldr r3,[r2]; svc #7; b .
    put(code, 0, 0xe3a0002a);
    put(code, 4, 0xe2801008);
    put(code, 8, 0xe5821000);
    put(code, 12, 0xe5923000);
    put(code, 16, 0xef000007);
    put(code, 20, 0xeafffffe);
    // Thumb: movs r0,#3; adds r0,#4; svc #9; b .
    put(code, 128, 0x30042003);
    put(code, 132, 0xe7fedf09);
    // ARM undefined instruction: must stop, not silently execute as a no-op.
    put(code, 256, 0xe7f000f0);
    memory.map({0x1000, 4096, uint8_t(Read | Execute), "original test instructions"}, code.data(),
               code.size());
    memory.map({0x2000, 4096, uint8_t(Read | Write), "data"});
    std::vector<uint8_t> mutableCode(4096);
    put(mutableCode, 0, 0xe3a00001);
    put(mutableCode, 4, 0xef000000);
    put(mutableCode, 8, 0xeafffffc);
    memory.map({0x4000, 4096, uint8_t(Read | Write | Execute), "guest self-modification test"},
               mutableCode.data(), mutableCode.size());
    Cpu cpu(memory);
    auto state = initial();
    state.registers[2] = 0x2000;
    cpu.setState(state);
    auto result = cpu.run(32);
    assert(result.reason == StopReason::SupervisorCall && result.service == 7);
    assert(result.attemptedInstructions == 5 && result.pc == 0x1010);
    assert(cpu.state().registers[0] == 42 && cpu.state().registers[1] == 50);
    assert(cpu.state().registers[3] == 50 && memory.read32(0x2000) == 50);
    // Safe continuation after a service trap is a caller policy, not implicit syscall emulation.
    state = cpu.state();
    state.registers[15] = 0x1014;
    cpu.setState(state);
    result = cpu.run(12);
    assert(result.reason == StopReason::Budget && result.attemptedInstructions == 12);
    assert(cpu.state().registers[15] == 0x1014);
    state = initial(true);
    state.registers[15] = 0x1080;
    cpu.setState(state);
    result = cpu.run(10);
    assert(result.reason == StopReason::SupervisorCall && result.service == 9);
    assert(cpu.state().registers[0] == 7 && result.attemptedInstructions == 3);
    // A data fault must be structured and forbid unsafe continuation without reset.
    state = initial();
    state.registers[2] = 0x3000;
    cpu.setState(state);
    result = cpu.run(10);
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_UNMAPPED");
    assert(result.address == 0x3000);
    bool threw = false;
    try {
        cpu.run(1);
    } catch (const Error& e) {
        threw = e.code == "CPU_FAULTED";
    }
    assert(threw);
    state = initial();
    state.registers[15] = 0x1100;
    cpu.setState(state);
    result = cpu.run(1);
    assert(result.reason == StopReason::Fault);
    state = initial();
    state.registers[15] = 0x2000;
    cpu.setState(state);
    result = cpu.run(1);
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_ACCESS");
    state = initial();
    cpu.setState(state);
    result = cpu.run(0);
    assert(result.reason == StopReason::Budget && result.attemptedInstructions == 0);
    // Guest writes cannot alter an RX text mapping.
    state = initial();
    state.registers[2] = 0x1000;
    cpu.setState(state);
    result = cpu.run(10);
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_ACCESS");
    assert(memory.fetch32(0x1000) == 0xe3a0002a);
    state = initial();
    state.registers[15] = 0x4000;
    cpu.setState(state);
    result = cpu.run(8);
    assert(result.reason == StopReason::SupervisorCall && cpu.state().registers[0] == 1);
    memory.write32(0x4000, 0xe3a00002);
    // Resume without setState/Reset; the memory epoch must invalidate the translated old MOV.
    result = cpu.run(8);
    assert(result.reason == StopReason::SupervisorCall && cpu.state().registers[0] == 2);
    std::cout << "Real Dynarmic ARM/Thumb, memory, service-trap, budget and fault tests passed\n";
}
