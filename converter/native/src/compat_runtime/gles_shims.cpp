// OpenGL ES 1.1 forwarding layer for the compat runtime.
//
// Design: the guest is an ES 1.1 fixed-function app, so its GL imports are
// forwarded to the platform's own GLES/EGL driver instead of being
// reimplemented. Android uses the device's `/system/lib*/libGLESv1_CM.so` and
// `libEGL.so` (the same hardware-accelerated driver every other app uses);
// desktop hosts use whatever `libGLESv1_CM`/`libEGL` the system provides. When
// no driver is present the probe fails, every call is refused with a named
// diagnostic, and the boot report states it - nothing is emulated silently.
//
// Consequences of the design that matter for correctness:
//   * guest pointers are translated per call with the byte range the call
//     needs (`GuestAddressSpace::guestToHost`), so the driver never reads host
//     memory outside mapped guest regions;
//   * the guest's client arrays are re-issued at draw time with translated
//     pointers, so a guest array that moved or an array left over from an
//     earlier binding can never point the driver at stale host memory;
//   * EAGL's drawable is a real EGL surface: `renderbufferStorage:fromDrawable:`
//     creates the EGL context/surface (window surface when the Android glue
//     supplied an `ANativeWindow`, offscreen pbuffer otherwise) and
//     `presentRenderbuffer:` is `eglSwapBuffers`.
#include "compat_runtime/gles_shims.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <dlfcn.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace radek::compat_runtime::gles {
namespace {

// ---- GL/EGL types and the values the forwarding logic reasons about --------
using GLenum = unsigned int;
using GLfloat = float;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLbitfield = unsigned int;

constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_FRAMEBUFFER_OES = 0x8D40;
constexpr GLenum GL_RENDERBUFFER_OES = 0x8D41;
constexpr GLenum GL_RENDERBUFFER_WIDTH_OES = 0x8D42;
constexpr GLenum GL_RENDERBUFFER_HEIGHT_OES = 0x8D43;
constexpr GLenum GL_COLOR_ATTACHMENT0_OES = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_OES = 0x8CD5;

constexpr GLenum GL_VERTEX_ARRAY = 0x8074;
constexpr GLenum GL_NORMAL_ARRAY = 0x8075;
constexpr GLenum GL_COLOR_ARRAY = 0x8076;
constexpr GLenum GL_TEXTURE_COORD_ARRAY = 0x8078;

constexpr GLenum GL_BYTE = 0x1400;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_SHORT = 0x1402;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_FIXED = 0x140C;
constexpr GLenum GL_FLOAT = 0x1406;

using EGLint = int;
using EGLBoolean = unsigned int;
using EGLDisplay = void *;
using EGLConfig = void *;
using EGLSurface = void *;
using EGLContext = void *;
using EGLNativeDisplayType = void *;
using EGLNativeWindowType = void *;

constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_TRUE_VALUE = 1;
constexpr EGLint EGL_DEFAULT_DISPLAY_VALUE = 0;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033;
constexpr EGLint EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_WINDOW_BIT = 0x0004;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040;
constexpr EGLint EGL_OPENGL_ES_BIT = 0x0001;
constexpr EGLint EGL_OPENGL_ES2_BIT = 0x0004;
constexpr EGLint EGL_CONTEXT_CLIENT_VERSION = 0x3098;
constexpr EGLint EGL_DEPTH_SIZE = 0x3025;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_HEIGHT = 0x3056;

// The callout window used by the forwarding bindings (free range between the
// libsystem window ending at 0xf004ffff and the loader's reserved region).
constexpr GuestAddress kCalloutBase = 0xf0080000u;
constexpr GuestAddress kCalloutEnd = 0xf00C0000u;

struct AtomicProgress {
    std::atomic<bool> driverGlesLoaded{false};
    std::atomic<bool> driverEglLoaded{false};
    std::atomic<bool> drawableReady{false};
    std::atomic<bool> presentingToWindow{false};
    std::atomic<std::uint32_t> drawableWidth{0};
    std::atomic<std::uint32_t> drawableHeight{0};
    std::atomic<std::uint64_t> guestCallsObserved{0};
    std::atomic<std::uint64_t> forwardedCalls{0};
    std::atomic<std::uint64_t> refusedCalls{0};
    std::atomic<std::uint64_t> framesPresented{0};
};

AtomicProgress gProgress;

std::mutex gNoteMutex;
std::string gLastNote;

void publishNoteLocked(const std::string &note) {
    std::lock_guard<std::mutex> lock(gNoteMutex);
    gLastNote = note;
}

void recordDriver(const DriverReport &report) {
    gProgress.driverGlesLoaded.store(report.glesLoaded || report.gles2Loaded, std::memory_order_relaxed);
    gProgress.driverEglLoaded.store(report.eglLoaded, std::memory_order_relaxed);
}

void recordDrawable(bool ready, bool window, std::uint32_t width, std::uint32_t height) {
    gProgress.drawableReady.store(ready, std::memory_order_relaxed);
    gProgress.presentingToWindow.store(ready && window, std::memory_order_relaxed);
    gProgress.drawableWidth.store(ready ? width : 0, std::memory_order_relaxed);
    gProgress.drawableHeight.store(ready ? height : 0, std::memory_order_relaxed);
}

/** iOS ARM32 base AAPCS arguments: r0-r3, then [sp]. Floating-point
 * values are passed as integer bit patterns even when guest code uses VFP.
 * https://developer.apple.com/library/archive/documentation/Xcode/Conceptual/iPhoneOSABIReference/Articles/ARMv6FunctionCallingConventions.html
 */
struct Args {
    CpuRegisterState &registers;
    GuestAddressSpace &memory;
    std::string &reason;

    std::uint32_t u32(int index) const {
        if (index < 4)
            return registers.r[static_cast<std::size_t>(index)];
        std::uint32_t value = 0;
        const auto address = static_cast<std::uint64_t>(registers.r[13]) +
                             4u * static_cast<unsigned>(index - 4);
        if (registers.r[13] == 0 || address > std::numeric_limits<GuestAddress>::max() ||
            !memory.read(static_cast<GuestAddress>(address), &value, sizeof(value)))
            throw std::runtime_error("streaming argument is unreadable");
        return value;
    }
    GLint i32(int index) const { return static_cast<GLint>(u32(index)); }
    GLenum enumValue(int index) const { return static_cast<GLenum>(u32(index)); }
    GLfloat f(int index) const {
        const std::uint32_t bits = u32(index);
        GLfloat value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    GuestAddress pointer(int index) const { return u32(index); }
};

std::size_t elementSize(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_FIXED:
    case GL_FLOAT:
        return 4;
    default:
        return 0;
    }
}

/** Loads the platform GL/EGL libraries for this forwarder. */
struct Driver {
    void *gles1 = nullptr;
    void *gles2 = nullptr;
    void *egl = nullptr;
    bool glesLoaded = false;
    bool gles2Loaded = false;
    bool gles2Probed = false;
    bool eglLoaded = false;
    unsigned api = 1;
    std::string detail;
    std::map<std::string, void *> symbols[2];

    Driver() {
        for (const char *name : {"libGLESv1_CM.so.1", "libGLESv1_CM.so"}) {
            gles1 = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (gles1) {
                glesLoaded = true;
                detail += std::string(detail.empty() ? "" : ", ") + "GLES1=" + name;
                break;
            }
        }
        if (!gles1)
            detail = "GLES1=unavailable";
        for (const char *name : {"libEGL.so.1", "libEGL.so", "libEGL_angle.so"}) {
            egl = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (egl) {
                eglLoaded = true;
                detail += ", EGL=" + std::string(name);
                break;
            }
        }
        if (!egl)
            detail += ", EGL=unavailable";
    }

    bool loadGles2() {
        if (gles2Probed) return gles2Loaded;
        gles2Probed = true;
        // ES1-only games do not need to load the programmable driver at all.
        for (const char *name : {"libGLESv2.so.2", "libGLESv2.so"}) {
            gles2 = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (gles2) {
                gles2Loaded = true;
                detail += ", GLES2=" + std::string(name);
                break;
            }
        }
        if (!gles2) detail += ", GLES2=unavailable";
        return gles2Loaded;
    }

    ~Driver() {
        if (gles2) ::dlclose(gles2);
        if (gles1) ::dlclose(gles1);
        if (egl) ::dlclose(egl);
    }

    void *find(const char *name) {
        auto &cache = symbols[api == 2 ? 1 : 0];
        const auto cached = cache.find(name);
        if (cached != cache.end())
            return cached->second;
        void *address = nullptr;
        void *gles = api == 2 ? gles2 : gles1;
        if (gles)
            address = ::dlsym(gles, name);
        if (!address && egl)
            address = ::dlsym(egl, name);
        cache.emplace(name, address);
        return address;
    }

    template <typename Fn> Fn function(const char *name) {
        return reinterpret_cast<Fn>(find(name));
    }
};

// ANativeWindow_fromSurface returns a borrowed-to-this-call reference. Hold
// independent references for the currently published window and the EGL
// surface built from it, including while UI and guest threads race replacement.
struct NativeWindowRef {
    explicit NativeWindowRef(void *value) : pointer(value) {
#ifdef __ANDROID__
        if (pointer) ANativeWindow_acquire(static_cast<ANativeWindow *>(pointer));
#endif
    }
    ~NativeWindowRef() {
#ifdef __ANDROID__
        if (pointer) ANativeWindow_release(static_cast<ANativeWindow *>(pointer));
#endif
    }
    void *pointer = nullptr;
};
struct WindowSnapshot {
    std::shared_ptr<NativeWindowRef> reference;
    std::uint64_t generation = 0;
    void *pointer() const { return reference ? reference->pointer : nullptr; }
};

struct EglState {
    EGLDisplay display = nullptr;
    EGLConfig config = nullptr;
    EGLContext context = nullptr;
    EGLSurface surface = nullptr;
    bool windowSurface = false;
    bool current = false;
    WindowSnapshot boundWindow; // keeps the window alive until EGL destroys its surface
    int width = 0;
    int height = 0;

    void *eglGetDisplay = nullptr;
    void *eglInitialize = nullptr;
    void *eglChooseConfig = nullptr;
    void *eglCreateContext = nullptr;
    void *eglCreateWindowSurface = nullptr;
    void *eglCreatePbufferSurface = nullptr;
    void *eglMakeCurrent = nullptr;
    void *eglSwapBuffers = nullptr;
    void *eglGetError = nullptr;
};

} // namespace

void resetProgress() {
    gProgress.driverGlesLoaded.store(false, std::memory_order_relaxed);
    gProgress.driverEglLoaded.store(false, std::memory_order_relaxed);
    gProgress.drawableReady.store(false, std::memory_order_relaxed);
    gProgress.presentingToWindow.store(false, std::memory_order_relaxed);
    gProgress.drawableWidth.store(0, std::memory_order_relaxed);
    gProgress.drawableHeight.store(0, std::memory_order_relaxed);
    gProgress.guestCallsObserved.store(0, std::memory_order_relaxed);
    gProgress.forwardedCalls.store(0, std::memory_order_relaxed);
    gProgress.refusedCalls.store(0, std::memory_order_relaxed);
    gProgress.framesPresented.store(0, std::memory_order_relaxed);
}

ProgressSnapshot progressSnapshot() {
    ProgressSnapshot snapshot;
    snapshot.driverGlesLoaded = gProgress.driverGlesLoaded.load(std::memory_order_relaxed);
    snapshot.driverEglLoaded = gProgress.driverEglLoaded.load(std::memory_order_relaxed);
    snapshot.drawableReady = gProgress.drawableReady.load(std::memory_order_relaxed);
    snapshot.presentingToWindow = gProgress.presentingToWindow.load(std::memory_order_relaxed);
    snapshot.drawableWidth = gProgress.drawableWidth.load(std::memory_order_relaxed);
    snapshot.drawableHeight = gProgress.drawableHeight.load(std::memory_order_relaxed);
    snapshot.guestCallsObserved = gProgress.guestCallsObserved.load(std::memory_order_relaxed);
    snapshot.forwardedCalls = gProgress.forwardedCalls.load(std::memory_order_relaxed);
    snapshot.refusedCalls = gProgress.refusedCalls.load(std::memory_order_relaxed);
    snapshot.framesPresented = gProgress.framesPresented.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(gNoteMutex);
        snapshot.lastNote = gLastNote;
    }
    return snapshot;
}

void publishNote(const std::string &note) { publishNoteLocked(note); }

// ---------------------------------------------------------------------------
struct Forwarder::Impl {
    static constexpr std::size_t kMaxDiagnostics = 64;

    Driver driver;
    DriverReport report;
    EglState egl;
    // The UI thread publishes a retained reference; EGL runs on the guest thread.
    std::mutex windowMutex;
    WindowSnapshot nativeWindow;
    std::uint64_t failedWindowGeneration = 0;
    std::chrono::steady_clock::time_point nextWindowRetry{};

    WindowSnapshot windowSnapshot() {
        std::lock_guard<std::mutex> lock(windowMutex);
        return nativeWindow;
    }
    void setWindow(void *window) {
        std::lock_guard<std::mutex> lock(windowMutex);
        if (nativeWindow.pointer() == window) return;
        nativeWindow.reference = window ? std::make_shared<NativeWindowRef>(window) : nullptr;
        ++nativeWindow.generation;
    }

    std::map<std::pair<unsigned, GLenum>, GuestAddress> guestDriverStrings;
    // Deleted renderbuffer names may be reused while an unbound FBO still
    // retains the old object. Distinguish those objects with generation IDs.
    std::map<int, std::uint64_t> drawableRenderbuffers; // guest name -> generation
    std::map<int, std::uint64_t> drawableFramebuffers; // guest FBO -> drawable generation
    std::uint64_t nextDrawableGeneration = 0;
    int boundRenderbuffer = 0;
    int boundFramebuffer = 0;
    int boundArrayBuffer = 0;
    int boundElementArrayBuffer = 0;
    GLint unpackAlignment = 4;
    GLint packAlignment = 4;

    struct ArrayState {
        bool enabled = false;
        GLint size = 0;
        GLenum type = 0;
        GLsizei stride = 0;
        GuestAddress pointer = 0;
        GLuint buffer = 0; // ARRAY_BUFFER binding captured by the pointer call
    };
    ArrayState vertexArray;
    ArrayState colorArray;
    ArrayState texCoordArray;
    ArrayState normalArray;
    struct VertexAttrib {
        bool enabled = false;
        GLint size = 4;
        GLenum type = GL_FLOAT;
        unsigned char normalized = 0;
        GLsizei stride = 0;
        GuestAddress pointer = 0;
        GLuint buffer = 0; // binding captured when glVertexAttribPointer ran
    };
    std::map<GLuint, VertexAttrib> vertexAttribs;

    std::uint64_t guestCalls = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t refused = 0;
    std::uint64_t frames = 0;
    std::vector<std::string> diagnostics;
    std::uint32_t drawableWidth = 0;
    std::uint32_t drawableHeight = 0;
    GuestAddress contextOwner = 0;
    bool drawableAttached = false;

    GuestAddress nextCallout = kCalloutBase;

    Impl() {
        report.glesLoaded = driver.glesLoaded;
        report.gles2Loaded = driver.gles2Loaded;
        report.eglLoaded = driver.eglLoaded;
        report.detail = driver.detail;
        recordDriver(report);
    }

    void note(const std::string &message) {
        publishNoteLocked(message);
        if (diagnostics.size() >= kMaxDiagnostics)
            return;
        if (std::find(diagnostics.begin(), diagnostics.end(), message) != diagnostics.end())
            return;
        diagnostics.push_back(message);
    }

    /** Records that a call could not be handed to the driver and why. */
    void refuse(const std::string &call, const std::string &why = {}) {
        ++refused;
        gProgress.refusedCalls.fetch_add(1, std::memory_order_relaxed);
        if (why.empty())
            note(call + " refused: no GLES driver is loaded on this host");
        else
            note(call + " refused: " + why);
    }

    template <typename Fn> Fn symbol(const char *name) { return driver.function<Fn>(name); }

    template <typename Fn, typename... A> bool call(const char *name, A... args) {
        Fn fn = symbol<Fn>(name);
        if (!fn) {
            refuse(name);
            return false;
        }
        fn(args...);
        ++forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /** Guest range -> host pointer, or null (with a diagnostic) when unmapped. */
    void *hostAddress(GuestAddressSpace &memory, GuestAddress address, std::size_t bytes,
                      const char *call) {
        if (bytes == 0)
            return nullptr;
        void *host = memory.guestToHost(address, bytes, MemoryPermission::Read);
        if (!host)
            refuse(call, "the guest range " + describe(address, bytes) +
                             " leaves mapped guest memory");
        return host;
    }

    /** Writable guest memory is also marked dirty for the guest CPU backend. */
    void *hostOutput(GuestAddressSpace &memory, GuestAddress address, std::size_t bytes,
                     const char *call) {
        if (bytes == 0) return nullptr;
        void *host = memory.guestToHost(address, bytes, MemoryPermission::Write);
        if (!host)
            refuse(call, "the guest output range " + describe(address, bytes) +
                             " is not writable");
        return host;
    }

    // Use the guest output directly on the common aligned path. A malformed
    // unaligned output gets aligned staging so a host driver never receives an
    // unaligned GLuint pointer. Zero/negative counts still reach GL for its
    // ordinary error semantics without touching guest memory.
    void generateNames(Args &args, const char *name) {
        const GLsizei count = args.i32(0);
        if (count <= 0) { call<void (*)(GLsizei, GLuint *)>(name, count, nullptr); return; }
        const auto bytes64 = static_cast<std::uint64_t>(count) * sizeof(GLuint);
        if (bytes64 > 64u * 1024u * 1024u) { refuse(name, "object count exceeds the safety limit"); return; }
        const auto bytes = static_cast<std::size_t>(bytes64);
        void *output = hostOutput(args.memory, args.pointer(1), bytes, name);
        if (!output) return;
        if (reinterpret_cast<std::uintptr_t>(output) % alignof(GLuint) == 0) {
            call<void (*)(GLsizei, GLuint *)>(name, count, static_cast<GLuint *>(output));
            return;
        }
        std::array<GLuint, 16> small{};
        std::vector<GLuint> large;
        if (count > static_cast<GLsizei>(small.size())) large.resize(static_cast<std::size_t>(count));
        GLuint *names = large.empty() ? small.data() : large.data();
        (void)args.memory.read(args.pointer(1), names, bytes);
        if (call<void (*)(GLsizei, GLuint *)>(name, count, names)) std::memcpy(output, names, bytes);
    }

    // Delete lists may be unaligned in the guest. Keep the common small list
    // on the stack and update our mirrors only after the driver was called.
    template <typename AfterDelete>
    void deleteNames(Args &args, const char *name, AfterDelete afterDelete) {
        const GLsizei count = args.i32(0);
        if (count <= 0) { call<void (*)(GLsizei, const GLuint *)>(name, count, nullptr); return; }
        const auto bytes64 = static_cast<std::uint64_t>(count) * sizeof(GLuint);
        if (bytes64 > 64u * 1024u * 1024u) { refuse(name, "object count exceeds the safety limit"); return; }
        std::array<GLuint, 16> small{};
        std::vector<GLuint> large;
        if (count > static_cast<GLsizei>(small.size())) large.resize(static_cast<std::size_t>(count));
        GLuint *names = large.empty() ? small.data() : large.data();
        if (!args.memory.read(args.pointer(1), names, static_cast<std::size_t>(bytes64))) {
            refuse(name, "object list is unreadable"); return;
        }
        if (!call<void (*)(GLsizei, const GLuint *)>(name, count, names)) return;
        for (GLsizei i = 0; i < count; ++i) if (names[i] != 0) afterDelete(names[i]);
    }

    void detachCurrentDrawableFramebuffer() {
        if (boundFramebuffer != 0 && drawableFramebuffers.erase(boundFramebuffer))
            call<void (*)(GLenum, GLuint)>(driver.api == 2 ? "glBindFramebuffer" : "glBindFramebufferOES",
                                           GL_FRAMEBUFFER_OES, static_cast<GLuint>(boundFramebuffer));
    }

    void deletedRenderbuffer(GLuint name) {
        const auto object = drawableRenderbuffers.find(static_cast<int>(name));
        // GL detaches only from the currently bound framebuffer. Non-bound
        // FBOs retain the deleted object, even if its integer name is reused.
        // https://docs.gl/es2/glDeleteRenderbuffers
        const auto current = drawableFramebuffers.find(boundFramebuffer);
        if (object != drawableRenderbuffers.end() && current != drawableFramebuffers.end() &&
            current->second == object->second) detachCurrentDrawableFramebuffer();
        if (object != drawableRenderbuffers.end()) drawableRenderbuffers.erase(object);
        if (static_cast<GLuint>(boundRenderbuffer) == name) boundRenderbuffer = 0;
    }

    void deletedFramebuffer(GLuint name) {
        drawableFramebuffers.erase(static_cast<int>(name));
        if (static_cast<GLuint>(boundFramebuffer) == name) boundFramebuffer = 0;
    }

    void queryRenderbuffer(Args &args, const char *name) {
        if (!args.memory.contains(args.pointer(2), sizeof(GLint), MemoryPermission::Write)) {
            refuse(name, "query output is not writable"); return;
        }
        GLint result = 0;
        (void)args.memory.read(args.pointer(2), &result, sizeof(result));
        const auto target = args.enumValue(0), pname = args.enumValue(1);
        if (target == GL_RENDERBUFFER_OES && drawableRenderbuffers.count(boundRenderbuffer) &&
            (pname == GL_RENDERBUFFER_WIDTH_OES || pname == GL_RENDERBUFFER_HEIGHT_OES)) {
            result = static_cast<GLint>(pname == GL_RENDERBUFFER_WIDTH_OES ? drawableWidth : drawableHeight);
        } else if (!call<void (*)(GLenum, GLenum, GLint *)>(name, target, pname, &result)) {
            return;
        }
        (void)args.memory.write(args.pointer(2), &result, sizeof(result));
    }

    static bool validFramebufferTexture(GLenum target, GLuint texture, GLint level) {
        // For texture 0 the attachment is detached and the texture target and
        // level are ignored. ES2 otherwise accepts GL_TEXTURE_2D or cube faces.
        // https://docs.gl/es2/glFramebufferTexture2D
        return texture == 0 || (level == 0 && (target == 0x0DE1 || (target >= 0x8515 && target <= 0x851A)));
    }

    /** Bounded NUL-terminated guest string. One mapped region is scanned at a
     * time, without dereferencing an unchecked host pointer. */
    bool guestString(GuestAddressSpace &memory, GuestAddress address, std::string &text,
                     const char *call) {
        constexpr std::size_t maxBytes = 16u * 1024u * 1024u;
        if (address == 0) { refuse(call, "a string pointer is null"); return false; }
        const auto regions = memory.regions();
        std::uint64_t cursor = address;
        text.clear();
        while (text.size() < maxBytes && cursor <= std::numeric_limits<GuestAddress>::max()) {
            const GuestRegionView *region = nullptr;
            for (const auto &candidate : regions) {
                const auto end = static_cast<std::uint64_t>(candidate.base) + candidate.size;
                if (cursor >= candidate.base && cursor < end) { region = &candidate; break; }
            }
            if (!region || !hasPermission(region->permissions, MemoryPermission::Read)) break;
            const auto available = static_cast<std::size_t>(
                static_cast<std::uint64_t>(region->base) + region->size - cursor);
            const auto amount = std::min(available, maxBytes - text.size());
            const auto *host = static_cast<const char *>(memory.guestToHost(
                static_cast<GuestAddress>(cursor), amount, MemoryPermission::Read));
            if (!host) break;
            const auto *end = static_cast<const char *>(std::memchr(host, 0, amount));
            if (end) { text.append(host, static_cast<std::size_t>(end - host)); return true; }
            text.append(host, amount);
            cursor += amount;
        }
        refuse(call, "unterminated or oversized guest string");
        return false;
    }

    static std::string describe(GuestAddress address, std::size_t bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string text = "0x";
        for (int shift = 28; shift >= 0; shift -= 4)
            text.push_back(digits[(address >> shift) & 0xF]);
        return text + " + " + std::to_string(bytes) + " byte(s)";
    }

    // ES pixel transfers have row padding determined by PACK/UNPACK_ALIGNMENT.
    // https://registry.khronos.org/OpenGL-Refpages/es2.0/glPixelStorei.xml
    static bool pixelBytes(GLsizei width, GLsizei height, GLenum format, GLenum type,
                           GLint alignment, std::size_t &bytes) {
        if (width < 0 || height < 0 || (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8))
            return false;
        unsigned components = 0;
        switch (format) {
        case 0x1906: case 0x1909: case 0x1902: components = 1; break; // alpha, luminance, depth
        case 0x190A: components = 2; break;                           // luminance-alpha
        case 0x1907: components = 3; break;                           // RGB
        case 0x1908: case 0x80E1: components = 4; break;             // RGBA, BGRA
        default: return false;
        }
        unsigned pixel = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE: pixel = components; break;
        case GL_UNSIGNED_SHORT: pixel = components * 2; break;
        case GL_FLOAT: pixel = components * 4; break;
        case 0x8D61: case 0x140B: pixel = components * 2; break; // half float OES/core
        case 0x8033: case 0x8034: case 0x8363: pixel = 2; break; // packed 16-bit
        case 0x1405: pixel = components * 4; break;              // unsigned int components
        case 0x84FA: pixel = 4; break;                             // packed depth-stencil
        default: return false;
        }
        if (width == 0 || height == 0) { bytes = 0; return true; }
        const auto row = static_cast<std::uint64_t>(width) * pixel;
        const auto stride = (row + static_cast<unsigned>(alignment) - 1u) &
                            ~static_cast<std::uint64_t>(alignment - 1);
        const auto rowsBeforeLast = static_cast<std::uint64_t>(height - 1);
        if (rowsBeforeLast != 0 && stride > (std::numeric_limits<std::uint64_t>::max() - row) / rowsBeforeLast)
            return false;
        const auto total = stride * rowsBeforeLast + row;
        if (total > 0xffffffffULL || total > std::numeric_limits<std::size_t>::max()) return false;
        bytes = static_cast<std::size_t>(total);
        return true;
    }

    // Core ES 1.1 and ES 2.0 glGet output arities. Unknown extension pnames
    // are refused rather than letting a driver write an unbounded array.
    // https://registry.khronos.org/OpenGL-Refpages/es1.1/xhtml/glGet.xml
    // https://registry.khronos.org/OpenGL-Refpages/es2.0/xhtml/glGet.xml
    static int integerQueryCount(GLenum pname) {
        switch (pname) {
        case 0x0B10: // GL_POINT_SMOOTH
        case 0x0B11: // GL_POINT_SIZE
        case 0x0B20: // GL_LINE_SMOOTH
        case 0x0B21: // GL_LINE_WIDTH
        case 0x0B44: // GL_CULL_FACE
        case 0x0B45: // GL_CULL_FACE_MODE
        case 0x0B46: // GL_FRONT_FACE
        case 0x0B50: // GL_LIGHTING
        case 0x0B52: // GL_LIGHT_MODEL_TWO_SIDE
        case 0x0B54: // GL_SHADE_MODEL
        case 0x0B57: // GL_COLOR_MATERIAL
        case 0x0B60: // GL_FOG
        case 0x0B62: // GL_FOG_DENSITY
        case 0x0B63: // GL_FOG_START
        case 0x0B64: // GL_FOG_END
        case 0x0B65: // GL_FOG_MODE
        case 0x0B71: // GL_DEPTH_TEST
        case 0x0B72: // GL_DEPTH_WRITEMASK
        case 0x0B73: // GL_DEPTH_CLEAR_VALUE
        case 0x0B74: // GL_DEPTH_FUNC
        case 0x0B90: // GL_STENCIL_TEST
        case 0x0B91: // GL_STENCIL_CLEAR_VALUE
        case 0x0B92: // GL_STENCIL_FUNC
        case 0x0B93: // GL_STENCIL_VALUE_MASK
        case 0x0B94: // GL_STENCIL_FAIL
        case 0x0B95: // GL_STENCIL_PASS_DEPTH_FAIL
        case 0x0B96: // GL_STENCIL_PASS_DEPTH_PASS
        case 0x0B97: // GL_STENCIL_REF
        case 0x0B98: // GL_STENCIL_WRITEMASK
        case 0x0BA0: // GL_MATRIX_MODE
        case 0x0BA1: // GL_NORMALIZE
        case 0x0BA3: // GL_MODELVIEW_STACK_DEPTH
        case 0x0BA4: // GL_PROJECTION_STACK_DEPTH
        case 0x0BA5: // GL_TEXTURE_STACK_DEPTH
        case 0x0BC0: // GL_ALPHA_TEST
        case 0x0BC1: // GL_ALPHA_TEST_FUNC
        case 0x0BC2: // GL_ALPHA_TEST_REF
        case 0x0BD0: // GL_DITHER
        case 0x0BE0: // GL_BLEND_DST
        case 0x0BE1: // GL_BLEND_SRC
        case 0x0BE2: // GL_BLEND
        case 0x0BF0: // GL_LOGIC_OP_MODE
        case 0x0BF2: // GL_COLOR_LOGIC_OP
        case 0x0C11: // GL_SCISSOR_TEST
        case 0x0C50: // GL_PERSPECTIVE_CORRECTION_HINT
        case 0x0C51: // GL_POINT_SMOOTH_HINT
        case 0x0C52: // GL_LINE_SMOOTH_HINT
        case 0x0C54: // GL_FOG_HINT
        case 0x0CF5: // GL_UNPACK_ALIGNMENT
        case 0x0D05: // GL_PACK_ALIGNMENT
        case 0x0D31: // GL_MAX_LIGHTS
        case 0x0D32: // GL_MAX_CLIP_PLANES
        case 0x0D33: // GL_MAX_TEXTURE_SIZE
        case 0x0D36: // GL_MAX_MODELVIEW_STACK_DEPTH
        case 0x0D38: // GL_MAX_PROJECTION_STACK_DEPTH
        case 0x0D39: // GL_MAX_TEXTURE_STACK_DEPTH
        case 0x0D50: // GL_SUBPIXEL_BITS
        case 0x0D52: // GL_RED_BITS
        case 0x0D53: // GL_GREEN_BITS
        case 0x0D54: // GL_BLUE_BITS
        case 0x0D55: // GL_ALPHA_BITS
        case 0x0D56: // GL_DEPTH_BITS
        case 0x0D57: // GL_STENCIL_BITS
        case 0x0DE1: // GL_TEXTURE_2D
        case 0x2A00: // GL_POLYGON_OFFSET_UNITS
        case 0x3000: // GL_CLIP_PLANE0
        case 0x3001: // GL_CLIP_PLANE1
        case 0x3002: // GL_CLIP_PLANE2
        case 0x3003: // GL_CLIP_PLANE3
        case 0x3004: // GL_CLIP_PLANE4
        case 0x3005: // GL_CLIP_PLANE5
        case 0x4000: // GL_LIGHT0
        case 0x4001: // GL_LIGHT1
        case 0x4002: // GL_LIGHT2
        case 0x4003: // GL_LIGHT3
        case 0x4004: // GL_LIGHT4
        case 0x4005: // GL_LIGHT5
        case 0x4006: // GL_LIGHT6
        case 0x4007: // GL_LIGHT7
        case 0x8009: // GL_BLEND_EQUATION_RGB
        case 0x8037: // GL_POLYGON_OFFSET_FILL
        case 0x8038: // GL_POLYGON_OFFSET_FACTOR
        case 0x803A: // GL_RESCALE_NORMAL
        case 0x8069: // GL_TEXTURE_BINDING_2D
        case 0x8074: // GL_VERTEX_ARRAY
        case 0x8075: // GL_NORMAL_ARRAY
        case 0x8076: // GL_COLOR_ARRAY
        case 0x8078: // GL_TEXTURE_COORD_ARRAY
        case 0x807A: // GL_VERTEX_ARRAY_SIZE
        case 0x807B: // GL_VERTEX_ARRAY_TYPE
        case 0x807C: // GL_VERTEX_ARRAY_STRIDE
        case 0x807E: // GL_NORMAL_ARRAY_TYPE
        case 0x807F: // GL_NORMAL_ARRAY_STRIDE
        case 0x8081: // GL_COLOR_ARRAY_SIZE
        case 0x8082: // GL_COLOR_ARRAY_TYPE
        case 0x8083: // GL_COLOR_ARRAY_STRIDE
        case 0x8088: // GL_TEXTURE_COORD_ARRAY_SIZE
        case 0x8089: // GL_TEXTURE_COORD_ARRAY_TYPE
        case 0x808A: // GL_TEXTURE_COORD_ARRAY_STRIDE
        case 0x809D: // GL_MULTISAMPLE
        case 0x809E: // GL_SAMPLE_ALPHA_TO_COVERAGE
        case 0x809F: // GL_SAMPLE_ALPHA_TO_ONE
        case 0x80A0: // GL_SAMPLE_COVERAGE
        case 0x80A8: // GL_SAMPLE_BUFFERS
        case 0x80A9: // GL_SAMPLES
        case 0x80AA: // GL_SAMPLE_COVERAGE_VALUE
        case 0x80AB: // GL_SAMPLE_COVERAGE_INVERT
        case 0x80C8: // GL_BLEND_DST_RGB
        case 0x80C9: // GL_BLEND_SRC_RGB
        case 0x80CA: // GL_BLEND_DST_ALPHA
        case 0x80CB: // GL_BLEND_SRC_ALPHA
        case 0x8126: // GL_POINT_SIZE_MIN
        case 0x8127: // GL_POINT_SIZE_MAX
        case 0x8128: // GL_POINT_FADE_THRESHOLD_SIZE
        case 0x8192: // GL_GENERATE_MIPMAP_HINT
        case 0x84E0: // GL_ACTIVE_TEXTURE
        case 0x84E1: // GL_CLIENT_ACTIVE_TEXTURE
        case 0x84E2: // GL_MAX_TEXTURE_UNITS
        case 0x84E8: // GL_MAX_RENDERBUFFER_SIZE
        case 0x8514: // GL_TEXTURE_BINDING_CUBE_MAP
        case 0x851C: // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x86A2: // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        case 0x8800: // GL_STENCIL_BACK_FUNC
        case 0x8801: // GL_STENCIL_BACK_FAIL
        case 0x8802: // GL_STENCIL_BACK_PASS_DEPTH_FAIL
        case 0x8803: // GL_STENCIL_BACK_PASS_DEPTH_PASS
        case 0x883D: // GL_BLEND_EQUATION_ALPHA
        case 0x8861: // GL_POINT_SPRITE_OES
        case 0x8869: // GL_MAX_VERTEX_ATTRIBS
        case 0x8872: // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8894: // GL_ARRAY_BUFFER_BINDING
        case 0x8895: // GL_ELEMENT_ARRAY_BUFFER_BINDING
        case 0x8896: // GL_VERTEX_ARRAY_BUFFER_BINDING
        case 0x8897: // GL_NORMAL_ARRAY_BUFFER_BINDING
        case 0x8898: // GL_COLOR_ARRAY_BUFFER_BINDING
        case 0x889A: // GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING
        case 0x898A: // GL_POINT_SIZE_ARRAY_TYPE_OES
        case 0x898B: // GL_POINT_SIZE_ARRAY_STRIDE_OES
        case 0x8B4C: // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
        case 0x8B4D: // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        case 0x8B8D: // GL_CURRENT_PROGRAM
        case 0x8B9A: // GL_IMPLEMENTATION_COLOR_READ_TYPE
        case 0x8B9B: // GL_IMPLEMENTATION_COLOR_READ_FORMAT
        case 0x8B9C: // GL_POINT_SIZE_ARRAY_OES
        case 0x8B9F: // GL_POINT_SIZE_ARRAY_BUFFER_BINDING_OES
        case 0x8CA3: // GL_STENCIL_BACK_REF
        case 0x8CA4: // GL_STENCIL_BACK_VALUE_MASK
        case 0x8CA5: // GL_STENCIL_BACK_WRITEMASK
        case 0x8CA6: // GL_FRAMEBUFFER_BINDING
        case 0x8CA7: // GL_RENDERBUFFER_BINDING
        case 0x8DF9: // GL_NUM_SHADER_BINARY_FORMATS
        case 0x8DFA: // GL_SHADER_COMPILER
        case 0x8DFB: // GL_MAX_VERTEX_UNIFORM_VECTORS
        case 0x8DFC: // GL_MAX_VARYING_VECTORS
        case 0x8DFD: // GL_MAX_FRAGMENT_UNIFORM_VECTORS
            return 1;
        case 0x0B12: // GL_SMOOTH_POINT_SIZE_RANGE
        case 0x0B22: // GL_SMOOTH_LINE_WIDTH_RANGE
        case 0x0B70: // GL_DEPTH_RANGE
        case 0x0D3A: // GL_MAX_VIEWPORT_DIMS
        case 0x846D: // GL_ALIASED_POINT_SIZE_RANGE
        case 0x846E: // GL_ALIASED_LINE_WIDTH_RANGE
            return 2;
        case 0x0B02: // GL_CURRENT_NORMAL
        case 0x8129: // GL_POINT_DISTANCE_ATTENUATION
            return 3;
        case 0x0B00: // GL_CURRENT_COLOR
        case 0x0B03: // GL_CURRENT_TEXTURE_COORDS
        case 0x0B53: // GL_LIGHT_MODEL_AMBIENT
        case 0x0B66: // GL_FOG_COLOR
        case 0x0BA2: // GL_VIEWPORT
        case 0x0C10: // GL_SCISSOR_BOX
        case 0x0C22: // GL_COLOR_CLEAR_VALUE
        case 0x0C23: // GL_COLOR_WRITEMASK
        case 0x8005: // GL_BLEND_COLOR
            return 4;
        case 0x0BA6: // GL_MODELVIEW_MATRIX
        case 0x0BA7: // GL_PROJECTION_MATRIX
        case 0x0BA8: // GL_TEXTURE_MATRIX
            return 16;
        default: return -1;
        }
    }

    // Fixed-function arrays capture GL_ARRAY_BUFFER when their pointer is set,
    // and glNormalPointer has a different signature from the other setters.
    bool applyDrawArrays(GuestAddressSpace &memory, GLint first, GLsizei count) {
        if (count <= 0) return true;
        if (first < 0) { refuse("glDrawArrays", "negative first vertex"); return false; }
        const auto last = static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(count) - 1;
        if (last > 0xffffffffULL) { refuse("glDrawArrays", "vertex range overflows"); return false; }
        GLuint current = static_cast<GLuint>(boundArrayBuffer);
        const auto restore = [&] {
            return current == static_cast<GLuint>(boundArrayBuffer) ||
                   call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                                 static_cast<GLuint>(boundArrayBuffer));
        };
        const auto apply = [&](const char *name, const ArrayState &array, bool normal) {
            if (!array.enabled) return true;
            const auto component = elementSize(array.type);
            if (component == 0 || array.size < 1 || array.size > 4 || array.stride < 0) {
                refuse(name, "invalid client-array format");
                return false;
            }
            const auto element = static_cast<std::uint64_t>(component) * static_cast<unsigned>(array.size);
            const auto stride = array.stride ? static_cast<std::uint64_t>(array.stride) : element;
            const auto bytes = last * stride + element;
            if (bytes > 0xffffffffULL || bytes > std::numeric_limits<std::size_t>::max()) {
                refuse(name, "client-array range overflows guest memory");
                return false;
            }
            const void *pointer = reinterpret_cast<const void *>(static_cast<std::uintptr_t>(array.pointer));
            if (array.buffer == 0) {
                pointer = hostAddress(memory, array.pointer, static_cast<std::size_t>(bytes), name);
                if (!pointer) return false;
            }
            if (current != array.buffer) {
                if (!call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER, array.buffer)) return false;
                current = array.buffer;
            }
            if (normal)
                return call<void (*)(GLenum, GLsizei, const void *)>(name, array.type, array.stride, pointer);
            return call<void (*)(GLint, GLenum, GLsizei, const void *)>(name, array.size, array.type,
                                                                         array.stride, pointer);
        };
        const bool ok = apply("glVertexPointer", vertexArray, false) &&
                        apply("glColorPointer", colorArray, false) &&
                        apply("glTexCoordPointer", texCoordArray, false) &&
                        apply("glNormalPointer", normalArray, true);
        const bool restored = restore();
        return ok && restored;
    }

    bool hasClientArrays() const {
        for (const auto *array : {&vertexArray, &colorArray, &texCoordArray, &normalArray})
            if (array->enabled && array->buffer == 0) return true;
        return false;
    }

    bool applyVertexAttribs(GuestAddressSpace &memory, std::uint32_t maxIndex) {
        // Re-issue client-memory attributes immediately before a draw. GL
        // captures ARRAY_BUFFER at glVertexAttribPointer time, not draw time.
        bool rebound = false;
        for (const auto &entry : vertexAttribs) {
            const auto &a = entry.second;
            if (!a.enabled || a.buffer != 0) continue;
            const auto element = elementSize(a.type) * static_cast<std::size_t>(a.size);
            if (a.size < 1 || a.size > 4 || a.stride < 0 || element == 0) {
                refuse("glDraw", "invalid client vertex attribute");
                if (rebound) call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                                            static_cast<GLuint>(boundArrayBuffer));
                return false;
            }
            const auto stride = a.stride ? static_cast<std::uint64_t>(a.stride) : element;
            const auto bytes64 = static_cast<std::uint64_t>(maxIndex) * stride + element;
            if (bytes64 > std::numeric_limits<std::size_t>::max() || bytes64 > 0xffffffffULL) {
                refuse("glDraw", "client vertex attribute range overflows guest memory");
                if (rebound) call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                                            static_cast<GLuint>(boundArrayBuffer));
                return false;
            }
            const void *host = hostAddress(memory, a.pointer, static_cast<std::size_t>(bytes64),
                                           "glVertexAttribPointer");
            if (!host) {
                if (rebound) call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                                            static_cast<GLuint>(boundArrayBuffer));
                return false;
            }
            if (!rebound) {
                if (!call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER, 0)) return false;
                rebound = true;
            }
            if (!call<void (*)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void *)>(
                    "glVertexAttribPointer", entry.first, a.size, a.type, a.normalized, a.stride,
                    host)) {
                call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                               static_cast<GLuint>(boundArrayBuffer));
                return false;
            }
        }
        if (rebound)
            return call<void (*)(GLenum, GLuint)>("glBindBuffer", GL_ARRAY_BUFFER,
                                                   static_cast<GLuint>(boundArrayBuffer));
        return true;
    }

    bool hasClientAttribs() const {
        for (const auto &entry : vertexAttribs)
            if (entry.second.enabled && entry.second.buffer == 0) return true;
        return false;
    }

    // ------------------------------------------------------------------
    // EGL drawable
    // ------------------------------------------------------------------
    /** " (EGL error 0x...)" for the last EGL call on this thread, or empty if none. */
    std::string eglErrorSuffix() {
        auto eglGetError = symbol<EGLint (*)()>("eglGetError");
        if (!eglGetError)
            return {};
        const EGLint code = eglGetError();
        if (code == 0x3000) // EGL_SUCCESS
            return {};
        char text[32];
        std::snprintf(text, sizeof(text), " (EGL error 0x%04x)", static_cast<unsigned>(code));
        return text;
    }

    bool createEglSurface(int width, int height, std::string &why) {
        auto eglGetDisplay = symbol<EGLDisplay (*)(EGLNativeDisplayType)>("eglGetDisplay");
        auto eglInitialize = symbol<EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *)>("eglInitialize");
        auto eglChooseConfig =
            symbol<EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)>(
                "eglChooseConfig");
        auto eglCreateContext =
            symbol<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *)>(
                "eglCreateContext");
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglCreateContext ||
            !eglMakeCurrent) {
            why = "the EGL entry points are missing from the loaded libraries" + eglErrorSuffix();
            return false;
        }
        egl.display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(
            static_cast<std::uintptr_t>(EGL_DEFAULT_DISPLAY_VALUE)));
        if (!egl.display) {
            why = "eglGetDisplay returned no display" + eglErrorSuffix();
            return false;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(egl.display, &major, &minor) != EGL_TRUE_VALUE) {
            why = "eglInitialize failed" + eglErrorSuffix();
            return false;
        }
        const auto window = windowSnapshot();
        const bool wantWindow = window.pointer() != nullptr;
        auto choose = [&](EGLint renderable, EGLint surfaceType) {
            const EGLint attributes[] = {
                EGL_SURFACE_TYPE, surfaceType,
                EGL_RENDERABLE_TYPE, renderable, EGL_DEPTH_SIZE, 16, EGL_NONE};
            EGLint configCount = 0;
            if (eglChooseConfig(egl.display, attributes, &egl.config, 1, &configCount) !=
                    EGL_TRUE_VALUE ||
                configCount < 1)
                return false;
            return true;
        };
        const EGLint renderable = driver.api == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT;
        // Prefer a config that can later move an offscreen context to the
        // Android window without destroying its shaders, buffers and textures.
        const EGLint preferredSurface = wantWindow ? EGL_WINDOW_BIT : (EGL_WINDOW_BIT | EGL_PBUFFER_BIT);
        if (!choose(renderable, preferredSurface) &&
            (wantWindow || !choose(renderable, EGL_PBUFFER_BIT))) {
            why = "no EGL config supports the requested OpenGL ES " +
                  std::to_string(driver.api) + " context" + eglErrorSuffix();
            return false;
        }
        // EGL defaults to ES1; ES2 must request its own programmable context.
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION,
                                             static_cast<EGLint>(driver.api), EGL_NONE};
        egl.context = eglCreateContext(egl.display, egl.config, nullptr, contextAttributes);
        if (!egl.context) {
            why = "eglCreateContext failed for an OpenGL ES context" + eglErrorSuffix();
            return false;
        }
        if (wantWindow) {
            auto eglCreateWindowSurface =
                symbol<EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)>(
                    "eglCreateWindowSurface");
            if (!eglCreateWindowSurface) {
                why = "eglCreateWindowSurface is missing from the loaded libraries" + eglErrorSuffix();
                return false;
            }
            egl.surface = eglCreateWindowSurface(
                egl.display, egl.config,
                reinterpret_cast<EGLNativeWindowType>(window.pointer()), nullptr);
            egl.windowSurface = egl.surface != nullptr;
            egl.boundWindow = egl.windowSurface ? window : WindowSnapshot{};
            if (!egl.surface) {
                why = "eglCreateWindowSurface failed for the Android surface" + eglErrorSuffix();
                return false;
            }
        } else {
            auto eglCreatePbufferSurface =
                symbol<EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *)>(
                    "eglCreatePbufferSurface");
            if (!eglCreatePbufferSurface) {
                why = "eglCreatePbufferSurface is missing from the loaded libraries" + eglErrorSuffix();
                return false;
            }
            const EGLint attributes[] = {EGL_WIDTH, std::max(width, 1), EGL_HEIGHT,
                                         std::max(height, 1), EGL_NONE};
            egl.surface =
                eglCreatePbufferSurface(egl.display, egl.config, attributes);
            egl.windowSurface = false;
            egl.boundWindow = WindowSnapshot{};
            if (!egl.surface) {
                why = "eglCreatePbufferSurface failed" + eglErrorSuffix();
                return false;
            }
        }
        if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) !=
            EGL_TRUE_VALUE) {
            why = "eglMakeCurrent failed" + eglErrorSuffix();
            return false;
        }
        egl.current = true;
        egl.width = width;
        egl.height = height;
        return true;
    }

    /// Moves the drawable onto the Android window surface without losing it: the new
    /// window surface is created and made current first, and the old surface is
    /// destroyed only after that succeeds, so a failed bind keeps the offscreen drawable.
    bool rebindWindowSurface(const WindowSnapshot &window, std::string &why) {
        if (window.pointer() == nullptr) {
            why = "no Android window surface is available";
            return false;
        }
        if (!egl.display || !egl.context) {
            why = "no EGL context to rebind";
            return false;
        }
        auto eglCreateWindowSurface =
            symbol<EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *)>(
                "eglCreateWindowSurface");
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        auto eglDestroySurface = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglDestroySurface");
        if (!eglCreateWindowSurface || !eglMakeCurrent || !eglDestroySurface) {
            why = "EGL window-surface entry points are missing from the loaded libraries";
            return false;
        }
        EGLSurface surface = eglCreateWindowSurface(
            egl.display, egl.config, reinterpret_cast<EGLNativeWindowType>(window.pointer()), nullptr);
        if (!surface) {
            why = "eglCreateWindowSurface failed for the Android surface";
            return false;
        }
        if (eglMakeCurrent(egl.display, surface, surface, egl.context) != EGL_TRUE_VALUE) {
            eglDestroySurface(egl.display, surface);
            why = "eglMakeCurrent failed for the Android window surface";
            return false;
        }
        if (egl.surface)
            eglDestroySurface(egl.display, egl.surface);
        egl.surface = surface;
        egl.windowSurface = true;
        egl.boundWindow = window;
        egl.current = true;
        return true;
    }

    bool attachDrawable(GuestAddressSpace &memory, std::uint32_t width, std::uint32_t height) {
        (void)memory;
        if (width == 0 || height == 0) {
            recordDrawable(false, false, 0, 0);
            note("EAGL drawable has a zero-sized frame; the drawable stays unattached");
            return false;
        }
        if (boundRenderbuffer == 0) {
            note("EAGL drawable needs a bound renderbuffer");
            return false;
        }
        const auto markAttached = [&] {
            if (!drawableRenderbuffers.count(boundRenderbuffer))
                drawableRenderbuffers.emplace(boundRenderbuffer, ++nextDrawableGeneration);
            drawableAttached = true;
            drawableWidth = width;
            drawableHeight = height;
        };
        if (egl.current) {
            egl.width = static_cast<int>(width);
            egl.height = static_cast<int>(height);
            // The Android surface may arrive after the first (offscreen) attach:
            // recreate the drawable so the frames reach the screen instead of a
            // pbuffer nobody can see.
            const auto window = windowSnapshot();
            if (egl.windowSurface && !window.pointer()) {
                recordDrawable(false, false, 0, 0);
                note("Android window was detached; preserving the context without presenting");
                return false;
            }
            if (window.pointer() && (!egl.windowSurface || egl.boundWindow.generation != window.generation)) {
                std::string why;
                if (rebindWindowSurface(window, why)) {
                    failedWindowGeneration = 0;
                    markAttached();
                    recordDrawable(true, true, width, height);
                    note("EAGL drawable re-attached to the Android window surface");
                    return true;
                }
                note("EGL window surface setup failed: " + why + "; context preserved");
                recordDrawable(false, false, 0, 0);
                return false;
            }
            if (!egl.surface) {
                recordDrawable(false, false, 0, 0);
                note("EAGL drawable has no current EGL surface");
                return false;
            }
            markAttached();
            recordDrawable(true, egl.windowSurface, width, height);
            return true;
        }
        if (!driver.eglLoaded) {
            recordDrawable(false, false, 0, 0);
            note("no EGL library is available on this host: the EAGL drawable has no "
                 "context and every GL call is refused");
            return false;
        }
        std::string why;
        if (!createEglSurface(static_cast<int>(width), static_cast<int>(height), why)) {
            recordDrawable(false, false, 0, 0);
            note("EGL surface setup failed: " + why + "; GL calls are refused");
            return false;
        }
        markAttached();
        recordDrawable(true, egl.windowSurface, width, height);
        note(egl.windowSurface
                 ? "EAGL drawable attached to an EGL window surface (frames go to the "
                   "Android surface)"
                 : "EAGL drawable attached to an offscreen EGL pbuffer surface (no native "
                   "window was supplied; nothing is presented on screen)");
        return true;
    }

    bool presentDrawable() {
        if (boundRenderbuffer == 0 || !drawableRenderbuffers.count(boundRenderbuffer)) {
            refuse("eglSwapBuffers", "the bound renderbuffer has no EAGL drawable storage");
            return false;
        }
        const auto window = windowSnapshot();
        if (egl.windowSurface && !window.pointer()) {
            recordDrawable(false, false, 0, 0);
            refuse("eglSwapBuffers", "the Android window was detached");
            return false;
        }
        if (egl.current && window.pointer() &&
            (!egl.windowSurface || egl.boundWindow.generation != window.generation)) {
            const auto now = std::chrono::steady_clock::now();
            if (failedWindowGeneration == window.generation && now < nextWindowRetry) return false;
            std::string why;
            if (!rebindWindowSurface(window, why)) {
                failedWindowGeneration = window.generation;
                nextWindowRetry = now + std::chrono::milliseconds(250);
                recordDrawable(false, false, 0, 0);
                refuse("eglSwapBuffers", "could not bind the new Android window: " + why);
                return false;
            }
            failedWindowGeneration = 0;
            recordDrawable(true, true, static_cast<std::uint32_t>(egl.width),
                           static_cast<std::uint32_t>(egl.height));
            note("EAGL drawable bound to the Android window surface before presenting");
        }
        if (!drawableAttached || !egl.current || !egl.surface) {
            refuse("eglSwapBuffers", "the EAGL drawable has no current EGL surface");
            return false;
        }
        auto eglSwapBuffers = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglSwapBuffers");
        if (!eglSwapBuffers) {
            refuse("eglSwapBuffers");
            return false;
        }
        const auto result = eglSwapBuffers(egl.display, egl.surface);
        ++forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        if (result != EGL_TRUE_VALUE) {
            refuse("eglSwapBuffers", "the EGL driver reported a failed swap");
            return false;
        }
        ++frames;
        gProgress.framesPresented.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void destroyEgl() {
        if (!driver.eglLoaded)
            return;
        auto eglMakeCurrent =
            symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        auto eglDestroySurface = symbol<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglDestroySurface");
        auto eglDestroyContext = symbol<EGLBoolean (*)(EGLDisplay, EGLContext)>("eglDestroyContext");
        auto eglTerminate = symbol<EGLBoolean (*)(EGLDisplay)>("eglTerminate");
        if (eglMakeCurrent && egl.display)
            eglMakeCurrent(egl.display, nullptr, nullptr, nullptr);
        if (eglDestroySurface && egl.display && egl.surface)
            eglDestroySurface(egl.display, egl.surface);
        if (eglDestroyContext && egl.display && egl.context)
            eglDestroyContext(egl.display, egl.context);
        if (eglTerminate && egl.display)
            eglTerminate(egl.display);
        egl = EglState{};
        drawableAttached = false;
    }
};

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
void Forwarder::registerBindings(ShimRegistry &registry) {
    auto &impl = *impl_;
    auto bind = [&](const std::string &symbol, const char *adapter,
                    std::function<bool(Args &)> invoke) {
        if (impl.nextCallout >= kCalloutEnd)
            throw std::runtime_error("GLES forwarding callout window is exhausted");
        ShimBinding binding;
        binding.darwinSymbol = symbol;
        binding.library = "OpenGLES";
        binding.adapterName = adapter;
        binding.guestAddress = impl.nextCallout;
        impl.nextCallout += 4;
        binding.invoke = [&impl, invoke](CpuRegisterState &registers, GuestAddressSpace &memory,
                                         std::string &reason) {
            ++impl.guestCalls;
            gProgress.guestCallsObserved.fetch_add(1, std::memory_order_relaxed);
            Args args{registers, memory, reason};
            try {
                return invoke(args);
            } catch (const std::exception &error) {
                // A GL call never aborts the boot attempt: an unreadable argument or a
                // refused feature is recorded and the call becomes a no-op.
                impl.refuse("a GL call", error.what());
                return true;
            }
        };
        registry.registerBinding(std::move(binding));
    };

    // ---- fixed-function state -------------------------------------------------
    bind("_glClear", "gles-forward-clear", [&impl](Args &args) {
        impl.call<void (*)(GLbitfield)>("glClear", static_cast<GLbitfield>(args.u32(0)));
        return true;
    });
    bind("_glClearColor", "gles-forward-clear-color", [&impl](Args &args) {
        impl.call<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>("glClearColor", args.f(0),
                                                                args.f(1), args.f(2), args.f(3));
        return true;
    });
    bind("_glViewport", "gles-forward-viewport", [&impl](Args &args) {
        impl.call<void (*)(GLint, GLint, GLsizei, GLsizei)>("glViewport", args.i32(0), args.i32(1),
                                                            args.i32(2), args.i32(3));
        return true;
    });
    bind("_glScissor", "gles-forward-scissor", [&impl](Args &args) {
        impl.call<void (*)(GLint, GLint, GLsizei, GLsizei)>("glScissor", args.i32(0), args.i32(1),
                                                            args.i32(2), args.i32(3));
        return true;
    });
    bind("_glEnable", "gles-forward-enable", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glEnable", args.enumValue(0));
        return true;
    });
    bind("_glDisable", "gles-forward-disable", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glDisable", args.enumValue(0));
        return true;
    });
    bind("_glBlendFunc", "gles-forward-blend-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum)>("glBlendFunc", args.enumValue(0), args.enumValue(1));
        return true;
    });
    bind("_glDepthFunc", "gles-forward-depth-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glDepthFunc", args.enumValue(0));
        return true;
    });
    bind("_glDepthMask", "gles-forward-depth-mask", [&impl](Args &args) {
        impl.call<void (*)(unsigned char)>("glDepthMask",
                                           static_cast<unsigned char>(args.u32(0) != 0));
        return true;
    });
    bind("_glFrontFace", "gles-forward-front-face", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glFrontFace", args.enumValue(0));
        return true;
    });
    bind("_glCullFace", "gles-forward-cull-face", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glCullFace", args.enumValue(0));
        return true;
    });
    bind("_glLineWidth", "gles-forward-line-width", [&impl](Args &args) {
        impl.call<void (*)(GLfloat)>("glLineWidth", args.f(0));
        return true;
    });
    bind("_glColor4f", "gles-forward-color4f", [&impl](Args &args) {
        impl.call<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>("glColor4f", args.f(0), args.f(1),
                                                                args.f(2), args.f(3));
        return true;
    });
    bind("_glPixelStorei", "gles-forward-pixel-store", [&impl](Args &args) {
        const auto name = args.enumValue(0);
        const auto value = args.i32(1);
        if (impl.call<void (*)(GLenum, GLint)>("glPixelStorei", name, value) &&
            (value == 1 || value == 2 || value == 4 || value == 8)) {
            if (name == 0x0CF5) impl.unpackAlignment = value;
            if (name == 0x0D05) impl.packAlignment = value;
        }
        return true;
    });
    bind("_glTexEnvi", "gles-forward-tex-env", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLint)>("glTexEnvi", args.enumValue(0), args.enumValue(1),
                                                   args.i32(2));
        return true;
    });
    bind("_glTexEnvf", "gles-forward-tex-env-f", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLfloat)>("glTexEnvf", args.enumValue(0),
                                                     args.enumValue(1), args.f(2));
        return true;
    });
    bind("_glActiveTexture", "gles-forward-active-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glActiveTexture", args.enumValue(0));
        return true;
    });
    bind("_glClientActiveTexture", "gles-forward-client-active-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glClientActiveTexture", args.enumValue(0));
        return true;
    });
    bind("_glShadeModel", "gles-forward-shade-model", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glShadeModel", args.enumValue(0));
        return true;
    });
    bind("_glAlphaFunc", "gles-forward-alpha-func", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLfloat)>("glAlphaFunc", args.enumValue(0), args.f(1));
        return true;
    });
    bind("_glColorMask", "gles-forward-color-mask", [&impl](Args &args) {
        impl.call<void (*)(unsigned char, unsigned char, unsigned char, unsigned char)>(
            "glColorMask", static_cast<unsigned char>(args.u32(0) != 0),
            static_cast<unsigned char>(args.u32(1) != 0),
            static_cast<unsigned char>(args.u32(2) != 0),
            static_cast<unsigned char>(args.u32(3) != 0));
        return true;
    });
    bind("_glHint", "gles-forward-hint", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum)>("glHint", args.enumValue(0), args.enumValue(1));
        return true;
    });
    bind("_glFinish", "gles-forward-finish", [&impl](Args &) {
        impl.call<void (*)()>("glFinish");
        return true;
    });
    bind("_glFlush", "gles-forward-flush", [&impl](Args &) {
        impl.call<void (*)()>("glFlush");
        return true;
    });
    bind("_glGetError", "gles-forward-get-error", [&impl](Args &args) {
        auto *fn = impl.symbol<GLenum (*)()>("glGetError");
        if (!fn) {
            impl.refuse("glGetError");
            args.registers.r[0] = 0; // GL_NO_ERROR: the guest cannot observe a driver error
            return true;
        }
        args.registers.r[0] = fn();
        ++impl.forwarded;
        return true;
    });

    // ---- matrices -------------------------------------------------------------
    bind("_glMatrixMode", "gles-forward-matrix-mode", [&impl](Args &args) {
        impl.call<void (*)(GLenum)>("glMatrixMode", args.enumValue(0));
        return true;
    });
    bind("_glLoadMatrixf", "gles-forward-load-matrix", [&impl](Args &args) {
        const auto address = args.pointer(0);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, 16 * sizeof(GLfloat), "glLoadMatrixf"));
        if (!host)
            return true;
        impl.call<void (*)(const GLfloat *)>("glLoadMatrixf",
                                             static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glMultMatrixf", "gles-forward-mult-matrix", [&impl](Args &args) {
        const auto address = args.pointer(0);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, 16 * sizeof(GLfloat), "glMultMatrixf"));
        if (!host)
            return true;
        impl.call<void (*)(const GLfloat *)>("glMultMatrixf",
                                             static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glLoadIdentity", "gles-forward-load-identity", [&impl](Args &) {
        impl.call<void (*)()>("glLoadIdentity");
        return true;
    });
    bind("_glPushMatrix", "gles-forward-push-matrix", [&impl](Args &) {
        impl.call<void (*)()>("glPushMatrix");
        return true;
    });
    bind("_glPopMatrix", "gles-forward-pop-matrix", [&impl](Args &) {
        impl.call<void (*)()>("glPopMatrix");
        return true;
    });

    // ---- fixed-function lighting (the driver's own implementation) ------------
    bind("_glLightfv", "gles-forward-light", [&impl](Args &args) {
        const auto address = args.pointer(2);
        const bool isMode = args.enumValue(1) == 0x0B52 /* GL_LIGHT_MODEL_* */;
        auto *host = static_cast<const void *>(impl.hostAddress(
            args.memory, address, (isMode ? 4 : 4) * sizeof(GLfloat), "glLightfv"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLenum, const GLfloat *)>(
            "glLightfv", args.enumValue(0), args.enumValue(1),
            static_cast<const GLfloat *>(host));
        return true;
    });
    bind("_glMaterialfv", "gles-forward-material", [&impl](Args &args) {
        auto *host = static_cast<const void *>(impl.hostAddress(
            args.memory, args.pointer(2), 4 * sizeof(GLfloat), "glMaterialfv"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLenum, const GLfloat *)>(
            "glMaterialfv", args.enumValue(0), args.enumValue(1),
            static_cast<const GLfloat *>(host));
        return true;
    });

    // ---- client arrays --------------------------------------------------------
    auto bindPointer = [&](const std::string &symbol, const char *adapter,
                           Impl::ArrayState Impl::*member) {
        const bool normal = symbol == "_glNormalPointer";
        bind(symbol, adapter, [&impl, member, normal](Args &args) {
            auto &array = impl.*member;
            array.size = normal ? 3 : args.i32(0);
            array.type = args.enumValue(normal ? 0 : 1);
            array.stride = args.i32(normal ? 1 : 2);
            array.pointer = args.pointer(normal ? 2 : 3);
            array.buffer = static_cast<GLuint>(impl.boundArrayBuffer);
            return true;
        });
    };
    bindPointer("_glVertexPointer", "gles-forward-vertex-pointer", &Forwarder::Impl::vertexArray);
    bindPointer("_glColorPointer", "gles-forward-color-pointer", &Forwarder::Impl::colorArray);
    bindPointer("_glTexCoordPointer", "gles-forward-texcoord-pointer",
                &Forwarder::Impl::texCoordArray);
    bindPointer("_glNormalPointer", "gles-forward-normal-pointer", &Forwarder::Impl::normalArray);
    bind("_glEnableClientState", "gles-forward-enable-client-state", [&impl](Args &args) {
        switch (args.enumValue(0)) {
        case GL_VERTEX_ARRAY: impl.vertexArray.enabled = true; break;
        case GL_COLOR_ARRAY: impl.colorArray.enabled = true; break;
        case GL_TEXTURE_COORD_ARRAY: impl.texCoordArray.enabled = true; break;
        case GL_NORMAL_ARRAY: impl.normalArray.enabled = true; break;
        default: break;
        }
        impl.call<void (*)(GLenum)>("glEnableClientState", args.enumValue(0));
        return true;
    });
    bind("_glDisableClientState", "gles-forward-disable-client-state", [&impl](Args &args) {
        switch (args.enumValue(0)) {
        case GL_VERTEX_ARRAY: impl.vertexArray.enabled = false; break;
        case GL_COLOR_ARRAY: impl.colorArray.enabled = false; break;
        case GL_TEXTURE_COORD_ARRAY: impl.texCoordArray.enabled = false; break;
        case GL_NORMAL_ARRAY: impl.normalArray.enabled = false; break;
        default: break;
        }
        impl.call<void (*)(GLenum)>("glDisableClientState", args.enumValue(0));
        return true;
    });

    // ---- draws ----------------------------------------------------------------
    bind("_glDrawArrays", "gles-forward-draw-arrays", [&impl](Args &args) {
        const auto mode = args.enumValue(0);
        const GLint first = args.i32(1);
        const GLsizei count = args.i32(2);
        if (impl.driver.api == 2 && count > 0) {
            if (first < 0 || static_cast<std::uint64_t>(first) + static_cast<unsigned>(count) - 1 > 0xffffffffULL) {
                impl.refuse("glDrawArrays", "invalid vertex range");
                return true;
            }
            if (!impl.applyVertexAttribs(args.memory, static_cast<std::uint32_t>(first) +
                                                        static_cast<std::uint32_t>(count) - 1))
                return true;
        } else if (impl.driver.api != 2 && !impl.applyDrawArrays(args.memory, first, count)) {
            return true;
        }
        impl.call<void (*)(GLenum, GLint, GLsizei)>("glDrawArrays", mode, first, count);
        return true;
    });
    bind("_glDrawElements", "gles-forward-draw-elements", [&impl](Args &args) {
        const auto mode = args.enumValue(0);
        const GLsizei count = args.i32(1);
        const auto type = args.enumValue(2);
        const auto indices = args.pointer(3);
        if (count < 0 || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT)) {
            impl.refuse("glDrawElements", "invalid count or index type");
            return true;
        }
        if (count == 0) {
            impl.call<void (*)(GLenum, GLsizei, GLenum, const void *)>("glDrawElements", mode,
                                                                       count, type, nullptr);
            return true;
        }
        if (impl.boundElementArrayBuffer != 0) {
            if (impl.driver.api == 2 && impl.hasClientAttribs()) {
                impl.refuse("glDrawElements", "cannot bound client attributes with GPU-only indices");
                return true;
            }
            if (impl.driver.api != 2 && impl.hasClientArrays()) {
                impl.refuse("glDrawElements", "cannot bound client arrays with GPU-only indices");
                return true;
            }
            if (impl.driver.api != 2 && !impl.applyDrawArrays(args.memory, 0, 1)) return true;
            impl.call<void (*)(GLenum, GLsizei, GLenum, const void *)>(
                "glDrawElements", mode, count, type,
                reinterpret_cast<const void *>(static_cast<std::uintptr_t>(indices)));
            return true;
        }
        const std::size_t elementBytes = type == GL_UNSIGNED_BYTE ? 1u : 2u;
        const auto byteCount = static_cast<std::uint64_t>(count) * elementBytes;
        if (byteCount > 0xffffffffULL) { impl.refuse("glDrawElements", "index range overflows"); return true; }
        const auto *host = static_cast<const unsigned char *>(impl.hostAddress(
            args.memory, indices, static_cast<std::size_t>(byteCount), "glDrawElements"));
        if (!host) return true;
        const bool needsMaximum = impl.driver.api == 2 ? impl.hasClientAttribs() : impl.hasClientArrays();
        std::uint32_t maximum = 0;
        if (needsMaximum) {
            // Only client-memory arrays need a CPU scan to bound the guest
            // read. When all enabled arrays are VBO-backed, the driver handles
            // their index range; avoid walking the guest indices on every draw.
            for (GLsizei i = 0; i < count; ++i) {
                std::uint16_t index = host[static_cast<std::size_t>(i) * elementBytes];
                if (elementBytes == 2)
                    std::memcpy(&index, host + static_cast<std::size_t>(i) * 2u, 2);
                maximum = std::max(maximum, static_cast<std::uint32_t>(index));
            }
        }
        if (impl.driver.api == 2) {
            if (needsMaximum && !impl.applyVertexAttribs(args.memory, maximum)) return true;
        } else if (!impl.applyDrawArrays(args.memory, 0, static_cast<GLsizei>(maximum + 1))) {
            return true;
        }
        impl.call<void (*)(GLenum, GLsizei, GLenum, const void *)>("glDrawElements", mode, count,
                                                                   type, host);
        return true;
    });

    // ---- buffer objects -------------------------------------------------------
    bind("_glGenBuffers", "gles-forward-gen-buffers", [&impl](Args &args) {
        impl.generateNames(args, "glGenBuffers");
        return true;
    });
    bind("_glDeleteBuffers", "gles-forward-delete-buffers", [&impl](Args &args) {
        impl.deleteNames(args, "glDeleteBuffers", [&impl](GLuint name) {
            // A deleted VBO no longer backs either a current or a captured array.
            // https://docs.gl/es2/glDeleteBuffers
            if (static_cast<GLuint>(impl.boundArrayBuffer) == name) impl.boundArrayBuffer = 0;
            if (static_cast<GLuint>(impl.boundElementArrayBuffer) == name) impl.boundElementArrayBuffer = 0;
            for (auto *array : {&impl.vertexArray, &impl.colorArray, &impl.texCoordArray, &impl.normalArray})
                if (array->buffer == name) array->buffer = 0;
            for (auto &attribute : impl.vertexAttribs)
                if (attribute.second.buffer == name) attribute.second.buffer = 0;
        });
        return true;
    });
    bind("_glBindBuffer", "gles-forward-bind-buffer", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const int name = args.i32(1);
        if (target == GL_ARRAY_BUFFER)
            impl.boundArrayBuffer = name;
        else if (target == GL_ELEMENT_ARRAY_BUFFER)
            impl.boundElementArrayBuffer = name;
        impl.call<void (*)(GLenum, GLuint)>("glBindBuffer", target, static_cast<GLuint>(name));
        return true;
    });
    bind("_glBufferData", "gles-forward-buffer-data", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const auto size = args.u32(1);
        const auto address = args.pointer(2);
        const auto usage = args.enumValue(3);
        const void *host = nullptr;
        if (address != 0 && size != 0) {
            host = impl.hostAddress(args.memory, address, size, "glBufferData");
            if (!host)
                return true;
        }
        impl.call<void (*)(GLenum, long, const void *, GLenum)>(
            "glBufferData", target, static_cast<long>(size), host, usage);
        return true;
    });
    bind("_glBufferSubData", "gles-forward-buffer-sub-data", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const auto offset = args.u32(1);
        const auto size = args.u32(2);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, args.pointer(3), size, "glBufferSubData"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, long, long, const void *)>("glBufferSubData", target,
                                                              static_cast<long>(offset),
                                                              static_cast<long>(size), host);
        return true;
    });

    // ---- textures -------------------------------------------------------------
    bind("_glGenTextures", "gles-forward-gen-textures", [&impl](Args &args) {
        impl.generateNames(args, "glGenTextures");
        return true;
    });
    bind("_glDeleteTextures", "gles-forward-delete-textures", [&impl](Args &args) {
        impl.deleteNames(args, "glDeleteTextures", [](GLuint) {});
        return true;
    });
    bind("_glBindTexture", "gles-forward-bind-texture", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLuint)>("glBindTexture", args.enumValue(0),
                                            static_cast<GLuint>(args.u32(1)));
        return true;
    });
    bind("_glTexParameteri", "gles-forward-tex-parameter", [&impl](Args &args) {
        impl.call<void (*)(GLenum, GLenum, GLint)>("glTexParameteri", args.enumValue(0),
                                                   args.enumValue(1), args.i32(2));
        return true;
    });
    auto texImage = [&impl](const char *name, Args &args, int pointerIndex) {
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const GLint internalFormat = args.i32(2);
        const GLsizei width = args.i32(3);
        const GLsizei height = args.i32(4);
        const GLint border = args.i32(5);
        const auto format = args.enumValue(6);
        const auto type = args.enumValue(7);
        const auto address = args.pointer(pointerIndex);
        const void *host = nullptr;
        if (address != 0) {
            std::size_t bytes = 0;
            if (!impl.pixelBytes(width, height, format, type, impl.unpackAlignment, bytes)) {
                impl.refuse(name, "unsupported or overflowing pixel format/range");
                return;
            }
            if (bytes != 0) {
                host = impl.hostAddress(args.memory, address, bytes, name);
                if (!host) return;
            }
        }
        impl.call<void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                           const void *)>(name, target, level, internalFormat, width, height,
                                          border, format, type, host);
    };
    bind("_glTexImage2D", "gles-forward-tex-image-2d", [texImage](Args &args) {
        texImage("glTexImage2D", args, 8);
        return true;
    });
    bind("_glTexSubImage2D", "gles-forward-tex-sub-image-2d", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const GLint xoffset = args.i32(2);
        const GLint yoffset = args.i32(3);
        const GLsizei width = args.i32(4);
        const GLsizei height = args.i32(5);
        const auto format = args.enumValue(6);
        const auto type = args.enumValue(7);
        const auto address = args.pointer(8);
        const void *host = nullptr;
        std::size_t bytes = 0;
        if (!impl.pixelBytes(width, height, format, type, impl.unpackAlignment, bytes)) {
            impl.refuse("glTexSubImage2D", "unsupported or overflowing pixel format/range");
            return true;
        }
        if (bytes != 0) {
            host = impl.hostAddress(args.memory, address, bytes, "glTexSubImage2D");
            if (!host) return true;
        }
        impl.call<void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                           const void *)>("glTexSubImage2D", target, level, xoffset, yoffset,
                                          width, height, format, type, host);
        return true;
    });
    bind("_glCompressedTexImage2D", "gles-forward-compressed-tex-image-2d", [&impl](Args &args) {
        // Compressed uploads (PVRTC and friends) go to the driver as-is: decoding them
        // is exactly the driver's job, and a driver without support reports the GL error.
        const auto target = args.enumValue(0);
        const GLint level = args.i32(1);
        const auto format = args.enumValue(2);
        const GLsizei width = args.i32(3);
        const GLsizei height = args.i32(4);
        const GLint border = args.i32(5);
        const GLsizei imageSize = args.i32(6);
        const auto address = args.pointer(7);
        auto *host = static_cast<const void *>(
            impl.hostAddress(args.memory, address, static_cast<std::size_t>(std::max(imageSize, 1)),
                             "glCompressedTexImage2D"));
        if (!host)
            return true;
        impl.call<void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)>(
            "glCompressedTexImage2D", target, level, format, width, height, border, imageSize,
            host);
        return true;
    });

    bind("_glReadPixels", "gles-forward-read-pixels", [&impl](Args &args) {
        const GLsizei width = args.i32(2), height = args.i32(3);
        const auto format = args.enumValue(4), type = args.enumValue(5);
        std::size_t bytes = 0;
        if (!impl.pixelBytes(width, height, format, type, impl.packAlignment, bytes)) {
            impl.refuse("glReadPixels", "unsupported or overflowing pixel format/range");
            return true;
        }
        void *host = nullptr;
        if (bytes != 0) {
            host = impl.hostOutput(args.memory, args.pointer(6), bytes, "glReadPixels");
            if (!host) return true;
        }
        impl.call<void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)>(
            "glReadPixels", args.i32(0), args.i32(1), width, height, format, type, host);
        return true;
    });

    // ---- OES framebuffer / renderbuffer objects --------------------------------
    bind("_glGenRenderbuffersOES", "gles-forward-gen-renderbuffers", [&impl](Args &args) {
        impl.generateNames(args, "glGenRenderbuffersOES");
        return true;
    });
    bind("_glDeleteRenderbuffersOES", "gles-forward-delete-renderbuffers", [&impl](Args &args) {
        impl.deleteNames(args, "glDeleteRenderbuffersOES", [&impl](GLuint name) { impl.deletedRenderbuffer(name); });
        return true;
    });
    bind("_glBindRenderbufferOES", "gles-forward-bind-renderbuffer", [&impl](Args &args) {
        if (impl.call<void (*)(GLenum, GLuint)>("glBindRenderbufferOES", args.enumValue(0), args.u32(1)) &&
            args.enumValue(0) == GL_RENDERBUFFER_OES)
            impl.boundRenderbuffer = args.i32(1);
        return true;
    });
    bind("_glRenderbufferStorageOES", "gles-forward-renderbuffer-storage", [&impl](Args &args) {
        if (args.enumValue(0) == GL_RENDERBUFFER_OES && impl.drawableRenderbuffers.count(impl.boundRenderbuffer)) {
            // Replacing drawable storage with an ordinary renderbuffer would
            // require remapping every attachment; don't pretend it succeeded.
            impl.refuse("glRenderbufferStorageOES", "replacing EAGL drawable storage is unsupported");
            return true;
        }
        impl.call<void (*)(GLenum, GLenum, GLsizei, GLsizei)>(
            "glRenderbufferStorageOES", args.enumValue(0), args.enumValue(1), args.i32(2),
            args.i32(3));
        return true;
    });
    bind("_glGetRenderbufferParameterivOES", "gles-forward-get-renderbuffer-parameter", [&impl](Args &args) {
        impl.queryRenderbuffer(args, "glGetRenderbufferParameterivOES");
        return true;
    });
    bind("_glGenFramebuffersOES", "gles-forward-gen-framebuffers", [&impl](Args &args) {
        impl.generateNames(args, "glGenFramebuffersOES");
        return true;
    });
    bind("_glDeleteFramebuffersOES", "gles-forward-delete-framebuffers", [&impl](Args &args) {
        impl.deleteNames(args, "glDeleteFramebuffersOES", [&impl](GLuint name) { impl.deletedFramebuffer(name); });
        return true;
    });
    bind("_glBindFramebufferOES", "gles-forward-bind-framebuffer", [&impl](Args &args) {
        const auto target = args.enumValue(0);
        const int name = args.i32(1);
        const auto hostName = target == GL_FRAMEBUFFER_OES && name != 0 && impl.drawableFramebuffers.count(name)
                                  ? 0u : args.u32(1);
        if (impl.call<void (*)(GLenum, GLuint)>("glBindFramebufferOES", target, hostName) && target == GL_FRAMEBUFFER_OES)
            impl.boundFramebuffer = name;
        return true;
    });
    bind("_glFramebufferRenderbufferOES", "gles-forward-framebuffer-renderbuffer", [&impl](Args &args) {
        const auto target = args.enumValue(0), attachment = args.enumValue(1), rbTarget = args.enumValue(2);
        const int renderbuffer = args.i32(3);
        const bool validTarget = target == GL_FRAMEBUFFER_OES && (renderbuffer == 0 || rbTarget == GL_RENDERBUFFER_OES) && impl.boundFramebuffer != 0;
        if (validTarget && attachment == GL_COLOR_ATTACHMENT0_OES && renderbuffer != 0 &&
            impl.drawableRenderbuffers.count(renderbuffer)) {
            impl.drawableFramebuffers[impl.boundFramebuffer] = impl.drawableRenderbuffers.at(renderbuffer);
            impl.call<void (*)(GLenum, GLuint)>("glBindFramebufferOES", GL_FRAMEBUFFER_OES, 0);
            return true;
        }
        if (validTarget && attachment == GL_COLOR_ATTACHMENT0_OES) impl.detachCurrentDrawableFramebuffer();
        impl.call<void (*)(GLenum, GLenum, GLenum, GLuint)>("glFramebufferRenderbufferOES",
                                                           target, attachment, rbTarget, args.u32(3));
        return true;
    });
    bind("_glFramebufferTexture2DOES", "gles-forward-framebuffer-texture", [&impl](Args &args) {
        const auto target = args.enumValue(0), attachment = args.enumValue(1), texTarget = args.enumValue(2);
        const auto texture = args.u32(3); const auto level = args.i32(4);
        if (target == GL_FRAMEBUFFER_OES && attachment == GL_COLOR_ATTACHMENT0_OES && impl.boundFramebuffer != 0 &&
            Impl::validFramebufferTexture(texTarget, texture, level)) impl.detachCurrentDrawableFramebuffer();
        impl.call<void (*)(GLenum, GLenum, GLenum, GLuint, GLint)>("glFramebufferTexture2DOES",
                                                                  target, attachment, texTarget, texture, level);
        return true;
    });
    bind("_glCheckFramebufferStatusOES", "gles-forward-check-framebuffer-status",
         [&impl](Args &args) {
             if (args.enumValue(0) == GL_FRAMEBUFFER_OES && impl.boundFramebuffer != 0 &&
                 impl.drawableFramebuffers.count(impl.boundFramebuffer) != 0) {
                 args.registers.r[0] = GL_FRAMEBUFFER_COMPLETE_OES;
                 return true;
             }
             auto *fn = impl.symbol<GLenum (*)(GLenum)>("glCheckFramebufferStatusOES");
             if (!fn) {
                 impl.refuse("glCheckFramebufferStatusOES");
                 args.registers.r[0] = 0; // no framebuffer status is available
                 return true;
             }
             args.registers.r[0] = fn(args.enumValue(0));
             ++impl.forwarded;
             return true;
         });

    // ---- queries ---------------------------------------------------------------
    bind("_glGetIntegerv", "gles-forward-get-integer", [&impl](Args &args) {
        const auto pname = args.enumValue(0);
        int count = impl.integerQueryCount(pname);
        if (pname == 0x86A3 || pname == 0x8DF8) { // compressed textures / shader binaries
            GLint length = -1;
            const GLenum countPname = pname == 0x86A3 ? 0x86A2 : 0x8DF9;
            if (!impl.call<void (*)(GLenum, GLint *)>("glGetIntegerv", countPname, &length)) return true;
            if (length < 0 || length > 16384) {
                impl.refuse("glGetIntegerv", "invalid variable-length query count");
                return true;
            }
            count = length;
        }
        if (count < 0) {
            impl.refuse("glGetIntegerv", "unsupported extension query size");
            return true;
        }
        const auto bytes = static_cast<std::size_t>(count) * sizeof(GLint);
        if (bytes != 0 && !args.memory.contains(args.pointer(1), bytes, MemoryPermission::Write)) {
            impl.refuse("glGetIntegerv", "query output is not writable");
            return true;
        }
        std::array<GLint, 16> small{};
        std::vector<GLint> large;
        if (count > static_cast<int>(small.size())) large.resize(static_cast<std::size_t>(count));
        GLint *values = large.empty() ? small.data() : large.data();
        if (bytes) (void)args.memory.read(args.pointer(1), values, bytes);
        // Drawable FBOs use the EGL default framebuffer internally, but the
        // guest must see the object name it bound.
        if (pname == 0x8CA6) values[0] = impl.boundFramebuffer;       // FRAMEBUFFER_BINDING
        else if (pname == 0x8CA7) values[0] = impl.boundRenderbuffer; // RENDERBUFFER_BINDING
        else if (!impl.call<void (*)(GLenum, GLint *)>("glGetIntegerv", pname, values)) return true;
        if (bytes) (void)args.memory.write(args.pointer(1), values, bytes);
        return true;
    });
    bind("_glGetString", "gles-forward-get-string", [&impl](Args &args) {
        auto *fn = impl.symbol<const unsigned char *(*)(GLenum)>("glGetString");
        if (!fn) {
            impl.refuse("glGetString");
            args.registers.r[0] = 0;
            return true;
        }
        const auto key = std::make_pair(impl.driver.api, args.enumValue(0));
        const auto existing = impl.guestDriverStrings.find(key);
        if (existing != impl.guestDriverStrings.end() &&
            args.memory.contains(existing->second, 1, MemoryPermission::Read)) {
            args.registers.r[0] = existing->second;
            return true;
        }
        const auto *text = fn(args.enumValue(0));
        ++impl.forwarded;
        if (!text) {
            args.registers.r[0] = 0;
            return true;
        }
        // Copy the driver string into guest memory so the guest owns the bytes.
        const std::size_t length = std::strlen(reinterpret_cast<const char *>(text)) + 1;
        const auto address = args.memory.mapAny(length, MemoryPermission::Read | MemoryPermission::Write,
                                                "gles-driver-string", 16);
        if (!args.memory.write(address, text, length)) {
            impl.refuse("glGetString", "the driver string could not be copied into guest memory");
            args.registers.r[0] = 0;
            return true;
        }
        impl.guestDriverStrings[key] = address;
        args.registers.r[0] = address;
        return true;
    });

#include "gles2_bindings.inc"
}

// ---------------------------------------------------------------------------
// Public plumbing
// ---------------------------------------------------------------------------
Forwarder::Forwarder() : impl_(new Impl()) {}
Forwarder::~Forwarder() {
    // Clear the non-owning active pointer before the object becomes invalid.
    deactivateForwarder(this);
    if (impl_)
        impl_->destroyEgl();
}

bool Forwarder::supportsApi(unsigned api) const {
    if (api == 1) return impl_->driver.glesLoaded;
    if (api != 2) return false;
    const bool loaded = impl_->driver.loadGles2();
    impl_->report.gles2Loaded = loaded;
    impl_->report.detail = impl_->driver.detail;
    recordDriver(impl_->report);
    return loaded;
}

bool Forwarder::makeCurrent(GuestAddress context, unsigned api, std::uint32_t width,
                            std::uint32_t height) {
    if (context == 0 || !supportsApi(api) || !impl_->driver.eglLoaded)
        return false;
    // Independent or shared multiple guest contexts are not yet implemented.
    // Refuse a second object rather than silently aliasing its GL state.
    if (impl_->contextOwner != 0 && impl_->contextOwner != context) {
        impl_->note("multiple EAGL contexts are not supported by the Native bridge");
        return false;
    }
    if (impl_->contextOwner == context && impl_->driver.api != api)
        return false;
    impl_->driver.api = api;
    if (impl_->egl.context && impl_->egl.surface) {
        const auto window = impl_->windowSnapshot();
        if (impl_->egl.windowSurface &&
            (!window.pointer() || impl_->egl.boundWindow.generation != window.generation)) {
            std::string why;
            if (!window.pointer() || !impl_->rebindWindowSurface(window, why)) {
                impl_->note("EAGL context cannot use the detached or replaced Android window" +
                            (why.empty() ? std::string() : ": " + why));
                return false;
            }
            return true;
        }
        auto make = impl_->symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
        if (!make || make(impl_->egl.display, impl_->egl.surface, impl_->egl.surface,
                          impl_->egl.context) != EGL_TRUE_VALUE)
            return false;
        impl_->egl.current = true;
        return true;
    }
    std::string why;
    if (!impl_->createEglSurface(static_cast<int>(std::max(width, 1u)),
                                 static_cast<int>(std::max(height, 1u)), why)) {
        impl_->note("EAGL context creation failed: " + why);
        impl_->destroyEgl();
        return false;
    }
    impl_->contextOwner = context;
    return true;
}

bool Forwarder::clearCurrent() {
    if (!impl_->egl.context || !impl_->egl.display) return true;
    auto make = impl_->symbol<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent");
    if (!make || make(impl_->egl.display, nullptr, nullptr, nullptr) != EGL_TRUE_VALUE)
        return false;
    impl_->egl.current = false;
    return true;
}

void Forwarder::setNativeWindow(void *window) { impl_->setWindow(window); }

bool Forwarder::attachDrawable(GuestAddressSpace &memory, std::uint32_t width,
                              std::uint32_t height) {
    return impl_->attachDrawable(memory, width, height);
}

bool Forwarder::presentDrawable() { return impl_->presentDrawable(); }

const DriverReport &Forwarder::driver() const { return impl_->report; }

bool Forwarder::drawableReady() const { return impl_->drawableAttached && impl_->egl.current && impl_->egl.surface; }
bool Forwarder::presentingToWindow() const { return impl_->egl.windowSurface; }
std::uint32_t Forwarder::drawableWidth() const { return impl_->drawableWidth; }
std::uint32_t Forwarder::drawableHeight() const { return impl_->drawableHeight; }
std::uint64_t Forwarder::guestCallsObserved() const { return impl_->guestCalls; }
std::uint64_t Forwarder::forwardedCalls() const { return impl_->forwarded; }
std::uint64_t Forwarder::refusedCalls() const { return impl_->refused; }
std::uint64_t Forwarder::framesPresented() const { return impl_->frames; }
const std::vector<std::string> &Forwarder::diagnostics() const { return impl_->diagnostics; }

namespace {
std::mutex gRegistryMutex;
std::map<GuestAddressSpace *, std::unique_ptr<Forwarder>> gForwarders;
Forwarder *gLastForwarder = nullptr;
std::shared_ptr<NativeWindowRef> gDefaultNativeWindow;
} // namespace

Forwarder &forwarderFor(GuestAddressSpace &memory) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto found = gForwarders.find(&memory);
    if (found == gForwarders.end()) {
        auto forwarder = std::make_unique<Forwarder>();
        forwarder->setNativeWindow(gDefaultNativeWindow ? gDefaultNativeWindow->pointer : nullptr);
        gLastForwarder = forwarder.get();
        found = gForwarders.emplace(&memory, std::move(forwarder)).first;
    }
    gLastForwarder = found->second.get();
    return *found->second;
}

void activateForwarder(Forwarder *forwarder) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gLastForwarder = forwarder;
    if (forwarder)
        forwarder->setNativeWindow(gDefaultNativeWindow ? gDefaultNativeWindow->pointer : nullptr);
}

void deactivateForwarder(Forwarder *forwarder) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    if (gLastForwarder == forwarder)
        gLastForwarder = nullptr;
}

void setDefaultNativeWindow(void *window) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    if ((!gDefaultNativeWindow && window) ||
        (gDefaultNativeWindow && gDefaultNativeWindow->pointer != window))
        gDefaultNativeWindow = window ? std::make_shared<NativeWindowRef>(window) : nullptr;
    if (gLastForwarder)
        gLastForwarder->setNativeWindow(window);
}

Forwarder *lastForwarder() {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    return gLastForwarder;
}

} // namespace radek::compat_runtime::gles
