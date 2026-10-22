#pragma once
#include "audio.h"
#include "coregraphics.h"
#include "cxx.h"
#include "exports.h"
#include "frameworks.h"
#include "fs.h"
#include "libc.h"
#include "objc.h"
#include "stdio.h"
#include "uikit.h"

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
    // Guest console output (guest printf/NSLog) destination.
    void setLogSink(std::function<void(const std::string&)> sink) {
        libc.setLogSink(sink);
        cxx.setLogSink(sink);
        stdio.setLogSink(sink);
        objc.setLogSink(sink);
        uikit.setLogSink(sink);
    }
    // The image's own Objective-C classes, registered before guest code runs.
    void registerGuestClasses(const std::vector<uint32_t>& classList) {
        objc.registerGuestClasses(classList);
    }
    std::string guestClassSummary() const { return objc.guestClassSummary(); }
    std::string uiSummary() const { return uikit.describe(); }
    // True when a service stop was the lifecycle gate rather than an import call.
    bool lifecycleGate(uint32_t pc) const { return uikit.atLifecycleGate(pc); }
    void setEnvironment(const std::map<std::string, std::string>& values);
    // Guest-visible description of the sandbox for the launch report.
    std::string storageSummary() const { return stdio.describe(); }
    // Flushes and closes every guest-owned file once a run stops.
    void closeGuestFiles() { stdio.closeAll(); }
    // Runs the guest's autorelease pools once a launch stops.
    void drainGuestPools() { objc.drainPools(); }
    // `dependencies` are the image's LC_LOAD_* install names: every one of them becomes an
    // export-graph image so two-level ordinals from the binary resolve.
    void install(ExportGraph& graph, const std::vector<std::string>& dependencies = {}) const;
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
    AudioSession audio;
    ObjC objc;
    UIKit uikit;
    std::vector<std::string> symbols;
    std::vector<std::string> traps;
    std::map<std::string, uint32_t> trapIndex;
    void mapTrapPage(size_t index) const;
    void resumeLifecycle(CpuState& state);
    // Which iOS library exports a symbol; two-level ordinals are checked against it.
    const std::string& libraryOf(const std::string& symbol) const;
    void invoke(const std::string& symbol, CpuState& state);
    static constexpr uint32_t Base = 0x6f000000;
    static constexpr uint32_t TrapBase = 0x6f100000;
    static constexpr size_t TrapsPerPage = Memory::PageSize / 8;
};
}  // namespace podnative::runtime
