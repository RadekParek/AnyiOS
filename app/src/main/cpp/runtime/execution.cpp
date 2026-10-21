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
GuestRun runGuest(LoadedImage& image, Services& services, const ExportGraph& graph,
                  size_t requester, uint64_t instructionBudget, double seconds) {
    GuestRun run;
    if (!image.memory) throw Error("GUEST_MEMORY", "image has no mapped memory");
    // Unimplemented required imports bind to a faulting gate, so the guest starts and
    // stops at the first missing API rather than being refused up front. Weak imports
    // keep C semantics and stay null when nothing defines them.
    image.dyld.applyWithGraph(*image.memory, 0, graph, requester,
                              [&](const std::string& name) { return services.trapFor(name); });
    run.trappedImports = services.trapCount();

    Cpu cpu(*image.memory);
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
        run.stop = "BUDGET";
        run.detail = "instruction budget or wall-clock slice reached";
        return run;
    }
    run.stop = "BUDGET";
    run.detail = "instruction budget reached";
    return run;
}
std::string describeRun(const GuestRun& run) {
    std::ostringstream out;
    out << "GUEST_EXECUTION: " << run.instructions << " instructions, final PC=" << pointer(run.pc)
        << ", " << run.serviceCalls << " host service calls, " << run.trappedImports
        << " unimplemented imports trapped.";
    out << "\nGUEST_STOP: " << run.stop;
    if (!run.detail.empty()) out << " — " << run.detail;
    if (!run.called.empty()) {
        out << "\nHOST_CALLS: ";
        for (size_t n = 0; n < run.called.size(); n++) out << (n ? " " : "") << run.called[n];
    }
    return out.str();
}
}  // namespace podnative::runtime
