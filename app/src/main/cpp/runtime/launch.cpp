#include "launch.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#include "error.h"
#ifdef PODNATIVE_CPU_IN_APP
#include "execution.h"
#endif
#include "exports.h"
#include "loader.h"
#include "services.h"

namespace podnative::runtime {
namespace {
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
                          const std::string& containerHost, const LaunchOptions& options) {
    if (options.cancelRequested && options.cancelRequested->load(std::memory_order_relaxed))
        return "LAUNCH_CANCELLED: the user cancelled before the image could be loaded.\n"
               "Guest code was not executed.";
    try {
        auto image =  // The guest's address-space budget covers the image, the framework pages this
                      // runtime
            // maps and the stacks of every thread the title creates. A title that starts its
            // audio and loader threads needs more than the image alone, so the budget is the
            // whole 32-bit user space the runtime is willing to own, not a startup probe.
            loadMachO(data, size, size_t(2048) * 1024 * 1024);
        Services services(*image.memory);
        // The guest's own console output is the most direct evidence of what it is doing,
        // so the tail of it belongs in the report instead of being dropped.
        // Every module writes into the service ring; the report reads its tail below.
        services.setLogSink({});
        if (options.logLines >= 0) services.setLogCapacity(options.verbose ? 20000 : 512);
        services.setVerbose(options.verbose);
        services.setPrecise(options.precise);
        services.setNativeGles(options.nativeGles);
        services.setScreenSize(options.screenWidth, options.screenHeight);
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
        // Verbose logging names every import that still has no implementation: that list is
        // the compatibility work queue, so it is printed instead of only counted.
        std::string missing;
        if (options.verbose) {
            for (const std::string& name : coverage.missingRequired) missing += "\n  " + name;
            if (missing.empty()) missing = " (none)";
        }
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
        std::string guestFrameStatus;
        try {
            run = runGuest(image, services, graph, main, options.instructions, options.seconds,
                           options.cancelRequested);
            if (options.frameSink) {
                GLES& graphics = services.graphics();
                bool captured = graphics.frameCurrent();
                if (!captured) captured = graphics.present();
                if (captured) {
                    const auto& pixels = graphics.lastFrame();
                    const int width = graphics.drawableWidth(), height = graphics.drawableHeight();
                    if (width > 0 && height > 0 &&
                        pixels.size() == size_t(width) * size_t(height) * 4) {
                        try {
                            options.frameSink(width, height, pixels);
                            guestFrameStatus = "\nGUEST_FRAME: captured " + std::to_string(width) +
                                               "x" + std::to_string(height) +
                                               " RGBA8 drawable pixels.";
                        } catch (const std::exception& e) {
                            guestFrameStatus =
                                "\nGUEST_FRAME: host transfer failed: " + std::string(e.what());
                        }
                    } else {
                        guestFrameStatus = "\nGUEST_FRAME: readback dimensions were invalid.";
                    }
                } else {
                    guestFrameStatus = "\nGUEST_FRAME: unavailable; " + graphics.describe();
                }
            }
            services.closeGuestFiles();
            services.drainGuestPools();
        } catch (const Error& e) {
            return "LAUNCH_BLOCKED: GUEST_SETUP: " + std::string(e.what()) +
                   "\nNo guest instruction retired.\nDYLD_EXPORT_AUDIT: " + report + "\n" +
                   image.summary();
        }
        const std::string header =
            run.stop == "UNIMPLEMENTED"
                ? "LAUNCH_BLOCKED: HLE_GAP: the guest called an API that is not implemented yet."
            : run.stop == "TRAPPED"
                ? "LAUNCH_BLOCKED: GUEST_RUNTIME_TRAP: the guest stopped inside a named runtime "
                  "operation."
            : run.stop == "FAULT"
                ? "LAUNCH_BLOCKED: GUEST_FAULT: the guest hit an invalid instruction or memory "
                  "access."
            : run.stop == "CANCELLED"
                ? "LAUNCH_CANCELLED: the user stopped this bounded guest attempt."
                : "LAUNCH_ATTEMPT: the guest image actually executed.";
        std::string log;
        if (options.logLines > 0 && services.logSize()) {
            const size_t shown = std::min<size_t>(services.logSize(), size_t(options.logLines));
            log = "\nGUEST_LOG: " + std::to_string(services.logSize()) + " line(s), last " +
                  std::to_string(shown) + ":";
            for (const std::string& line : services.logTail(shown)) log += "\n| " + line;
        }
        const std::string gaps = missing.empty() ? std::string() : "\nMISSING_REQUIRED:" + missing;
        // Verbose runs list every mapping: a guest pointer that is "obviously" wrong is
        // usually a pointer into a mapping whose extent nobody checked.
        // The Objective-C call graph the guest built, once per receiver/selector pair: this
        // is what shows whether a title started its own loop thread or waited for events.
        std::string trace;
        if (options.verbose && !services.objectiveCTrace().empty()) {
            trace = "\nOBJC_TRACE: " + std::to_string(services.objectiveCTrace().size()) +
                    " receiver/selector pair(s):";
            for (const std::string& line : services.objectiveCTrace()) trace += "\n  " + line;
        }
        const std::string serviceTrace = options.verbose ? services.traceSummary() : std::string();
        std::string mappings;
        if (options.verbose && image.memory) {
            for (const Region& region : image.memory->regions()) {
                char line[160];
                std::snprintf(line, sizeof(line), "\n  0x%08x-0x%08x %s %s", region.base,
                              region.base + region.size,
                              region.permissions & Execute ? "rx"
                              : region.permissions & Write ? "rw"
                                                           : "r-",
                              region.label.c_str());
                mappings += line;
            }
            if (!mappings.empty()) mappings = "\nMAPPED_REGIONS:" + mappings;
        }
        return header + "\n" + describeRun(run) + "\nDYLD_EXPORT_AUDIT: " + report + gaps +
               mappings + trace + serviceTrace + log + "\n" + services.uiSummary() + "\n" +
               services.graphicsSummary() + guestFrameStatus + "\n" + services.guestClassSummary() +
               "\nGUEST_THREADS: " + services.threadSummary() +
               "\nGUEST_AUDIO: " + services.audioSummary() +
               "\nGUEST_STORAGE: " + services.storageSummary() +
               "\nGUEST_HEAP: " + services.heapSummary() + "\n" + image.summary() +
               "\nA GUEST_FRAME capture is framebuffer readback, not a device-side screenshot; "
               "visual appearance and gameplay still require device review.";
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
