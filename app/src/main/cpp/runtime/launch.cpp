#include "launch.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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
// fixups and classic symbol stubs. Resolved imports are intentionally not listed; the
// audit is about export-graph resolution, not a claim that every registered gate has full
// runtime semantics.
struct Coverage {
    using ImportKey = std::pair<std::string, int>;  // symbol, dylib ordinal
    size_t requiredReferences = 0, weakReferences = 0;
    std::map<ImportKey, size_t> missingRequired, missingWeak;

    std::string describe(size_t registeredServices) const {
        return "registered service gate names=" + std::to_string(registeredServices) +
               "; unresolved required imports=" + std::to_string(requiredReferences) +
               " references across " + std::to_string(missingRequired.size()) +
               " unique symbol/ordinal pairs; unresolved weak imports=" +
               std::to_string(weakReferences) + " references across " +
               std::to_string(missingWeak.size()) + " unique symbol/ordinal pairs.";
    }

    std::string importDetails(bool verbose) const {
        std::ostringstream out;
        auto append = [&](const char* heading, const std::map<ImportKey, size_t>& imports,
                          size_t limit) {
            out << "\n" << heading << ':';
            if (imports.empty()) {
                out << " none.";
                return;
            }
            std::vector<std::pair<ImportKey, size_t>> ordered(imports.begin(), imports.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
                if (left.second != right.second) return left.second > right.second;
                return left.first < right.first;
            });
            const size_t shown = std::min(limit, ordered.size());
            for (size_t index = 0; index < shown; index++) {
                const auto& [key, references] = ordered[index];
                out << "\n  ";
                for (unsigned char c : key.first) out << (c >= 32 && c < 127 ? char(c) : '?');
                out << " (" << references << (references == 1 ? " reference" : " references")
                    << "; ordinal=" << key.second << ')';
            }
            if (shown < ordered.size())
                out << "\n  ... " << (ordered.size() - shown)
                    << " additional unique symbol/ordinal pair(s) omitted.";
        };
        append("MISSING_REQUIRED_IMPORTS", missingRequired, verbose ? 512 : 64);
        if (verbose && !missingWeak.empty()) append("MISSING_WEAK_IMPORTS", missingWeak, 512);
        return out.str();
    }
};

Coverage audit(const LoadedImage& image, const ExportGraph& graph, size_t main) {
    Coverage coverage;
    auto check = [&](int ordinal, const std::string& symbol, bool weak) {
        if (graph.resolve(main, ordinal, symbol)) return;
        auto& missing = weak ? coverage.missingWeak : coverage.missingRequired;
        ++missing[{symbol, ordinal}];
        if (weak)
            ++coverage.weakReferences;
        else
            ++coverage.requiredReferences;
    };
    for (const auto& fixup : image.dyld.fixups) {
        if (fixup.rebase) continue;
        check(fixup.ordinal, fixup.symbol, fixup.weak);
    }
    for (const auto& stub : image.stubs) check(stub.ordinal, stub.symbol, stub.weak);
    return coverage;
}

#ifdef PODNATIVE_CPU_IN_APP
std::string reportSafeGuestLog(const std::string& line) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string safe;
    safe.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        const auto byte = static_cast<uint8_t>(line[i]);
        if (byte == '\n') {
            safe += "\\n";
            ++i;
        } else if (byte == '\r') {
            safe += "\\r";
            ++i;
        } else if (byte == '\t') {
            safe += "\\t";
            ++i;
        } else if (byte >= 0x20 && byte <= 0x7e) {
            safe.push_back(char(byte));
            ++i;
        } else if (byte >= 0xc2) {
            const size_t length = byte <= 0xdf ? 2 : byte <= 0xef ? 3 : byte <= 0xf4 ? 4 : 0;
            uint32_t codepoint = length == 2   ? byte & 0x1f
                                 : length == 3 ? byte & 0x0f
                                 : length == 4 ? byte & 0x07
                                               : 0;
            bool valid = length && i + length <= line.size();
            for (size_t j = 1; valid && j < length; ++j) {
                const auto continuation = static_cast<uint8_t>(line[i + j]);
                valid = continuation >= 0x80 && continuation <= 0xbf;
                codepoint = (codepoint << 6) | (continuation & 0x3f);
            }
            const uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
            valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
                    !(codepoint >= 0xd800 && codepoint <= 0xdfff);
            if (valid) {
                safe.append(line, i, length);
                i += length;
            } else {
                safe += "\\x";
                safe.push_back(Hex[byte >> 4]);
                safe.push_back(Hex[byte & 0x0f]);
                ++i;
            }
        } else {
            safe += "\\x";
            safe.push_back(Hex[byte >> 4]);
            safe.push_back(Hex[byte & 0x0f]);
            ++i;
        }
    }
    return safe;
}

std::string compactGuestLog(size_t captured, const std::vector<std::string>& lines) {
    if (lines.empty()) return {};
    std::vector<std::pair<std::string, size_t>> groups;
    for (const std::string& line : lines) {
        if (!groups.empty() && groups.back().first == line)
            ++groups.back().second;
        else
            groups.emplace_back(line, 1);
    }
    std::ostringstream out;
    out << "\nGUEST_LOG: " << captured << " captured line(s), tail " << lines.size()
        << " line(s) in " << groups.size() << " consecutive group(s):";
    for (const auto& [line, count] : groups) {
        out << "\n| ";
        if (count > 1) out << '[' << count << "x] ";
        out << reportSafeGuestLog(line);
    }
    return out.str();
}

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
        if ((!bundleHost.empty() || !options.bundleArchive.empty()) && !containerHost.empty())
            services.mountSandbox(bundleHost, containerHost, options.bundleArchive,
                                  options.bundleArchivePrefix, options.bundleCache,
                                  options.bundleOverlay);
        ExportGraph graph;
        services.install(graph, image.dependencies);
        auto main = graph.addImage("<executable>");
        graph.setMainExecutable(main);
        graph.dependencies(main, image.dependencies);
        for (const auto& [name, definition] : image.exports) graph.define(main, name, definition);

        const Coverage coverage = audit(image, graph, main);
        const std::string report =
            coverage.describe(services.count()) + coverage.importDetails(options.verbose);
        if (!image.requiresDyld || (image.dyld.fixups.empty() && image.stubs.empty())) {
            return "LAUNCH_BLOCKED: GUEST_ABI_UNIMPLEMENTED: the image needs no dyld work, but "
                   "Darwin process bootstrap is not implemented.\nDYLD_EXPORT_AUDIT: " +
                   report + "\n" + image.summary();
        }
        // Objective-C class pointers and class_ro_t data can still be rebased or bound at this
        // point. Resolve the image's dyld plan before reading instanceSize/name metadata; doing
        // so prevents startup from interpreting pre-fixup bytes as a changing class size.
        image.dyld.applyWithGraph(*image.memory, 0, graph, main,
                                  [&](const std::string& name) { return services.trapFor(name); });
        services.registerGuestClasses(image.objcClassAddresses());
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
                           options.cancelRequested, true);
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
            : run.stop == "WAITING"
                ? "LAUNCH_WAITING: UIKit is keeping the app alive, but no guest thread can run "
                  "until a main-run-loop event is available."
                : "LAUNCH_ATTEMPT: the guest image actually executed.";
        std::string log;
        if (options.logLines > 0 && services.logSize()) {
            const size_t shown = std::min<size_t>(services.logSize(), size_t(options.logLines));
            log = compactGuestLog(services.logSize(), services.logTail(shown));
        }
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
        return header + "\n" + describeRun(run, options.verbose) +
               "\nDYLD_EXPORT_AUDIT: " + report + mappings + trace + serviceTrace + log + "\n" +
               services.uiSummary() + "\n" + services.graphicsSummary() + guestFrameStatus + "\n" +
               services.guestClassSummary() + "\nGUEST_THREADS: " + services.threadSummary() +
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
