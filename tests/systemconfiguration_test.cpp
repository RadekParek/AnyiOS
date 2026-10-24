#include "runtime/systemconfiguration.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/error.h"
#include "runtime/frameworks.h"
using namespace podnative::runtime;
namespace {
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        assert(e.code == code);
        return;
    }
    std::cerr << "Expected " << code << '\n';
    std::abort();
}
void call(SystemConfiguration& system, Memory& memory, CpuState& cpu, const char* symbol,
          uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    cpu = {};
    cpu.registers[0] = a;
    cpu.registers[1] = b;
    cpu.registers[2] = c;
    assert(system.invoke(symbol, cpu, memory));
}
}  // namespace
int main() {
    Memory memory;
    std::vector<uint8_t> code(Memory::PageSize, 0);
    memory.map({0x1000, Memory::PageSize, Read | Execute, "guest callback code"}, code.data(),
               code.size());
    memory.map({0x2000, Memory::PageSize, Read | Write, "reachability arguments"});
    SystemConfiguration system;
    CoreFoundation cf;
    system.setRunLoopValidators(
        [&](uint32_t runLoop) { return cf.isRunLoopObject(runLoop); },
        [&](uint32_t mode) { return mode == 0x2500; });
    CpuState cpu{};
    const char host[] = "example.invalid";
    memory.write(0x2100, host, sizeof(host));

    call(system, memory, cpu, "_SCNetworkReachabilityCreateWithName", 0, 0x2100);
    const uint32_t byName = cpu.registers[0];
    assert(byName && system.owns(byName));
    memory.write32(0x2200, UINT32_MAX);
    call(system, memory, cpu, "_SCNetworkReachabilityGetFlags", byName, 0x2200);
    assert(cpu.registers[0] == 1 && memory.read32(0x2200) == 0);

    memory.write32(0x2400, 0);  // SCNetworkReachabilityContext.version == 0
    memory.write32(0x2404, 0xdeadbeef);
    memory.write32(0x2408, 0);
    memory.write32(0x240c, 0);
    memory.write32(0x2410, 0);
    call(system, memory, cpu, "_SCNetworkReachabilitySetCallback", byName, 0x1001, 0x2400);
    assert(cpu.registers[0] == 1);
    memory.write32(0x2408, 0x1001);  // no guest callback-retain ABI is wired into this model
    call(system, memory, cpu, "_SCNetworkReachabilitySetCallback", byName, 0x1001, 0x2400);
    assert(cpu.registers[0] == 0);
    memory.write32(0x2408, 0);
    CpuState loopState{};
    assert(cf.invoke("_CFRunLoopGetCurrent", loopState, memory));
    const uint32_t runLoop = loopState.registers[0];
    assert(runLoop);
    const char runLoopMode[] = "NSDefaultRunLoopMode";
    memory.write(0x2560, runLoopMode, sizeof(runLoopMode));
    memory.write32(0x2500, 0);       // __CFConstantString class word
    memory.write32(0x2504, 0x7c8);  // 8-bit constant string
    memory.write32(0x2508, 0x2560);
    memory.write32(0x250c, sizeof(runLoopMode) - 1);
    call(system, memory, cpu, "_SCNetworkReachabilityScheduleWithRunLoop", byName,
         runLoop, 0x2510);
    assert(cpu.registers[0] == 0);  // a non-string mode is refused
    call(system, memory, cpu, "_SCNetworkReachabilityScheduleWithRunLoop", byName,
         runLoop, 0x2500);
    assert(cpu.registers[0] == 1);
    call(system, memory, cpu, "_SCNetworkReachabilityUnscheduleFromRunLoop", byName,
         runLoop + 4, 0x2500);
    assert(cpu.registers[0] == 0);  // a different run-loop pair stays scheduled
    call(system, memory, cpu, "_SCNetworkReachabilityUnscheduleFromRunLoop", byName,
         runLoop, 0x2500);
    assert(cpu.registers[0] == 1);

    call(system, memory, cpu, "_SCNetworkReachabilitySetCallback", byName, 0, 0);
    assert(cpu.registers[0] == 1);
    call(system, memory, cpu, "_SCNetworkReachabilityScheduleWithRunLoop", byName,
         runLoop, 0x2500);
    assert(cpu.registers[0] == 0);  // scheduling without a callback is not claimed as success

    // sockaddr on this 32-bit Darwin ABI starts with sa_len and sa_family.
    uint8_t ipv4[16] = {16, 2};
    uint8_t ipv4Peer[16] = {16, 2};
    memory.write(0x2300, ipv4, sizeof(ipv4));
    memory.write(0x2320, ipv4Peer, sizeof(ipv4Peer));
    call(system, memory, cpu, "_SCNetworkReachabilityCreateWithAddress", 0, 0x2300);
    const uint32_t byAddress = cpu.registers[0];
    assert(byAddress && byAddress != byName && system.owns(byAddress));
    call(system, memory, cpu, "_SCNetworkReachabilityCreateWithAddressPair", 0, 0x2300, 0x2320);
    const uint32_t byPair = cpu.registers[0];
    assert(byPair && byPair != byAddress && system.owns(byPair));

    // CoreFoundation arrays can retain/release these CFType references through the same
    // external-object hooks Services installs; a reachability ref is not mistaken for a raw
    // guest pointer or leaked when the containing array dies.
    cf.setTypeArrayCallbacksAddress(0x2600);
    cf.setExternalObjectHooks(
        [&](uint32_t value) { return system.owns(value); },
        [&](uint32_t value) { system.retain(value); },
        [&](uint32_t value) { system.release(value); },
        [&](uint32_t left, uint32_t right) { return system.equal(left, right); });
    memory.write32(0x2700, byName);
    cpu = {};
    cpu.registers[1] = 0x2700;
    cpu.registers[2] = 1;
    cpu.registers[3] = 0x2600;
    assert(cf.invoke("_CFArrayCreate", cpu, memory));
    const uint32_t array = cpu.registers[0];
    assert(array && system.owns(byName));
    system.release(byName);  // only the array's retained reference remains
    assert(system.owns(byName));
    cpu.registers[0] = array;
    assert(cf.invoke("_CFRelease", cpu, memory));
    assert(!system.owns(byName));
    fault("SC_REACHABILITY", [&] { system.release(byName); });

    system.retain(byAddress);
    system.release(byAddress);
    assert(system.owns(byAddress));
    system.release(byAddress);
    system.release(byPair);
    assert(!system.owns(byAddress) && !system.owns(byPair));

    std::cout << "SystemConfiguration reachability: address/name creation, offline flags, "
                 "callback scheduling and CF ownership passed\n";
}
