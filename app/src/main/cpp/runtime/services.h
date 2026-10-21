#pragma once
#include "coregraphics.h"
#include "cxx.h"
#include "exports.h"
#include "frameworks.h"
#include "fs.h"
#include "libc.h"
#include "objc.h"
#include "stdio.h"

namespace podnative::runtime {
// ARM code gates contain SVC + BX LR. Guest Thumb callers use BLX interworking.
// Dispatch authenticates both the trap PC and number; arbitrary guest SVCs are not APIs.
class Services final {
   public:
    explicit Services(Memory& memory);
    // Guest file access is refused until the launcher mounts the directories that back
    // the game. `/var/mobile` is the read-only extracted bundle, the container below it
    // is writable.
    void mountSandbox(const std::string& bundleHost, const std::string& containerHost);
    void setEnvironment(const std::map<std::string, std::string>& values);
    // Guest-visible description of the sandbox for the launch report.
    std::string storageSummary() const { return stdio.describe(); }
    // Flushes and closes every guest-owned file once a run stops.
    void closeGuestFiles() { stdio.closeAll(); }
    void install(ExportGraph& graph) const;
    void dispatch(const CpuStop& stop, CpuState& state);
    uint32_t address(const std::string& symbol) const;
    // Unimplemented imports bind here so the guest can run and stop precisely at
    // the first missing API instead of being refused before it starts.
    uint32_t trapFor(const std::string& symbol);
    size_t trapCount() const { return traps.size(); }
    size_t count() const { return symbols.size(); }

   private:
    Memory& memory;
    CoreFoundation cf;
    CoreGraphics cg;
    Libc libc;
    CxxRuntime cxx;
    FileSystem files;
    Stdio stdio;
    ObjC objc;
    std::vector<std::string> symbols;
    std::vector<std::string> traps;
    std::map<std::string, uint32_t> trapIndex;
    void mapTrapPage(size_t index) const;
    void invoke(const std::string& symbol, CpuState& state);
    static constexpr uint32_t Base = 0x6f000000;
    static constexpr uint32_t TrapBase = 0x6f100000;
    static constexpr size_t TrapsPerPage = Memory::PageSize / 8;
};
}  // namespace podnative::runtime
