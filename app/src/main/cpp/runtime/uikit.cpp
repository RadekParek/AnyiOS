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
    std::vector<uint8_t> gate(Memory::PageSize, 0);
    writeWord(gate, 0, SvC | 0xffu);
    writeWord(gate, 4, BxLr);
    memory.map({LifecycleGate & ~(Memory::PageSize - 1), Memory::PageSize, uint8_t(Read | Execute),
                "UIKit lifecycle return gate"},
               gate.data(), gate.size());
}
void UIKit::emit(const std::string& text) const {
    if (logSink) logSink(text);
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
    (void)objc;  // no second guest callback is issued until the run loop has sources.
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
        stage = Stage::RunLoop;
        // Everything that would keep a real application alive lives in the window server and
        // the event sources; neither exists yet, so the loop is entered and immediately
        // reports itself empty rather than spinning.
        outcome =
            "the delegate finished launching, but the main run loop has no implemented "
            "sources: there is no event source, timer or display link to dispatch, so the "
            "bootstrap returns to the guest instead of pretending the app is running.";
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
    if (!delegateDescription.empty()) out << "\nUIKIT_DELEGATE: " << delegateDescription;
    if (!argv.empty()) {
        out << "\nUIKIT_ARGV:";
        for (const auto& value : argv) out << " \"" << value << "\"";
    }
    return out.str();
}
}  // namespace podnative::runtime
