#include "runtime/cpu.h"

#include <atomic>
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
    // The analogous Thumb-1 load faults at its own halfword address, not the block entry.
    put(code, 320, 0xe7fe6808);  // ldr r0,[r1]; b .
    // ARM undefined instruction: must stop, not silently execute as a no-op.
    put(code, 256, 0xe7f000f0);
    // A batched block that faults on its second instruction must report that instruction,
    // not the block's entry PC.
    put(code, 272, 0xe3a02001);  // mov r2,#1 at 0x1110
    put(code, 276, 0xe5910000);  // ldr r0,[r1] at 0x1114
    put(code, 512, 0xe3a00000);  // mov r0,#0
    put(code, 516, 0xe3a08044);  // mov r8,#0x44
    put(code, 520, 0xe3a06077);  // mov r6,#0x77
    put(code, 524, 0xe7806008);  // str r6,[r0,r8] at 0x120c
    put(code, 528, 0xe3a00001);  // mov r0,#1 would expose execution past a fault
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
    state.registers[1] = 0x44;
    state.registers[15] = 0x1110;
    cpu.setState(state);
    result = cpu.run(8);
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_NULL");
    assert(result.address == 0x44 && result.pc == 0x1114);
    state = initial(true);
    state.registers[1] = 0x44;
    state.registers[15] = 0x1140;
    cpu.setState(state);
    result = cpu.run(8);
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_NULL");
    assert(result.address == 0x44 && result.pc == 0x1140);
    // Several instructions in one translated block establish the null base and 0x44 offset.
    // The MEMORY_NULL report must preserve those fault-time operands and stop at the store,
    // not expose block-entry registers or the following instruction's MOV.
    state = initial();
    state.registers[0] = 0x6f500000;
    state.registers[6] = 0xdeadbeef;
    state.registers[8] = 0;
    state.registers[15] = 0x1200;
    cpu.setState(state);
    result = cpu.run(8);
    const CpuState storeFault = cpu.faultState();
    const CpuState stoppedState = cpu.state();
    assert(result.reason == StopReason::Fault && result.code == "MEMORY_NULL");
    assert(result.address == 0x44 && result.pc == 0x120c);
    assert(storeFault.registers[0] == 0 && storeFault.registers[6] == 0x77 &&
           storeFault.registers[8] == 0x44);
    assert(stoppedState.registers[0] == 0 && stoppedState.registers[6] == 0x77 &&
           stoppedState.registers[8] == 0x44);
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
        const std::string described = describeRun(run);
        assert(described.find("GUEST_STOP: UNIMPLEMENTED") != std::string::npos);
        // The crash screen consumes this line; it must name the API, not a generic stop.
        assert(described.find("GUEST_BLOCKER: kind=Unimplemented API") != std::string::npos);
        assert(described.find("_UIApplicationMain") != std::string::npos);
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
        std::atomic<bool> cancel{true};
        auto cancelled = runGuest(working, workServices, workGraph, workMain, 0, 0, &cancel);
        assert(cancelled.stop == "CANCELLED" && cancelled.instructions == 0);
        assert(describeRun(cancelled).find("GUEST_BLOCKER: kind=Cancelled") != std::string::npos);
        cancelled.called = {"_malloc", "_malloc", "_free"};
        assert(describeRun(cancelled).find("HOST_CALLS:") == std::string::npos);
        assert(
            describeRun(cancelled, true)
                .find(
                    "HOST_CALLS: last 3 call(s), 2 distinct service name(s): _malloc (2x) _free") !=
            std::string::npos);
        LoadedImage faulting;
        faulting.memory = std::make_unique<Memory>();
        Services faultServices(*faulting.memory);
        ExportGraph faultGraph;
        faultServices.install(faultGraph);
        const size_t faultMain = faultGraph.addImage("<faulting guest>");
        faultGraph.setMainExecutable(faultMain);
        std::vector<uint8_t> badLoad(4096, 0), faultStack(4096, 0), faultWx(4096, 0);
        put(badLoad, 0, 0xe5910000);  // ldr r0, [r1] -> MEMORY_NULL at 0x44
        faulting.memory->map({0xB000, 4096, uint8_t(Read | Execute), "faulting guest"},
                             badLoad.data(), badLoad.size());
        faulting.memory->map({0xC000, 4096, uint8_t(Read | Write), "faulting stack"},
                             faultStack.data(), faultStack.size());
        // Android's 16 KiB host pages use checked memory rather than exact-page fastmem.
        // Make this report test exercise the same checked null-fault path on 4 KiB hosts.
        faulting.memory->map(
            {0xD000, 4096, uint8_t(Read | Write | Execute), "checked-path selector"},
            faultWx.data(), faultWx.size());
        faulting.registers[1] = 0x44;
        faulting.registers[4] = 0x12345678;
        faulting.registers[13] = 0xCFF0;
        faulting.registers[15] = 0xB000;
        faulting.cpsr = 0x10;
        const auto faultRun = runGuest(faulting, faultServices, faultGraph, faultMain, 8, 2.0);
        assert(faultRun.stop == "FAULT" &&
               faultRun.detail.find("MEMORY_NULL") != std::string::npos);
        assert(faultRun.detail.find("at 0xb000") != std::string::npos);
        assert(faultRun.detail.find("r1=0x00000044") != std::string::npos);
        assert(faultRun.detail.find("r4=0x12345678") != std::string::npos);
        // The reported 0x6f000480 was slot 144 in the original service table; adding
        // runtime exports moves later slots, so resolve _exit by symbol rather than pinning
        // a diagnostic address. An explicit exit and an entry-point BX LR must differ.
        LoadedImage exiting;
        exiting.memory = std::make_unique<Memory>();
        Services exitServices(*exiting.memory);
        ExportGraph exitGraph;
        exitServices.install(exitGraph);
        const size_t exitMain = exitGraph.addImage("<exiting guest>");
        exitGraph.setMainExecutable(exitMain);
        std::vector<uint8_t> exitCode(4096, 0);
        put(exitCode, 0, 0xe3a0002a);  // mov r0, #42
        putMovAddress(exitCode, 4, exitServices.address("_exit"));
        put(exitCode, 12, 0xe12fff33);  // blx r3 -> _exit
        exiting.memory->map({0x8000, 4096, uint8_t(Read | Execute), "exiting guest"},
                            exitCode.data(), exitCode.size());
        exiting.registers[13] = 0x8ff0;
        exiting.registers[15] = 0x8000;
        exiting.cpsr = 0x10;
        const auto exitRun = runGuest(exiting, exitServices, exitGraph, exitMain, 64, 2.0);
        assert(exitRun.stop == "EXITED");
        assert(exitRun.detail.find("_exit with status 42") != std::string::npos);

        LoadedImage returning;
        returning.memory = std::make_unique<Memory>();
        Services returnServices(*returning.memory);
        ExportGraph returnGraph;
        returnServices.install(returnGraph);
        const size_t returnMain = returnGraph.addImage("<returning guest>");
        returnGraph.setMainExecutable(returnMain);
        std::vector<uint8_t> returnCode(4096, 0);
        put(returnCode, 0, 0xe12fff1e);  // bx lr, with the process-entry LR still zero
        returning.memory->map({0x9000, 4096, uint8_t(Read | Execute), "returning guest"},
                              returnCode.data(), returnCode.size());
        returning.registers[13] = 0x9ff0;
        returning.registers[15] = 0x9000;
        returning.cpsr = 0x10;
        auto returnRun = runGuest(returning, returnServices, returnGraph, returnMain, 64, 2.0);
        assert(returnRun.stop == "RETURNED");
        assert(describeRun(returnRun).find("GUEST_BLOCKER: kind=Unexpected entry-point return") !=
               std::string::npos);
        std::cout
            << "Bounded guest execution, host service dispatch, cancellation and trap reporting passed\n";
    }
    // Darwin runs __mod_init_func entries before the process entry point. The initializer
    // writes a flag; main copies it and its incoming r0 to guest memory, proving both order
    // and that the runtime trampoline preserves the process-entry registers.
    {
        LoadedImage initialized;
        initialized.memory = std::make_unique<Memory>();
        Services initServices(*initialized.memory);
        ExportGraph initGraph;
        initServices.install(initGraph);
        const size_t initMain = initGraph.addImage("<initializer guest>");
        initGraph.setMainExecutable(initMain);

        std::vector<uint8_t> program(4096, 0), data(4096, 0), stack(4096, 0);
        put(program, 0, 0xe59f4018);   // ldr r4, [pc, #0x18] -> 0x9020
        put(program, 4, 0xe5941000);   // ldr r1, [r4]
        put(program, 8, 0xe5841004);   // str r1, [r4, #4]
        put(program, 12, 0xe5840008);  // str r0, [r4, #8]
        put(program, 16, 0xeafffffe);  // b .
        put(program, 32, 0x9020);
        put(program, 0x100, 0xe59f0008);  // initializer: ldr r0, [pc, #8] -> 0x9020
        put(program, 0x104, 0xe3a01001);  // mov r1, #1
        put(program, 0x108, 0xe5801000);  // str r1, [r0]
        put(program, 0x10c, 0xe12fff1e);  // bx lr
        put(program, 0x110, 0x9020);
        initialized.memory->map({0x8000, 4096, uint8_t(Read | Execute), "initializer guest"},
                                program.data(), program.size());
        initialized.memory->map({0x9000, 4096, uint8_t(Read | Write), "initializer data"},
                                data.data(), data.size());
        initialized.memory->map({0xa000, 4096, uint8_t(Read | Write), "initializer stack"},
                                stack.data(), stack.size());
        initialized.memory->write32(0x9040, 0x8100);
        initialized.initializerSections.emplace_back(0x9040, 1);
        initialized.registers[0] = 0x21;
        initialized.registers[13] = 0xb000;
        initialized.registers[15] = 0x8000;
        initialized.cpsr = 0x10;
        const auto run = runGuest(initialized, initServices, initGraph, initMain, 1000, 2.0);
        assert(run.stop == "BUDGET");
        assert(initialized.memory->read32(0x9020) == 1);
        assert(initialized.memory->read32(0x9024) == 1);
        assert(initialized.memory->read32(0x9028) == 0x21);
        std::cout << "Mach-O static initializer order and entry-register preservation passed\n";
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
    // The engine's fast paths, asserted rather than assumed: guest atomics, the thread-id
    // register iOS code reads through CP15, hint instructions, and a code cache that
    // survives the host service calls a running title makes thousands of times a second.
    {
        Memory fast;
        std::vector<uint8_t> code(4096, 0), data(4096, 0);
        put(code, 0x000, 0xe3a0004d);  // mov r0, #77
        put(code, 0x004, 0xe1923f9f);  // ldrex r3, [r2]
        put(code, 0x008, 0xe1821f90);  // strex r1, r0, [r2]
        put(code, 0x00c, 0xef000000);  // svc #0
        put(code, 0x010, 0xe3a00063);  // mov r0, #99
        put(code, 0x014, 0xe1821f90);  // strex r1, r0, [r2] with no reservation
        put(code, 0x018, 0xef000000);  // svc #0
        put(code, 0x020, 0xe1923f9f);  // ldrex r3, [r2]
        put(code, 0x024, 0xea000001);  // b 0x1030, so the pair spans two blocks
        put(code, 0x030, 0xe1821f90);  // strex r1, r0, [r2] after a context switch
        put(code, 0x034, 0xef000000);  // svc #0
        put(code, 0x040, 0xee1d0f70);  // mrc p15, 0, r0, c13, c0, 3
        put(code, 0x044, 0xef000000);  // svc #0
        put(code, 0x050, 0xe320f001);  // yield
        put(code, 0x054, 0xe320f002);  // wfe
        put(code, 0x058, 0xe320f003);  // wfi
        put(code, 0x05c, 0xe320f004);  // sev
        put(code, 0x060, 0xf592f000);  // pld [r2]
        put(code, 0x064, 0xef000000);  // svc #0
        fast.map({0x1000, 4096, uint8_t(Read | Execute), "fast-path instructions"}, code.data(),
                 code.size());
        fast.map({0x2000, 4096, uint8_t(Read | Write), "fast-path data"}, data.data(), data.size());
        fast.write32(0x2000, 5);
        Cpu cpu(fast);
        cpu.setThreadIdRegister(0x12340000);
        CpuState state;
        state.cpsr = 0x10;
        state.registers[2] = 0x2000;
        state.registers[13] = 0x2ff0;
        // A matched LDREX/STREX pair stores and reports success in the status register.
        state.registers[15] = 0x1000;
        cpu.setState(state);
        auto stop = cpu.run(16);
        if (stop.reason != StopReason::SupervisorCall)
            std::cerr << "exclusive stop=" << int(stop.reason) << " code=" << stop.code
                      << " detail=" << stop.detail << " pc=" << std::hex << stop.pc << '\n';
        assert(stop.reason == StopReason::SupervisorCall);
        assert(cpu.state().registers[1] == 0 && cpu.state().registers[3] == 5);
        assert(fast.read32(0x2000) == 77);
        // An unmatched STREX fails and leaves the guest word alone: this is the guarantee
        // iOS atomics, spin locks and dispatch_once are written against.
        state = cpu.state();
        state.registers[15] = 0x1010;
        cpu.setState(state);
        stop = cpu.run(16);
        assert(stop.reason == StopReason::SupervisorCall);
        assert(cpu.state().registers[1] == 1);
        assert(fast.read32(0x2000) == 77);
        // A reservation belongs to one context: switching threads between the two halves of
        // the pair must make the store fail rather than corrupt another thread's atomic.
        state = cpu.state();
        state.registers[0] = 55;
        state.registers[15] = 0x1020;
        cpu.setState(state);
        stop = cpu.run(2);
        assert(stop.reason == StopReason::Budget && cpu.state().registers[15] == 0x1030);
        cpu.setState(cpu.state());
        stop = cpu.run(16);
        assert(stop.reason == StopReason::SupervisorCall);
        assert(cpu.state().registers[1] == 1);
        assert(fast.read32(0x2000) == 77);
        // The thread-id register answers with the value this thread was given instead of
        // raising a coprocessor exception, so inlined TLS access works.
        state = cpu.state();
        state.registers[15] = 0x1040;
        cpu.setState(state);
        stop = cpu.run(16);
        assert(stop.reason == StopReason::SupervisorCall);
        assert(cpu.state().registers[0] == 0x12340000);
        // Yield/WFE/WFI/SEV/PLD keep the guest running and are counted for the report.
        state = cpu.state();
        state.registers[15] = 0x1050;
        cpu.setState(state);
        stop = cpu.run(32);
        assert(stop.reason == StopReason::SupervisorCall);
        assert(cpu.counters().hintInstructions >= 5);
        assert(cpu.counters().waitInstructions >= 2);
        // Translated code survived every state restore above. Discarding the code cache on
        // each host service call is what made a running title spend its time recompiling.
        assert(cpu.counters().fullCacheClears == 0);
#if defined(__unix__) || defined(__APPLE__)
        if (!cpu.counters().directMemory)
            std::cerr << "warning: the host refused a flat guest-memory reservation\n";
        assert(cpu.counters().directMemory);
#endif
        std::cout << "Guest atomics, CP15 thread id, hints and code-cache reuse passed ("
                  << (cpu.counters().fastMemory ? "hardware fastmem" : "checked page table") << ", "
                  << cpu.counters().callbackReads << " checked reads)\n";
    }
    std::cout << "Real Dynarmic ARM/Thumb, memory, service-trap, budget and fault tests passed\n";
}
