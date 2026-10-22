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

// Coverage audit over everything that must bind before guest code may run: pointer
// fixups and classic symbol stubs. Stub symbols are reported too, because a stub that
// forwards to an unimplemented API is exactly as fatal as an unresolvable pointer.
struct Coverage {
    size_t required = 0, weak = 0;
    std::set<std::string> missingRequired, missingWeak;
    std::string describe(size_t implemented) const {
        return std::to_string(implemented) + " implemented APIs; " + std::to_string(required) +
               " required and " + std::to_string(weak) +
               " weak imported symbols still lack implementations.";
    }
};

Coverage audit(const LoadedImage& image, const ExportGraph& graph, size_t main) {
    Coverage coverage;
    auto check = [&](int ordinal, const std::string& symbol, bool weak) {
        if (graph.resolve(main, ordinal, symbol)) return;
        (weak ? coverage.missingWeak : coverage.missingRequired).insert(symbol);
    };
    for (const auto& fixup : image.dyld.fixups) {
        if (fixup.rebase) continue;
        check(fixup.ordinal, fixup.symbol, fixup.weak);
    }
    for (const auto& stub : image.stubs) check(stub.ordinal, stub.symbol, stub.weak);
    coverage.required = coverage.missingRequired.size();
    coverage.weak = coverage.missingWeak.size();
    return coverage;
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
        // The image's own Objective-C classes are registered before the first message is
        // sent, so a title's delegate and view classes are the objects its own code finds.
        services.registerGuestClasses(image.objcClasses);
        ExportGraph graph;
        services.install(graph, image.dependencies);
        auto main = graph.addImage("<executable>");
        graph.setMainExecutable(main);
        graph.dependencies(main, image.dependencies);
        for (const auto& [name, definition] : image.exports) graph.define(main, name, definition);

        const Coverage coverage = audit(image, graph, main);
        const std::string report = coverage.describe(services.count());
        if (!image.requiresDyld || (image.dyld.fixups.empty() && image.stubs.empty())) {
            return "LAUNCH_BLOCKED: GUEST_ABI_UNIMPLEMENTED: the image needs no dyld work, but "
                   "Darwin process bootstrap is not implemented.\nDYLD_EXPORT_AUDIT: " +
                   report + "\n" + image.summary();
        }
        // Redirect every classic symbol stub before the first instruction: to the real
        // implementation when one exists, otherwise to a trap gate that names the symbol.
        if (!image.stubs.empty())
            bindLegacyStubs(image, graph, main,
                            [&](const std::string& name) { return services.trapFor(name); });
#ifdef PODNATIVE_CPU_IN_APP
        prepareStack(image, *image.memory);
        GuestRun run;
        try {
            run = runGuest(image, services, graph, main, InstructionBudget, Seconds);
            services.closeGuestFiles();
            services.drainGuestPools();
        } catch (const Error& e) {
            return "LAUNCH_BLOCKED: GUEST_SETUP: " + std::string(e.what()) +
                   "\nNo guest instruction retired.\nDYLD_EXPORT_AUDIT: " + report + "\n" +
                   image.summary();
        }
        const std::string header =
            run.stop == "UNIMPLEMENTED" || run.stop == "TRAPPED"
                ? "LAUNCH_BLOCKED: HLE_GAP: the guest started and stopped at an API that is not "
                  "implemented yet."
                : "LAUNCH_ATTEMPT: the guest image actually executed.";
        return header + "\n" + describeRun(run) + "\nDYLD_EXPORT_AUDIT: " + report + "\n" +
               services.uiSummary() + "\n" + services.graphicsSummary() + "\n" +
               services.guestClassSummary() + "\nGUEST_STORAGE: " + services.storageSummary() +
               "\n" + image.summary() +
               "\nThis is bounded execution of a validated image, not evidence of gameplay, "
               "rendering or a working app.";
#else
        return "LAUNCH_BLOCKED: CPU_NOT_INTEGRATED: the tested CPU engine is not linked into "
               "this build.\nDYLD_EXPORT_AUDIT: " +
               report + "\n" + image.summary();
#endif
    } catch (const Error& e) {
        return "LAUNCH_BLOCKED: " + std::string(e.what()) +
               "\nGuest code was not executed. Original game files are unchanged.";
    }
}
}  // namespace podnative::runtime
