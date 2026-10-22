#include "services.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <system_error>

#include "error.h"

namespace podnative::runtime {
Services::Services(Memory& m)
    : memory(m),
      libc(m),
      cxx(m, libc.guestHeap()),
      stdio(m, libc.guestHeap(), files),
      audio(m),
      objc(m),
      uikit(m),
      symbols{
          // libSystem: C runtime.
          "_malloc",
          "_calloc",
          "_realloc",
          "_free",
          "_memcpy",
          "_memmove",
          "_memset",
          "_memcmp",
          "_memchr",
          "_bzero",
          "_strlen",
          "_strcmp",
          "_strncmp",
          "_strcasecmp",
          "_strncasecmp",
          "_strcpy",
          "_strncpy",
          "_strcat",
          "_strlcat",
          "_strchr",
          "_strrchr",
          "_strstr",
          "_strdup",
          "_strerror_r",
          // Formatted output. Varargs arrive in r0-r3 and then on the stack.
          "_printf",
          "_snprintf",
          "_sprintf",
          "_vsnprintf",
          "_puts",
          "_getenv",
          "_mach_absolute_time",
          "_mach_timebase_info",
          "_sysctlbyname",
          "_strtok_r",
          "_atoi",
          "_atol",
          "_atof",
          "_strtol",
          "_strtoul",
          "_strtoll",
          "_strtoull",
          "_strtod",
          "_rand",
          "_srand",
          "_time",
          "_gettimeofday",
          "_OSAtomicCompareAndSwap32Barrier",
          "_OSAtomicAnd32OrigBarrier",
          "_OSAtomicOr32OrigBarrier",
          // libm, re-exported through libSystem.
          "_sin",
          "_cos",
          "_pow",
          "_fmod",
          "_sinf",
          "_cosf",
          "_tanf",
          "_acosf",
          "_atanf",
          "_atan2f",
          "_powf",
          // CoreFoundation.
          "_CFBitVectorCreate",
          "_CFBitVectorCreateMutableCopy",
          "_CFBitVectorGetBitAtIndex",
          "_CFBitVectorSetBitAtIndex",
          "_CFRetain",
          "_CFRelease",
          // CoreGraphics geometry and colour spaces, plus QuartzCore media time.
          "_CGAffineTransformMakeTranslation",
          "_CGAffineTransformTranslate",
          "_CGAffineTransformScale",
          "_CGRectApplyAffineTransform",
          "_CGRectEqualToRect",
          "_CGRectGetMidX",
          "_CGRectGetMidY",
          "_CGRectInset",
          "_CGColorSpaceCreateDeviceRGB",
          "_CGColorSpaceCreateDeviceGray",
          "_CGColorSpaceRetain",
          "_CGColorSpaceRelease",
          "_CACurrentMediaTime",
          // CoreFoundation bundle and URL access, so a title can locate its own assets.
          "_CFBundleGetMainBundle",
          "_CFBundleCopyResourcesDirectoryURL",
          "_CFBundleCopyBundleURL",
          "_CFURLGetFileSystemRepresentation",
          "_CFURLCopyFileSystemPath",
          // libstdc++.6.dylib and libgcc_s.1.dylib: the C++ ABI and the integer
          // helpers the ARM backend calls. Both are serviced by CxxRuntime.
          "__Znwm",
          "__Znam",
          "__ZdlPv",
          "__ZdaPv",
          "__ZSt9terminatev",
          "_Znwm",
          "_Znam",
          "_ZdlPv",
          "_ZdaPv",
          "_ZSt9terminatev",
          "___cxa_allocate_exception",
          "___cxa_free_exception",
          "___cxa_throw",
          "___cxa_begin_catch",
          "___cxa_end_catch",
          "___cxa_rethrow",
          "___cxa_call_unexpected",
          "___cxa_bad_typeid",
          "___cxa_pure_virtual",
          "___cxa_atexit",
          "___cxa_guard_acquire",
          "___cxa_guard_release",
          "___cxa_guard_abort",
          "___objc_personality_v0",
          "___divsi3",
          "___udivsi3",
          "___modsi3",
          "___umodsi3",
          "___divdi3",
          "___udivdi3",
          "___moddi3",
          "___umoddi3",
          "___floatundidf",
          "___floatdidf",
          "___fixdfdi",
          "___fixunsdfdi",
          "___gxx_personality_sj0",
          "__Unwind_SjLj_Register",
          "__Unwind_SjLj_Unregister",
          "__Unwind_SjLj_Resume",
          // libsystem_c: fortified (object-size aware) entry points.
          "_atexit",
          "_exit",
          "_Exit",
          "___assert_rtn",
          "___stack_chk_fail",
          "___memcpy_chk",
          "___memmove_chk",
          "___memset_chk",
          "___strcpy_chk",
          "___strcat_chk",
          "___strncpy_chk",
          "___sprintf_chk",
          // AudioToolbox AudioSession configuration (state, not playback).
          "_AudioSessionInitialize",
          "_AudioSessionSetActive",
          "_AudioSessionSetProperty",
          "_AudioSessionGetProperty",
          "_AudioSessionGetPropertySize",
          // QuartzCore transforms and CoreGraphics' own CA bridge.
          "_CATransform3DMakeRotation",
          "_CATransform3DScale",
          // UIKit process bootstrap: argv parsing, delegate instantiation and the launch
          // callback, with a real return path through the lifecycle gate.
          "_UIApplicationMain",
          // Guest stdio over the mounted sandbox.
          "_fopen",
          "_fclose",
          "_fflush",
          "_fprintf",
          "_fread",
          "_fwrite",
          "_fseek",
          "_ftell",
          "_rewind",
          "_feof",
          "_ferror",
          "_clearerr",
          "_fileno",
          "_fgetc",
          "_getc",
          "_fputc",
          "_putc",
          "_fgets",
          "_fputs",
          "_setvbuf",
          "_remove",
          "_rename",
      } {
    std::vector<uint8_t> code(Memory::PageSize);
    auto put = [&](size_t offset, uint32_t word) {
        for (unsigned n = 0; n < 4; n++) code[offset + n] = uint8_t(word >> (n * 8));
    };
    for (size_t n = 0; n < symbols.size(); n++) {
        put(n * 8, 0xef000000 | uint32_t(n + 1));
        put(n * 8 + 4, 0xe12fff1e);
    }
    // Remaining slots are undefined instructions, not zero-filled ARM no-ops.
    for (size_t n = symbols.size() * 8; n < code.size(); n += 4) put(n, 0xe7f000f0);
    memory.map({Base, Memory::PageSize, uint8_t(Read | Execute), "AnyiOS HLE call gates"},
               code.data(), code.size());
    float constants[] = {1,
                         0,
                         0,
                         1,
                         0,
                         0,
                         std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::infinity(),
                         0,
                         0};
    memory.map({Base + Memory::PageSize, Memory::PageSize, Read, "CoreGraphics constants"},
               reinterpret_cast<const uint8_t*>(constants), sizeof(constants));
    objc.setTrapName([this](uint32_t address) { return trapSymbol(address); });
    objc.installGates();
    uikit.installGates();
}
uint32_t Services::address(const std::string& name) const {
    auto it = std::find(symbols.begin(), symbols.end(), name);
    if (it == symbols.end()) throw Error("HLE_SYMBOL", "API has no implementation");
    return Base + uint32_t(it - symbols.begin()) * 8;
}
namespace {
// Which iOS library exports a symbol. Two-level ordinals are checked, so a gate that is
// registered under the wrong image would never bind; the mapping below is what the
// platform actually does with these families.
}  // namespace
const std::string& Services::libraryOf(const std::string& symbol) const {
    static const std::string system = "/usr/lib/libSystem.B.dylib";
    static const std::string foundation =
        "/System/Library/Frameworks/Foundation.framework/Foundation";
    static const std::string coreFoundation =
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    static const std::string coreGraphics =
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
    static const std::string quartzCore =
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
    static const std::string audioToolbox =
        "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox";
    static const std::string openGLES = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES";
    static const std::string standard = "/usr/lib/libstdc++.6.dylib";
    static const std::string helpers = "/usr/lib/libgcc_s.1.dylib";
    static const std::string objc = "/usr/lib/libobjc.A.dylib";
    static const std::string uiKit = "/System/Library/Frameworks/UIKit.framework/UIKit";
    if (symbol.rfind("_UI", 0) == 0) return uiKit;
    if (symbol.rfind("_CF", 0) == 0) return coreFoundation;
    if (symbol.rfind("_CG", 0) == 0) return coreGraphics;
    if (symbol.rfind("_CA", 0) == 0 || symbol.rfind("_kCA", 0) == 0) return quartzCore;
    if (symbol.rfind("_AudioSession", 0) == 0) return audioToolbox;
    if (symbol.rfind("_gl", 0) == 0) return openGLES;
    if (symbol.rfind("_objc_", 0) == 0) return objc;
    if (cxx.isCxxSymbol(symbol)) return standard;
    if (cxx.isCompilerHelper(symbol)) return helpers;
    if (symbol.rfind("_NS", 0) == 0) return foundation;
    return system;
}
void Services::install(ExportGraph& graph, const std::vector<std::string>& dependencies) const {
    auto imageFor = [&graph](const std::string& installName) {
        if (auto existing = graph.index(installName)) return *existing;
        return graph.addImage(installName);
    };
    auto system = imageFor("/usr/lib/libSystem.B.dylib");
    graph.alias(system, "/usr/lib/libSystem.dylib");
    graph.alias(system, "/usr/lib/libm.dylib");
    graph.alias(system, "/usr/lib/system/libsystem_c.dylib");
    auto foundation = imageFor("/System/Library/Frameworks/Foundation.framework/Foundation");
    auto coreFoundation =
        imageFor("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
    auto coreGraphics = imageFor("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
    auto quartzCore = imageFor("/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
    auto audioToolbox = imageFor("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox");
    auto objcLibrary = imageFor("/usr/lib/libobjc.A.dylib");
    // Every framework the image links against must exist in the graph, or its two-level
    // ordinals cannot resolve at all. Register them all, in the binary's own order.
    for (const auto& dependency : dependencies) imageFor(dependency);
    for (size_t n = 0; n < symbols.size(); n++)
        graph.define(imageFor(libraryOf(symbols[n])), symbols[n], {Base + uint32_t(n) * 8});
    const size_t uiKit = imageFor("/System/Library/Frameworks/UIKit.framework/UIKit");
    const size_t openGLES = imageFor("/System/Library/Frameworks/OpenGLES.framework/OpenGLES");
    objc.defineSymbols(graph, foundation, objcLibrary, quartzCore);
    // A class and metaclass symbol is exported by the framework that owns the class, and a
    // two-level bind names that image by ordinal. Defining every class in Foundation left a
    // UIKit-ordinal bind of `_OBJC_CLASS_$_UIScreen` unresolved, so the guest messaged the
    // unimplemented-import trap instead of the class object.
    auto classOwner = [&](const std::string& name) -> size_t {
        if (name.rfind("UI", 0) == 0) return uiKit;
        if (name.rfind("EAGL", 0) == 0 || name.rfind("CAEAGL", 0) == 0) return openGLES;
        if (name.rfind("CA", 0) == 0) return quartzCore;
        return foundation;
    };
    // The toll-free bridged Foundation classes are really exported by CoreFoundation on the
    // SDK generation these titles were built against: AngryBirds binds
    // `_OBJC_CLASS_$_NSDictionary` with the CoreFoundation ordinal, not Foundation's.
    const std::set<std::string> bridged{
        "NSString", "NSArray", "NSMutableArray", "NSDictionary", "NSMutableDictionary",
        "NSNumber", "NSData",  "NSSet",          "NSMutableSet", "NSDate"};
    for (const auto& [name, address] : objc.classObjects()) {
        const size_t owner = classOwner(name);
        auto define = [&](size_t image) {
            const std::string classSymbol = "_OBJC_CLASS_$_" + name;
            if (graph.strong(image, classSymbol)) return;
            graph.define(image, classSymbol, {address});
            graph.define(image, "_OBJC_METACLASS_$_" + name, {address});
        };
        define(owner);
        if (bridged.count(name)) define(coreFoundation);
    }
    // libobjc's own data symbols: a class carries an empty cache and vtable until its first
    // message. This runtime never dereferences them, but the image binds them, so they point
    // at real mapped memory instead of an unimplemented-import trap.
    for (const char* symbol : {"__objc_empty_cache", "__objc_empty_vtable"})
        graph.define(objcLibrary, symbol, {ObjC::GateBase + Memory::PageSize - 16});
    graph.define(coreGraphics, "_CGAffineTransformIdentity", {Base + Memory::PageSize});
    graph.define(coreGraphics, "_CGRectNull", {Base + Memory::PageSize + 24});
    // Keep the handles referenced: the images must exist even when nothing is defined in
    // them yet, because the binary's ordinals index this table.
    (void)system;
    (void)foundation;
    (void)coreFoundation;
    (void)audioToolbox;
}
void Services::mountSandbox(const std::string& bundleHost, const std::string& containerHost) {
    if (bundleHost.empty() || containerHost.empty())
        throw Error("FS_MOUNT", "the bundle and container directories are both required");
    files.mount("/var/mobile/Documents", containerHost, true);
    files.mount("/var/mobile", bundleHost, false);
    // NSBundle/NSHomeDirectory/NSSearchPath answer with the guest paths of those mounts,
    // and only report a resource path when the file really exists inside the sandbox.
    objc.setGuestPaths("/var/mobile", "/var/mobile/Documents");
    cf.setPaths("/var/mobile", "/var/mobile/Documents");
    objc.setFileProbe([this](const std::string& guestPath) {
        std::string reason;
        const std::string host = files.resolve(guestPath, false, reason);
        if (host.empty()) return false;
        std::error_code error;
        return std::filesystem::is_regular_file(host, error);
    });
}
void Services::setEnvironment(const std::map<std::string, std::string>& values) {
    libc.setEnvironment(values);
}
void Services::mapTrapPage(size_t index) const {
    if (index >= 32) throw Error("HLE_TRAP_LIMIT", "too many unimplemented imports");
    std::vector<uint8_t> code(Memory::PageSize, 0);
    for (size_t n = 0; n < TrapsPerPage; n++)
        for (unsigned byte = 0; byte < 8; byte++) {
            const uint32_t word = byte < 4 ? 0xef000000u : 0xe12fff1eu;
            code[n * 8 + byte] = uint8_t(word >> ((byte % 4) * 8));
        }
    const uint32_t address = TrapBase + uint32_t(index * Memory::PageSize);
    memory.map(
        {address, Memory::PageSize, uint8_t(Read | Execute), "AnyiOS unimplemented-import traps"},
        code.data(), code.size());
}
std::string Services::trapSymbol(uint32_t address) const {
    if (address < TrapBase) return {};
    const size_t index = size_t(address - TrapBase) / 8;
    return index < traps.size() ? traps[index] : std::string();
}
uint32_t Services::trapFor(const std::string& symbol) {
    auto existing = trapIndex.find(symbol);
    if (existing != trapIndex.end()) return existing->second;
    if (traps.size() >= 32 * TrapsPerPage) throw Error("HLE_TRAP_LIMIT", "too many imports");
    const size_t index = traps.size();
    if (index % TrapsPerPage == 0) mapTrapPage(index / TrapsPerPage);
    traps.push_back(symbol);
    const uint32_t address = TrapBase + uint32_t(index * 8);
    trapIndex.emplace(symbol, address);
    return address;
}
void Services::dispatch(const CpuStop& stop, CpuState& state) {
    if (stop.reason != StopReason::SupervisorCall) throw Error("HLE_TRAP", "not a service stop");
    if ((state.cpsr & 0x23f) != 0x10)
        throw Error("HLE_TRAP", "service call left ARM user mode", stop.pc);
    if (memory.fetch32(stop.pc) != (0xef000000 | stop.service) ||
        state.registers[15] != stop.pc + 4)
        throw Error("HLE_TRAP", "unregistered or inconsistent guest service trap", stop.pc);
    const uint32_t lr = state.registers[14], returnAddress = lr & ~1u;
    const bool thumb = lr & 1;
    if ((!thumb && returnAddress % 4) || !memory.permits(returnAddress, thumb ? 2 : 4, Execute))
        throw Error("HLE_RETURN", "invalid guest return address", lr);
    // A host-initiated guest callback returned into the lifecycle gate: the bootstrap decides
    // where execution continues.
    if (uikit.atLifecycleGate(stop.pc)) {
        uikit.resume(state, objc);
        return;
    }
    if (stop.pc >= TrapBase && (stop.pc - TrapBase) % 8 == 0 &&
        (stop.pc - TrapBase) / 8 < traps.size())
        throw Error("HLE_UNIMPLEMENTED",
                    "guest called unimplemented import " + traps[(stop.pc - TrapBase) / 8],
                    stop.pc);
    if (objc.dispatch(stop.pc, state)) {
        // A handler may have entered guest code itself (an Objective-C method the image
        // defines). Its PC is authoritative then; only a plain return is redirected.
        if (state.registers[15] == stop.pc + 4) {
            state.registers[15] = returnAddress;
            state.cpsr = (state.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
        }
        return;
    }
    if (stop.pc < Base || (stop.pc - Base) % 8 || (stop.pc - Base) / 8 >= symbols.size() ||
        stop.service != (stop.pc - Base) / 8 + 1)
        throw Error("HLE_TRAP", "unregistered guest service trap", stop.pc);
    const uint32_t entry = (stop.pc - Base) / 8;
    invoke(symbols[entry], state);
    // Same rule as above: a service that starts a guest callback keeps the PC it set.
    if (state.registers[15] == stop.pc + 4) {
        state.registers[15] = returnAddress;
        state.cpsr = (state.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
    }
}
void Services::invoke(const std::string& name, CpuState& state) {
    // Order matters only for diagnostics: every module reports "not mine" without
    // inventing a result, and anything unclaimed is a hard guest-visible failure.
    if (uikit.invoke(name, state, objc)) return;
    if (libc.invoke(name, state)) return;
    if (stdio.invoke(name, state)) return;
    if (audio.invoke(name, state)) return;
    if (cxx.invoke(name, state)) return;
    if (cf.invoke(name, state, memory)) return;
    if (cg.invoke(name, state, memory)) return;
    throw Error("HLE_SYMBOL", "service has no implementation: " + name);
}
}  // namespace podnative::runtime
