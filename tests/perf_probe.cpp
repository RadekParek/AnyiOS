// Throughput probe for the guest execution engine.
//
//   perf-probe [--seconds N] [--slice N] [--mode guest|service|both]
//
// It runs two synthetic ARM guests on the real engine and reports retired instructions per
// second: one that never leaves guest code, and one that calls a real host service
// (`memcpy`) every 1,000 iterations, which is the shape of a running title's loop. The rates
// are printed, never asserted — a rate depends on the host, so this is a measurement tool.
// What *is* asserted is structural: the engine must reach guest memory directly and must not
// throw away its translated code when the guest returns from a host call, because those two
// properties are what the rate is made of.
//
// No game files are involved, so this probe is safe to run anywhere.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/cpu.h"
#include "runtime/error.h"
#include "runtime/exports.h"
#include "runtime/services.h"

using namespace podnative::runtime;

namespace {
void put(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; i++) bytes.at(offset + i) = uint8_t(value >> (i * 8));
}

constexpr uint32_t CodeBase = 0x1000;
constexpr uint32_t DataBase = 0x2000;
constexpr uint32_t DataSize = 64 * 1024;
// The inner loop walks 1,000 words from here, so the host-call buffers sit above it.
constexpr uint32_t WalkBase = DataBase + 8;
constexpr uint32_t CopyTarget = DataBase + 0x1000;
constexpr uint32_t CopySource = DataBase + 0x1100;
constexpr uint32_t ServiceEntry = 0x1000;
constexpr uint32_t GuestEntry = 0x1100;

// Both programs run the same 1,000-iteration load/add/store loop; the service one also calls
// a real host gate before restarting. Encodings are plain ARM, checked by disassembly.
std::vector<uint8_t> buildPrograms(uint32_t gate) {
    std::vector<uint8_t> code(4096, 0);
    auto movAddress = [&code](size_t at, uint32_t registerIndex, uint32_t value) {
        put(code, at,
            0xe3000000u | ((value & 0xf000u) << 4) | (value & 0xfffu) | (registerIndex << 12));
        put(code, at + 4,
            0xe3400000u | (((value >> 16) & 0xf000u) << 4) | ((value >> 16) & 0xfffu) |
                (registerIndex << 12));
    };
    movAddress(0x000, 12, gate);       // r12 = host memcpy gate
    movAddress(0x008, 8, WalkBase);    // r8 = walk base
    movAddress(0x010, 0, CopyTarget);  // r0 = memcpy destination
    movAddress(0x018, 1, CopySource);  // r1 = memcpy source
    put(code, 0x020, 0xe3a02008u);     // mov r2, #8
    put(code, 0x024, 0xe3a04000u);     // mov r4, #0
    put(code, 0x028, 0xe1a06008u);     // mov r6, r8            <- outer loop
    put(code, 0x02c, 0xe30053e8u);     // movw r5, #1000
    put(code, 0x030, 0xe4963004u);     // ldr r3, [r6], #4      <- inner loop
    put(code, 0x034, 0xe0844003u);     // add r4, r4, r3
    put(code, 0x038, 0xe5064008u);     // str r4, [r6, #-8]
    put(code, 0x03c, 0xe2555001u);     // subs r5, r5, #1
    put(code, 0x040, 0x1afffffau);     // bne inner
    put(code, 0x044, 0xe12fff3cu);     // blx r12               <- one host service call
    put(code, 0x048, 0xeafffff6u);     // b outer
    movAddress(0x100, 8, WalkBase);    // r8 = walk base
    put(code, 0x108, 0xe3a04000u);     // mov r4, #0
    put(code, 0x10c, 0xe1a06008u);     // mov r6, r8            <- outer loop
    put(code, 0x110, 0xe30053e8u);     // movw r5, #1000
    put(code, 0x114, 0xe4963004u);     // ldr r3, [r6], #4      <- inner loop
    put(code, 0x118, 0xe0844003u);     // add r4, r4, r3
    put(code, 0x11c, 0xe5064008u);     // str r4, [r6, #-8]
    put(code, 0x120, 0xe2555001u);     // subs r5, r5, #1
    put(code, 0x124, 0x1afffffau);     // bne inner
    put(code, 0x128, 0xeafffff7u);     // b outer
    return code;
}

int runProbe(const char* name, uint32_t entry, double seconds, uint64_t slice) {
    Memory memory(size_t(64) * 1024 * 1024);
    Services services(memory);
    const std::vector<uint8_t> code = buildPrograms(services.address("_memcpy"));
    std::vector<uint8_t> data(DataSize, 0);
    memory.map({CodeBase, 4096, uint8_t(Read | Execute), "perf probe instructions"}, code.data(),
               code.size());
    memory.map({DataBase, DataSize, uint8_t(Read | Write), "perf probe data"}, data.data(),
               data.size());
    Cpu cpu(memory);
    CpuState state;
    state.cpsr = 0x10;
    state.registers[13] = DataBase + DataSize - 16;
    state.registers[15] = entry;
    cpu.setState(state);

    uint64_t instructions = 0, serviceCalls = 0;
    double hostNanoseconds = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [](std::chrono::steady_clock::time_point begin) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    };
    while (elapsed(started) < seconds) {
        CpuStop stop;
        try {
            stop = cpu.run(slice);
        } catch (const Error& e) {
            std::cerr << "perf-probe: " << e.code << ": " << e.what() << '\n';
            return 1;
        }
        instructions += stop.attemptedInstructions;
        if (stop.reason == StopReason::Fault) {
            std::cerr << "perf-probe: guest fault " << stop.code << ": " << stop.detail << '\n';
            return 1;
        }
        if (stop.reason != StopReason::SupervisorCall) continue;
        serviceCalls++;
        // Timed separately from guest execution: the per-call host cost is what a title that
        // makes many small framework calls spends its budget on, and it is worth naming. The
        // two clock reads add a little under a microsecond of their own, which is why the
        // figure is reported per call rather than used as a threshold.
        const auto callStarted = std::chrono::steady_clock::now();
        CpuState current = cpu.state();
        try {
            services.dispatch(stop, current);
        } catch (const Error& e) {
            std::cerr << "perf-probe: service stop " << e.code << ": " << e.what() << '\n';
            return 1;
        }
        cpu.setState(current);
        hostNanoseconds +=
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - callStarted)
                .count();
    }
    const double wall = elapsed(started);
    const CpuCounters counters = cpu.counters();
    std::printf(
        "%-8s %15llu instructions in %6.3f s = %9.1f MIPS, %9llu host call(s) at %8.0f ns "
        "each (%4.1f%% of wall), %s%s, "
        "%llu full cache clear(s), %llu invalidated range(s), %llu checked read(s), "
        "%llu checked write(s)\n",
        name, static_cast<unsigned long long>(instructions), wall,
        wall > 0 ? double(instructions) / wall / 1e6 : 0.0,
        static_cast<unsigned long long>(serviceCalls),
        serviceCalls ? hostNanoseconds / double(serviceCalls) : 0.0,
        wall > 0 ? 100.0 * (hostNanoseconds / 1e9) / wall : 0.0,
        counters.directMemory ? (counters.fastMemory ? "direct guest memory (fastmem)"
                                                     : "direct guest memory (page table)")
                              : "checked-callback guest memory",
        counters.exclusiveMonitor ? ", exclusive monitor" : "",
        static_cast<unsigned long long>(counters.fullCacheClears),
        static_cast<unsigned long long>(counters.invalidatedRanges),
        static_cast<unsigned long long>(counters.callbackReads),
        static_cast<unsigned long long>(counters.callbackWrites));
    // Structural, not timing: these hold on any host, and a run that discards its code cache
    // on every host call is exactly the regression this probe exists to catch.
    if (counters.fullCacheClears != 0) {
        std::cerr << "perf-probe: the engine discarded its whole code cache "
                  << counters.fullCacheClears << " time(s)\n";
        return 1;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (!counters.directMemory) {
        std::cerr << "perf-probe: the host refused a flat guest-memory reservation, so guest "
                     "loads and stores stayed on the checked callback path\n";
        return 1;
    }
#endif
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    double seconds = 1.0;
    uint64_t slice = 20000000;
    std::string mode = "both";
    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (argument == "--slice" && i + 1 < argc) {
            slice = std::strtoull(argv[++i], nullptr, 10);
        } else if (argument == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else {
            std::cerr << "usage: perf-probe [--seconds N] [--slice N] "
                         "[--mode guest|service|both]\n";
            return 2;
        }
    }
    if (seconds <= 0 || slice == 0) {
        std::cerr << "perf-probe: --seconds and --slice must be positive\n";
        return 2;
    }
    if (mode != "guest" && mode != "service" && mode != "both") {
        std::cerr << "perf-probe: --mode must be guest, service or both\n";
        return 2;
    }
    if (mode != "service" && runProbe("guest", GuestEntry, seconds, slice)) return 1;
    if (mode != "guest" && runProbe("service", ServiceEntry, seconds, slice)) return 1;
    return 0;
}
