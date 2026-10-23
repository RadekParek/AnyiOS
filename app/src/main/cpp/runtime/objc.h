#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cpu.h"
#include "exports.h"
#include "gles.h"
#include "memory.h"

namespace podnative::runtime {
// Session-owned Objective-C runtime core. Classes, instances and selectors are guest
// addresses; the object store lives on the host and is never exposed as host pointers.
// Only implemented classes/selectors resolve: anything else is a hard failure.
class ObjC final {
   public:
    explicit ObjC(Memory& memory);
    void installGates();
    // `foundation`, `objcLibrary` and `quartzCore` are export-graph image indices; runtime
    // entries are registered under the library that really exports them on iOS.
    void defineSymbols(ExportGraph& graph, size_t foundation, size_t objcLibrary,
                       size_t quartzCore) const;
    bool dispatch(uint32_t pc, CpuState& state);
    size_t gateCount() const { return entries.size(); }
    // The class objects this runtime models, by name. The export graph registers each
    // class symbol under the framework that really exports it.
    const std::map<std::string, uint32_t>& classObjects() const { return classes; }
    // Call-gate address for a runtime entry, or 0 when the runtime does not export it.
    uint32_t gateAddress(const std::string& entry) const;
    // Guest sandbox roots used by NSBundle/NSHomeDirectory/NSSearchPath. They are guest
    // paths; the file probe decides whether a bundle resource actually exists.
    void setGuestPaths(std::string bundle, std::string documents);
    void setFileProbe(std::function<bool(const std::string&)> probe) {
        fileExists = std::move(probe);
    }
    void setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
    // Names the unimplemented import a call-gate/trap address stands for, so a message sent
    // to a trap says which API is missing instead of "not an object".
    void setTrapName(std::function<std::string(uint32_t)> lookup) { trapName = std::move(lookup); }
    // How many guest threads are alive, for NSThread's own class methods: -isMultiThreaded
    // answers with the scheduler's real state instead of a constant.
    void setLiveThreads(std::function<size_t()> count) { liveThreads = std::move(count); }
    // Starting an NSThread is a real guest thread: the scheduler creates it and this runtime
    // enters the target's selector on it. The hook returns false when no scheduler is wired.
    // A call another guest thread asked the main thread to run. The run loop drains the
    // queue; a waiting caller stays blocked until the main thread has run its entry.
    struct MainThreadTask {
        uint32_t target = 0;
        uint32_t argument = 0;
        uint32_t eventId = 0;
        std::string selector;
    };
    void performOnMainThread(uint32_t target, const std::string& selector, uint32_t argument,
                             bool wait);
    bool nextMainThreadTask(MainThreadTask& task);
    void finishMainThreadTask(uint32_t eventId);
    void setMainThreadHooks(std::function<void(uint32_t)> block, std::function<void(uint32_t)> wake,
                            std::function<void()> signal) {
        blockOnEvent = std::move(block);
        wakeEvent = std::move(wake);
        signalRunLoop = std::move(signal);
    }
    void setThreadSpawn(std::function<bool(uint32_t, const std::string&, uint32_t)> spawn) {
        spawnThread = std::move(spawn);
    }
    // The GL bridge backing EAGL. EAGLContext drawable/present calls run real EGL/GLES;
    // without a driver they fail by name instead of pretending a frame exists.
    void setGraphics(GLES* graphics) { gles = graphics; }
    // Guest log/exit lifecycle: releasing the pool stack at exit is real bookkeeping.
    void drainPools();
    // Classes the image defines itself, from its `__objc_classlist`. Their metadata stays
    // guest-owned; the runtime reads it in place, so a class the title looks up by name is
    // the same object its own code uses.
    void registerGuestClasses(const std::vector<uint32_t>& classList);
    std::string guestClassSummary() const;
    // Instantiates a guest class the way +alloc does: a zeroed object of the class's own
    // instance size whose isa points at the class object.
    uint32_t allocateGuestInstance(uint32_t classAddress);
    // Sends a message to a guest receiver from host code (the UIApplicationMain bootstrap).
    // Returns true when guest code was entered: registers[15] then holds the entry point and
    // registers[0..3] the arguments, so the caller must not overwrite the PC.
    bool sendGuestMessage(uint32_t receiver, const std::string& selectorName, CpuState& state,
                          const std::vector<uint32_t>& arguments, uint32_t returnAddress);
    // Guest selectors are their own C strings; this interns one for host-initiated sends.
    uint32_t selectorString(const std::string& name);
    // Resolves a class name for the host bootstrap (guest class first, then runtime class).
    uint32_t classAddress(const std::string& name, bool required) {
        return resolveClass(name, required);
    }
    // Old apps may pass a nil UIApplicationMain delegate because their main NIB creates it.
    // Until NIB archives are decoded, a single unambiguous image-defined *AppDelegate class
    // that implements one documented launch selector is the only safe fallback.
    std::pair<std::string, std::string> inferMainNibDelegate() const;
    bool guestClassDefined(uint32_t address) const { return guestClassAt(address) != nullptr; }
    // Whether a guest instance (or class object) implements a selector, as -respondsToSelector:
    // answers and as UIKit checks before calling an optional delegate method.
    bool respondsTo(uint32_t receiver, const std::string& selectorName) const {
        const GuestClass* cls = guestClassAt(receiver);
        const bool instance = guestInstances.count(receiver) != 0;
        if (!cls && !instance) return false;
        const uint32_t target = instance ? guestInstances.at(receiver) : receiver;
        return guestMethod(target, cls != nullptr, selectorName) != 0;
    }
    // Super dispatch for a guest receiver: lookup starts at the superclass of the class
    // that contained the `super` call, then continues into the runtime's own classes.
    bool guestSuperMessage(uint32_t receiver, uint32_t classOfMethod, uint32_t selectorAddress,
                           CpuState& state);
    // Name of a runtime-owned class object, or empty when the address is not one.
    std::string hostClassName(uint32_t address) const;
    // Instantiates one of the runtime's own classes (UIApplication and friends).
    uint32_t makeHostObject(const std::string& className) { return makeObject(className); }
    uint32_t makeHostBoolean(bool value);
    // Reads a string object or string literal; refused by name when it is neither.
    std::string stringValue(uint32_t object) { return stringOf(object); }
    static constexpr uint32_t GateBase = 0x6f200000;

   private:
    struct Object {
        std::string className;
        uint32_t references = 1;
        uint32_t text = 0;  // NSString payload: pointer to a terminated guest string.
        // Collection payloads are host-owned so a guest cannot corrupt them with a
        // stray store; every element is a guest object address.
        std::vector<uint32_t> elements;                    // NSArray / NSMutableArray
        std::vector<std::pair<uint32_t, uint32_t>> pairs;  // NSDictionary / NSMutableDictionary
        std::vector<uint8_t> bytes;                        // NSData
        int64_t integer = 0;                               // NSNumber
        double real = 0;
        // UIKit payload. A view's frame and bounds are guest-visible state, so they are
        // stored and reported back rather than invented on each read.
        bool hasFrame = false;
        float frame[4] = {0, 0, 0, 0};
        float bounds[4] = {0, 0, 0, 0};
        float alpha = 1;
        float contentScale = 1;
        bool hidden = false, userInteraction = true, multipleTouch = false;
        uint32_t superview = 0, window = 0, backgroundColor = 0;
        uint32_t rootViewController = 0, delegate = 0, keyWindow = 0;
        // A view's backing layer. Its class follows the view's own +layerClass override
        // (CAEAGLLayer for an OpenGL ES view), so the drawable properties reach the layer
        // kind the title asked for.
        uint32_t layer = 0;
        // NSThread payload: what -initWithTarget:selector:object: was given and whether
        // -start has already created the guest thread for it.
        uint32_t threadTarget = 0, threadArgument = 0;
        std::string threadSelector;
        bool threadStarted = false, threadCancelled = false, threadFinished = false;
    };
    Memory& memory;
    std::vector<std::string> entries;
    std::map<std::string, uint32_t> classes;
    std::map<uint32_t, Object> objects;
    std::vector<std::vector<uint32_t>> pools;
    // NSUserDefaults standardUserDefaults: session-scoped store so a title that reads
    // back what it wrote behaves correctly, without inventing a persisted file yet.
    std::vector<std::pair<uint32_t, uint32_t>> defaults;
    uint32_t nextObject = 0x6f400000;
    uint32_t nextText = 0x6f480000;
    uint32_t nextBytes = 0x6f4c0000;
    uint32_t userDefaults = 0;
    uint32_t applicationObject = 0;   // +[UIApplication sharedApplication]
    uint32_t mainScreen = 0;          // +[UIScreen mainScreen]
    uint32_t accelerometer = 0;       // +[UIAccelerometer sharedAccelerometer]
    uint32_t currentEaglContext = 0;  // +[EAGLContext setCurrentContext:]
    uint32_t mainBundle = 0;
    std::map<uint32_t, uint32_t> syncDepth;  // @synchronized nesting per object
    struct GuestClass {
        uint32_t address = 0, meta = 0, superclass = 0, instanceSize = 0;
        std::string name;
        bool modern = true;  // class_ro_t metadata rather than the legacy class layout
    };
    std::map<std::string, GuestClass> guestClasses;
    std::map<uint32_t, uint32_t> guestInstances;  // instance address -> class address
    // View state for instances whose class the image defines but which inherit from a
    // UIView/UIWindow the runtime models.
    std::map<uint32_t, Object> guestViews;
    std::map<uint32_t, uint32_t> guestRefs;  // instance address -> retain count
    uint32_t nextGuestObject = 0x6f500000;
    bool guestArena = false;
    // A class's superclass is an extern bind, not a rebase, so it is still zero when the
    // image is first registered (dyld has not run yet). Read it from guest memory on use so
    // a class defined by the image inherits from UIView once fixups are applied.
    uint32_t superclassOf(const GuestClass& info) const;
    void traceSend(const std::string& receiver, const std::string& method);
    // Name of the nearest runtime-modelled class in an image-defined class's superclass
    // chain, or an empty string when the chain has none. Inherited methods such as
    // -[UIView setMultipleTouchEnabled:] live there, not in the guest's own method lists.
    std::string hostAncestorOf(uint32_t classAddress) const;
    // Reads the class's method table (only methods, not caches) and walks the superclass
    // chain. Returns 0 when no class in the guest chain implements the selector.
    void mapGuestArena();
    // A __CFConstantString literal in the image, recognised by its own layout.
    std::optional<std::string> literalString(uint32_t address) const;
    std::string stringOf(uint32_t object);
    bool stringMessage(const std::string& text, uint32_t bytes, const std::string& method,
                       CpuState& state);
    // UIKit: application, window, view, screen and colour semantics. Each answer is stored
    // object state or a documented constant, never a fabricated success.
    bool uiKitMessage(const std::string& className, uint32_t object, const std::string& method,
                      CpuState& state);
    // Guest class, then a runtime class, else a named failure (never a silent nil).
    uint32_t resolveClass(const std::string& name, bool required);
    uint32_t guestMethod(uint32_t classAddress, bool classMethod, const std::string& name) const;
    // Enters a guest IMP: r0/r1 are self and _cmd and the PC moves to the implementation.
    bool enterGuest(uint32_t imp, CpuState& state, uint32_t self, uint32_t cmd);
    bool guestChainContains(uint32_t classAddress, uint32_t candidate) const;
    // Finds a guest class by the address of its class object or of its metaclass.
    const GuestClass* guestClassAt(uint32_t address) const;
    std::string guestClassName(uint32_t classAddress) const;
    // Dispatches a message whose receiver is a guest class object or a guest instance.
    // Returns true when the message was handled (guest code may have been entered).
    bool guestMessage(uint32_t receiver, uint32_t selectorAddress, CpuState& state);
    // Fallback for messages a guest object inherits from NSObject: those semantics are the
    // runtime's, not the image's.
    bool guestFoundation(uint32_t receiver, const std::string& className, const std::string& method,
                         CpuState& state);
    std::string bundlePath = "/var/mobile", documentsPath = "/var/mobile/Documents";
    std::function<bool(const std::string&)> fileExists;
    std::function<std::string(uint32_t)> trapName;
    std::function<size_t()> liveThreads;
    std::function<bool(uint32_t, const std::string&, uint32_t)> spawnThread;
    uint32_t currentThreadObject = 0;
    GLES* gles = nullptr;
    std::function<void(const std::string&)> logSink;
    // Unique receiver/selector pairs already logged: the trace then reads as the call graph
    // the guest built, without pushing the interesting lines out of the log ring.
    std::set<std::string> tracedSends;
    std::deque<MainThreadTask> mainThreadTasks;
    uint32_t nextEventId = 1;
    std::function<void(uint32_t)> blockOnEvent;
    std::function<void(uint32_t)> wakeEvent;
    std::function<void()> signalRunLoop;
    uint32_t makeString(const std::string& text);
    uint32_t makeArray(const std::vector<uint32_t>& elements);
    void emitLog(const std::string& text) const;
    bool bundleMessage(const std::string& method, CpuState& state);
    bool foundationEntry(const std::string& entry, CpuState& state);

    void mapRange(uint32_t base, uint32_t size, const char* label);
    uint32_t classNamePointer(const std::string& name);
    uint32_t makeObject(const std::string& className);
    uint32_t internText(const std::string& text);
    uint32_t internBytes(const std::vector<uint8_t>& bytes);
    std::string classNameOf(uint32_t object) const;
    std::string describe(uint32_t object) const;
    std::string readText(uint32_t address) const;
    // Selector bytes as a guest string; shared by the NSThread entry points.
    std::string selectorText(uint32_t address) const;
    std::string selector(uint32_t address) const;
    uint32_t classObject(const std::string& name);
    void invoke(const std::string& entry, CpuState& state);
    bool collection(const std::string& className, const std::string& method, Object& object,
                    CpuState& state);
    uint32_t textOf(uint32_t object) const;
    void message(CpuState& state);
    void release(uint32_t object);
};
}  // namespace podnative::runtime
