#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "memory.h"

namespace podnative::runtime {
class ObjC;
// UIKit process bootstrap. `UIApplicationMain` is not a stub here: it parses the guest's
// argv, creates the UIApplication object, instantiates the delegate class *the image
// defines*, and sends `application:didFinishLaunchingWithOptions:` into guest code. When
// that method returns, control comes back through a dedicated lifecycle gate so the host
// can dispatch queued main-thread selectors or park the guest in the application run loop.
//
// The runtime does not invent events, timers, display links, a window server or input. When
// no guest work is available it reports an idle main run loop instead of returning from
// UIApplicationMain; real UIKit keeps an application alive while that loop waits.
class UIKit final {
   public:
    explicit UIKit(Memory& memory) : memory(memory) {}
    // Maps the lifecycle return gate the guest delegate returns into. The UIKit entry
    // points themselves are ordinary service gates owned by `Services`.
    void installGates();
    static constexpr uint32_t LifecycleGate = 0x6f2fffc0;
    bool invoke(const std::string& symbol, CpuState& state, ObjC& objc);
    // True when `pc` is the lifecycle gate: a host-initiated guest callback returned.
    bool atLifecycleGate(uint32_t pc) const { return pc == LifecycleGate; }
    // Called at that gate. Either enters the next guest callback or finishes the bootstrap.
    void resume(CpuState& state, ObjC& objc);
    void setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
    void setFrameCapture(std::function<void()> capture) { frameCapture = std::move(capture); }
    // The bootstrap needs the number of live guest threads and a way to park the main thread.
    // A title that runs its game loop on a worker keeps running there while UIKit waits for
    // main-thread work; a single-threaded app also remains in its idle run loop.
    void setThreadHooks(std::function<size_t()> live, std::function<void()> park) {
        liveThreads = std::move(live);
        parkMainThread = std::move(park);
    }
    std::string describe() const;

   private:
    enum class Stage { Idle, DelegateLaunching, Activating, RunLoop, Finished };
    Memory& memory;

    Stage stage = Stage::Idle;
    std::function<size_t()> liveThreads;
    // The main thread waits here for a run-loop event; it is woken when a guest thread
    // queues one with performSelectorOnMainThread:.
    std::function<void()> parkMainThread;
    std::function<void(const std::string&)> logSink;
    std::function<void()> frameCapture;
    std::string applicationDescription = "none";
    std::string delegateDescription = "none";
    std::string outcome = "not started";
    // The life-cycle decisions this runtime made, kept out of the general log ring so a report
    // still shows them after an audio thread has filled the ring.
    mutable std::vector<std::string> events;
    // The main-thread call the run loop is currently running, so its waiter can be released
    // when the call returns to the lifecycle gate.
    uint32_t activeTaskEvent = 0;
    bool hasActiveTask = false;
    uint32_t appObject = 0;
    uint32_t delegateObject = 0;
    uint32_t launchReturn = 0;
    // Which of the two documented launch callbacks the delegate implements. iOS 3.0 titles
    // implement `applicationDidFinishLaunching:`; later ones implement the options variant.
    std::string launchSelector;
    std::vector<std::string> argv;
    std::map<std::string, size_t> runLoopTaskCounts;
    void emit(const std::string& text);
    // Reads the guest argv array into printable strings; a malformed array is refused.
    std::vector<std::string> readArguments(uint32_t argc, uint32_t argvAddress) const;
};
}  // namespace podnative::runtime
