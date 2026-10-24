#include "systemconfiguration.h"

#include <algorithm>
#include <limits>

#include "abi.h"
#include "error.h"

namespace podnative::runtime {
std::string SystemConfiguration::readCString(Memory& memory, uint32_t address, size_t limit,
                                             const char* description) const {
    if (!address) throw Error("SC_REACHABILITY", std::string(description) + " is null");
    std::string value;
    value.reserve(std::min<size_t>(limit, 128));
    for (size_t offset = 0; offset < limit; ++offset) {
        const uint64_t current = uint64_t(address) + offset;
        if (current > UINT32_MAX || !memory.permits(uint32_t(current), 1, Read))
            throw Error("SC_REACHABILITY", std::string(description) + " is outside guest memory",
                        address);
        char byte = 0;
        memory.read(uint32_t(current), &byte, 1);
        if (!byte) return value;
        value.push_back(byte);
    }
    throw Error("SC_REACHABILITY", std::string(description) + " has no terminator within the limit",
                address);
}
uint32_t SystemConfiguration::create(Reachability object) {
    if (reachability.size() >= 128)
        throw Error("SC_LIMIT", "too many live reachability references");
    const uint32_t handle = nextHandle;
    if (handle > UINT32_MAX - 16) throw Error("SC_LIMIT", "reachability handle space is exhausted");
    nextHandle += 16;
    reachability.emplace(handle, std::move(object));
    return handle;
}
SystemConfiguration::Reachability& SystemConfiguration::get(uint32_t handle) {
    auto found = reachability.find(handle);
    if (found == reachability.end())
        throw Error("SC_REACHABILITY", "unknown or released reachability reference", handle);
    return found->second;
}
void SystemConfiguration::retain(uint32_t handle) {
    Reachability& object = get(handle);
    if (object.references == UINT32_MAX)
        throw Error("SC_RETAIN", "reachability retain count overflow", handle);
    ++object.references;
}
void SystemConfiguration::release(uint32_t handle) {
    Reachability& object = get(handle);
    if (--object.references == 0) reachability.erase(handle);
}
bool SystemConfiguration::invoke(const std::string& symbol, CpuState& cpu, Memory& memory) {
    auto& r = cpu.registers;
    if (symbol == "_SCNetworkReachabilityCreateWithName") {
        Reachability object;
        object.name = readCString(memory, r[1], 1024, "host name");
        if (object.name.empty())
            throw Error("SC_REACHABILITY", "reachability host name is empty", r[1]);
        r[0] = create(std::move(object));
        return true;
    }
    if (symbol == "_SCNetworkReachabilityCreateWithAddress" ||
        symbol == "_SCNetworkReachabilityCreateWithAddressPair") {
        auto copyAddress = [&](uint32_t address) {
            if (!address || !memory.permits(address, 2, Read))
                throw Error("SC_REACHABILITY", "sockaddr is not readable", address);
            uint8_t header[2] = {};
            memory.read(address, header, sizeof(header));
            const uint8_t family = header[1];  // Darwin sockaddr: sa_len, then sa_family.
            const uint32_t size = family == 2 ? 16u : family == 30 ? 28u : 0u;
            if (!size || (header[0] && header[0] < size) || !memory.permits(address, size, Read))
                throw Error("SC_REACHABILITY", "sockaddr has an unsupported or truncated family",
                            address);
            std::vector<uint8_t> bytes(size);
            memory.read(address, bytes.data(), bytes.size());
            return bytes;
        };
        Reachability object;
        object.address = copyAddress(r[1]);
        if (symbol == "_SCNetworkReachabilityCreateWithAddressPair")
            object.peerAddress = copyAddress(r[2]);
        r[0] = create(std::move(object));
        return true;
    }
    if (symbol == "_SCNetworkReachabilityGetFlags") {
        Reachability& object = get(r[0]);
        if (!r[1]) {
            r[0] = 0;
            return true;
        }
        if (!memory.permits(r[1], 4, Write))
            throw Error("SC_REACHABILITY", "flags output is not writable", r[1]);
        memory.write32(r[1], object.flags);
        r[0] = 1;  // query completed; zero flags honestly report that the sandbox is offline
        return true;
    }
    if (symbol == "_SCNetworkReachabilitySetCallback") {
        Reachability& object = get(r[0]);
        const uint32_t callback = r[1], context = r[2];
        if (callback && !memory.permits(callback & ~1u, 2, Execute)) {
            r[0] = 0;
            return true;
        }
        uint32_t info = 0;
        if (context) {
            if (!memory.permits(context, 20, Read)) {
                r[0] = 0;
                return true;
            }
            if (memory.read32(context) != 0 || memory.read32(context + 8) ||
                memory.read32(context + 12) || memory.read32(context + 16)) {
                r[0] = 0;  // only version 0 contexts without guest callbacks are safe here
                return true;
            }
            info = memory.read32(context + 4);
        }
        object.callback = callback;
        object.contextInfo = info;
        if (!callback) {
            object.runLoop = 0;
            object.runLoopMode = 0;
        }
        r[0] = 1;
        return true;
    }
    if (symbol == "_SCNetworkReachabilityScheduleWithRunLoop") {
        Reachability& object = get(r[0]);
        if (!object.callback || !r[1] || !r[2] || (validRunLoop && !validRunLoop(r[1])) ||
            (validRunLoopMode && !validRunLoopMode(r[2]))) {
            r[0] = 0;
            return true;
        }
        object.runLoop = r[1];
        object.runLoopMode = r[2];
        r[0] = 1;
        return true;
    }
    if (symbol == "_SCNetworkReachabilityUnscheduleFromRunLoop") {
        Reachability& object = get(r[0]);
        if (!object.runLoop || object.runLoop != r[1] || object.runLoopMode != r[2]) {
            r[0] = 0;
            return true;
        }
        object.runLoop = 0;
        object.runLoopMode = 0;
        r[0] = 1;
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
