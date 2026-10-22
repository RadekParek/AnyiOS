#include "execution.h"

#include <chrono>
#include <sstream>

#include "cpu.h"
#include "error.h"

namespace podnative::runtime {
namespace {
constexpr size_t MaxRememberedCalls = 12;
void remember(GuestRun& run, const std::string& name) {
    if (run.called.size() < MaxRememberedCalls) {
        run.called.push_back(name);
    } else if (run.called.size() == MaxRememberedCalls) {
        run.called.push_back("…");
    }
}
std::string pointer(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
}  // namespace
namespace {
GuestRun execute(LoadedImage& image, Services& services, const ExportGraph& graph, size_t requester,
                 uint64_t instructionBudget, double seconds) {
    GuestRun run;
    if (!image.memory) throw Error("GUEST_MEMORY", "image has no mapped memory");
    // Unimplemented required imports bind to a faulting gate, so the guest starts and
    // stops at the first missing API rather than being refused up front. Weak imports
    // keep C semantics and stay null when nothing defines them.
    image.dyld.applyWithGraph(*image.memory, 0, graph, requester,
                              [&](const std::string& name) { return services.trapFor(name); });
    run.trappedImports = services.trapCount();

    Cpu cpu(*image.memory, image.arch);
    CpuState state;
    state.registers = image.registers;
    state.cpsr = image.cpsr;
    cpu.setState(state);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(uint64_t(seconds * 1000));
    constexpr uint64_t Slice = 200000;
    while (run.instructions < instructionBudget) {
        if (std::chrono::steady_clock::now() >= deadline) {
            run.stop = "BUDGET";
            run.detail = "wall-clock slice reached";
            return run;
        }
        const uint64_t slice = std::min(Slice, instructionBudget - run.instructions);
        CpuStop stop = cpu.run(slice);
        run.instructions += stop.attemptedInstructions;
        run.pc = stop.pc;
        if (stop.reason == StopReason::Fault) {
            run.stop = "FAULT";
            run.detail = stop.code + ": " + stop.detail + " at " + pointer(stop.pc);
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
                return run;
            }
            remember(run, stop.code);
            const uint32_t next = current.registers[15];
            if (!next) {
                run.stop = "EXITED";
                run.detail = "guest returned from its entry point";
                return run;
            }
            cpu.setState(current);
            continue;
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
                  size_t requester, uint64_t instructionBudget, double seconds) {
    GuestRun run = execute(image, services, graph, requester, instructionBudget, seconds);
    run.pcSymbol = image.nearestSymbol(run.pc);
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
        run.blockerSubject = detail.empty() ? "the guest returned from its entry point" : detail;
        return;
    }
    run.blockerKind = "Time slice";
    run.blockerSubject =
        detail.empty() ? "the bounded slice ended before the app finished starting" : detail;
}
}  // namespace
std::string describeRun(GuestRun& run) {
    if (run.blockerKind.empty()) classify(run);
    std::ostringstream out;
    out << "GUEST_EXECUTION: " << run.instructions << " instructions, final PC=" << pointer(run.pc)
        << ", " << run.serviceCalls << " host service calls, " << run.trappedImports
        << " unimplemented imports trapped.";
    out << "\nGUEST_STOP: " << run.stop;
    if (!run.detail.empty()) out << " — " << run.detail;
    out << "\nGUEST_BLOCKER: kind=" << run.blockerKind << " subject=" << run.blockerSubject
        << " pc=" << pointer(run.pc) << " instructions=" << run.instructions;
    // "pc=0x47054" alone does not tell anyone what the guest was doing; the nearest
    // preceding function symbol does.
    if (!run.pcSymbol.empty()) out << " symbol=" << run.pcSymbol;
    if (!run.called.empty()) {
        out << "\nHOST_CALLS: ";
        for (size_t n = 0; n < run.called.size(); n++) out << (n ? " " : "") << run.called[n];
    }
    return out.str();
}
}  // namespace podnative::runtime
