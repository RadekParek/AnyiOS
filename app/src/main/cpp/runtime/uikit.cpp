#include "uikit.h"

#include <sstream>

#include "error.h"
#include "exports.h"
#include "objc.h"

namespace podnative::runtime {
namespace {
constexpr uint32_t SvC = 0xef000000u;
constexpr uint32_t BxLr = 0xe12fff1eu;
void writeWord(std::vector<uint8_t>& code, size_t offset, uint32_t word) {
    for (unsigned byte = 0; byte < 4; byte++) code[offset + byte] = uint8_t(word >> (byte * 8));
}
}  // namespace
void UIKit::installGates() {
    // The lifecycle gate is where a host-initiated guest callback returns: ordinary guest
    // code that traps back into the host, so the guest's own `bx lr` needs no special case.
    // LifecycleGate is a labelled address inside the page, not the page base: the trap has to
    // live *at* that address, or a guest `bx lr` into it executes the zero fill after the
    // code and runs off the end of the mapping.
    const uint32_t page = LifecycleGate & ~(Memory::PageSize - 1);
    const size_t offset = LifecycleGate - page;
    if (offset + 8 > Memory::PageSize)
        throw Error("UIKIT_GATE", "lifecycle gate does not fit its page", LifecycleGate);
    std::vector<uint8_t> gate(Memory::PageSize, 0);
    writeWord(gate, offset, SvC | 0xffu);
    writeWord(gate, offset + 4, BxLr);
    memory.map({page, Memory::PageSize, uint8_t(Read | Execute), "UIKit lifecycle return gate"},
               gate.data(), gate.size());
}
void UIKit::emit(const std::string& text) {
    if (logSink) logSink(text);
    if (events.size() >= 64) events.erase(events.begin());
    events.push_back(text);
}
std::vector<std::string> UIKit::readArguments(uint32_t argc, uint32_t argvAddress) const {
    if (!argc) return {};
    if (argc > 512 || !memory.permits(argvAddress, size_t(argc) * 4, Read))
        throw Error("UIKIT_ARGV", "UIApplicationMain received an invalid argv array");
    std::vector<std::string> values;
    for (uint32_t index = 0; index < argc; index++) {
        const uint32_t pointer = memory.read32(argvAddress + index * 4);
        if (!pointer) {
            values.emplace_back();
            continue;
        }
        std::string value;
        for (size_t n = 0; n < 4096; n++) {
            char c = 0;
            memory.read(pointer + uint32_t(n), &c, 1);
            if (!c) break;
            value.push_back(c);
        }
        values.push_back(std::move(value));
    }
    return values;
}
bool UIKit::invoke(const std::string& symbol, CpuState& state, ObjC& objc) {
    if (symbol != "_UIApplicationMain") return false;
    auto& r = state.registers;
    if (stage != Stage::Idle)
        throw Error("UIKIT_REENTRY", "UIApplicationMain was called while a bootstrap is running");
    // int UIApplicationMain(int argc, char *argv[], NSString *principal, NSString *delegate)
    argv = readArguments(r[0], r[1]);
    launchReturn = r[14];
    applicationDescription = "UIApplication";
    appObject = objc.makeHostObject("UIApplication");
    std::string delegateName;
    if (r[3]) delegateName = objc.stringValue(r[3]);
    if (r[2]) objc.stringValue(r[2]);  // principal class name is validated when present
    if (argv.size() > 1 && !argv[1].empty()) applicationDescription += " (" + argv[1] + ")";
    std::ostringstream boot;
    boot << "UIApplicationMain: argc=" << argv.size();
    if (!argv.empty()) boot << ", argv[0]=\"" << argv[0] << "\"";
    if (!delegateName.empty()) boot << ", delegate=" << delegateName;
    emit(boot.str());
    if (delegateName.empty()) {
        stage = Stage::Finished;
        // nil delegate is legal: UIKit then instantiates the class named by the bundle's
        // NSMainNibFile. NIB loading is not implemented, so this is reported instead of
        // pretending the application started.
        outcome =
            "UIApplicationMain was called without a delegate class. UIKit would load the "
            "main NIB named by the bundle's Info.plist; NIB object graphs are not implemented "
            "by this runtime yet, so no application was started.";
        r[0] = 0;
        return true;
    }
    const uint32_t delegateClass = objc.classAddress(delegateName, true);
    if (!objc.guestClassDefined(delegateClass))
        throw Error("UIKIT_DELEGATE",
                    "delegate class \"" + delegateName +
                        "\" is not defined by the image; only the image's own delegate can be "
                        "instantiated");
    delegateObject = objc.allocateGuestInstance(delegateClass);
    delegateDescription = delegateName;
    // The delegate's launch callback is guest code: enter it with a host-supplied return
    // address so its result comes back through the lifecycle gate.
    // UIKit calls the options variant when the delegate implements it and falls back to the
    // iOS 3.0 `applicationDidFinishLaunching:` otherwise. A title is not wrong for using the
    // older selector, so refusing it would reject every 2009-era binary.
    stage = Stage::DelegateLaunching;
    if (objc.respondsTo(delegateObject, "application:didFinishLaunchingWithOptions:")) {
        launchSelector = "application:didFinishLaunchingWithOptions:";
        objc.sendGuestMessage(delegateObject, launchSelector, state, {appObject, 0}, LifecycleGate);
    } else if (objc.respondsTo(delegateObject, "applicationDidFinishLaunching:")) {
        launchSelector = "applicationDidFinishLaunching:";
        objc.sendGuestMessage(delegateObject, launchSelector, state, {appObject}, LifecycleGate);
    } else {
        stage = Stage::Finished;
        throw Error("UIKIT_DELEGATE",
                    "delegate " + delegateName +
                        " implements neither application:didFinishLaunchingWithOptions: nor "
                        "applicationDidFinishLaunching:");
    }
    r[14] = LifecycleGate;
    return true;
}
void UIKit::resume(CpuState& state, ObjC& objc) {
    if (stage == Stage::DelegateLaunching) {
        const uint32_t launched = state.registers[0];
        std::ostringstream line;
        line << "UIApplicationMain: -[" << launchSelector << "] returned ";
        if (launchSelector == "applicationDidFinishLaunching:")
            line << "void";  // the iOS 3.0 selector returns nothing
        else
            line << (launched ? "YES" : "NO");
        line << " from " << delegateDescription;
        emit(line.str());
        // Real UIKit sends -applicationDidBecomeActive: right after the launch callback, when
        // the application comes to the foreground. Titles hang their engine loop off it — this
        // one starts the thread that calls -[AppController mainloop] — so skipping it would
        // leave such a title alive but idle forever.
        if (objc.respondsTo(delegateObject, "applicationDidBecomeActive:")) {
            stage = Stage::Activating;
            emit("UIApplicationMain: -[" + delegateDescription +
                 " applicationDidBecomeActive:] (the application became active)");
            objc.sendGuestMessage(delegateObject, "applicationDidBecomeActive:", state, {appObject},
                                  LifecycleGate);
            return;
        }
        stage = Stage::RunLoop;
    }
    if (stage == Stage::Activating) {
        emit("UIApplicationMain: -[" + delegateDescription +
             " applicationDidBecomeActive:] returned void");
        stage = Stage::RunLoop;
    }
    if (stage == Stage::RunLoop) {
        // The run loop dispatches everything that is queued, then waits. The queue is checked
        // immediately before parking because a guest thread may have queued a call while this
        // thread was still on its way here.
        for (;;) {
            if (hasActiveTask) {
                objc.finishMainThreadTask(activeTaskEvent);
                hasActiveTask = false;
            }
            ObjC::MainThreadTask task;
            if (objc.nextMainThreadTask(task)) {
                activeTaskEvent = task.eventId;
                hasActiveTask = true;
                char line[128];
                std::snprintf(line, sizeof(line),
                              "UIApplicationMain: run loop runs %s on 0x%08x for the main thread",
                              task.selector.c_str(), task.target);
                emit(line);
                state.registers[0] = 0;
                objc.sendGuestMessage(task.target, task.selector, state, {task.argument},
                                      LifecycleGate);
                return;
            }
            // A title that started its own guest threads keeps running on them, exactly as it
            // would on a device: the main thread waits in the run loop instead of returning
            // out of UIApplicationMain. The decision looks at live threads, not runnable ones,
            // because a game loop thread sleeping between frames is still the app running.
            if (runnableThreads && parkMainThread && runnableThreads() > 1) {
                outcome =
                    "the delegate finished launching; the application keeps running on the "
                    "guest thread it started, while the main thread waits in the run loop for "
                    "events.";
                state.registers[0] = 0;
                state.registers[15] = LifecycleGate;  // re-entered when a task is queued
                parkMainThread();
                emit(outcome);
                return;
            }
            break;
        }
        // Nothing keeps the application alive yet: no event source, timer or display link
        // exists, so the bootstrap returns to the guest rather than pretending otherwise.
        outcome =
            "the delegate finished launching, but the main run loop has no implemented "
            "sources: there is no event source, timer or display link to dispatch, so the "
            "bootstrap returns to the guest instead of pretending to run the app.";
        stage = Stage::Finished;
        state.registers[15] = launchReturn & ~1u;
        state.cpsr = (state.cpsr & ~0x20u) | (launchReturn & 1u ? 0x20u : 0u);
        state.registers[0] = 0;
        emit(outcome);
        return;
    }
    throw Error("UIKIT_LIFECYCLE", "lifecycle gate entered outside a bootstrap");
}
std::string UIKit::describe() const {
    std::ostringstream out;
    out << "UIKIT_STATE: " << outcome;
    if (!events.empty()) {
        out << "\nUIKIT_EVENTS:";
        for (const std::string& line : events) out << "\n  " << line;
    }
    if (!delegateDescription.empty()) out << "\nUIKIT_DELEGATE: " << delegateDescription;
    if (!argv.empty()) {
        out << "\nUIKIT_ARGV:";
        for (const auto& value : argv) out << " \"" << value << "\"";
    }
    return out.str();
}
}  // namespace podnative::runtime
