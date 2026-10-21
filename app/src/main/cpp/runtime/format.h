#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "abi.h"

namespace podnative::runtime {
// printf-style formatting over guest argument words. iOS ARMv7 passes every vararg in
// the core registers and then the stack, so one word cursor serves every conversion,
// including floats and doubles. Unsupported conversions are hard failures rather than
// silently dropped text. %@ is Objective-C only and renders through the supplied
// description callback.
using ObjectDescriber = std::function<std::string(uint32_t)>;
std::string formatGuest(const std::string& format, Arguments& args,
                        const ObjectDescriber& object = {});

// Bounded guest string read used by %s and by callers that need a NUL-terminated copy.
std::string readGuestString(Memory& memory, uint32_t address, size_t limit = 1024 * 1024);
}  // namespace podnative::runtime
