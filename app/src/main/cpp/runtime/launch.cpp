#include "launch.h"

#include <set>

#include "error.h"
#ifdef PODNATIVE_CPU_IN_APP
#include "execution.h"
#endif
#include "exports.h"
#include "loader.h"
#include "services.h"

namespace podnative::runtime {
namespace {
constexpr uint64_t InstructionBudget = 2000000;
constexpr double Seconds = 2.5;

std::string audit(const LoadedImage& image, Services& services) {
    ExportGraph graph;
    services.install(graph);
    auto main = graph.addImage("<executable>");
    graph.setMainExecutable(main);
    graph.dependencies(main, image.dependencies);
    for (const auto& [name, definition] : image.exports) graph.define(main, name, definition);

    std::set<std::string> missingRequired, missingWeak;
    for (const auto& fixup : image.dyld.fixups) {
        if (fixup.rebase) continue;
        if (graph.resolve(main, fixup.ordinal, fixup.symbol)) continue;
        (fixup.weak ? missingWeak : missingRequired).insert(fixup.symbol);
    }
    return std::to_string(services.count()) + " implemented APIs; " +
           std::to_string(missingRequired.size()) + " required and " +
           std::to_string(missingWeak.size()) + " weak imported symbols still " +
           "lack implementations.";
}

#ifdef PODNATIVE_CPU_IN_APP
// Real guest stack: argc/argv/envp plus a null-terminated environment block.
void prepareStack(LoadedImage& image, Memory& memory) {
    constexpr uint32_t StackBase = 0x70000000;
    constexpr uint32_t StackSize = 1024 * 1024;
    const uint32_t top = StackBase + StackSize - 64;
    const char argv0[] = "AnyiOS";
    const char env0[] = "HOME=/var/mobile";
    memory.write(top - 32, argv0, sizeof(argv0));
    memory.write(top - 64, env0, sizeof(env0));
    const uint32_t argv = top - 96;
    memory.write32(argv, top - 32);
    memory.write32(argv + 4, 0);
    const uint32_t envp = argv - 8;
    memory.write32(envp, top - 64);
    memory.write32(envp + 4, 0);
    image.registers[0] = 1;
    image.registers[1] = argv;
    image.registers[2] = envp;
    image.registers[13] = envp - 8;
    image.registers[14] = 0;
}
#endif
}  // namespace

std::string attemptLaunch(const uint8_t* data, size_t size, const std::string& bundleHost,
                          const std::string& containerHost) {
    try {
        auto image = loadMachO(data, size, 64 * 1024 * 1024);
        Services services(*image.memory);
        services.setEnvironment({{"HOME", "/var/mobile"},
                                 {"TMPDIR", "/var/mobile/Documents"},
                                 {"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"}});
        if (!bundleHost.empty() && !containerHost.empty())
            services.mountSandbox(bundleHost, containerHost);
        ExportGraph graph;
        services.install(graph);
        auto main = graph.addImage("<executable>");
        graph.setMainExecutable(main);
        graph.dependencies(main, image.dependencies);
        for (const auto& [name, definition] : image.exports) graph.define(main, name, definition);

        const std::string coverage = audit(image, services);
        if (!image.requiresDyld || image.dyld.fixups.empty()) {
            return "LAUNCH_BLOCKED: GUEST_ABI_UNIMPLEMENTED: the image needs no dyld work, but "
                   "Darwin process bootstrap is not implemented.\nDYLD_EXPORT_AUDIT: " +
                   coverage + "\n" + image.summary();
        }
#ifdef PODNATIVE_CPU_IN_APP
        prepareStack(image, *image.memory);
        GuestRun run;
        try {
            run = runGuest(image, services, graph, main, InstructionBudget, Seconds);
            services.closeGuestFiles();
        } catch (const Error& e) {
            return "LAUNCH_BLOCKED: GUEST_SETUP: " + std::string(e.what()) +
                   "\nNo guest instruction retired.\nDYLD_EXPORT_AUDIT: " + coverage + "\n" +
                   image.summary();
        }
        const std::string header =
            run.stop == "UNIMPLEMENTED" || run.stop == "TRAPPED"
                ? "LAUNCH_BLOCKED: HLE_GAP: the guest started and stopped at an API that is not "
                  "implemented yet."
                : "LAUNCH_ATTEMPT: the guest image actually executed.";
        return header + "\n" + describeRun(run) + "\nDYLD_EXPORT_AUDIT: " + coverage +
               "\nGUEST_STORAGE: " + services.storageSummary() + "\n" + image.summary() +
               "\nThis is bounded execution of a validated image, not evidence of gameplay, "
               "rendering or a working app.";
#else
        return "LAUNCH_BLOCKED: CPU_NOT_INTEGRATED: the tested CPU engine is not linked into "
               "this build.\nDYLD_EXPORT_AUDIT: " +
               coverage + "\n" + image.summary();
#endif
    } catch (const Error& e) {
        return "LAUNCH_BLOCKED: " + std::string(e.what()) +
               "\nGuest code was not executed. Original game files are unchanged.";
    }
}
}  // namespace podnative::runtime
