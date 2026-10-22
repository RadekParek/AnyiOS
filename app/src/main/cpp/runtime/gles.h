#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "memory.h"

namespace podnative::runtime {
// OpenGL ES 1.1 over a real driver.
//
// The guest is ARM code, so every GL entry point is a call gate: the CPU stops on the
// SVC, this class marshals the guest's registers into the C prototype the driver
// expects and performs the actual call. Nothing here answers "success" on its own:
// when no driver is loaded every GL symbol stays unbound and the guest stops at the
// first GL call with its name.
//
// The driver this app ships is ANGLE, which is OpenGL ES 2.0 and has no ES 1.1 frontend
// at all, while the titles this runtime runs are ES 1.1: they live on the fixed-function
// entry points (glVertexPointer, glMatrixMode, glColor4f, glTexEnvi) that ANGLE does not
// export. Rather than fall back to whichever driver the device's GPU vendor shipped, and
// inherit a different set of quirks per device, the bridge answers those entry points
// itself: it keeps the ES 1.1 client and matrix state and turns each draw into a generated
// ES 2.0 shader draw over buffer objects. One driver family everywhere the app runs. What
// the emulation approximates rather than implements is counted and named in describe(), so
// a wrong pixel is traceable to a decision instead of being silent.
//
// Two details are easy to get wrong and are handled explicitly:
//
//  * Argument registers. ARM iOS code uses the VFP variant of AAPCS, so integer and
//    pointer arguments live in r0-r3 (then the stack) while `float` arguments live in
//    s0-s15, each kind numbered independently. A mis-marshalled glClearColor would
//    clear to a colour read out of a pointer register.
//  * Guest pointers. Client-side vertex/colour/normal/texcoord arrays are only read
//    when the draw happens, so they are copied into host memory at draw time; pixel
//    uploads and `glGet*` results are copied in and out of guest memory around the call.
class GLES final {
   public:
    explicit GLES(Memory& memory);
    ~GLES();
    GLES(const GLES&) = delete;
    GLES& operator=(const GLES&) = delete;

    // Settings ▸ "Native OpenGL ES". Off, the driver is the ANGLE this app bundles: one
    // implementation with one set of quirks on every device, which is the point of shipping
    // it. On, the device's own driver is tried first and ANGLE becomes the fallback, for a
    // title that needs something the bundled driver does not provide. Either way, an ES 1.1
    // title whose fixed-function entry points no loaded driver exports is served by this
    // bridge emulating them over ES 2.0. Read before the first GL call; a loaded driver is
    // not swapped underneath a running guest.
    void setPreferNativeDriver(bool on) { preferNativeDriver = on; }
    bool prefersNativeDriver() const { return preferNativeDriver; }
    // Resolves the driver and its entry points. Safe to call more than once.
    bool load();
    bool available() const { return !functions.empty(); }
    // Real library that answered (ANGLE on Android, the system GLES elsewhere).
    const std::string& backend() const { return backendName; }
    // Every GL symbol this bridge implements, as Mach-O import names (`_glClear`).
    std::vector<std::string> symbols() const;
    // True when the loaded driver has no ES 1.1 frontend, so this bridge is the one
    // answering the fixed-function entry points.
    bool emulatesFixedFunction() const { return fixedFunction; }
    // True when `symbol` was a GL/EGL entry point this bridge owns.
    bool invoke(const std::string& symbol, CpuState& state);
    // "Verbose logging": every GL call with its arguments plus the real glGetError result,
    // decoded. Off by default; it never changes what is drawn.
    void setVerbose(bool on) { verbose = on; }
    void setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
    // One-line evidence for the launch report: backend, driver strings and counters.
    std::string describe() const;
    size_t callCount() const { return calls; }
    size_t drawCount() const { return draws; }

    // EAGL support. The context is created lazily, on the first EAGL call the guest
    // makes, because a title may create several contexts and switch between them.
    // The API version is the one the guest asked for in -[EAGLContext initWithAPI:]. A
    // driver with an ES 1.1 frontend gets a 1.1 context, because a 2.0 context rejects the
    // fixed-function calls (glMatrixMode, glVertexPointer, glColor4f) the title lives on.
    // ANGLE has no 1.1 context to give, so when this bridge is the one emulating the
    // fixed function it asks for a 2.0 context and answers those calls itself.
    void setClientApi(int api);
    int clientApiVersion() const { return clientApi; }
    bool makeCurrent();
    // Creates the colour renderbuffer and the framebuffer an EAGL drawable backs onto.
    // Returns false when the driver refused; the caller then reports the real error.
    bool createDrawable(int width, int height);
    // Copies the drawable's pixels into guest memory as tightly packed RGBA8 and, when a
    // window surface exists, swaps it onto the display.
    bool present();
    // Host-visible copy of the last presented frame (RGBA8, top row first).
    const std::vector<uint8_t>& lastFrame() const { return frame; }
    int drawableWidth() const { return width; }
    int drawableHeight() const { return height; }

   private:
    using Entry = void (*)();
    struct ClientArray {
        bool enabled = false;
        int size = 0;
        uint32_t type = 0;
        int stride = 0;
        uint32_t pointer = 0;
        bool buffered = false;  // the pointer is an offset into a bound buffer object
    };
    // Reads the guest's argument registers the way AAPCS-VFP lays them out: integers and
    // pointers from r0-r3 and then the stack, floats from s0-s15, each numbered separately.
    struct Marshal {
        const CpuState& cpu;
        const Memory& memory;
        size_t core = 0;
        size_t fp = 0;
        uint32_t word();
        float f32();
    };
    Memory& memory;
    std::string backendName;
    std::map<std::string, Entry> functions;
    size_t calls = 0, draws = 0;
    bool driverLoaded = false;
    // Display/context/drawable. Opaque so this header stays free of EGL types.
    // Handles of the libraries the entry points were resolved from. Resolving through a
    // handle instead of the process-global table is what keeps ANGLE's EGL and ANGLE's
    // GLESv2 together: Android also has a system libEGL in memory, and mixing the two
    // produces a display that the other library knows nothing about.
    void* eglLibrary = nullptr;
    void* glesLibrary = nullptr;
    // A device driver keeps OpenGL ES 1.1 in a library of its own (libGLESv1_CM.so), which
    // the ES 2.0 library does not re-export. Consulted only for names the ES 2.0 handle does
    // not have, so a driver that provides both is never mixed with itself.
    void* gles1Library = nullptr;
    bool preferNativeDriver = false;
    // Which libraries answered, for the launch report: "the driver loaded" is not evidence
    // of which driver that was.
    std::string driverLibraries;
    void* display = nullptr;
    void* context = nullptr;
    void* surface = nullptr;
    uint32_t framebuffer = 0, renderbuffer = 0;
    int clientApi = 1;  // kEAGLRenderingAPIOpenGLES1 until the guest asks otherwise
    int width = 0, height = 0;
    std::vector<uint8_t> frame;
    // Client-side array state, one set per active client texture unit.
    ClientArray vertex, colour, normal;
    std::map<int, ClientArray> texCoord;
    int clientTextureUnit = 0x84c0;  // GL_TEXTURE0, the enum a guest passes to select it
    bool arrayBufferBound = false, elementBufferBound = false;

    // ---- ES 1.1 fixed function, emulated when the driver is ES 2.0-only ------------------
    // Everything here is state a real ES 1.1 driver would keep. It is only touched in that
    // mode; with an ES 1.1 driver the entry points are forwarded and this stays empty.
    bool fixedFunction = false;
    // Column-major, exactly as ES 1.1 stores them: glLoadMatrixf copies 16 floats in.
    static constexpr std::array<float, 16> Identity = {1, 0, 0, 0, 0, 1, 0, 0,
                                                       0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> modelview = Identity, projection = Identity, textureMatrix = Identity;
    std::array<float, 16>* matrix = &modelview;  // the stack glMatrixMode selected
    uint32_t matrixMode = 0x1700;                // GL_MODELVIEW
    float currentColour[4] = {1, 1, 1, 1};       // glColor4f, used when no colour array is
    uint32_t boundArrayBuffer = 0, boundElementBuffer = 0;
    int textureEnvMode = 0x2100;  // GL_MODULATE, the ES 1.1 default
    bool texture2dEnabled = false, lightingEnabled = false;
    // Approximations, counted so describe() can name them instead of hiding them.
    size_t lightingCalls = 0, approximatedTexEnv = 0, extraTextureUnits = 0;
    int activeTextureUnit = 0;  // glActiveTexture, as an offset from GL_TEXTURE0
    // A generated program and the uniform locations a draw needs from it, so a draw never
    // asks the driver for a location it already asked for.
    struct Program {
        uint32_t id = 0;
        int mvp = -1, colour = -1, texture = -1, textureMatrix = -1;
    };
    std::map<uint32_t, Program> programs;     // variant key -> compiled program
    std::array<uint32_t, 3> streamBuffers{};  // position, colour, texcoord uploads
    uint32_t indexBuffer = 0, activeProgram = 0;

    // The ES 1.1 entry points the driver does not have, answered from the state above.
    bool fixedFunctionEntry(const std::string& name, Marshal& m);
    // One draw through a generated program: client arrays become buffer objects, the
    // modelview and projection the guest loaded become a matrix uniform.
    bool drawFixedFunction(uint32_t mode, int first, int count, uint32_t indexType,
                           uint32_t indices);
    const Program& fixedFunctionProgram();
    // Uploads one client array and points an attribute at it. Returns false when the guest
    // memory behind it could not be read.
    bool bindFixedFunctionArray(const ClientArray& array, uint32_t location, uint32_t& buffer,
                                bool normalise, size_t count);
    void useFixedFunctionProgram(uint32_t program);

    Entry function(const char* name) const;
    bool resolve(const char* name);
    // Takes a handle on a driver library, preferring one the linker already loaded.
    static void* openLibrary(const char* name);
    // One lookup in a handle, then through eglGetProcAddress, then the other handle.
    void* lookup(const char* name) const;
    // EGL is not in ImportNames (the guest never calls it), so its entry points are
    // resolved on use; every caller goes through here instead of fn() directly.
    template <typename Fn>
    Fn egl(const char* name) {
        resolve(name);
        return fn<Fn>(name);
    }
    // Same for the entry points the runtime itself needs to drive a drawable: the guest
    // imports the OES spellings, the framebuffer objects are also reached by their core
    // names, and resolve() tries both.
    template <typename Fn>
    Fn glFn(const char* name) {
        resolve(name);
        return fn<Fn>(name);
    }
    template <typename Fn>
    Fn fn(const char* name) const {
        return reinterpret_cast<Fn>(function(name));
    }
    // Copies every enabled client array into host memory for the draw that follows.
    bool bindClientArrays(size_t count, std::vector<std::vector<uint8_t>>& storage);
    void setError(const std::string& text);
    std::string lastError;
    // The dispatch table itself, without the tracing wrapper.
    bool invokeEntry(const std::string& symbol, CpuState& state);
    // Reads the driver's error state after a call and names it, or returns an empty string.
    std::string takeError();
    bool verbose = false;
    std::function<void(const std::string&)> logSink;
};
}  // namespace podnative::runtime
