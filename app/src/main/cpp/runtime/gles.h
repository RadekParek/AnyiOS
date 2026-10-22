#pragma once
#include <cstdint>
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

    // Resolves the driver and its entry points. Safe to call more than once.
    bool load();
    bool available() const { return !functions.empty(); }
    // Real library that answered (ANGLE on Android, the system GLES elsewhere).
    const std::string& backend() const { return backendName; }
    // Every GL symbol this bridge implements, as Mach-O import names (`_glClear`).
    std::vector<std::string> symbols() const;
    // True when `symbol` was a GL/EGL entry point this bridge owns.
    bool invoke(const std::string& symbol, CpuState& state);
    // One-line evidence for the launch report: backend, driver strings and counters.
    std::string describe() const;
    size_t callCount() const { return calls; }
    size_t drawCount() const { return draws; }

    // EAGL support. The context is created lazily, on the first EAGL call the guest
    // makes, because a title may create several contexts and switch between them.
    // The API version is the one the guest asked for in -[EAGLContext initWithAPI:]; an
    // ES 1.1 title must get an ES 1.1 context, because a 2.0 context rejects the
    // fixed-function calls (glMatrixMode, glVertexPointer, glColor4f) it lives on.
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
    int clientTextureUnit = 0;
    bool arrayBufferBound = false, elementBufferBound = false;

    Entry function(const char* name) const;
    bool resolve(const char* name);
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
};
}  // namespace podnative::runtime
