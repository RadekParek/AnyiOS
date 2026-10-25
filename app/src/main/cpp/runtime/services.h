#pragma once

#include <chrono>

#include "audio.h"
#include "commoncrypto.h"
#include "coregraphics.h"
#include "cxx.h"
#include "exports.h"
#include "frameworks.h"
#include "fs.h"
#include "gles.h"
#include "libc.h"
#include "objc.h"
#include "openal.h"
#include "posix.h"
#include "stdio.h"
#include "systemconfiguration.h"
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
    void mountSandbox(const std::string& bundleHost, const std::string& containerHost,
                      const std::string& bundleArchive = {},
                      const std::string& bundleArchivePrefix = {},
                      const std::string& bundleCache = {}, const std::string& bundleOverlay = {});
    // Guest console output (guest printf/NSLog) destination.
    void setLogSink(std::function<void(const std::string&)> sink) {
        externalLog = std::move(sink);
        // Every module writes through this one door, so the ring, the optional external sink
        // and the verbose trace can never disagree about what the guest did.
        auto forward = [this](const std::string& line) { log(line); };
        libc.setLogSink(forward);
        cxx.setLogSink(forward);
        stdio.setLogSink(forward);
        audio.setLogSink(forward);
        // Objective-C diagnostics always enter the common report ring. When verbose tracing
        // is enabled, retain a separate bounded copy as well; never fall back to host stdout.
        objc.setLogSink([this](const std::string& line) {
            if (verbose && objcTrace.size() < objcTraceCapacity) objcTrace.push_back(line);
            log(line);
        });
        uikit.setLogSink(forward);
        gles.setLogSink(forward);
    }
    // Guest-visible log lines kept for the report. Verbose logging asks for more of them.
    void setLogCapacity(size_t lines) { logCapacity = lines ? lines : 1; }
    void log(const std::string& line);
    std::vector<std::string> logTail(size_t count) const;
    size_t logSize() const { return guestLog.size(); }
    // Counts for verbose traces whose hot-path repetitions were deliberately summarized.
    std::string traceSummary() const;
    // Deep tracing for the "Verbose logging" setting: every guest-visible service call, the
    // values it was given and what it returned, plus the runtime's own decisions.
    // Threads the image starts through NSThread: the target/selector pair a created guest
    // thread must enter, keyed by the request the trampoline gate receives.
    struct ThreadStart {
        uint32_t target = 0;
        std::string selector;
        uint32_t argument = 0;
    };
    std::map<uint32_t, ThreadStart> threadStarts;
    uint32_t nextThreadStart = 1;
    bool spawnCallbackThread(uint32_t target, const std::string& selector, uint32_t argument);
    void runThreadStart(CpuState& state);
    void setVerbose(bool on) {
        verbose = on;
        cxx.setVerbose(on);
        gles.setVerbose(on);
        stdio.setVerbose(on);
        // The Objective-C sink installed by setLogSink observes this flag and also keeps a
        // bounded trace. It remains wired to the common report ring in both modes.
    }
    // Settings ▸ "Native OpenGL ES": the device's own driver is tried before the bundled
    // ANGLE. Off by default, because one driver on every device is the point of bundling it.
    // Read before the driver is resolved; a loaded driver is not swapped under a running guest.
    void setNativeGles(bool on) { gles.setPreferNativeDriver(on); }
    void setScreenSize(int width, int height) {
        objc.setScreenSize(width, height);
        gles.setScreenSize(width, height);
    }
    void setInterfaceOrientation(int orientation) { objc.setInterfaceOrientation(orientation); }
    void setFrameSink(std::function<void(int, int, const std::vector<uint8_t>&)> sink);
    void captureFrameIfDue();
    bool isVerbose() const { return verbose; }
    const std::vector<std::string>& objectiveCTrace() const { return objcTrace; }
    void setPrecise(bool on) { precise = on; }
    bool isPrecise() const { return precise; }
    // The image's own Objective-C classes, registered before guest code runs.
    void registerGuestClasses(const std::vector<uint32_t>& classList) {
        objc.registerGuestClasses(classList);
    }
    std::string guestClassSummary() const { return objc.guestClassSummary(); }
    // Graphics state for the launch report: driver, call counts and drawable size.
    std::string graphicsSummary() const { return gles.describe(); }
    GLES& graphics() { return gles; }
    std::string uiSummary() const { return uikit.describe(); }
    // True when a service stop was the lifecycle gate rather than an import call.
    bool lifecycleGate(uint32_t pc) const { return uikit.atLifecycleGate(pc); }
    void setEnvironment(const std::map<std::string, std::string>& values);
    // Threading and scheduling state for the launch report.
    std::string threadSummary() const { return posix.describe(); }
    // Scheduler hand-off used by the run loop: the guest has several threads and the current
    // quantum ended, or a thread blocked.
    void adoptMainThread(const CpuState& state) { posix.adoptMainThread(state); }
    bool threadSwitchWanted() const { return posix.wantsSwitch(); }
    bool switchGuestThread(CpuState& state) { return posix.switchThreads(state); }
    bool guestThreadsFinished() const { return posix.finished(); }
    const std::string& guestThreadStall() const { return posix.stallReason(); }
    size_t guestThreadCount() const { return posix.threadCount(); }
    bool mainRunLoopWaiting() const { return posix.mainRunLoopWaiting(); }
    // OpenAL device/buffer/source state for the launch report.
    std::string audioSummary() const { return openal.describe(); }
    // Guest-visible description of the sandbox for the launch report.
    std::string storageSummary() const { return stdio.describe(); }
    std::string heapSummary() const {
        return std::to_string(libc.guestHeap().used()) + " of " +
               std::to_string(libc.guestHeap().committed()) + " mapped byte(s) (" +
               std::to_string(libc.guestHeap().capacity()) + " reserved) held by " +
               std::to_string(libc.guestHeap().blocks()) + " live allocation(s), largest " +
               std::to_string(libc.guestHeap().largestFreeBlock()) + " contiguous free byte(s), " +
               std::to_string(libc.guestHeap().largeRequests()) + " request(s) of " +
               std::to_string(GuestHeap::LargeRequestBytes / (1024 * 1024)) +
               " MiB or more, largest " + std::to_string(libc.guestHeap().largestRequest()) +
               " byte(s)";
    }
    // Flushes and closes every guest-owned file once a run stops.
    void closeGuestFiles() { stdio.closeAll(); }
    // Runs the guest's autorelease pools once a launch stops.
    void drainGuestPools() { objc.drainPools(); }
    // `dependencies` are the image's LC_LOAD_* install names: every one of them becomes an
    // export-graph image so two-level ordinals from the binary resolve.
    void install(ExportGraph& graph, const std::vector<std::string>& dependencies = {});
    void dispatch(const CpuStop& stop, CpuState& state);
    uint32_t address(const std::string& symbol) const;
    // Unimplemented imports bind here so the guest can run and stop precisely at
    // the first missing API instead of being refused before it starts.
    uint32_t trapFor(const std::string& symbol);
    size_t trapCount() const { return traps.size(); }
    // Name of the unimplemented import a trap address stands for, or an empty string.
    std::string trapSymbol(uint32_t address) const;
    size_t count() const { return symbols.size(); }
    // Name of the most recent service the guest called, for the stop report.
    const std::string& lastCall() const { return lastSymbol; }
    bool guestExitRequested() const { return libc.guestExitRequested(); }
    const std::string& guestExitRoutine() const { return libc.exitRoutine(); }
    uint32_t guestExitStatus() const { return libc.exitStatus(); }

   private:
    Memory& memory;
    bool verbose = false;
    bool precise = false;
    static constexpr size_t objcTraceCapacity = 2048;
    std::vector<std::string> objcTrace;
    std::vector<std::string> guestLog;
    std::map<std::string, size_t> serviceTraceCalls, serviceTraceSuppressed;
    size_t logCapacity = 512;
    bool frameCaptureEnabled = false;
    bool frameCaptureFailureLogged = false;
    std::chrono::steady_clock::time_point lastFrameCapture;
    std::function<void(const std::string&)> externalLog;
    void traceSymbol(const std::string& name, const CpuState& state);
    CoreFoundation cf;
    CoreGraphics cg;
    SystemConfiguration systemConfiguration;
    CommonCrypto commonCrypto;
    Libc libc;
    CxxRuntime cxx;
    FileSystem files;
    Stdio stdio;
    AudioSession audio;
    Posix posix;
    OpenAL openal;
    ObjC objc;
    GLES gles;
    UIKit uikit;
    std::vector<std::string> symbols;
    std::string lastSymbol;
    std::vector<std::string> traps;
    std::map<std::string, uint32_t> trapIndex;
    void mapTrapPage(size_t index) const;
    // Darwin's default rune tables, so a guest that indexes __DefaultRuneLocale or calls
    // ___maskrune sees the same classification and case mapping a device would report.
    uint32_t runeLocaleCell = 0;
    uint32_t cfTypeArrayCallbacks = 0;
    uint32_t runeLocale();
    // A guest CFString global initialised to `text`.
    uint32_t constantStringVariable(const std::string& text);
    void resumeLifecycle(CpuState& state);
    // Which iOS library exports a symbol; two-level ordinals are checked against it.
    const std::string& libraryOf(const std::string& symbol) const;
    void invoke(const std::string& symbol, CpuState& state);
    static constexpr uint32_t Base = 0x6f000000;
    static constexpr uint32_t TrapBase = 0x6f100000;
    static constexpr size_t TrapsPerPage = Memory::PageSize / 8;
};
}  // namespace podnative::runtime
