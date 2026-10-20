#include "launch.h"

#include "error.h"
#include "loader.h"

namespace podnative::runtime {
std::string attemptLaunch(const uint8_t* data, size_t size) {
    try {
        auto image = loadMachO(data, size, 64 * 1024 * 1024);
        const std::string blocker =
            image.requiresDyld
                ? "DYLD_NOT_IMPLEMENTED: imports, indirect symbols and relocations are not resolved."
                : "CPU_NOT_INTEGRATED: the tested CPU engine is not linked into this Android app.";
        return "LAUNCH_BLOCKED: " + blocker + "\n" + image.summary() +
               "\nGuest code was not executed. Validation mappings released.";
    } catch (const Error& e) {
        return "LAUNCH_BLOCKED: " + std::string(e.what()) +
               "\nGuest code was not executed. Original game files are unchanged.";
    }
}
}  // namespace podnative::runtime
