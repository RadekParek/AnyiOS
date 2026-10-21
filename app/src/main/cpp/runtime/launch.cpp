#include "launch.h"

#include <set>

#include "error.h"
#include "exports.h"
#include "loader.h"
#include "services.h"

namespace podnative::runtime {
namespace {
std::string audit(const LoadedImage& image) {
    ExportGraph graph;
    Services services(*image.memory);
    services.install(graph);
    auto main = graph.addImage("<executable>");
    graph.setMainExecutable(main);
    graph.dependencies(main, image.dependencies);
    for (const auto& [name, definition] : image.exports) graph.define(main, name, definition);

    std::set<std::string> missingRequired, missingWeak, resolvedRequired, resolvedWeak;
    for (const auto& fixup : image.dyld.fixups) {
        if (fixup.rebase) continue;
        const bool found = graph.resolve(main, fixup.ordinal, fixup.symbol).has_value();
        auto& bucket = found ? (fixup.weak ? resolvedWeak : resolvedRequired)
                             : (fixup.weak ? missingWeak : missingRequired);
        bucket.insert(fixup.symbol);
    }
    size_t strongMissing = 0;
    for (const auto& name : image.dyld.strongDefinitions)
        if (!graph.strong(main, name)) strongMissing++;

    std::string out =
        "\nDYLD_EXPORT_AUDIT: " + std::to_string(resolvedRequired.size()) + " of " +
        std::to_string(resolvedRequired.size() + missingRequired.size()) +
        " unique required imports have host implementations; " +
        std::to_string(missingRequired.size()) + " required, " +
        std::to_string(missingWeak.size()) +
        " weak imports lack one. This is an implementation audit, not a runtime trace.";
    out +=
        "\nHLE_SERVICES: " + std::to_string(services.count()) +
        " checked ARM call gates (C memory/string plus CoreFoundation bit vectors). Missing APIs are hard failures.";
    if (image.dyld.needsCoalescing)
        out += "\nDYLD_COALESCING: " + std::to_string(image.dyld.strongDefinitions.size()) +
               " strong declarations; " + std::to_string(strongMissing) +
               " lack a registered strong definition, so coalescing cannot be applied.";
    if (!missingRequired.empty()) {
        out += "\nDYLD_BLOCKED_EXPORT: ";
        size_t shown = 0;
        for (const auto& name : missingRequired) {
            if (shown++ == 8) break;
            out += name + " ";
        }
        if (missingRequired.size() > 8) out += "(and more)";
    }
    return out;
}
}  // namespace
std::string attemptLaunch(const uint8_t* data, size_t size) {
    try {
        auto image = loadMachO(data, size, 64 * 1024 * 1024);
        const std::string blocker =
            image.requiresDyld
                ? "DYLD_RESOLUTION_BLOCKED: export resolution and coalescing are implemented as checked operations, but this launch remains blocked while required imports lack host implementations and Darwin process bootstrap is absent."
#ifdef PODNATIVE_CPU_IN_APP
                : "GUEST_ABI_UNIMPLEMENTED: CPU engine is linked; Darwin process bootstrap and service dispatch are not ready. Run Diagnostics CPU self-test separately.";
#else
                : "CPU_NOT_INTEGRATED: the tested CPU engine is not linked into this Android app.";
#endif
        std::string details;
        try {
            details = audit(image);
        } catch (const Error& e) {
            details = "\nDYLD_EXPORT_AUDIT: unavailable (" + std::string(e.what()) + ")";
        }
        return "LAUNCH_BLOCKED: " + blocker + details + "\n" + image.summary() +
               "\nGuest code was not executed. Validation mappings released.";
    } catch (const Error& e) {
        return "LAUNCH_BLOCKED: " + std::string(e.what()) +
               "\nGuest code was not executed. Original game files are unchanged.";
    }
}
}  // namespace podnative::runtime
