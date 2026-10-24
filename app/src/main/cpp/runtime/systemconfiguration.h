#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "memory.h"

namespace podnative::runtime {
// Small, explicit SystemConfiguration reachability model. The sandbox has no guest network
// backend, so valid queries report flags == 0 (unreachable); callback/run-loop registration is
// tracked but no fabricated online transition is delivered.
class SystemConfiguration final {
   public:
    bool invoke(const std::string& symbol, CpuState& cpu, Memory& memory);
    bool owns(uint32_t handle) const { return reachability.count(handle) != 0; }
    void retain(uint32_t handle);
    void release(uint32_t handle);
    bool equal(uint32_t left, uint32_t right) const { return left == right && owns(left); }
    void setRunLoopValidators(std::function<bool(uint32_t)> runLoop,
                              std::function<bool(uint32_t)> mode) {
        validRunLoop = std::move(runLoop);
        validRunLoopMode = std::move(mode);
    }

   private:
    struct Reachability {
        uint32_t references = 1;
        std::string name;
        std::vector<uint8_t> address, peerAddress;
        uint32_t callback = 0, contextInfo = 0;
        uint32_t runLoop = 0, runLoopMode = 0;
        uint32_t flags = 0;  // offline: no network interface/backend is available to the guest
    };
    std::map<uint32_t, Reachability> reachability;
    uint32_t nextHandle = 0xe4300000;
    std::function<bool(uint32_t)> validRunLoop, validRunLoopMode;

    uint32_t create(Reachability object);
    Reachability& get(uint32_t handle);
    std::string readCString(Memory& memory, uint32_t address, size_t limit,
                            const char* description) const;
};
}  // namespace podnative::runtime
