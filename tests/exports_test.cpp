#include "runtime/exports.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/dyld.h"
#include "runtime/error.h"
#include "runtime/services.h"
using namespace podnative::runtime;
namespace {
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        if (e.code == code) return;
        std::cerr << e.what() << " expected " << code << '\n';
        std::abort();
    }
    std::cerr << "Expected fault " << code << '\n';
    std::abort();
}
CpuState call(Services& services, const char* name, uint32_t a, uint32_t b = 0, uint32_t c = 0,
              uint32_t lr = 0x1000) {
    uint32_t gate = services.address(name);
    CpuState state;
    state.registers[0] = a;
    state.registers[1] = b;
    state.registers[2] = c;
    state.registers[14] = lr;
    state.registers[15] = gate + 4;
    CpuStop stop{
        StopReason::SupervisorCall, 1, gate, (gate - 0x6f000000U) / 8 + 1, 0, "CPU_SVC", ""};
    services.dispatch(stop, state);
    return state;
}
CpuState objcCall(Services& services, uint32_t receiver, uint32_t selector, uint32_t arg2 = 0) {
    CpuState state;
    state.registers[0] = receiver;
    state.registers[1] = selector;
    state.registers[2] = arg2;
    state.registers[14] = 0x1000;
    state.registers[15] = ObjC::GateBase + 4;
    CpuStop stop{StopReason::SupervisorCall, 1, ObjC::GateBase, 0, 0, "CPU_SVC", ""};
    services.dispatch(stop, state);
    return state;
}
}  // namespace
int main() {
    Memory memory;
    Services services(memory);
    const uint32_t memcpyGate = services.address("_memcpy");
    assert(memory.fetch32(memcpyGate) == (0xef000000 | ((memcpyGate - 0x6f000000U) / 8 + 1)));
    assert(memory.fetch32(memcpyGate + 4) == 0xe12fff1e);
    assert(services.count() > 7);
    const uint32_t finishGate = services.address("_glFinish");
    const uint32_t getStringGate = services.address("_glGetString");
    assert(finishGate >= 0x6f000000 && getStringGate >= 0x6f000000);
    assert(memory.fetch32(finishGate) == (0xef000000 | ((finishGate - 0x6f000000U) / 8 + 1)));
    assert(memory.fetch32(getStringGate) == (0xef000000 | ((getStringGate - 0x6f000000U) / 8 + 1)));

    std::vector<uint8_t> text(4096, 0), data(4096, 0);
    memory.map({0x1000, 4096, uint8_t(Read | Execute), "guest return code"}, text.data(),
               text.size());
    memory.map({0x3000, 4096, uint8_t(Read | Write), "service data"}, data.data(), data.size());

    ExportGraph graph;
    // The binary's own dependency list is registered as images, so every two-level
    // ordinal it uses has a target.
    services.install(
        graph,
        {"/usr/lib/libSystem.B.dylib", "/usr/lib/libobjc.A.dylib", "/usr/lib/libstdc++.6.dylib",
         "/usr/lib/libgcc_s.1.dylib", "/System/Library/Frameworks/Foundation.framework/Foundation",
         "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
         "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
         "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
         "/System/Library/Frameworks/UIKit.framework/UIKit",
         "/System/Library/Frameworks/OpenGLES.framework/OpenGLES", "/usr/lib/libc++.1.dylib",
         "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
         "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration"});
    size_t executable = graph.addImage("<executable>");
    // Every dependency image exists before the executable, so no ordinal dangles.
    for (const char* dependency :
         {"/usr/lib/libSystem.B.dylib", "/usr/lib/libobjc.A.dylib", "/usr/lib/libstdc++.6.dylib",
          "/usr/lib/libgcc_s.1.dylib", "/System/Library/Frameworks/Foundation.framework/Foundation",
          "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
          "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
          "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
          "/System/Library/Frameworks/UIKit.framework/UIKit",
          "/System/Library/Frameworks/OpenGLES.framework/OpenGLES", "/usr/lib/libc++.1.dylib",
          "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
          "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration"})
        assert(graph.index(dependency).has_value());
    assert(executable > 9);
    graph.setMainExecutable(executable);
    const auto openGles =
        graph.index("/System/Library/Frameworks/OpenGLES.framework/OpenGLES").value();
    assert(graph.strong(openGles, "_glFinish") && graph.strong(openGles, "_glGetString"));
    assert(graph.resolve(executable, -2, "_glFinish") == services.address("_glFinish"));
    assert(graph.resolve(executable, -2, "_glGetString") == services.address("_glGetString"));
    bool glesUsesSecondPage = false;
    for (const std::string& name : services.graphics().symbols()) {
        const uint32_t gate = services.address(name);
        assert(memory.fetch32(gate) == (0xef000000 | ((gate - 0x6f000000U) / 8 + 1)));
        assert(memory.fetch32(gate + 4) == 0xe12fff1e);
        glesUsesSecondPage |= gate >= 0x6f001000;
    }
    assert(glesUsesSecondPage);
    assert(!graph.resolve(executable, -1, "_memcpy"));
    graph.define(executable, "_local", {0x999, false});
    assert(graph.resolve(executable, -1, "_local") == 0x999u);
    const size_t coreFoundation =
        graph.index("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
            .value_or(SIZE_MAX);
    assert(coreFoundation != SIZE_MAX);
    graph.setMainExecutable(coreFoundation);
    assert(graph.resolve(executable, -1, "_CFRelease"));
    graph.setMainExecutable(executable);
    assert(graph.resolve(executable, -2, "_memcpy") == memcpyGate);
    assert(!graph.resolve(executable, -2, "_missing"));
    assert(graph.strong(coreFoundation, "_CFRelease"));
    assert(graph.strong(coreFoundation, "_NSDefaultRunLoopMode"));
    assert(graph.resolve(executable, -2, "_CFArrayCreateMutable") ==
           services.address("_CFArrayCreateMutable"));
    const auto cfTypeCallbacks = graph.resolve(executable, -2, "_kCFTypeArrayCallBacks");
    assert(cfTypeCallbacks && memory.read32(*cfTypeCallbacks) == 0);
    assert(memory.read32(*cfTypeCallbacks + 4) == services.address("_CFArrayRetainValueCallBack"));
    assert(memory.read32(*cfTypeCallbacks + 8) == services.address("_CFArrayReleaseValueCallBack"));
    assert(memory.read32(*cfTypeCallbacks + 12) == 0);
    assert(memory.read32(*cfTypeCallbacks + 16) == services.address("_CFArrayEqualValueCallBack"));
    assert(graph.resolve(executable, -2, "_malloc") == services.address("_malloc"));
    assert(graph.hasStrong("_CFRelease") && !graph.hasStrong("_missing"));
    const size_t objcImage = graph.index("/usr/lib/libobjc.A.dylib").value();
    assert(graph.strong(objcImage, "___objc_personality_v0"));
    const auto rectZero = graph.resolve(executable, -2, "_CGRectZero");
    assert(rectZero.has_value());
    assert(memory.read32(*rectZero) == 0 && memory.read32(*rectZero + 4) == 0 &&
           memory.read32(*rectZero + 8) == 0 && memory.read32(*rectZero + 12) == 0);
    const size_t securityImage =
        graph.index("/System/Library/Frameworks/Security.framework/Security").value();
    const size_t commonCryptoImage = graph.index("/usr/lib/libcommonCrypto.dylib").value();
    assert(graph.strong(commonCryptoImage, "_CCCrypt"));
    assert(graph.strong(commonCryptoImage, "_CC_MD5"));
    assert(graph.strong(commonCryptoImage, "_CC_SHA1"));
    assert(graph.strong(securityImage, "_CCCrypt"));
    assert(graph.resolve(executable, -2, "_CCCrypt") == services.address("_CCCrypt"));
    assert(graph.resolve(executable, -2, "_CC_MD5") == services.address("_CC_MD5"));
    assert(graph.resolve(executable, -2, "_CC_SHA1") == services.address("_CC_SHA1"));
    assert(graph.resolve(executable, -2, "_kSecAttrAccount").has_value());
    const auto cfTrue = graph.resolve(executable, -2, "_kCFBooleanTrue");
    assert(cfTrue.has_value() && memory.read32(*cfTrue) != 0);
    uint8_t randomBytes[16] = {};
    memory.write(0x3000, randomBytes, sizeof(randomBytes));
    const CpuState random = call(services, "_SecRandomCopyBytes", 0, sizeof(randomBytes), 0x3000);
    assert(random.registers[0] == 0);
    memory.read(0x3000, randomBytes, sizeof(randomBytes));
    bool randomWasFilled = false;
    for (uint8_t byte : randomBytes) randomWasFilled = randomWasFilled || byte != 0;
    assert(randomWasFilled);
    // Each gate is defined under the library that exports it on iOS, so strict
    // ordinals from a real binary resolve instead of failing at bind time.
    // Ordinals number the dependency list the binary declares; each gate lives in the
    // library that really exports it.
    graph.dependencies(
        executable,
        {"/usr/lib/libSystem.B.dylib", "/usr/lib/libobjc.A.dylib", "/usr/lib/libstdc++.6.dylib",
         "/usr/lib/libgcc_s.1.dylib", "/System/Library/Frameworks/Foundation.framework/Foundation",
         "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
         "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
         "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
         "/System/Library/Frameworks/UIKit.framework/UIKit",
         "/System/Library/Frameworks/OpenGLES.framework/OpenGLES", "/usr/lib/libc++.1.dylib",
         "/System/Library/Frameworks/AVFoundation.framework/AVFoundation",
         "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration"});
    assert(graph.resolve(executable, 3, "_Znwm") == services.address("_Znwm"));
    assert(graph.resolve(executable, 11, "__Znwm") == services.address("__Znwm"));
    assert(graph.resolve(executable, 3, "__ZNSt8ios_base4InitC1Ev") ==
           services.address("__ZNSt8ios_base4InitC1Ev"));
    assert(graph.resolve(executable, 3, "__ZNSsC1EPKcRKSaIcE") ==
           services.address("__ZNSsC1EPKcRKSaIcE"));
    assert(graph.resolve(executable, 4, "___divsi3") == services.address("___divsi3"));
    assert(graph.resolve(executable, 4, "__Unwind_SjLj_Register") ==
           services.address("__Unwind_SjLj_Register"));
    assert(graph.resolve(executable, 1, "__Unwind_SjLj_Register") ==
           services.address("__Unwind_SjLj_Register"));
    assert(graph.resolve(executable, 1, "dyld_stub_binder") ==
           services.address("dyld_stub_binder"));
    const auto stackGuard = graph.resolve(executable, 1, "___stack_chk_guard");
    assert(stackGuard && memory.read32(*stackGuard) != 0);
    assert(graph.resolve(executable, 11, "___cxa_pure_virtual") ==
           services.address("___cxa_pure_virtual"));
    assert(graph.resolve(executable, 11, "___cxa_begin_catch") ==
           services.address("___cxa_begin_catch"));
    assert(graph.resolve(
               executable, 11,
               "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm") ==
           services.address(
               "__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm"));
    assert(!graph.resolve(executable, 3, "___divsi3"));
    assert(graph.resolve(executable, 6, "_CFRelease") == services.address("_CFRelease"));
    assert(graph.resolve(executable, 6, "_CFRunLoopGetCurrent") ==
           services.address("_CFRunLoopGetCurrent"));
    assert(graph.resolve(executable, 7, "_CGColorSpaceCreateDeviceRGB") ==
           services.address("_CGColorSpaceCreateDeviceRGB"));
    assert(graph.resolve(executable, 7, "_CGBitmapContextCreate") ==
           services.address("_CGBitmapContextCreate"));
    assert(graph.resolve(executable, 7, "_CGImageGetWidth") ==
           services.address("_CGImageGetWidth"));
    assert(graph.resolve(executable, 13, "_SCNetworkReachabilityCreateWithName") ==
           services.address("_SCNetworkReachabilityCreateWithName"));
    assert(graph.resolve(executable, 13, "_SCNetworkReachabilityGetFlags") ==
           services.address("_SCNetworkReachabilityGetFlags"));
    const auto avAudioSessionClass = graph.resolve(executable, 12, "_OBJC_CLASS_$_AVAudioSession");
    assert(avAudioSessionClass && memory.read32(*avAudioSessionClass) == *avAudioSessionClass);
    const auto ambientCategory = graph.resolve(executable, 12, "_AVAudioSessionCategoryAmbient");
    assert(ambientCategory);
    const uint32_t ambientRecord = memory.read32(*ambientCategory);
    const uint32_t ambientBytes = memory.read32(ambientRecord + 8);
    assert(memory.read32(ambientRecord + 12) == std::strlen("AVAudioSessionCategoryAmbient"));
    char ambientText[32]{};
    memory.read(ambientBytes, ambientText, sizeof(ambientText));
    assert(std::string(ambientText) == "AVAudioSessionCategoryAmbient");
    // libobjc.A.dylib exports the runtime entry points.
    // objc_msgSend is a runtime gate, not a plain service symbol.
    assert(graph.resolve(executable, 2, "_objc_msgSend") == ObjC::GateBase);
    assert(graph.resolve(executable, 5, "_OBJC_CLASS_$_NSString").has_value());
    const auto recursiveLockClass = graph.resolve(executable, 5, "_OBJC_CLASS_$_NSRecursiveLock");
    assert(recursiveLockClass.has_value());
    // Exercise the actual Objective-C allocation and lock selectors, not only their exports.
    CpuState mainGuestThread;
    services.adoptMainThread(mainGuestThread);
    auto selectorAt = [&](uint32_t address, const char* selector) {
        memory.write(address, selector, std::strlen(selector) + 1);
        return address;
    };
    const uint32_t allocSelector = selectorAt(0x3500, "alloc");
    const uint32_t initSelector = selectorAt(0x3510, "init");
    const uint32_t lockSelector = selectorAt(0x3520, "lock");
    const uint32_t tryLockSelector = selectorAt(0x3530, "tryLock");
    const uint32_t unlockSelector = selectorAt(0x3540, "unlock");
    const uint32_t releaseSelector = selectorAt(0x3550, "release");
    CpuState recursiveLock = objcCall(services, *recursiveLockClass, allocSelector);
    const uint32_t lockObject = recursiveLock.registers[0];
    assert(lockObject != 0);
    assert(objcCall(services, lockObject, initSelector).registers[0] == lockObject);
    assert(objcCall(services, lockObject, lockSelector).registers[0] == 0);
    assert(objcCall(services, lockObject, lockSelector).registers[0] == 0);
    assert(objcCall(services, lockObject, tryLockSelector).registers[0] == 1);
    assert(objcCall(services, lockObject, unlockSelector).registers[0] == 0);
    assert(objcCall(services, lockObject, unlockSelector).registers[0] == 0);
    assert(objcCall(services, lockObject, unlockSelector).registers[0] == 0);
    assert(objcCall(services, lockObject, releaseSelector).registers[0] == lockObject);

    // NSRecursiveLock delegates to this same cooperative mutex scheduler. Check recursive
    // acquisition, contention, handoff on unlock, and rejection of a non-owner unlock.
    Memory schedulerMemory;
    std::vector<uint8_t> schedulerCode(Memory::PageSize, 0), schedulerData(Memory::PageSize, 0);
    schedulerMemory.map({0x2000, Memory::PageSize, uint8_t(Read | Execute), "worker code"},
                        schedulerCode.data(), schedulerCode.size());
    schedulerMemory.map({0x3000, Memory::PageSize, uint8_t(Read | Write), "mutex storage"},
                        schedulerData.data(), schedulerData.size());
    Posix scheduler(schedulerMemory);
    CpuState schedulerState;
    scheduler.adoptMainThread(schedulerState);
    constexpr uint32_t recursiveMutexAddress = 0x3000;
    scheduler.initializeRecursiveMutex(recursiveMutexAddress);
    assert(scheduler.lockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.lockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.tryLockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.lockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.spawnThread(0x2000, 0) == 2);
    assert(scheduler.switchThreads(schedulerState));
    assert(scheduler.tryLockMutex(recursiveMutexAddress) == 16);  // Darwin EBUSY
    assert(scheduler.lockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.wantsSwitch() && scheduler.runnableCount() == 1);
    assert(scheduler.switchThreads(schedulerState));  // The contending worker blocks.
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.wantsSwitch());
    assert(scheduler.switchThreads(schedulerState));  // Unlock hands ownership to the worker.
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 0);
    assert(scheduler.switchThreads(schedulerState));
    assert(scheduler.unlockMutex(recursiveMutexAddress) == 1);  // Darwin EPERM
    scheduler.destroyInternalMutex(recursiveMutexAddress);

    // Foundation's own C entry points bind through the Foundation image.
    assert(graph.resolve(executable, 5, "_NSLog"));
    assert(graph.resolve(executable, 10, "_glColorMask") == services.address("_glColorMask"));
    assert(graph.resolve(executable, 10, "_glGenTextures") == services.address("_glGenTextures"));
    // The full implemented GLES import table is registered as call gates, including names
    // outside the hand-maintained legacy subset.
    assert(graph.resolve(executable, 10, "_glReadPixels") == services.address("_glReadPixels"));
    fault("EXPORT_IMAGE", [&] { graph.addImage("<executable>"); });

    ExportGraph deps;
    size_t exe = deps.addImage("<exe>");
    size_t system = deps.addImage("/usr/lib/libSystem.B.dylib");
    deps.alias(system, "/usr/lib/libSystem.dylib");
    deps.dependencies(exe, {"/usr/lib/libSystem.dylib"});
    deps.define(system, "_puts", {0x1234, false});
    assert(deps.resolve(exe, 1, "_puts") == 0x1234u);
    assert(!deps.resolve(exe, -3, "_missing"));
    fault("EXPORT_ORDINAL", [&] { deps.resolve(exe, -4, "_puts"); });
    fault("EXPORT_DEPENDENCIES", [&] { deps.dependencies(exe, {}); });

    ExportGraph weakGraph;
    size_t requester = weakGraph.addImage("<exe>");
    size_t weakImage = weakGraph.addImage("weak-image");
    size_t strongImage = weakGraph.addImage("strong-image");
    weakGraph.define(weakImage, "_mostlyEqual", {0x1111, true});
    weakGraph.define(strongImage, "_mostlyEqual", {0x2222, false});
    assert(weakGraph.resolve(requester, -2, "_mostlyEqual") == 0x1111u);
    assert(weakGraph.resolve(requester, -3, "_mostlyEqual") == 0x2222u);
    assert(weakGraph.resolve(requester, -3, "_onlyWeak") == std::nullopt);

    Memory pages;
    std::vector<uint8_t> page(4096, 0);
    pages.map({0x2000, 4096, uint8_t(Read | Write), "fixups"}, page.data(), page.size());
    pages.write32(0x2008, 0xdeadbeef);
    pages.write32(0x200c, 0xdeadbeef);

    DyldPlan coalesced;
    coalesced.needsCoalescing = true;
    coalesced.fixups.push_back({0x2000, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.fixups.push_back({0x2004, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.fixups.push_back({0x2000, false, false, false, -3, 0, "_mostlyEqual"});
    coalesced.applyWithGraph(pages, 0, weakGraph, requester);
    assert(pages.read32(0x2000) == 0x2222 && pages.read32(0x2004) == 0x2222);
    assert(pages.read32(0x2008) == 0xdeadbeef);

    // A slot that is both rebased and bound is a lazy pointer, not a conflict: dyld rebases
    // first and binds second, so the binding is the value the slot must end up with.
    DyldPlan rebasedAndBound;
    rebasedAndBound.fixups.push_back({0x2008, true, false, false, 0, 0, ""});
    rebasedAndBound.fixups.push_back({0x2008, false, false, false, -3, 0, "_mostlyEqual"});
    rebasedAndBound.applyWithGraph(pages, 0, weakGraph, requester);
    assert(pages.read32(0x2008) == 0x2222);
    DyldPlan conflict;
    conflict.fixups.push_back({0x2008, false, false, false, -2, 0, "_mostlyEqual"});
    conflict.fixups.push_back({0x2008, false, false, false, -2, 0, "_onlyWeak"});
    fault("DYLD_TARGET", [&] { conflict.applyWithGraph(pages, 0, weakGraph, requester); });
    DyldPlan addendConflict;
    addendConflict.needsCoalescing = true;
    addendConflict.fixups.push_back({0x2008, false, false, false, -3, 0, "_mostlyEqual"});
    addendConflict.fixups.push_back({0x2008, false, false, false, -3, 4, "_mostlyEqual"});
    fault("DYLD_TARGET", [&] { addendConflict.applyWithGraph(pages, 0, weakGraph, requester); });
    assert(pages.read32(0x2008) == 0x2222);  // the earlier merge already wrote that slot

    DyldPlan missing;
    missing.fixups.push_back({0x200c, false, false, false, -2, 0, "_notImplemented"});
    fault("DYLD_UNRESOLVED", [&] { missing.applyWithGraph(pages, 0, weakGraph, requester); });
    assert(pages.read32(0x200c) == 0xdeadbeef);

    ExportGraph weakOnly;
    size_t onlyWeak = weakOnly.addImage("weak-only-image");
    weakOnly.define(onlyWeak, "_mostlyEqual", {0x1111, true});
    DyldPlan undeclaredStrong;
    undeclaredStrong.needsCoalescing = true;
    undeclaredStrong.strongDefinitions.push_back("_mostlyEqual");
    fault("DYLD_WEAK_DEFINITION",
          [&] { undeclaredStrong.applyWithGraph(pages, 0, weakOnly, onlyWeak); });

    DyldPlan absentWeak;
    absentWeak.fixups.push_back({0x200c, false, true, false, -3, 0, "_absentWeak"});
    absentWeak.applyWithGraph(pages, 0, weakGraph, requester);
    assert(pages.read32(0x200c) == 0);

    const char source[] = "hello service";
    memory.write(0x3000, source, sizeof(source));
    memory.write(0x3100, "hello", 6);
    memory.write(0x3200, "hellp", 6);
    auto state = call(services, "_strlen", 0x3000);
    assert(state.registers[0] == 13);
    state = call(services, "_strcmp", 0x3100, 0x3100);
    assert(state.registers[0] == 0);
    state = call(services, "_strcmp", 0x3100, 0x3200);
    assert(int32_t(state.registers[0]) < 0);
    state = call(services, "_memcpy", 0x3300, 0x3000, 14);
    assert(state.registers[0] == 0x3300);
    std::vector<uint8_t> copied(14);
    memory.read(0x3300, copied.data(), copied.size());
    assert(std::memcmp(copied.data(), source, 14) == 0);
    state = call(services, "_memcmp", 0x3300, 0x3000, 14);
    assert(state.registers[0] == 0);
    state = call(services, "_memset", 0x3400, 0x5a, 8);
    assert(state.registers[0] == 0x3400);
    memory.read(0x3400, copied.data(), 8);
    for (int i = 0; i < 8; i++) assert(copied[i] == 0x5a);
    state = call(services, "_bzero", 0x3400, 8);
    memory.read(0x3400, copied.data(), 8);
    for (int i = 0; i < 8; i++) assert(copied[i] == 0);
    fault("LIBC_OVERLAP", [&] { call(services, "_memcpy", 0x3100, 0x3102, 8); });
    fault("LIBC_LIMIT", [&] { call(services, "_memset", 0x3100, 1, 32 * 1024 * 1024); });

    state = call(services, "_memcpy", 0x3500, 0x3100, 4, 0x1001);
    assert(state.registers[15] == 0x1000 && (state.cpsr & 0x20));
    state = call(services, "_memcpy", 0x3500, 0x3100, 4, 0x1000);
    assert(state.registers[15] == 0x1000 && !(state.cpsr & 0x20));
    fault("HLE_RETURN", [&] { call(services, "_memcpy", 0x3500, 0x3100, 4, 0x9001); });

    CpuState forged;
    forged.registers[14] = 0x1000;
    forged.registers[15] = 0x1004;
    CpuStop intercepted{StopReason::SupervisorCall, 1, 0x1000, 1, 0, "CPU_SVC", ""};
    fault("HLE_TRAP", [&] { services.dispatch(intercepted, forged); });
    CpuStop wrongNumber{StopReason::SupervisorCall, 1, memcpyGate, 2, 0, "CPU_SVC", ""};
    fault("HLE_TRAP", [&] { services.dispatch(wrongNumber, forged); });
    CpuStop wrongMode = wrongNumber;
    wrongMode.service = 1;
    forged.cpsr = 0x30;
    fault("HLE_TRAP", [&] { services.dispatch(wrongMode, forged); });
    std::cout << "Export graph, weak coalescing, call gates and service dispatch tests passed\n";
}
