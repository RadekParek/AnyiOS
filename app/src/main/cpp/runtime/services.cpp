#include "services.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

#include "error.h"

namespace podnative::runtime {
namespace {
bool repetitiveServiceTrace(const std::string& name) {
    static const std::set<std::string> hot = {"_malloc",
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
                                              "_mach_absolute_time",
                                              "_gettimeofday",
                                              "_CFAbsoluteTimeGetCurrent",
                                              "_CACurrentMediaTime"};
    return hot.count(name) || name.rfind("___mod", 0) == 0 || name.rfind("___div", 0) == 0 ||
           name.rfind("__aeabi_", 0) == 0 || name.rfind("__Unwind_SjLj_", 0) == 0;
}
}  // namespace

Services::Services(Memory& m)
    : memory(m),
      libc(m),
      cxx(m, libc.guestHeap()),
      stdio(m, libc.guestHeap(), files),
      audio(m),
      posix(m),
      openal(m),
      objc(m),
      gles(m, &libc.guestHeap()),
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
          "_vsprintf",
          "_vprintf",
          "_puts",
          "_getenv",
          "_mach_absolute_time",
          "_mach_timebase_info",
          "dyld_stub_binder",
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
          "_SecRandomCopyBytes",
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
          "_CFDataCreate",
          "_CFDataGetBytePtr",
          "_CFDataGetLength",
          "_CFArrayCreate",
          "_CFArrayCreateMutable",
          "_CFArrayCreateMutableCopy",
          "_CFArrayCreateCopy",
          "_CFArrayGetCount",
          "_CFArrayGetValueAtIndex",
          "_CFArrayGetValues",
          "_CFArrayAppendValue",
          "_CFArrayInsertValueAtIndex",
          "_CFArraySetValueAtIndex",
          "_CFArrayRemoveValueAtIndex",
          "_CFArrayRemoveAllValues",
          "_CFArrayRetainValueCallBack",
          "_CFArrayReleaseValueCallBack",
          "_CFArrayEqualValueCallBack",
          "_CFStringConvertEncodingToNSStringEncoding",
          "_CFRunLoopGetCurrent",
          "_CFRunLoopGetMain",
          "_CFAbsoluteTimeGetCurrent",
          "_CFRetain",
          "_CFRelease",
          // SystemConfiguration reachability is explicitly offline in this sandbox; valid
          // callbacks and scheduling are retained as state without a fabricated online event.
          "_SCNetworkReachabilityCreateWithAddress",
          "_SCNetworkReachabilityCreateWithAddressPair",
          "_SCNetworkReachabilityCreateWithName",
          "_SCNetworkReachabilityGetFlags",
          "_SCNetworkReachabilityScheduleWithRunLoop",
          "_SCNetworkReachabilitySetCallback",
          "_SCNetworkReachabilityUnscheduleFromRunLoop",
          // CoreGraphics geometry and colour spaces, plus QuartzCore media time.
          "_CGAffineTransformMakeTranslation",
          "_CGAffineTransformMakeRotation",
          "_CGAffineTransformTranslate",
          "_CGAffineTransformRotate",
          "_CGAffineTransformScale",
          "_CGRectApplyAffineTransform",
          "_CGRectContainsPoint",
          "_CGRectEqualToRect",
          "_CGRectGetHeight",
          "_CGRectGetMinX",
          "_CGRectGetMinY",
          "_CGRectGetMidX",
          "_CGRectGetMidY",
          "_CGRectGetWidth",
          "_CGRectInset",
          "_CGRectOffset",
          "_CGColorSpaceCreateDeviceRGB",
          "_CGColorSpaceCreateDeviceGray",
          "_CGColorSpaceRetain",
          "_CGColorSpaceRelease",
          "_CGBitmapContextCreate",
          "_CGBitmapContextCreateImage",
          "_CGContextClearRect",
          "_CGContextDrawImage",
          "_CGContextRelease",
          "_CGContextRetain",
          "_CGContextTranslateCTM",
          "_CGImageGetHeight",
          "_CGImageGetWidth",
          "_CGImageRelease",
          "_CGImageRetain",
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
          "___dynamic_cast",
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
          "___floatdisf",
          "___floatundisf",
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
          // ctype: Darwin's case-mapping and classification entry points.
          "___tolower",
          "___toupper",
          "_tolower",
          "_toupper",
          "___maskrune",
          // Strings.
          "_strncat",
          "_strlcpy",
          "_strndup",
          "_strspn",
          "_strcspn",
          "_strpbrk",
          "_strcasestr",
          "_strcoll",
          "_strtok",
          "_strsep",
          "_memmem",
          "_strerror",
          "___error",
          "_setlocale",
          "_localeconv",
          "_setjmp",
          "_sigsetjmp",
          "setjmp",
          "_longjmp",
          "_siglongjmp",
          "longjmp",
          // POSIX threads and scheduling, plus the gate a returning thread routine lands on.
          "___podnative_thread_exit",
          "___podnative_thread_callback",
          "_pthread_create",
          // The rest of pthread's API: a second thread needs these as soon as it starts.
          "_pthread_attr_init",
          "_pthread_attr_destroy",
          "_pthread_attr_setstacksize",
          "_pthread_attr_getstacksize",
          "_pthread_attr_setdetachstate",
          "_pthread_detach",
          "_pthread_equal",
          "_pthread_kill",
          "_nanosleep",
          "_sleep",
          "_pthread_join",
          "_pthread_mutex_init",
          "_pthread_mutex_destroy",
          "_pthread_mutex_lock",
          "_pthread_mutex_trylock",
          "_pthread_mutex_unlock",
          "_pthread_mutexattr_init",
          "_pthread_mutexattr_destroy",
          "_pthread_mutexattr_settype",
          "_pthread_self",
          "_pthread_getschedparam",
          "_pthread_setschedparam",
          "_pthread_exit",
          // Process and scheduling.
          "_getpid",
          "_getuid",
          "_geteuid",
          "_system",
          "_usleep",
          "_sched_yield",
          "_clock",
          // Time.
          "_difftime",
          "_gmtime",
          "_localtime",
          "_gmtime_r",
          "_localtime_r",
          "_mktime",
          "_timegm",
          "_strftime",
          "_tzset",
          // libm, both precisions. iOS ARMv7 passes floating-point arguments in the core
          // registers, so every one of these is marshalled through the shared ABI helpers.
          "_tan",
          "_asin",
          "_acos",
          "_atan",
          "_sinh",
          "_cosh",
          "_tanh",
          "_exp",
          "_exp2",
          "_log",
          "_log10",
          "_log2",
          "_sqrt",
          "_cbrt",
          "_ceil",
          "_floor",
          "_round",
          "_trunc",
          "_fabs",
          "_asinf",
          "_sinhf",
          "_coshf",
          "_tanhf",
          "_expf",
          "_exp2f",
          "_logf",
          "_log10f",
          "_log2f",
          "_sqrtf",
          "_cbrtf",
          "_ceilf",
          "_floorf",
          "_roundf",
          "_truncf",
          "_fabsf",
          "_expm1f",
          "_log1pf",
          "_atan2",
          "_hypot",
          "_copysign",
          "_remainder",
          "_fmin",
          "_fmax",
          "_fmodf",
          "_hypotf",
          "_copysignf",
          "_remainderf",
          "_fminf",
          "_fmaxf",
          "_frexp",
          "_frexpf",
          "_ldexp",
          "_ldexpf",
          "_modf",
          "_modff",
          // Standard I/O beyond the streaming primitives.
          "_freopen",
          "_ungetc",
          "_sscanf",
          "_fscanf",
          "_perror",
          "_setbuf",
          "_tmpfile",
          "_tmpnam",
          // POSIX file descriptors over the same sandboxed handles as stdio.
          "_open",
          "_creat",
          "_read",
          "_write",
          "_close",
          "_lseek",
          "_fcntl",
          "_select",
          "_fsync",
          "_abort",
          // OpenAL: the null output device, with real buffers, queues and playback timing.
          "_alcOpenDevice",
          "_alcCloseDevice",
          "_alcCreateContext",
          "_alcDestroyContext",
          "_alcMakeContextCurrent",
          "_alcProcessContext",
          "_alcSuspendContext",
          "_alcGetContextsDevice",
          "_alcGetError",
          "_alcGetIntegerv",
          "_alGetError",
          "_alGenBuffers",
          "_alDeleteBuffers",
          "_alGenSources",
          "_alDeleteSources",
          "_alBufferData",
          "_alSourceQueueBuffers",
          "_alSourceUnqueueBuffers",
          "_alSourcePlay",
          "_alSourcePause",
          "_alSourceStop",
          "_alSourceRewind",
          "_alSourcePlayv",
          "_alGetSourcei",
          "_alGetSourcef",
          "_alSourcei",
          "_alSourcef",
          "_alSource3f",
          "_alGetBufferi",
          "_alListenerf",
          "_alListener3f",
          "_alListenerfv",
          "_alGetListenerf",
          "_alDistanceModel",
          "_alDopplerFactor",
          "_alSpeedOfSound",
          // AudioToolbox AudioSession configuration (state, not playback).
          "_AudioSessionInitialize",
          "_AudioSessionSetActive",
          "_AudioSessionSetProperty",
          "_AudioSessionGetProperty",
          "_AudioSessionGetPropertySize",
          // QuartzCore transforms and CoreGraphics' own CA bridge.
          "_CATransform3DMakeRotation",
          "_CATransform3DMakeTranslation",
          "_CATransform3DRotate",
          "_CATransform3DTranslate",
          "_CATransform3DScale",
          // UIKit process bootstrap: argv parsing, delegate instantiation and the launch
          // callback, with a real return path through the lifecycle gate.
          "_UIApplicationMain",
          // Guest stdio over the mounted sandbox.
          "_fopen",
          "_fclose",
          "_fflush",
          "_fprintf",
          "_vfprintf",
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
          // CoreGraphics/GLES: real calls into the loaded driver, never no-ops.
          "_glActiveTexture",
          "_glBindBuffer",
          "_glBindFramebufferOES",
          "_glBindRenderbufferOES",
          "_glBindTexture",
          "_glBlendFunc",
          "_glBufferData",
          "_glCheckFramebufferStatusOES",
          "_glClear",
          "_glClearColor",
          "_glClientActiveTexture",
          "_glColor4f",
          "_glColorMask",
          "_glColorPointer",
          "_glCompressedTexImage2D",
          "_glDeleteBuffers",
          "_glDeleteFramebuffersOES",
          "_glDeleteRenderbuffersOES",
          "_glDeleteTextures",
          "_glDepthFunc",
          "_glDepthMask",
          "_glDisable",
          "_glDisableClientState",
          "_glDrawArrays",
          "_glDrawElements",
          "_glEnable",
          "_glEnableClientState",
          "_glFramebufferRenderbufferOES",
          "_glFramebufferTexture2DOES",
          "_glFrontFace",
          "_glGenBuffers",
          "_glGenFramebuffersOES",
          "_glGenRenderbuffersOES",
          "_glGenTextures",
          "_glGetIntegerv",
          "_glGetRenderbufferParameterivOES",
          "_glLightfv",
          "_glLineWidth",
          "_glLoadMatrixf",
          "_glMaterialfv",
          "_glMatrixMode",
          "_glNormalPointer",
          "_glPixelStorei",
          "_glRenderbufferStorageOES",
          "_glScissor",
          "_glTexCoordPointer",
          "_glTexEnvi",
          "_glTexImage2D",
          "_glTexParameteri",
          "_glTexSubImage2D",
          "_glVertexPointer",
          "_glViewport",
      } {
    // The C++ runtime's dispatcher already knows these ABI spellings. Register every one as
    // an actual call gate too; classifying a symbol's library without exporting its gate left
    // ios_base::Init and both string ABIs bound to the missing-import trap.
    for (const std::string& symbol : cxx.symbols())
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(symbol);
    // GLES::invoke already implements every name in its import table. Register the complete
    // table here as call gates, not only the older hand-selected subset, so dyld can bind a
    // symbol such as _glFinish before the guest reaches its first frame.
    for (const std::string& symbol : gles.symbols())
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(symbol);
    for (const std::string& symbol : commonCrypto.symbols())
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
            symbols.push_back(symbol);
    // Two entries for one symbol would define it twice in the same image, which the export
    // graph refuses; catching it here names the symbol instead of failing the launch.
    {
        std::set<std::string> unique(symbols.begin(), symbols.end());
        if (unique.size() != symbols.size())
            throw Error("HLE_LIMIT", "duplicate service entry point",
                        uint32_t(symbols.size() - unique.size()));
    }
    const size_t gateBytes = symbols.size() * 8;
    const size_t mappedGateBytes =
        (gateBytes + Memory::PageSize - 1) & ~(size_t(Memory::PageSize) - 1);
    // The second service page follows the first. Keep the RTTI ABI page at 0x6f002000
    // disjoint; the CoreGraphics constants live on a separate page below.
    if (!mappedGateBytes || mappedGateBytes > 0x2000)
        throw Error("HLE_LIMIT", "service gates exceed their reserved two-page region",
                    uint32_t(symbols.size()));
    std::vector<uint8_t> code(mappedGateBytes);
    auto put = [&](size_t offset, uint32_t word) {
        for (unsigned n = 0; n < 4; n++) code[offset + n] = uint8_t(word >> (n * 8));
    };
    for (size_t n = 0; n < symbols.size(); n++) {
        put(n * 8, 0xef000000 | uint32_t(n + 1));
        put(n * 8 + 4, 0xe12fff1e);
    }
    // Remaining slots are undefined instructions, not zero-filled ARM no-ops.
    for (size_t n = gateBytes; n < code.size(); n += 4) put(n, 0xe7f000f0);
    memory.map({Base, uint32_t(mappedGateBytes), uint8_t(Read | Execute), "AnyiOS HLE call gates"},
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
                         0,
                         0,
                         0,
                         0,
                         0};
    constexpr uint32_t CoreGraphicsConstantsBase = Base + 0x4000;
    memory.map({CoreGraphicsConstantsBase, Memory::PageSize, Read, "CoreGraphics constants"},
               reinterpret_cast<const uint8_t*>(constants), sizeof(constants));
    posix.setExitGate(address("___podnative_thread_exit"));
    cg.setHeapHooks(
        [this](uint32_t bytes, uint32_t alignment) {
            return libc.guestHeap().allocate(bytes, alignment);
        },
        [this](uint32_t address) { libc.guestHeap().release(address); });
    objc.setTrapName([this](uint32_t address) { return trapSymbol(address); });
    objc.setGraphics(&gles);
    systemConfiguration.setRunLoopValidators(
        [this](uint32_t runLoop) { return cf.isRunLoopObject(runLoop); },
        [this](uint32_t mode) { return objc.isStringValue(mode); });
    cf.setExternalObjectHooks(
        [this](uint32_t value) {
            return objc.ownsObject(value) || systemConfiguration.owns(value);
        },
        [this](uint32_t value) {
            if (objc.ownsObject(value))
                objc.retainObject(value);
            else
                systemConfiguration.retain(value);
        },
        [this](uint32_t value) {
            if (objc.ownsObject(value))
                objc.releaseObject(value);
            else
                systemConfiguration.release(value);
        },
        [this](uint32_t left, uint32_t right) {
            return (objc.ownsObject(left) && objc.ownsObject(right) &&
                    objc.equalObjects(left, right)) || systemConfiguration.equal(left, right);
        });
    objc.setRecursiveLockHooks(
        [this] {
            const uint32_t storage = libc.guestHeap().allocate(8, 8);
            if (!storage)
                throw Error("OBJC_LOCK", "guest heap cannot allocate NSRecursiveLock state");
            posix.initializeRecursiveMutex(storage);
            return storage;
        },
        [this](uint32_t mutex) { return posix.lockMutex(mutex); },
        [this](uint32_t mutex) { return posix.tryLockMutex(mutex); },
        [this](uint32_t mutex) { return posix.unlockMutex(mutex); },
        [this](uint32_t mutex) {
            posix.destroyInternalMutex(mutex);
            libc.guestHeap().release(mutex);
        });
    objc.installGates();
    uikit.installGates();
    uikit.setThreadHooks([this] { return posix.liveThreadCount(); },
                         [this] { posix.blockInRunLoop(); });
    objc.setMainThreadHooks([this](uint32_t event) { posix.blockOnEvent(event); },
                            [this](uint32_t event) { posix.wakeEvent(event); },
                            [this] { posix.wakeRunLoopThread(); });
    objc.setLiveThreads([this] { return posix.threadCount(); });
    objc.setThreadSpawn([this](uint32_t target, const std::string& selector, uint32_t argument) {
        return spawnCallbackThread(target, selector, argument);
    });
    // Install the in-memory console sink before any guest callback can run. This also
    // prevents Objective-C or audio diagnostics from falling back to host stdout when a
    // caller does not explicitly provide an external logger.
    setLogSink({});
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
    static const std::string systemConfiguration =
        "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration";
    static const std::string coreGraphics =
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
    static const std::string quartzCore =
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
    static const std::string audioToolbox =
        "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox";
    static const std::string openAL = "/System/Library/Frameworks/OpenAL.framework/OpenAL";
    static const std::string openGLES = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES";
    static const std::string standard = "/usr/lib/libstdc++.6.dylib";
    static const std::string libcxx = "/usr/lib/libc++.1.dylib";
    static const std::string helpers = "/usr/lib/libgcc_s.1.dylib";
    static const std::string objc = "/usr/lib/libobjc.A.dylib";
    static const std::string security = "/System/Library/Frameworks/Security.framework/Security";
    static const std::string commonCrypto = "/usr/lib/libcommonCrypto.dylib";
    static const std::string uiKit = "/System/Library/Frameworks/UIKit.framework/UIKit";
    if (symbol.rfind("_UI", 0) == 0) return uiKit;
    if (symbol.rfind("_SCNetworkReachability", 0) == 0) return systemConfiguration;
    if (symbol.rfind("_CF", 0) == 0) return coreFoundation;
    if (symbol.rfind("_CG", 0) == 0) return coreGraphics;
    if (symbol.rfind("_CA", 0) == 0 || symbol.rfind("_kCA", 0) == 0) return quartzCore;
    if (symbol.rfind("_AudioSession", 0) == 0) return audioToolbox;
    // OpenAL's entry points all start with al/alc followed by a capitalised verb. The image
    // is imported by ordinal, so a gate defined anywhere else never binds.
    for (const char* prefix :
         {"_alc", "_alBuffer", "_alDelete", "_alGen", "_alSource", "_alGetSource", "_alGetBuffer",
          "_alGetListener", "_alListener", "_alDistanceModel", "_alDoppler", "_alSpeedOfSound"})
        if (symbol.rfind(prefix, 0) == 0) return openAL;
    if (symbol.rfind("_gl", 0) == 0) return openGLES;
    if (symbol.rfind("_objc_", 0) == 0 || symbol == "___objc_personality_v0") return objc;
    if (symbol.rfind("_CC", 0) == 0) return commonCrypto;
    if (symbol.rfind("_Sec", 0) == 0 || symbol.rfind("_kSec", 0) == 0) return security;
    if (cxx.isLibcxxSymbol(symbol)) return libcxx;
    if (cxx.isCxxSymbol(symbol)) return standard;
    if (cxx.isCompilerHelper(symbol)) return helpers;
    if (symbol.rfind("_NS", 0) == 0) return foundation;
    return system;
}
void Services::install(ExportGraph& graph, const std::vector<std::string>& dependencies) {
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
    auto systemConfiguration =
        imageFor("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration");
    auto coreGraphics = imageFor("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics");
    auto quartzCore = imageFor("/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
    auto audioToolbox = imageFor("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox");
    auto avFoundation = imageFor("/System/Library/Frameworks/AVFoundation.framework/AVFoundation");
    auto objcLibrary = imageFor("/usr/lib/libobjc.A.dylib");
    const size_t security = imageFor("/System/Library/Frameworks/Security.framework/Security");
    const size_t commonCrypto = imageFor("/usr/lib/libcommonCrypto.dylib");
    for (const char* alias : {"/usr/lib/libCommonCrypto.dylib",
                              "/usr/lib/system/libcommonCrypto.dylib"})
        if (!graph.index(alias)) graph.alias(commonCrypto, alias);
    // libc++ has its own two-level namespace owner. Accept both historic install-name aliases
    // before registering the image's dependency list, so either ordinal reaches the same gates.
    size_t libcxx;
    if (auto existing = graph.index("/usr/lib/libc++.1.dylib"))
        libcxx = *existing;
    else if (auto existing = graph.index("/usr/lib/libc++.dylib"))
        libcxx = *existing;
    else
        libcxx = graph.addImage("/usr/lib/libc++.1.dylib");
    if (!graph.index("/usr/lib/libc++.1.dylib")) graph.alias(libcxx, "/usr/lib/libc++.1.dylib");
    if (!graph.index("/usr/lib/libc++.dylib")) graph.alias(libcxx, "/usr/lib/libc++.dylib");
    // Every framework the image links against must exist in the graph, or its two-level
    // ordinals cannot resolve at all. Register them all, in the binary's own order.
    for (const auto& dependency : dependencies) imageFor(dependency);
    // The image binds an import by ordinal, so a definition in the *right* library is what
    // makes it resolve. Most names have exactly one owner; the few C++ runtime entry points
    // this SDK generation exports from both libstdc++ and libgcc_s are listed here, so a bind
    // that names either ordinal still finds the implementation. Everything else stays
    // single-owner: defining every gate everywhere would make a name resolve from a library
    // that does not export it, which is exactly the class of bug the audit exists to catch.
    const size_t helpers = imageFor("/usr/lib/libgcc_s.1.dylib");
    const size_t standard = imageFor("/usr/lib/libstdc++.6.dylib");
    // The exception matcher reads these target ABI data objects directly; exporting the
    // missing vtable symbols as call gates would put executable code where type_info expects
    // data and still leave derived-to-base catches broken.
    cxx.installTypeInfoVtables(graph, standard);
    for (size_t n = 0; n < symbols.size(); n++)
        graph.define(imageFor(libraryOf(symbols[n])), symbols[n], {Base + uint32_t(n) * 8});
    // Older iOS SDKs also expose CommonCrypto through Security.framework. Keep these names
    // available from either two-level ordinal without making them global exports.
    for (const std::string& symbol : this->commonCrypto.symbols()) {
        const auto found = std::find(symbols.begin(), symbols.end(), symbol);
        if (found == symbols.end()) throw Error("HLE_SYMBOL", "CommonCrypto gate is missing");
        const uint32_t gate = Base + uint32_t(found - symbols.begin()) * 8;
        if (!graph.strong(security, symbol)) graph.define(security, symbol, {gate});
    }
    // Which library a shared name is defined in follows the titles, not a guess. AngryBirds
    // 1.0 binds ___cxa_atexit with the libSystem.B ordinal and ___gxx_personality_sj0 with
    // libstdc++'s: on this SDK generation libSystem re-exports the C ABI half of the C++
    // runtime but not the personality routine. A bind whose ordinal names a library the graph
    // has no definition in resolves to the unimplemented-import trap, so the owner list per
    // name is what keeps a working implementation reachable.
    const std::pair<const char*, std::vector<size_t>> sharedNames[] = {
        {"__Znwm", {standard, libcxx}},
        {"_Znwm", {standard, libcxx}},
        {"__Znam", {standard, libcxx}},
        {"_Znam", {standard, libcxx}},
        {"__ZdlPv", {standard, libcxx}},
        {"_ZdlPv", {standard, libcxx}},
        {"__ZdaPv", {standard, libcxx}},
        {"_ZdaPv", {standard, libcxx}},
        {"___cxa_atexit", {standard, helpers, system, libcxx}},
        {"___cxa_finalize", {standard, helpers, system, libcxx}},
        {"___cxa_allocate_exception", {standard, libcxx}},
        {"___cxa_free_exception", {standard, libcxx}},
        {"___cxa_throw", {standard, libcxx}},
        {"___cxa_begin_catch", {standard, libcxx}},
        {"___cxa_end_catch", {standard, libcxx}},
        {"___cxa_rethrow", {standard, libcxx}},
        {"___cxa_call_unexpected", {standard, libcxx}},
        {"___cxa_bad_typeid", {standard, libcxx}},
        {"___cxa_pure_virtual", {standard, libcxx}},
        {"___dynamic_cast", {standard, libcxx}},
        {"___cxa_guard_acquire", {standard, libcxx}},
        {"___cxa_guard_release", {standard, libcxx}},
        {"___cxa_guard_abort", {standard, libcxx}},
        {"___gxx_personality_sj0", {standard, helpers, libcxx}},
        // These SjLj ABI helpers are re-exported by libSystem on the SDK used by several
        // ARMv7 games as well as by libgcc_s. Their single service implementation must be
        // visible through either two-level namespace.
        {"__Unwind_SjLj_Register", {helpers, system}},
        {"__Unwind_SjLj_Unregister", {helpers, system}},
        {"__Unwind_SjLj_Resume", {helpers, system}},
    };
    for (const auto& [sharedName, owners] : sharedNames) {
        const auto found = std::find(symbols.begin(), symbols.end(), sharedName);
        if (found == symbols.end()) continue;
        const uint32_t address = Base + uint32_t(found - symbols.begin()) * 8;
        for (size_t image : owners)
            if (!graph.strong(image, sharedName)) graph.define(image, sharedName, {address});
    }
    const size_t uiKit = imageFor("/System/Library/Frameworks/UIKit.framework/UIKit");
    const size_t openGLES = imageFor("/System/Library/Frameworks/OpenGLES.framework/OpenGLES");
    objc.defineSymbols(graph, foundation, objcLibrary, quartzCore);
    // A class and metaclass symbol is exported by the framework that owns the class, and a
    // two-level bind names that image by ordinal. Defining every class in Foundation left a
    // UIKit-ordinal bind of `_OBJC_CLASS_$_UIScreen` unresolved, so the guest messaged the
    // unimplemented-import trap instead of the class object.
    auto classOwner = [&](const std::string& name) -> size_t {
        if (name == "AVAudioSession") return avFoundation;
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
    // A few classes are bound through an image that re-exports them rather than defining
    // them, so the two-level lookup has to find them in both places.
    const std::set<std::string> reexportedByCoreFoundation{"NSObject", "NSProxy"};
    const std::set<std::string> reexportedByQuartzCore{"CAEAGLLayer"};
    for (const auto& [name, address] : objc.classObjects()) {
        const size_t owner = classOwner(name);
        auto define = [&](size_t image) {
            const std::string classSymbol = "_OBJC_CLASS_$_" + name;
            if (graph.strong(image, classSymbol)) return;
            graph.define(image, classSymbol, {address});
            graph.define(image, "_OBJC_METACLASS_$_" + name, {address});
        };
        define(owner);
        if (bridged.count(name) || reexportedByCoreFoundation.count(name)) define(coreFoundation);
        if (reexportedByQuartzCore.count(name)) define(quartzCore);
    }
    // libobjc's own data symbols: a class carries an empty cache and vtable until its first
    // message. This runtime never dereferences them, but the image binds them, so they point
    // at real mapped memory instead of an unimplemented-import trap.
    for (const char* symbol : {"__objc_empty_cache", "__objc_empty_vtable"})
        graph.define(objcLibrary, symbol, {ObjC::GateBase + Memory::PageSize - 16});
    // Real data cells, not gate code: the standard-stream `FILE *` variables hold the
    // address of the stream object, and Darwin's rune tables are the real classification
    // data a ctype macro indexes.
    // The Mach-O spelling of the C variable `__stdinp` carries the image's own leading
    // underscore, so the symbol the guest imports is `___stdinp`. Defining `__stdinp`
    // resolved nothing and left three required imports trapped.
    graph.define(system, "___stdinp", {stdio.standardPointerCell(0)});
    graph.define(system, "___stdoutp", {stdio.standardPointerCell(1)});
    graph.define(system, "___stderrp", {stdio.standardPointerCell(2)});
    const uint32_t stackGuardCell = libc.guestHeap().allocate(4, 4);
    if (!stackGuardCell) throw Error("HLE_ALLOC", "could not allocate the stack guard cell");
    std::random_device stackRandom;
    uint32_t stackGuard = uint32_t(stackRandom()) ^ (uint32_t(stackRandom()) << 16);
    if (!stackGuard) stackGuard = 0x59a5d00d;
    memory.write32(stackGuardCell, stackGuard);
    if (!graph.strong(system, "___stack_chk_guard"))
        graph.define(system, "___stack_chk_guard", {stackGuardCell});
    graph.define(system, "__DefaultRuneLocale", {runeLocale()});
    // EAGL's drawable keys and colour-format values are CFString globals. A title reads the
    // global and hands the string to -setValue:forKey:, so the value must be a real string.
    graph.define(openGLES, "_kEAGLColorFormatRGBA8", {constantStringVariable("RGBA8")});
    graph.define(openGLES, "_kEAGLColorFormatRGB565", {constantStringVariable("RGB565")});
    graph.define(openGLES, "_kEAGLDrawablePropertyColorFormat",
                 {constantStringVariable("ColorFormat")});
    graph.define(openGLES, "_kEAGLDrawablePropertyRetainedBacking",
                 {constantStringVariable("RetainedBacking")});
    graph.define(coreFoundation, "___CFConstantStringClassReference",
                 {objc.classAddress("NSConstantString", false)});
    // SDK globals that are CFStringRef/NSString* constants, not functions. Games pass these
    // through collection, notification, locale and run-loop APIs, so each needs a valid
    // guest-side constant-string cell rather than an executable gate.
    for (const auto& [image, symbol, text] : {
             std::tuple<size_t, const char*, const char*>{foundation, "_NSDefaultRunLoopMode",
                                                          "NSDefaultRunLoopMode"},
             {foundation, "_NSFilePosixPermissions", "NSFilePosixPermissions"},
             {foundation, "_NSGregorianCalendar", "gregorian"},
             {foundation, "_NSKeyValueChangeNotificationIsPriorKey", "notificationIsPrior"},
             {foundation, "_NSLocaleCountryCode", "country"},
             {foundation, "_NSLocaleCurrencyCode", "currency"},
             {foundation, "_NSLocalizedDescriptionKey", "NSLocalizedDescription"},
             {foundation, "_NSMachErrorDomain", "NSMachErrorDomain"},
             {foundation, "_NSPOSIXErrorDomain", "NSPOSIXErrorDomain"},
             {foundation, "_NSUnderlyingErrorKey", "NSUnderlyingError"},
             {foundation, "_NSInvalidArgumentException", "NSInvalidArgumentException"},
             {foundation, "_NSInternalInconsistencyException", "NSInternalInconsistencyException"},
             {coreFoundation, "_kCFRunLoopDefaultMode", "kCFRunLoopDefaultMode"},
             {coreFoundation, "_kCFBundleIdentifierKey", "CFBundleIdentifier"},
             {coreFoundation, "_kCFBundleVersionKey", "CFBundleVersion"},
             {avFoundation, "_AVAudioSessionCategoryAmbient", "AVAudioSessionCategoryAmbient"},
             {avFoundation, "_AVAudioSessionCategorySoloAmbient",
              "AVAudioSessionCategorySoloAmbient"},
             {avFoundation, "_AVAudioSessionCategoryPlayback", "AVAudioSessionCategoryPlayback"},
             {avFoundation, "_AVAudioSessionCategoryRecord", "AVAudioSessionCategoryRecord"},
             {avFoundation, "_AVAudioSessionCategoryPlayAndRecord",
              "AVAudioSessionCategoryPlayAndRecord"},
             {avFoundation, "_AVAudioSessionCategoryMultiRoute",
              "AVAudioSessionCategoryMultiRoute"},
             {avFoundation, "_AVAudioSessionModeDefault", "AVAudioSessionModeDefault"},
             {quartzCore, "_kCAFillModeForwards", "forwards"},
             {quartzCore, "_kCAMediaTimingFunctionLinear", "linear"},
             {security, "_kSecAttrAccessible", "pdmn"},
             {security, "_kSecAttrAccessibleWhenUnlocked", "ak"},
             {security, "_kSecAttrAccount", "acct"},
             {security, "_kSecAttrGeneric", "genp"},
             {security, "_kSecAttrService", "svce"},
             {security, "_kSecClass", "class"},
             {security, "_kSecClassGenericPassword", "genp"},
             {security, "_kSecMatchLimit", "m_Limit"},
             {security, "_kSecMatchLimitOne", "m_LimitOne"},
             {security, "_kSecReturnData", "r_Data"},
             {security, "_kSecValueData", "v_Data"},
             {uiKit, "_UIApplicationDidBecomeActiveNotification",
              "UIApplicationDidBecomeActiveNotification"},
             {uiKit, "_UIApplicationWillEnterForegroundNotification",
              "UIApplicationWillEnterForegroundNotification"},
             {uiKit, "_UIApplicationWillResignActiveNotification",
              "UIApplicationWillResignActiveNotification"},
             {uiKit, "_UIApplicationWillTerminateNotification",
              "UIApplicationWillTerminateNotification"},
         })
        if (!graph.strong(image, symbol))
            graph.define(image, symbol, {constantStringVariable(text)});
    if (!graph.strong(coreFoundation, "_NSDefaultRunLoopMode"))
        graph.define(coreFoundation, "_NSDefaultRunLoopMode",
                     {constantStringVariable("NSDefaultRunLoopMode")});
    // Core Foundation's default allocator is the null handle. UIKit's invalid background-task
    // token is zero; both are data cells whose contents are read by the guest.
    const uint32_t zeroCell = libc.guestHeap().allocate(4, 4);
    if (!zeroCell) throw Error("HLE_ALLOC", "could not allocate a constant data cell");
    memory.write32(zeroCell, 0);
    if (!graph.strong(coreFoundation, "_kCFAllocatorDefault"))
        graph.define(coreFoundation, "_kCFAllocatorDefault", {zeroCell});
    if (!cfTypeArrayCallbacks) {
        cfTypeArrayCallbacks = libc.guestHeap().allocate(20, 4);
        if (!cfTypeArrayCallbacks)
            throw Error("HLE_ALLOC", "could not allocate kCFTypeArrayCallBacks");
        memory.write32(cfTypeArrayCallbacks, 0);  // CFArrayCallBacks.version
        memory.write32(cfTypeArrayCallbacks + 4, address("_CFArrayRetainValueCallBack"));
        memory.write32(cfTypeArrayCallbacks + 8, address("_CFArrayReleaseValueCallBack"));
        memory.write32(cfTypeArrayCallbacks + 12, 0);  // optional description callback
        memory.write32(cfTypeArrayCallbacks + 16, address("_CFArrayEqualValueCallBack"));
    }
    cf.setTypeArrayCallbacksAddress(cfTypeArrayCallbacks);
    if (!graph.strong(coreFoundation, "_kCFTypeArrayCallBacks"))
        graph.define(coreFoundation, "_kCFTypeArrayCallBacks", {cfTypeArrayCallbacks});
    const uint32_t trueCell = libc.guestHeap().allocate(4, 4);
    const uint32_t falseCell = libc.guestHeap().allocate(4, 4);
    if (!trueCell || !falseCell) throw Error("HLE_ALLOC", "could not allocate CFBoolean cells");
    memory.write32(trueCell, objc.makeHostBoolean(true));
    memory.write32(falseCell, objc.makeHostBoolean(false));
    if (!graph.strong(coreFoundation, "_kCFBooleanTrue"))
        graph.define(coreFoundation, "_kCFBooleanTrue", {trueCell});
    if (!graph.strong(coreFoundation, "_kCFBooleanFalse"))
        graph.define(coreFoundation, "_kCFBooleanFalse", {falseCell});
    if (!graph.strong(security, "_kSecRandomDefault"))
        graph.define(security, "_kSecRandomDefault", {zeroCell});
    if (!graph.strong(uiKit, "_UIBackgroundTaskInvalid"))
        graph.define(uiKit, "_UIBackgroundTaskInvalid", {zeroCell});
    constexpr uint32_t CoreGraphicsConstantsBase = Base + 0x4000;
    graph.define(coreGraphics, "_CGAffineTransformIdentity", {CoreGraphicsConstantsBase});
    graph.define(coreGraphics, "_CGRectNull", {CoreGraphicsConstantsBase + 24});
    graph.define(coreGraphics, "_CGRectZero", {CoreGraphicsConstantsBase + 40});
    // Keep the handles referenced: the images must exist even when nothing is defined in
    // them yet, because the binary's ordinals index this table.
    (void)system;
    (void)foundation;
    (void)coreFoundation;
    (void)systemConfiguration;
    (void)audioToolbox;
    (void)security;
}
uint32_t Services::constantStringVariable(const std::string& text) {
    // A CFString global is a variable holding a pointer to a __CFConstantString: a class
    // word, the 0x7c8 flag word, the bytes and the length. The runtime recognises literals by
    // that layout, so a guest that passes one of these to a framework call sees a real string.
    GuestHeap& heap = libc.guestHeap();
    const uint32_t bytes = heap.allocate(uint32_t(text.size() + 1), 4);
    if (!bytes) throw Error("HLE_ALLOC", "could not allocate a guest constant string");
    memory.write(bytes, text.c_str(), text.size() + 1);
    const uint32_t record = heap.allocate(16, 4);
    if (!record) throw Error("HLE_ALLOC", "could not allocate a __CFConstantString");
    memory.write32(record, 0);          // class reference: never dereferenced by this runtime
    memory.write32(record + 4, 0x7c8);  // 8-bit constant string
    memory.write32(record + 8, bytes);
    memory.write32(record + 12, uint32_t(text.size()));
    const uint32_t variable = heap.allocate(4, 4);
    if (!variable) throw Error("HLE_ALLOC", "could not allocate a CFString global");
    memory.write32(variable, record);
    return variable;
}
uint32_t Services::runeLocale() {
    if (runeLocaleCell) return runeLocaleCell;
    // _RuneLocale (Darwin runetype.h): char __magic[8]; char __encoding[32];
    // int (*__sgetrune)(); int (*__sputrune)(); __darwin_rune_t __invalid_rune;
    // __uint32_t __runetype[256]; __darwin_rune_t __maplower[256]; __darwin_rune_t __mapupper[256];
    // union { ... } __lc;
    constexpr uint32_t Runetype = 52, Maplower = 52 + 1024, Mapupper = 52 + 2048, TableSize = 4096;
    runeLocaleCell = libc.guestHeap().allocate(TableSize, 4);
    if (!runeLocaleCell) throw Error("HLE_ALLOC", "could not allocate the guest rune tables");
    std::vector<uint8_t> table(TableSize, 0);
    auto put32 = [&table](uint32_t offset, uint32_t value) {
        for (unsigned n = 0; n < 4; n++) table[offset + n] = uint8_t(value >> (n * 8));
    };
    std::memcpy(table.data(), "RuneMagi", 8);
    std::memcpy(table.data() + 8, "NONE", 4);
    put32(48, 0xfffd);  // __invalid_rune
    for (uint32_t c = 0; c < 256; c++) {
        uint32_t bits = 0;
        if (std::isalpha(int(c))) bits |= 0x00000100;   // _CTYPE_A
        if (std::iscntrl(int(c))) bits |= 0x00000200;   // _CTYPE_C
        if (std::isdigit(int(c))) bits |= 0x00000400;   // _CTYPE_D
        if (std::isgraph(int(c))) bits |= 0x00000800;   // _CTYPE_G
        if (std::islower(int(c))) bits |= 0x00001000;   // _CTYPE_L
        if (std::ispunct(int(c))) bits |= 0x00002000;   // _CTYPE_P
        if (std::isspace(int(c))) bits |= 0x00004000;   // _CTYPE_S
        if (std::isupper(int(c))) bits |= 0x00008000;   // _CTYPE_U
        if (std::isxdigit(int(c))) bits |= 0x00010000;  // _CTYPE_X
        if (c == ' ' || c == '\t') bits |= 0x00020000;  // _CTYPE_B
        if (std::isprint(int(c))) bits |= 0x00040000;   // _CTYPE_R
        put32(Runetype + c * 4, bits ? bits | 0x01000000 : 0);
        // The high byte records the character class Darwin's table carries as a rune type.
        put32(Maplower + c * 4, uint32_t(std::tolower(int(c))));
        put32(Mapupper + c * 4, uint32_t(std::toupper(int(c))));
    }
    memory.write(runeLocaleCell, table.data(), table.size());
    return runeLocaleCell;
}
void Services::mountSandbox(const std::string& bundleHost, const std::string& containerHost,
                            const std::string& bundleArchive,
                            const std::string& bundleArchivePrefix, const std::string& bundleCache,
                            const std::string& bundleOverlay) {
    if (containerHost.empty() || (bundleHost.empty() == bundleArchive.empty()))
        throw Error("FS_MOUNT", "mount exactly one app bundle source and a writable container");
    files.mount("/var/mobile/Documents", containerHost, true);
    if (!bundleArchive.empty()) {
        if (bundleArchivePrefix.empty() || bundleCache.empty() || bundleOverlay.empty())
            throw Error("FS_MOUNT", "IPA mounts need an app root, file cache and write overlay");
        files.mountArchive("/var/mobile", bundleArchive, bundleArchivePrefix, bundleCache,
                           bundleOverlay, true);
    } else {
        // Extracted .app imports and pre-existing bundle caches stay directory-backed. New IPA
        // imports use a ZIP index and materialize only entries the guest actually opens.
        files.mount("/var/mobile", bundleHost, true);
    }
    // The bundle is writable on purpose. iOS 3 titles wrote caches next to their resources
    // (AngryBirds creates data/bundleIndex.idx on first run). For an IPA, writes live in the
    // private overlay; the original ZIP and its resources remain unchanged.
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
    // GL calls have a signature-aware trace in GLES; printing their raw r0-r3 here too
    // duplicates the information and mislabels VFP/stack arguments.
    if (verbose && symbols[entry].rfind("_gl", 0) != 0) traceSymbol(symbols[entry], state);
    invoke(symbols[entry], state);
    // Same rule as above: a service that starts a guest callback keeps the PC it set.
    if (state.registers[15] == stop.pc + 4) {
        state.registers[15] = returnAddress;
        state.cpsr = (state.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
    }
}
bool Services::spawnCallbackThread(uint32_t target, const std::string& selector,
                                   uint32_t argument) {
    const uint32_t id = nextThreadStart++;
    threadStarts[id] = ThreadStart{target, selector, argument};
    // The created thread starts on the trampoline gate: the gate runs on that thread, so the
    // selector really is entered with the new thread's own stack and registers.
    const uint32_t gate = address("___podnative_thread_callback");
    posix.spawnThread(gate, id);
    return true;
}
void Services::runThreadStart(CpuState& state) {
    const uint32_t id = state.registers[0];
    auto found = threadStarts.find(id);
    if (found == threadStarts.end())
        throw Error("POSIX_THREAD", "a thread trampoline ran without a pending start", id);
    const ThreadStart start = found->second;  // copy: the map entry is erased below
    threadStarts.erase(found);
    if (!objc.sendGuestMessage(start.target, start.selector, state, {start.argument},
                               address("___podnative_thread_exit")))
        throw Error("OBJC_UNIMPLEMENTED", "thread target " + objc.stringValue(start.target) +
                                              " does not implement " + start.selector);
}
void Services::log(const std::string& line) {
    guestLog.push_back(line);
    if (guestLog.size() > logCapacity) guestLog.erase(guestLog.begin());
    if (externalLog) externalLog(line);
}
std::vector<std::string> Services::logTail(size_t count) const {
    if (count >= guestLog.size()) return guestLog;
    return {guestLog.end() - std::ptrdiff_t(count), guestLog.end()};
}
std::string Services::traceSummary() const {
    if (serviceTraceSuppressed.empty()) return {};
    std::ostringstream out;
    out << "\nSERVICE_TRACE_SUMMARY: repetitive service calls were sampled; totals and suppressed "
           "counts follow:";
    for (const auto& [name, suppressed] : serviceTraceSuppressed) {
        const auto found = serviceTraceCalls.find(name);
        const size_t total = found == serviceTraceCalls.end() ? suppressed : found->second;
        out << "\n  " << name << ": " << total << " call(s), " << (total - suppressed) << " shown, "
            << suppressed << " summarized";
    }
    return out.str();
}
void Services::traceSymbol(const std::string& name, const CpuState& state) {
    const size_t occurrence = ++serviceTraceCalls[name];
    const size_t limit = repetitiveServiceTrace(name) ? 3 : 32;
    if (occurrence > limit) {
        ++serviceTraceSuppressed[name];
        return;
    }
    // Argument words as the guest passed them, and the guest stack pointer, which is what
    // makes the first few calls actionable. Hot libc/math helpers are sampled and their exact
    // totals are reported separately, rather than pushing useful later events out of the ring.
    char line[256];
    std::snprintf(line, sizeof(line),
                  "call %s(r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x) sp=0x%08x lr=0x%08x",
                  name.c_str(), state.registers[0], state.registers[1], state.registers[2],
                  state.registers[3], state.registers[13], state.registers[14]);
    log(line);
}
void Services::invoke(const std::string& name, CpuState& state) {
    // The last symbol the guest asked for, so a stop report can show the calls leading up
    // to it instead of a row of identical trap codes.
    lastSymbol = name;
    if (name == "dyld_stub_binder")
        throw Error("DYLD_LAZY_BINDER",
                    "guest entered dyld_stub_binder after startup, although this runtime eagerly "
                    "binds every lazy import before guest execution",
                    state.registers[0]);
    if (name == "_SecRandomCopyBytes") {
        const uint32_t generator = state.registers[0];
        const uint32_t count = state.registers[1];
        const uint32_t destination = state.registers[2];
        constexpr int32_t errSecParam = -50;
        constexpr int32_t errSecRandomFailure = -25291;
        if (generator || count > 1024 * 1024 ||
            (count && (!destination || !memory.permits(destination, count, Write)))) {
            state.registers[0] = uint32_t(errSecParam);
            return;
        }
        try {
            std::random_device random;
            std::vector<uint8_t> bytes(count);
            for (uint8_t& byte : bytes) byte = uint8_t(random());
            if (count) memory.write(destination, bytes.data(), bytes.size());
            state.registers[0] = 0;  // errSecSuccess
        } catch (const std::exception&) {
            state.registers[0] = uint32_t(errSecRandomFailure);
        }
        return;
    }
    if (name == "___podnative_thread_callback") {
        runThreadStart(state);
        return;
    }
    // Order matters only for diagnostics: every module reports "not mine" without
    // inventing a result, and anything unclaimed is a hard guest-visible failure.
    if (uikit.invoke(name, state, objc)) return;
    if (gles.invoke(name, state)) return;
    if (libc.invoke(name, state)) return;
    if (stdio.invoke(name, state)) return;
    if (audio.invoke(name, state)) return;
    if (openal.invoke(name, state)) return;
    if (posix.invoke(name, state)) return;
    if (cxx.invoke(name, state)) return;
    if (cf.invoke(name, state, memory)) return;
    if (cg.invoke(name, state, memory)) return;
    if (systemConfiguration.invoke(name, state, memory)) return;
    if (commonCrypto.invoke(name, state, memory)) return;
    throw Error("HLE_SYMBOL", "service has no implementation: " + name);
}
}  // namespace podnative::runtime
