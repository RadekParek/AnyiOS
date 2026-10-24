#include "execution.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "cpu.h"
#include "error.h"

namespace podnative::runtime {
namespace {
constexpr size_t MaxRememberedCalls = 24;
constexpr size_t MaxFrames = 16;
// ARM frames are {previous fp, return address} pairs. The chain is only followed while it
// stays inside mapped, readable guest memory and moves upwards, so a corrupt frame cannot
// turn a stop report into a crash.
std::vector<uint32_t> frames(const Memory& memory, const CpuState& state) {
    std::vector<uint32_t> addresses;
    uint32_t frame = state.registers[11], last = 0;
    for (size_t depth = 0; depth < MaxFrames; depth++) {
        if (!frame || frame <= last || (frame & 3) || !memory.permits(frame, 8, Read)) break;
        last = frame;
        const uint32_t previous = memory.read32(frame), caller = memory.read32(frame + 4);
        if (caller && memory.permits(caller & ~1u, 2, Execute)) addresses.push_back(caller & ~1u);
        frame = previous;
    }
    return addresses;
}
// The calls that matter when a run stops are the last ones, not the first: the first dozen
// are always startup boilerplate. A ring keeps the tail without letting a long run grow it.
void remember(GuestRun& run, const std::string& name) {
    if (run.called.size() < MaxRememberedCalls) {
        run.called.push_back(name);
        return;
    }
    run.called.erase(run.called.begin());
    run.called.push_back(name);
}
std::string pointer(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
std::vector<uint32_t> initializerFunctions(const LoadedImage& image) {
    if (!image.memory) throw Error("GUEST_MEMORY", "image has no mapped memory");
    std::vector<uint32_t> functions;
    for (const auto& [address, count] : image.initializerSections) {
        if (count > 100000 - functions.size())
            throw Error("GUEST_INITIALIZER_LIMIT", "too many static initializer functions",
                        address);
        const uint64_t bytes = uint64_t(count) * 4;
        if (!count || !image.memory->permits(address, size_t(bytes), Read)) {
            if (!count) continue;
            throw Error("GUEST_INITIALIZER", "initializer pointer section is unreadable", address);
        }
        for (uint32_t index = 0; index < count; index++) {
            const uint32_t function = image.memory->read32(address + index * 4);
            if (!function) continue;  // A null slot is harmless padding in some linkers.
            const uint32_t code = function & ~1u;
            if (!image.memory->permits(code, function & 1 ? 2 : 4, Execute))
                throw Error("GUEST_INITIALIZER",
                            "static initializer does not point to executable guest code", function);
            functions.push_back(function);
        }
    }
    return functions;
}
uint32_t installInitializerTrampoline(Memory& memory, const std::vector<uint32_t>& functions,
                                      uint32_t entry) {
    if (functions.empty()) return 0;
    // This small ARM wrapper calls each dyld initializer through BLX, then branches to the
    // original entry point. It preserves the process-entry registers and uses a dedicated
    // RX page in the unused runtime-gate window; no writable-executable mapping is needed.
    const size_t codeBytes = 20 + 16 * functions.size();
    const uint64_t mappedBytes =
        (uint64_t(codeBytes) + Memory::PageSize - 1) & ~(uint64_t(Memory::PageSize) - 1);
    if (mappedBytes > std::numeric_limits<uint32_t>::max())
        throw Error("GUEST_INITIALIZER_LIMIT", "initializer trampoline is too large");
    std::vector<uint8_t> code(codeBytes, 0);
    auto put = [&](size_t offset, uint32_t word) {
        for (unsigned byte = 0; byte < 4; byte++) code[offset + byte] = uint8_t(word >> (byte * 8));
    };
    put(0, 0xe92d500f);  // push {r0-r3, r12, lr}
    size_t offset = 4;
    for (uint32_t function : functions) {
        put(offset, 0xe59fc004);      // ldr r12, [pc, #4] -> literal at offset + 12
        put(offset + 4, 0xe12fff3c);  // blx r12, including ARM/Thumb interworking
        put(offset + 8, 0xea000000);  // b +0, skip the function pointer literal
        put(offset + 12, function);
        offset += 16;
    }
    put(offset, 0xe8bd500f);      // pop {r0-r3, r12, lr}
    put(offset + 4, 0xe59fc000);  // ldr r12, [pc, #0] -> the original entry address
    put(offset + 8, 0xe12fff1c);  // bx r12, restoring the entry's instruction set
    put(offset + 12, entry);

    constexpr uint32_t First = 0x6f003000;
    constexpr uint32_t Limit = 0x6f100000;  // the unimplemented-import trap pages start here
    const uint32_t regionSize = uint32_t(mappedBytes);
    for (uint64_t candidate = First; candidate + regionSize <= Limit;
         candidate += Memory::PageSize) {
        bool overlaps = false;
        for (const Region& region : memory.regions()) {
            if (candidate < uint64_t(region.base) + region.size &&
                region.base < candidate + regionSize) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;
        const uint32_t base = uint32_t(candidate);
        memory.map({base, regionSize, uint8_t(Read | Execute), "Mach-O initializer trampoline"},
                   code.data(), code.size());
        return base;
    }
    throw Error("GUEST_INITIALIZER", "no free runtime address for the initializer trampoline");
}
}  // namespace
namespace {
GuestRun execute(LoadedImage& image, Services& services, const ExportGraph& graph, size_t requester,
                 uint64_t instructionBudget, double seconds,
                 const std::atomic<bool>* cancelRequested) {
    GuestRun run;
    if (!image.memory) throw Error("GUEST_MEMORY", "image has no mapped memory");
    // Unimplemented required imports bind to a faulting gate, so the guest starts and
    // stops at the first missing API rather than being refused up front. Weak imports
    // keep C semantics and stay null when nothing defines them.
    image.dyld.applyWithGraph(*image.memory, 0, graph, requester,
                              [&](const std::string& name) { return services.trapFor(name); });
    run.trappedImports = services.trapCount();
    const std::vector<uint32_t> initializers = initializerFunctions(image);
    const uint32_t entry = image.registers[15] | ((image.cpsr & 0x20) ? 1u : 0u);
    const uint32_t initializerTrampoline =
        installInitializerTrampoline(*image.memory, initializers, entry);

    Cpu cpu(*image.memory, image.arch);
    // Every stop path returns `run`, so the engine's counters are published by scope exit
    // rather than at each of the dozen places a run can end.
    struct EngineCounters {
        Cpu& cpu;
        GuestRun& run;
        ~EngineCounters() { run.engine = cpu.counters(); }
    } engineCounters{cpu, run};
    // A precise trace asks the CPU for single-stepping: a trace whose PCs are block entries
    // would not be a trace of what the guest did. It is its own switch because it costs about
    // ten times the execution rate.
    cpu.setPrecise(services.isPrecise());
    CpuState state;
    state.registers = image.registers;
    state.cpsr = image.cpsr;
    if (initializerTrampoline) {
        state.registers[15] = initializerTrampoline;
        state.cpsr &= ~0x20u;  // The wrapper is ARM code; its final BX selects the entry mode.
    }
    cpu.setState(state);
    services.adoptMainThread(state);

    // No budget means no budget: the run continues until the guest itself stops. The slice
    // size only decides how often the loop can observe a host-side condition; it is not a
    // cap on the run.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(uint64_t(seconds * 1000));
    constexpr uint64_t Slice = 200000;
    // One sample per few seconds of host time keeps a long run observable without turning
    // the report into a trace.
    constexpr size_t MaxSamples = 120;
    auto lastSample = std::chrono::steady_clock::now();
    while (instructionBudget == 0 || run.instructions < instructionBudget) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
            run.stop = "CANCELLED";
            run.detail = "the user cancelled the guest launch";
            return run;
        }
        if (seconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            run.stop = "BUDGET";
            run.detail = "the caller's " + std::to_string(seconds) + " s probe budget was reached";
            return run;
        }
        const uint64_t slice =
            instructionBudget ? std::min(Slice, instructionBudget - run.instructions) : Slice;
        CpuStop stop = cpu.run(slice);
        run.instructions += stop.attemptedInstructions;
        run.pc = stop.pc;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSample >= std::chrono::seconds(3)) {
            lastSample = now;
            if (run.samples.size() >= MaxSamples) run.samples.erase(run.samples.begin());
            run.samples.push_back({run.instructions, stop.pc, services.lastCall()});
        }
        if (stop.reason == StopReason::Fault) {
            const CpuState faulted = cpu.faultState();
            if (stop.pc == 0 && faulted.registers[14] == 0) {
                run.stop = "RETURNED";
                run.detail =
                    "guest returned from its entry point to address zero without "
                    "calling _exit, _Exit or exit";
                run.frames = frames(*image.memory, faulted);
                return run;
            }
            // The register state at the fault is what makes a guest fault actionable: it says
            // whether the guest branched to a value it read from memory, returned through a
            // corrupted frame, or was handed a bad pointer by a service.
            std::ostringstream registers;
            registers << " (" << std::hex << std::setfill('0');
            for (size_t index = 0; index <= 12; index++) {
                if (index) registers << ' ';
                registers << 'r' << std::dec << index << "=0x" << std::hex << std::setw(8)
                          << faulted.registers[index];
            }
            registers << " sp=0x" << std::setw(8) << faulted.registers[13] << " lr=0x"
                      << std::setw(8) << faulted.registers[14] << " cpsr=0x" << std::setw(8)
                      << faulted.cpsr << ')';
            run.stop = "FAULT";
            run.detail =
                stop.code + ": " + stop.detail + " at " + pointer(stop.pc) + registers.str();
            // The last instructions before the fault: a jump to a value read out of memory is
            // only explainable by the code that produced it.
            std::ostringstream trace;
            bool first = true;
            for (uint32_t pc : cpu.recentPcs(48)) {
                trace << (first ? "" : " ") << pointer(pc);
                first = false;
            }
            if (!first) run.detail += "\nrecent PCs (oldest first): " + trace.str();
            // Which mapping the PC landed in, and with what permissions: "unmapped" and
            // "mapped but not executable" are different bugs.
            std::string placement = "unmapped";
            for (const Region& region : image.memory->regions()) {
                if (stop.pc < region.base || stop.pc - region.base >= region.size) continue;
                placement = region.label + " [" +
                            std::string(region.permissions & Execute ? "rx"
                                        : region.permissions & Write ? "rw"
                                                                     : "r-") +
                            ", " + std::to_string(region.size) + " bytes]";
                break;
            }
            run.detail += "\nfaulting PC is in: " + placement;
            // The stack the faulting instruction stands on: a corrupted return address is
            // visible here even when the branch source is already off the trace.
            std::ostringstream stack;
            for (size_t n = 0; n < 12; n++) {
                const uint32_t at = faulted.registers[13] + uint32_t(n * 4);
                if (!image.memory->permits(at, 4, Read)) break;
                char cell[16];
                std::snprintf(cell, sizeof(cell), " %08x", image.memory->read32(at));
                stack << cell;
            }
            if (!stack.str().empty()) run.detail += "\nstack from SP:" + stack.str();
            return run;
        }
        if (stop.reason == StopReason::SupervisorCall) {
            run.serviceCalls++;
            CpuState current = cpu.state();
            try {
                services.dispatch(stop, current);
            } catch (const Error& e) {
                run.stop = e.code == "HLE_UNIMPLEMENTED" ? "UNIMPLEMENTED" : "TRAPPED";
                run.detail = e.what();
                run.pc = stop.pc;
                run.frames = frames(*image.memory, current);
                return run;
            }
            remember(run, services.lastCall());
            const uint32_t next = current.registers[15];
            if (!next) {
                if (services.guestExitRequested()) {
                    run.stop = "EXITED";
                    run.detail = "guest terminated via " + services.guestExitRoutine() +
                                 " with status " + std::to_string(services.guestExitStatus());
                } else {
                    run.stop = "RETURNED";
                    run.detail =
                        "guest service returned control to address zero without an "
                        "explicit exit routine";
                }
                return run;
            }
            cpu.setState(current);
            // A blocking call (a mutex, a join) or an explicit yield asks for another guest
            // thread to run now. Nothing is switched until the guest asked for it, so a
            // single-threaded title sees exactly the same execution as before.
            if (services.threadSwitchWanted()) {
                CpuState next = current;
                if (!services.switchGuestThread(next)) {
                    run.stop = "TRAPPED";
                    run.detail = services.guestThreadStall();
                    run.pc = current.registers[15];
                    return run;
                }
                cpu.setState(next);
            }
            continue;
        }
        // The end of a quantum is a scheduling point when more than one guest thread exists:
        // the slice boundary is this runtime's preemption tick.
        if (services.guestThreadCount() > 1) {
            CpuState slice = cpu.state();
            if (!services.switchGuestThread(slice) && services.guestThreadsFinished()) {
                run.stop = "EXITED";
                run.detail = services.guestThreadStall();
                return run;
            }
            cpu.setState(slice);
        }
        // A slice that simply ran out of instruction budget is not the end of the run:
        // the loop continues until the caller's total budget or the wall clock stops it.
        // Returning here used to cut every guest off at the 200,000-instruction slice.
        if (stop.reason != StopReason::Budget) {
            run.stop = "BUDGET";
            run.detail = "the CPU slice ended without a reason the run loop understands";
            return run;
        }
    }
    run.stop = "BUDGET";
    run.detail = "instruction budget reached";
    return run;
}
}  // namespace
GuestRun runGuest(LoadedImage& image, Services& services, const ExportGraph& graph,
                  size_t requester, uint64_t instructionBudget, double seconds,
                  const std::atomic<bool>* cancelRequested) {
    GuestRun run =
        execute(image, services, graph, requester, instructionBudget, seconds, cancelRequested);
    run.pcSymbol = image.nearestSymbol(run.pc);
    for (uint32_t address : run.frames) {
        const std::string symbol = image.nearestSymbol(address);
        run.backtrace.push_back(symbol.empty() ? pointer(address) : symbol + "()");
    }
    return run;
}
namespace {
// Classifies a stopped run into the three things a crash screen must state: what kind
// of stop it was, what exactly is missing or wrong, and the guest PC.
void classify(GuestRun& run) {
    const std::string& detail = run.detail;
    if (run.stop == "UNIMPLEMENTED") {
        run.blockerKind = "Unimplemented API";
        const std::string marker = "guest called unimplemented import ";
        auto at = detail.find(marker);
        run.blockerSubject = at == std::string::npos ? detail : detail.substr(at + marker.size());
        return;
    }
    if (run.stop == "TRAPPED") {
        run.blockerKind = "Runtime trap";
        auto at = detail.find("HLE_");
        run.blockerSubject = at == std::string::npos ? detail : detail.substr(at);
        return;
    }
    if (run.stop == "FAULT") {
        run.blockerKind = "Guest fault";
        auto colon = detail.find(':');
        run.blockerSubject = colon == std::string::npos ? detail : detail.substr(0, colon);
        return;
    }
    if (run.stop == "EXITED") {
        run.blockerKind = "Guest exited";
        run.blockerSubject = detail.empty() ? "the guest called an exit routine" : detail;
        return;
    }
    if (run.stop == "RETURNED") {
        run.blockerKind = "Unexpected entry-point return";
        run.blockerSubject =
            detail.empty() ? "the guest returned without calling an exit routine" : detail;
        return;
    }
    if (run.stop == "CANCELLED") {
        run.blockerKind = "Cancelled";
        run.blockerSubject = detail.empty() ? "the user cancelled the guest launch" : detail;
        return;
    }
    run.blockerKind = "Time slice";
    run.blockerSubject =
        detail.empty() ? "the bounded slice ended before the app finished starting" : detail;
}
}  // namespace
std::string describeRun(GuestRun& run, bool includeRecentCalls) {
    if (run.blockerKind.empty()) classify(run);
    std::ostringstream out;
    out << "GUEST_EXECUTION: " << run.instructions << " instructions, final PC=" << pointer(run.pc)
        << ", " << run.serviceCalls << " host service calls, " << run.trappedImports
        << " unimplemented imports trapped.";
    out << "\nGUEST_STOP: " << run.stop;
    if (!run.detail.empty()) out << " — " << run.detail;
    out << "\nGUEST_BLOCKER: kind=" << run.blockerKind << " subject=" << run.blockerSubject
        << " pc=" << pointer(run.pc) << " instructions=" << run.instructions;
    // Engine evidence: how guest memory was reached and how much translated code survived.
    // A run that discards its code cache per host call is a run that spends its time
    // recompiling, which is exactly what this line makes visible.
    out << "\nCPU_ENGINE: guest memory "
        << (run.engine.directMemory
                ? (run.engine.fastMemory ? "direct (flat arena, hardware fault recovery)"
                                         : "direct (flat arena, checked page table)")
                : "checked callbacks")
        << ", exclusive monitor " << (run.engine.exclusiveMonitor ? "on" : "off") << ", "
        << run.engine.fullCacheClears << " full code-cache clears, " << run.engine.invalidatedRanges
        << " invalidated code range(s), " << run.engine.exclusiveStores << " exclusive store(s), "
        << run.engine.hintInstructions << " hint instruction(s) of which "
        << run.engine.waitInstructions << " wait(s), " << run.engine.callbackReads
        << " checked read(s) and " << run.engine.callbackWrites << " checked write(s)";
    // "pc=0x47054" alone does not tell anyone what the guest was doing; the nearest
    // preceding function symbol does.
    if (!run.pcSymbol.empty()) out << " symbol=" << run.pcSymbol;
    if (!run.backtrace.empty()) {
        out << "\nGUEST_BACKTRACE: ";
        for (size_t n = 0; n < run.backtrace.size(); n++)
            out << (n ? " <- " : "") << run.backtrace[n];
    }
    if (run.samples.size() > 1) {
        out << "\nGUEST_PROGRESS: " << run.samples.size() << " sample(s), last 12:";
        const size_t first = run.samples.size() > 12 ? run.samples.size() - 12 : 0;
        for (size_t n = first; n < run.samples.size(); n++)
            out << "\n  " << run.samples[n].instructions << " instructions at "
                << pointer(run.samples[n].pc)
                << (run.samples[n].lastCall.empty() ? "" : " after " + run.samples[n].lastCall);
    }
    if (includeRecentCalls && !run.called.empty()) {
        std::vector<std::pair<std::string, size_t>> calls;
        for (const std::string& name : run.called) {
            const auto found = std::find_if(calls.begin(), calls.end(),
                                            [&](const auto& entry) { return entry.first == name; });
            if (found == calls.end())
                calls.emplace_back(name, 1);
            else
                ++found->second;
        }
        out << "\nHOST_CALLS: last " << run.called.size() << " call(s), " << calls.size()
            << " distinct service name(s):";
        for (const auto& [name, count] : calls) {
            out << " " << name;
            if (count > 1) out << " (" << count << "x)";
        }
    }
    return out.str();
}
}  // namespace podnative::runtime
