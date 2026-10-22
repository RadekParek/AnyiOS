#pragma once
#include <cstdint>
#include <functional>
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
// can state what the main run loop can and cannot service.
//
// What it deliberately does not do: there is no window server, no event source and no
// display link yet, so the run loop has nothing to dispatch. That is reported, not faked as
// a successful launch.
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
    std::string describe() const;

   private:
    enum class Stage { Idle, DelegateLaunching, RunLoop, Finished };
    Memory& memory;

    Stage stage = Stage::Idle;
    std::function<void(const std::string&)> logSink;
    std::string applicationDescription = "none";
    std::string delegateDescription = "none";
    std::string outcome = "not started";
    uint32_t appObject = 0;
    uint32_t delegateObject = 0;
    uint32_t launchReturn = 0;
    // Which of the two documented launch callbacks the delegate implements. iOS 3.0 titles
    // implement `applicationDidFinishLaunching:`; later ones implement the options variant.
    std::string launchSelector;
    std::vector<std::string> argv;
    void emit(const std::string& text) const;
    // Reads the guest argv array into printable strings; a malformed array is refused.
    std::vector<std::string> readArguments(uint32_t argc, uint32_t argvAddress) const;
};
}  // namespace podnative::runtime
