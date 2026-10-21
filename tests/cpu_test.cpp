#include "runtime/cpu.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/abi.h"
#include "runtime/cpu_probe.h"
#include "runtime/error.h"
#include "runtime/execution.h"
#include "runtime/exports.h"
#include "runtime/services.h"
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
    assert(cpuProbe().find("CPU_SELF_TEST_PASS:") == 0);
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
    // Real guest calls through registered ARM SVC gates, including Thumb interworking.
    Services services(memory);
    uint32_t gate = services.address("_memcpy");
    auto putMovAddress = [](std::vector<uint8_t>& code, size_t at, uint32_t value) {
        auto movw = [&](uint32_t low) {
            return 0xe3000000u | ((low & 0xf000u) << 4) | (low & 0xfffu);
        };
        put(code, at, movw(value & 0xffff) | (3u << 12));
        put(code, at + 4,
            0xe3400000u | (((value >> 16) & 0xf000u) << 4) | ((value >> 16) & 0xfffu) | (3u << 12));
    };
    const char callText[] = "guest call";
    std::vector<uint8_t> armCode(4096, 0), dataCode(4096, 0);
    putMovAddress(armCode, 0, gate);
    put(armCode, 8, 0xe12fff33);   // blx r3
    put(armCode, 12, 0xeafffffe);  // b .
    // Thumb: blx r3 (interworking to the ARM gate), then b .
    put(armCode, 0x100, 0x4798);
    armCode[0x102] = 0xfe;
    armCode[0x103] = 0xe7;
    memory.map({0x6000, 4096, uint8_t(Read | Execute), "ARM caller with gate"}, armCode.data(),
               armCode.size());
    memory.map({0x7000, 4096, uint8_t(Read | Write), "call arguments"}, dataCode.data(),
               dataCode.size());
    memory.write(0x7000, callText, sizeof(callText));
    state = initial();
    state.registers[0] = 0x7200;
    state.registers[1] = 0x7000;
    state.registers[2] = sizeof(callText);
    state.registers[15] = 0x6000;
    cpu.setState(state);
    result = cpu.run(16);
    assert(result.reason == StopReason::SupervisorCall && result.pc == gate);
    state = cpu.state();
    services.dispatch(result, state);
    assert(state.registers[15] == 0x600c);
    cpu.setState(state);
    result = cpu.run(8);
    assert(result.reason == StopReason::Budget);
    uint8_t copied[sizeof(callText)]{};
    memory.read(0x7200, copied, sizeof(copied));
    assert(std::memcmp(copied, callText, sizeof(callText)) == 0);
    state = initial(true);
    state.registers[0] = 0x7300;
    state.registers[1] = 0x7000;
    state.registers[2] = 4;
    state.registers[3] = gate;
    state.registers[15] = 0x6100;
    cpu.setState(state);
    result = cpu.run(12);
    assert(result.reason == StopReason::SupervisorCall && result.pc == gate);
    state = cpu.state();
    services.dispatch(result, state);
    assert(state.registers[15] == 0x6102 && (state.cpsr & 0x20));
    cpu.setState(state);
    result = cpu.run(6);
    assert(result.reason == StopReason::Budget);
    memory.read(0x7300, copied, 5);
    assert(std::memcmp(copied, callText, 4) == 0);
    // Real bounded guest execution through the same path the app uses.
    {
        LoadedImage guest;
        guest.memory = std::make_unique<Memory>();
        Services guestServices(*guest.memory);
        ExportGraph guestGraph;
        guestServices.install(guestGraph);
        auto guestMain = guestGraph.addImage("<guest>");
        guestGraph.setMainExecutable(guestMain);
        std::vector<uint8_t> program(4096, 0), scratch(4096, 0);
        const uint32_t trap = guestServices.trapFor("_UIApplicationMain");
        putMovAddress(program, 0, trap);
        put(program, 8, 0xe12fff33);   // blx r3 -> unimplemented import
        put(program, 12, 0xeafffffe);  // b .
        guest.memory->map({0x8000, 4096, uint8_t(Read | Execute), "guest program"}, program.data(),
                          program.size());
        guest.memory->map({0x9000, 4096, uint8_t(Read | Write), "guest data"}, scratch.data(),
                          scratch.size());
        guest.registers[13] = 0x9ff0;
        guest.registers[15] = 0x8000;
        guest.cpsr = 0x10;
        auto run = runGuest(guest, guestServices, guestGraph, guestMain, 100000, 2.0);
        if (run.stop != "UNIMPLEMENTED")
            std::cerr << "run stop=" << run.stop << " detail=" << run.detail << '\n';
        assert(run.stop == "UNIMPLEMENTED");
        assert(run.detail.find("_UIApplicationMain") != std::string::npos);
        assert(run.instructions >= 3);
        assert(run.trappedImports >= 1);
        assert(describeRun(run).find("GUEST_STOP: UNIMPLEMENTED") != std::string::npos);
        // A guest that only calls an implemented API keeps running to its budget.
        LoadedImage working;
        working.memory = std::make_unique<Memory>();
        Services workServices(*working.memory);
        ExportGraph workGraph;
        workServices.install(workGraph);
        auto workMain = workGraph.addImage("<guest>");
        workGraph.setMainExecutable(workMain);
        const uint32_t workGate = workServices.address("_memcpy");
        std::vector<uint8_t> loops(4096, 0);
        putMovAddress(loops, 0, workGate);
        put(loops, 8, 0xe12fff33);
        put(loops, 12, 0xeafffffe);
        working.memory->map({0x8000, 4096, uint8_t(Read | Execute), "implemented call"},
                            loops.data(), loops.size());
        working.memory->map({0xA000, 4096, uint8_t(Read | Write), "call buffers"}, scratch.data(),
                            scratch.size());
        working.registers[0] = 0xA200;
        working.registers[1] = 0xA000;
        working.registers[2] = 8;
        working.registers[13] = 0xAFF0;
        working.registers[15] = 0x8000;
        working.cpsr = 0x10;
        auto call = runGuest(working, workServices, workGraph, workMain, 50000, 2.0);
        assert(call.serviceCalls >= 1);
        assert(call.stop == "BUDGET" || call.stop == "UNIMPLEMENTED");
        std::cout << "Bounded guest execution, host service dispatch and trap reporting passed\n";
    }
    // CoreGraphics through the iOS ARMv7 core-register call boundary, executed by the
    // real engine: a guest that loads float bit patterns into r0-r3 must observe the
    // same convention our HLE entry points marshal.
    {
        LoadedImage draw;
        draw.memory = std::make_unique<Memory>();
        Services drawServices(*draw.memory);
        ExportGraph drawGraph;
        drawServices.install(drawGraph);
        auto drawMain = drawGraph.addImage("<guest>");
        drawGraph.setMainExecutable(drawMain);
        const uint32_t midGate = drawServices.address("_CGRectGetMidX");
        const uint32_t affineGate = drawServices.address("_CGAffineTransformMakeTranslation");
        auto putMovRegister = [](std::vector<uint8_t>& code, size_t at, uint32_t reg,
                                 uint32_t value) {
            auto movw = [&](uint32_t low) {
                return 0xe3000000u | ((low & 0xf000u) << 4) | (low & 0xfffu);
            };
            put(code, at, movw(value & 0xffff) | (reg << 12));
            put(code, at + 4,
                0xe3400000u | (((value >> 16) & 0xf000u) << 4) | ((value >> 16) & 0xfffu) |
                    (reg << 12));
        };
        std::vector<uint8_t> drawCode(4096, 0), drawData(4096, 0);
        // r4 = result sink, then CGRectGetMidX(CGRect{10, 20, 30, 40}) -> 25.0f in r0.
        putMovRegister(drawCode, 0, 4, 0x9f40);
        putMovRegister(drawCode, 8, 12, midGate);
        putMovRegister(drawCode, 16, 0, floatBits(10));
        putMovRegister(drawCode, 24, 1, floatBits(20));
        putMovRegister(drawCode, 32, 2, floatBits(30));
        putMovRegister(drawCode, 40, 3, floatBits(40));
        put(drawCode, 48, 0xe12fff3c);  // blx r12
        put(drawCode, 52, 0xe5840000);  // str r0, [r4]
        put(drawCode, 56, 0xeafffffe);  // b .
        // CGAffineTransformMakeTranslation(3, 4): hidden result pointer in r0, floats
        // in r1 and r2, result written through the guest pointer at 0x9f00.
        putMovRegister(drawCode, 0x100, 12, affineGate);
        putMovRegister(drawCode, 0x108, 0, 0x9f00);
        putMovRegister(drawCode, 0x110, 1, floatBits(3));
        putMovRegister(drawCode, 0x118, 2, floatBits(4));
        put(drawCode, 0x120, 0xe12fff3c);
        put(drawCode, 0x124, 0xeafffffe);
        draw.memory->map({0x8000, 4096, uint8_t(Read | Execute), "coregraphics caller"},
                         drawCode.data(), drawCode.size());
        draw.memory->map({0x9000, 4096, uint8_t(Read | Write), "coregraphics data"},
                         drawData.data(), drawData.size());
        draw.registers[13] = 0x9ff0;
        draw.registers[15] = 0x8000;
        draw.cpsr = 0x10;
        auto mid = runGuest(draw, drawServices, drawGraph, drawMain, 200, 2.0);
        assert(mid.stop == "BUDGET" && mid.serviceCalls == 1);
        assert(draw.memory->read32(0x9f40) == floatBits(25));
        draw.registers[15] = 0x8100;
        auto affine = runGuest(draw, drawServices, drawGraph, drawMain, 200, 2.0);
        assert(affine.stop == "BUDGET" && affine.serviceCalls == 1);
        assert(draw.memory->read32(0x9f00) == floatBits(1));
        assert(draw.memory->read32(0x9f04) == 0);
        assert(draw.memory->read32(0x9f08) == 0);
        assert(draw.memory->read32(0x9f0c) == floatBits(1));
        assert(draw.memory->read32(0x9f10) == floatBits(3));
        assert(draw.memory->read32(0x9f14) == floatBits(4));
        std::cout << "Real-engine iOS ARMv7 core-register ABI for CoreGraphics passed\n";
    }
    // An image that declares ARMv6 runs on the v6K selector without losing execution:
    // the same guest program must reach its supervisor call either way. (Dynarmic takes
    // the architecture version as a translator hint rather than a decode gate, so the
    // load-time selection itself is asserted in the loader tests.)
    {
        Memory isa;
        std::vector<uint8_t> code(4096, 0);
        put(code, 0, 0xe3a00007);  // mov r0, #7
        put(code, 4, 0xef000000);  // svc #0
        isa.map({0x1000, 4096, uint8_t(Read | Execute), "isa probe"}, code.data(), code.size());
        for (IsaArch arch : {IsaArch::ArmV6, IsaArch::ArmV7}) {
            CpuState state;
            state.registers[15] = 0x1000;
            Cpu cpu(isa, arch);
            cpu.setState(state);
            const auto stop = cpu.run(4);
            assert(stop.reason == StopReason::SupervisorCall && cpu.state().registers[0] == 7);
        }
        std::cout << "ARMv6 and ARMv7 selectors both execute guest code passed\n";
    }
    std::cout << "Real Dynarmic ARM/Thumb, memory, service-trap, budget and fault tests passed\n";
}
