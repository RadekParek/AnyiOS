#include "gles.h"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "error.h"
#include "pvrtc.h"

namespace podnative::runtime {
namespace {
// GL constants the bridge has to interpret itself (everything else is passed through).
constexpr uint32_t GL_BYTE = 0x1400;
constexpr uint32_t GL_UNSIGNED_BYTE = 0x1401;
constexpr uint32_t GL_SHORT = 0x1402;
constexpr uint32_t GL_UNSIGNED_SHORT = 0x1403;
constexpr uint32_t GL_FLOAT = 0x1406;
constexpr uint32_t GL_FIXED = 0x140c;
constexpr uint32_t GL_UNSIGNED_SHORT_5_6_5 = 0x8363;
constexpr uint32_t GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033;
constexpr uint32_t GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034;
constexpr uint32_t GL_ALPHA = 0x1906;
constexpr uint32_t GL_RGB = 0x1907;
constexpr uint32_t GL_RGBA = 0x1908;
constexpr uint32_t GL_LUMINANCE = 0x1909;
constexpr uint32_t GL_LUMINANCE_ALPHA = 0x190a;
// ES 1.1 state an ES 2.0-only driver cannot hold, so the bridge holds it.
constexpr uint32_t GL_TEXTURE_2D = 0x0de1;
constexpr uint32_t GL_LIGHTING = 0x0b50;
constexpr uint32_t GL_TEXTURE_ENV_MODE = 0x2200;
constexpr uint32_t GL_ADD_TEXTURE_ENV = 0x0104;
constexpr uint32_t GL_BLEND_TEXTURE_ENV = 0x0be2;
constexpr uint32_t GL_REPLACE_TEXTURE_ENV = 0x1e01;
constexpr uint32_t GL_MODULATE_TEXTURE_ENV = 0x2100;
constexpr uint32_t GL_DECAL_TEXTURE_ENV = 0x2101;
constexpr uint32_t GL_MODELVIEW = 0x1700;
constexpr uint32_t GL_PROJECTION = 0x1701;
constexpr uint32_t GL_TEXTURE_MATRIX = 0x1702;
constexpr uint32_t GL_TEXTURE0 = 0x84c0;
constexpr uint32_t FixedFunctionTextureUnits = 2;
constexpr uint32_t GL_MAX_LIGHTS = 0x0d31;
// 0x8482, not 0x84e8: that one is GL_MAX_RENDERBUFFER_SIZE, and answering a texture-unit
// query with a renderbuffer limit is the kind of mistake no driver ever reports.
constexpr uint32_t GL_MAX_TEXTURE_UNITS = 0x8482;
constexpr uint32_t GL_MAX_MODELVIEW_STACK_DEPTH = 0x0d36;
constexpr uint32_t GL_MAX_PROJECTION_STACK_DEPTH = 0x0d38;
constexpr uint32_t GL_MAX_TEXTURE_STACK_DEPTH = 0x0d39;
constexpr uint32_t GL_TEXTURE_ENV = 0x1703;
constexpr uint32_t GL_STREAM_DRAW = 0x88e0;
// Attribute locations the generated programs bind by name, so a draw never has to ask the
// driver where an attribute ended up.
constexpr uint32_t AttributePosition = 0, AttributeColour = 1, AttributeTexCoord = 2,
                   AttributeNormal = 3;
constexpr uint32_t GL_VERTEX_SHADER = 0x8b31;
constexpr uint32_t GL_FRAGMENT_SHADER = 0x8b30;
constexpr uint32_t GL_COMPILE_STATUS = 0x8b81;
constexpr uint32_t GL_LINK_STATUS = 0x8b82;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074;
constexpr uint32_t GL_NORMAL_ARRAY = 0x8075;
constexpr uint32_t GL_COLOR_ARRAY = 0x8076;
constexpr uint32_t GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892;
constexpr uint32_t GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t GL_FRAMEBUFFER = 0x8d40;
constexpr uint32_t GL_FRAMEBUFFER_BINDING = 0x8ca6;
constexpr uint32_t GL_RGBA4 = 0x8056;
constexpr uint32_t GL_RGBA8 = 0x8058;
// EGL bits used by the drawable path; spelled out so this file needs no EGL headers and
// still compiles where the driver is loaded at run time.
constexpr int32_t EGL_NONE = 0x3038;
constexpr int32_t EGL_RENDERABLE_TYPE = 0x3040;
constexpr int32_t EGL_OPENGL_ES_BIT = 0x0001;
constexpr int32_t EGL_OPENGL_ES2_BIT = 0x0004;
constexpr int32_t EGL_CONTEXT_CLIENT_VERSION = 0x3098;
constexpr int32_t EGL_SURFACE_TYPE = 0x3033;
constexpr int32_t EGL_PBUFFER_BIT = 0x0001;
constexpr int32_t EGL_RED_SIZE = 0x3024;
constexpr int32_t EGL_GREEN_SIZE = 0x3023;
constexpr int32_t EGL_BLUE_SIZE = 0x3022;
constexpr int32_t EGL_ALPHA_SIZE = 0x3021;
constexpr int32_t EGL_DEPTH_SIZE = 0x3025;
constexpr int32_t EGL_PLATFORM_SURFACELESS_MESA = 0x31dd;
constexpr int32_t EGL_PLATFORM_ANGLE_ANGLE = 0x3202;
constexpr int32_t EGL_PLATFORM_ANGLE_TYPE_ANGLE = 0x3203;
constexpr int32_t EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE = 0x3450;
constexpr uint32_t EGL_OPENGL_ES_API = 0x30a0;
constexpr int32_t EGL_WIDTH = 0x3057;
constexpr int32_t EGL_HEIGHT = 0x3056;

// The largest index in a client-side index array, which is how far an indexed draw reaches
// into the vertex arrays. Guest ARM is little-endian, so the bytes are assembled directly.
size_t largestIndex(const std::vector<uint8_t>& indices, size_t element) {
    size_t largest = 0;
    if (element == 1) {
        for (uint8_t value : indices) largest = std::max<size_t>(largest, value);
        return largest;
    }
    if (element == 2) {
        for (size_t at = 0; at + 1 < indices.size(); at += 2)
            largest =
                std::max<size_t>(largest, size_t(indices[at]) | (size_t(indices[at + 1]) << 8));
        return largest;
    }
    for (size_t at = 0; at + 3 < indices.size(); at += 4) {
        uint32_t value = 0;
        std::memcpy(&value, &indices[at], 4);
        largest = std::max<size_t>(largest, value);
    }
    return largest;
}
// Column-major 4x4 product, in the layout ES 1.1 stores its matrices in. A vertex reaches
// clip space as projection * modelview * v, so that is the order the two are combined.
void multiplyMatrices(const float* left, const float* right, float* out) {
    for (int column = 0; column < 4; column++)
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += left[k * 4 + row] * right[column * 4 + k];
            out[column * 4 + row] = sum;
        }
}
// A draw is refused rather than allowed to ask for an absurd copy of guest memory.
constexpr size_t VertexBudget = 8 * 1024 * 1024;

size_t typeSize(uint32_t type) {
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
// Bytes per texel for the format/type combinations ES 1.1 defines.
size_t texelBytes(uint32_t format, uint32_t type) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_ALPHA:
            case GL_LUMINANCE:
                return 1;
            case GL_LUMINANCE_ALPHA:
                return 2;
            case GL_RGB:
                return 3;
            case GL_RGBA:
                return 4;
            default:
                return 0;
        }
    }
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;
    return 0;
}
// Every GL entry point this bridge implements. Keeping it in one place makes the
// unimplemented set explicit: anything absent stays an unimplemented-import trap.
const char* const ImportNames[] = {
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
};
}  // namespace

uint32_t GLES::Marshal::word() {
    const size_t index = core++;
    if (index < 4) return cpu.registers[index];
    return memory.read32(cpu.registers[13] + uint32_t((index - 4) * 4));
}

float GLES::Marshal::f32() {
    // s0-s15 are the low words of d0-d7 in the guest's VFP register file.
    const size_t index = fp++;
    if (index >= 16) return 0;
    const uint32_t bits = cpu.extensionRegisters[index];
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

GLES::GLES(Memory& memory) : memory(memory) {}
GLES::~GLES() {
    if (display) {
        using Display = void*;
        using Surface = void*;
        using Context = void*;
        using Boolean = uint32_t;
        if (auto makeCurrentFn =
                egl<Boolean (*)(Display, Surface, Surface, Context)>("eglMakeCurrent"))
            makeCurrentFn(display, nullptr, nullptr, nullptr);
        if (surface) {
            if (auto destroySurface = egl<Boolean (*)(Display, Surface)>("eglDestroySurface"))
                destroySurface(display, surface);
        }
        if (context) {
            if (auto destroyContext = egl<Boolean (*)(Display, Context)>("eglDestroyContext"))
                destroyContext(display, context);
        }
        if (auto terminate = egl<Boolean (*)(Display)>("eglTerminate")) terminate(display);
        if (auto releaseThread = egl<Boolean (*)()>("eglReleaseThread")) releaseThread();
    }
    if (gles1Library) dlclose(gles1Library);
    if (glesLibrary) dlclose(glesLibrary);
    if (eglLibrary) dlclose(eglLibrary);
}

GLES::Entry GLES::function(const char* name) const {
    auto found = functions.find(name);
    return found == functions.end() ? nullptr : found->second;
}

void* GLES::lookup(const char* name) const {
    // The guest's GL symbols and the runtime's own EGL calls must come from one driver
    // family, so the handle of the library that owns the name is tried first.
    const bool eglName = std::strncmp(name, "egl", 3) == 0;
    void* preferred = eglName ? eglLibrary : glesLibrary;
    void* fallback = eglName ? glesLibrary : eglLibrary;
    if (preferred) {
        if (void* symbol = dlsym(preferred, name)) return symbol;
    }
    if (fallback) {
        if (void* symbol = dlsym(fallback, name)) return symbol;
    }
    // A device driver's ES 1.1 library is the last place an ES 1.1 entry point can be: the
    // ES 2.0 library beside it does not re-export them. Consulted only for names neither of
    // the other two answered, so nothing is taken from a second driver family.
    if (gles1Library) {
        if (void* symbol = dlsym(gles1Library, name)) return symbol;
    }
    return nullptr;
}

bool GLES::resolve(const char* name) {
    if (function(name)) return true;
    void* symbol = lookup(name);
    auto getProc = fn<void* (*)(const char*)>("eglGetProcAddress");
    if (!symbol && getProc) symbol = getProc(name);
    if (!symbol && !eglLibrary && !glesLibrary) symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) {
        // The framebuffer/renderbuffer entry points exist twice: as the OES extension an
        // ES 1.1 title imports, and as the core ES 2.0 names. The runtime asks for the core
        // one when it drives a drawable, so both directions are tried: an ES 1.1 context
        // only answers the OES spelling, an ES 2.0 one only the core spelling.
        const std::string full(name);
        const bool oes = full.size() > 3 && full.compare(full.size() - 3, 3, "OES") == 0;
        const std::string alternate = oes ? full.substr(0, full.size() - 3) : full + "OES";
        symbol = lookup(alternate.c_str());
        if (!symbol && getProc) symbol = getProc(alternate.c_str());
        if (!symbol && !eglLibrary && !glesLibrary) symbol = dlsym(RTLD_DEFAULT, alternate.c_str());
    }
    if (!symbol) return false;
    functions.emplace(name, reinterpret_cast<Entry>(symbol));
    return true;
}

void* GLES::openLibrary(const char* name) {
    // Both ANGLE libraries are linked into this app, so they are already resident: taking a
    // handle on them must not load a second copy, and RTLD_NOLOAD is how a handle is taken
    // on what the linker already brought in. RTLD_GLOBAL stays, because the recording driver
    // the host tests use is reached through the same global table the bridge resolves from.
    if (void* resident = dlopen(name, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL)) return resident;
    return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}

bool GLES::load() {
    if (driverLoaded) return available();
    driverLoaded = true;
    // Driver families, each an EGL library plus the GLES library that goes with it. They are
    // tried as families on purpose: an EGL from one driver and a GLES from another produce a
    // display the two disagree about, which is a black screen with no error anywhere. The
    // bundled ANGLE is first, so GL behaves the same on every device instead of depending on
    // what the GPU vendor shipped; Settings ▸ "Native OpenGL ES" moves the device's own
    // driver to the front for a title that needs something ANGLE does not provide. A device
    // driver keeps OpenGL ES 1.1 in a third library, which ANGLE has no equivalent of.
    struct Family {
        const char* egl;
        const char* gles;
        const char* gles1;
    };
    const Family bundled{"libEGL_podnative.so", "libGLESv2_podnative.so", nullptr};
    const Family device{"libEGL.so", "libGLESv2.so", "libGLESv1_CM.so"};
    const Family deviceSoname{"libEGL.so", "libGLESv2.so.2", nullptr};
    const std::vector<Family> families = preferNativeDriver
                                             ? std::vector<Family>{device, deviceSoname, bundled}
                                             : std::vector<Family>{bundled, device, deviceSoname};
    for (const Family& family : families) {
        void* gles = openLibrary(family.gles);
        if (!gles) continue;
        glesLibrary = gles;
        // EGL is optional per family: a driver that ships both in one library (the recording
        // driver the host tests use) answers EGL names from the GLES handle, which lookup()
        // already falls back to.
        if (family.egl) eglLibrary = openLibrary(family.egl);
        if (family.gles1) gles1Library = openLibrary(family.gles1);
        backendName = family.gles;
        driverLibraries = family.gles;
        if (eglLibrary) driverLibraries = std::string(family.egl) + " + " + driverLibraries;
        if (gles1Library) driverLibraries += std::string(" + ") + family.gles1;
        break;
    }
    // A statically linked driver still answers through the global symbol table.
    if (resolve("eglGetProcAddress") || resolve("eglGetDisplay")) {
        if (backendName.empty()) backendName = "process";
    }
    size_t resolved = 0;
    for (const char* import : ImportNames) {
        if (resolve(import + 1)) resolved++;
    }
    if (!resolved) {
        functions.clear();
        backendName.clear();
        return false;
    }
    // ANGLE is OpenGL ES 2.0 and has no ES 1.1 frontend, so the fixed-function entry points
    // an ES 1.1 title imports are simply absent from it. Detecting that here, once, is what
    // lets the bridge answer those calls itself and draw through generated ES 2.0 shaders
    // instead of falling back to the device vendor's driver and its own set of quirks. A
    // driver that does have ES 1.1 keeps forwarding everything, and none of the emulation
    // state below is touched.
    fixedFunction = function("glDrawArrays") && !function("glVertexPointer");
    return true;
}

std::vector<std::string> GLES::symbols() const {
    // Ownership does not depend on the driver: these are the imports this bridge answers
    // for. Without a driver each one keeps its trap, so the guest stops on the exact name.
    std::vector<std::string> names;
    for (const char* import : ImportNames) names.emplace_back(import);
    return names;
}

std::string GLES::describe() const {
    std::ostringstream out;
    if (!available()) {
        out << "GLES_STATE: no OpenGL ES driver is loaded, so GL imports stay unimplemented";
        return out.str();
    }
    out << "GLES_STATE: " << functions.size() << " entry points through " << backendName << ", "
        << calls << " guest GL calls, " << draws << " draws";
    // "The driver loaded" is not evidence of which driver that was, so the libraries that
    // actually answered are named, and with them the setting that chose the order.
    if (!driverLibraries.empty())
        out << " [" << driverLibraries << ", "
            << (preferNativeDriver ? "native OpenGL ES preferred" : "bundled ANGLE preferred")
            << "]";
    if (fixedFunction) {
        out << ", OpenGL ES 1.1 fixed function emulated over ES 2.0 (" << programs.size()
            << " generated program(s))";
        // An approximation that is not reported is a wrong pixel nobody can trace.
        if (lightingCalls)
            out << ", " << lightingCalls
                << " glLightfv/glMaterialfv call(s) recorded and lighting not emulated";
        if (approximatedTexEnv)
            out << ", " << approximatedTexEnv
                << " draw(s) used a texture environment mode this bridge approximates with "
                   "modulate";
        if (clientTextureUnitUses)
            out << ", " << clientTextureUnitUses << " non-default client texture-unit selection(s)";
        if (nonDefaultTextureUnitDraws)
            out << ", " << nonDefaultTextureUnitDraws
                << " draw(s) used a non-default active texture unit; simultaneous texture stages "
                   "are not emulated";
        if (simultaneousTextureStageDraws)
            out << ", " << simultaneousTextureStageDraws
                << " draw(s) had both fixed-function texture units enabled (only the active unit "
                   "is sampled)";
    }
    if (pvrtcDecodedUploads)
        out << ", PVRTC1 software-decoded uploads=" << pvrtcDecodedUploads << " ("
            << pvrtcDecodedPixels << " texels)";
    if (width && height) out << ", drawable " << width << 'x' << height;
    if (!lastError.empty()) out << ", last error: " << lastError;
    return out.str();
}

void GLES::setError(const std::string& text) { lastError = text; }

bool GLES::bindClientArrays(size_t count, std::vector<std::vector<uint8_t>>& storage) {
    auto upload = [&](ClientArray& array, const char* entry, bool hasSize) -> bool {
        if (!array.enabled) return true;
        // Whether the pointer is an offset or a guest address follows the binding *now*, not
        // the binding at pointer-set time: a title may set arrays and then bind or unbind.
        if (arrayBufferBound) return true;  // the driver holds this array in a buffer object
        const size_t element = typeSize(array.type);
        if (!element || count == 0) return false;
        const size_t components = hasSize ? size_t(array.size) : 3;
        const size_t stride = array.stride ? size_t(array.stride) : components * element;
        const size_t bytes = stride * (count - 1) + components * element;
        storage.emplace_back(bytes, 0);
        const void* pointer = storage.back().data();
        if (array.pointer) {
            memory.read(array.pointer, storage.back().data(), bytes);
        } else {
            pointer = nullptr;
        }
        if (hasSize) {
            auto set = fn<void (*)(int, unsigned, int, const void*)>(entry);
            if (!set) return false;
            set(array.size, array.type, int(stride), pointer);
        } else {
            auto set = fn<void (*)(unsigned, int, const void*)>(entry);
            if (!set) return false;
            set(array.type, int(stride), pointer);
        }
        return true;
    };
    if (!upload(vertex, "glVertexPointer", true)) return false;
    if (!upload(colour, "glColorPointer", true)) return false;
    if (!upload(normal, "glNormalPointer", false)) return false;
    if (auto tex = texCoord.find(clientTextureUnit); tex != texCoord.end())
        if (!upload(tex->second, "glTexCoordPointer", true)) return false;
    return true;
}

bool GLES::invoke(const std::string& symbol, CpuState& state) {
    if (symbol.rfind("_gl", 0) != 0) return false;
    if (!available()) return false;
    if (!verbose) return invokeEntry(symbol, state);
    const std::array<uint32_t, 4> before{state.registers[0], state.registers[1], state.registers[2],
                                         state.registers[3]};
    const bool handled = invokeEntry(symbol, state);
    if (!handled) return false;
    const std::string error = takeError();
    if (logSink) {
        char call[128];
        std::snprintf(call, sizeof(call), "%s(0x%08x, 0x%08x, 0x%08x, 0x%08x)", symbol.c_str() + 1,
                      before[0], before[1], before[2], before[3]);
        // A GL error is only meaningful once it has been read; nothing here clears state
        // behind the guest's back beyond the glGetError this trace performs.
        logSink("gl: " + std::string(call) + (error.empty() ? "" : " -> " + error));
    }
    return true;
}
std::string GLES::takeError() {
    using GetError = uint32_t (*)();
    const auto getError = fn<GetError>("glGetError");
    // glGetError is only valid with a current context; without one the driver would report
    // an error that this call invented, so it is not asked.
    if (!getError || !context) return {};
    const uint32_t code = getError();
    switch (code) {
        case 0:
            return {};
        case 0x0500:
            return "GL_INVALID_ENUM (0x0500)";
        case 0x0501:
            return "GL_INVALID_VALUE (0x0501)";
        case 0x0502:
            return "GL_INVALID_OPERATION (0x0502)";
        case 0x0503:
            return "GL_STACK_OVERFLOW (0x0503)";
        case 0x0504:
            return "GL_STACK_UNDERFLOW (0x0504)";
        case 0x0505:
            return "GL_OUT_OF_MEMORY (0x0505)";
        case 0x0506:
            return "GL_INVALID_FRAMEBUFFER_OPERATION (0x0506)";
        default:
            return "GL error 0x" + std::to_string(code);
    }
}
bool GLES::invokeEntry(const std::string& symbol, CpuState& state) {
    Marshal m{state, memory};
    const std::string name = symbol.substr(1);
    calls++;

    // With an ES 2.0-only driver the fixed-function entry points are answered here, from the
    // state this bridge keeps, before any attempt to forward them to a driver that does not
    // have them. Names ES 2.0 does have (glEnable, glDrawArrays, glBindBuffer) fall through
    // to the branches below, which record what the emulation needs and then forward.
    if (fixedFunction && fixedFunctionEntry(name, m)) return true;

    if (name == "glClear") {
        auto f = fn<void (*)(uint32_t)>("glClear");
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glClearColor") {
        auto f = fn<void (*)(float, float, float, float)>("glClearColor");
        if (!f) return false;
        // Four GLfloat arguments: s0-s3 under AAPCS-VFP, not r0-r3. They are read into
        // locals first because the evaluation order of function arguments is unspecified:
        // `f(m.f32(), ...)` can consume s0-s3 right-to-left and reverse the colour.
        const float red = m.f32(), green = m.f32(), blue = m.f32(), alpha = m.f32();
        f(red, green, blue, alpha);
        return true;
    }
    if (name == "glColor4f") {
        auto f = fn<void (*)(float, float, float, float)>("glColor4f");
        if (!f) return false;
        const float red = m.f32(), green = m.f32(), blue = m.f32(), alpha = m.f32();
        f(red, green, blue, alpha);
        return true;
    }
    if (name == "glLineWidth") {
        auto f = fn<void (*)(float)>("glLineWidth");
        if (!f) return false;
        f(m.f32());
        return true;
    }
    if (name == "glViewport" || name == "glScissor") {
        auto f = fn<void (*)(int, int, int, int)>(name.c_str());
        if (!f) return false;
        const int x = int(m.word()), y = int(m.word()), w = int(m.word()), h = int(m.word());
        f(x, y, w, h);
        return true;
    }
    if (name == "glEnable" || name == "glDisable") {
        const uint32_t which = m.word();
        if (fixedFunction) {
            // Texturing and lighting are ES 1.1 capabilities with no ES 2.0 equivalent.
            // Keep them in the emulation state and do not forward those enums: a strict ES 2.0
            // implementation (including ANGLE) must reject GL_TEXTURE_2D/GL_LIGHTING.
            if (which == GL_TEXTURE_2D) {
                const bool enabled = name == "glEnable";
                if (activeTextureUnit == 0)
                    texture2dEnabled = enabled;
                else
                    texture2dUnits[activeTextureUnit] = enabled;
                return true;
            }
            if (which == GL_LIGHTING) {
                lightingEnabled = name == "glEnable";
                return true;
            }
        }
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(which);
        return true;
    }
    if (name == "glEnableClientState" || name == "glDisableClientState") {
        const uint32_t which = m.word();
        const bool enable = name == "glEnableClientState";
        if (which == GL_VERTEX_ARRAY)
            vertex.enabled = enable;
        else if (which == GL_COLOR_ARRAY)
            colour.enabled = enable;
        else if (which == GL_NORMAL_ARRAY)
            normal.enabled = enable;
        else if (which == GL_TEXTURE_COORD_ARRAY)
            texCoord[clientTextureUnit].enabled = enable;
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(which);
        return true;
    }
    if (name == "glVertexPointer" || name == "glColorPointer" || name == "glNormalPointer" ||
        name == "glTexCoordPointer") {
        ClientArray& array = name == "glVertexPointer"   ? vertex
                             : name == "glColorPointer"  ? colour
                             : name == "glNormalPointer" ? normal
                                                         : texCoord[clientTextureUnit];
        // glNormalPointer has no component count: a normal is always three components.
        array.size = name == "glNormalPointer" ? 3 : int(m.word());
        array.type = m.word();
        array.stride = int(m.word());
        array.pointer = m.word();
        // With a buffer object bound the pointer is an offset into that object, which the
        // driver owns. Without one it is a guest address, and a guest address must never be
        // handed to the driver as if it were host memory: the data is copied at draw time
        // (bindClientArrays) and the driver is told null until then.
        array.buffered = arrayBufferBound;
        const void* pointer =
            arrayBufferBound ? reinterpret_cast<const void*>(uintptr_t(array.pointer)) : nullptr;
        if (name == "glNormalPointer") {
            auto f = fn<void (*)(unsigned, int, const void*)>(name.c_str());
            if (!f) return false;
            f(array.type, array.stride, pointer);
            return true;
        }
        auto f = fn<void (*)(int, unsigned, int, const void*)>(name.c_str());
        if (!f) return false;
        f(array.size, array.type, array.stride, pointer);
        return true;
    }
    if (name == "glClientActiveTexture") {
        clientTextureUnit = int(m.word());
        return true;
    }
    if (name == "glActiveTexture") {
        const uint32_t texture = m.word();
        if (fixedFunction && texture >= GL_TEXTURE0 &&
            texture - GL_TEXTURE0 < FixedFunctionTextureUnits) {
            activeTextureUnit = int(texture - GL_TEXTURE0);
            if (matrixMode == GL_TEXTURE_MATRIX) matrix = &textureMatrixForUnit(activeTextureUnit);
        }
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(texture);
        return true;
    }
    if (name == "glBindBuffer") {
        const uint32_t target = m.word(), buffer = m.word();
        if (target == GL_ARRAY_BUFFER) {
            arrayBufferBound = buffer != 0;
            boundArrayBuffer = buffer;
        }
        if (target == GL_ELEMENT_ARRAY_BUFFER) {
            elementBufferBound = buffer != 0;
            boundElementBuffer = buffer;
        }
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        f(target, buffer);
        return true;
    }
    if (name == "glBufferData") {
        const uint32_t target = m.word(), size = m.word(), data = m.word(), usage = m.word();
        auto f = fn<void (*)(uint32_t, uint32_t, const void*, uint32_t)>(name.c_str());
        if (!f) return false;
        if (!data || !size) {
            f(target, size, nullptr, usage);
            return true;
        }
        std::vector<uint8_t> copy(size);
        memory.read(data, copy.data(), copy.size());
        f(target, size, copy.data(), usage);
        return true;
    }
    if (name == "glBindFramebufferOES") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), guestName = m.word();
        // EAGL's default framebuffer is a private FBO because this runtime has no Android
        // EGL window surface on the guest context. Preserve the guest's name (zero) while
        // binding the actual colour target the guest draws into.
        f(target, guestName == 0 && target == GL_FRAMEBUFFER ? framebuffer : guestName);
        if (target == GL_FRAMEBUFFER) boundGuestFramebuffer = guestName;
        return true;
    }
    if (name == "glBindRenderbufferOES") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), guestName = m.word();
        f(target, guestName);
        if (target == 0x8d41) boundGuestRenderbuffer = guestName;
        return true;
    }
    if (name == "glBindTexture") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word(), m.word());
        return true;
    }
    if (name == "glBlendFunc" || name == "glPixelStorei" || name == "glTexEnvi" ||
        name == "glTexParameteri") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t first = m.word(), second = m.word();
        f(first, second);
        return true;
    }
    if (name == "glDepthFunc" || name == "glMatrixMode" || name == "glFrontFace") {
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glDepthMask") {
        auto f = fn<void (*)(uint8_t)>("glDepthMask");
        if (!f) return false;
        f(uint8_t(m.word() & 0xff));
        return true;
    }
    if (name == "glDrawArrays") {
        const int mode = int(m.word()), first = int(m.word()), count = int(m.word());
        if (count <= 0) return true;
        if (first < 0) throw Error("GLES_ARGUMENT", "draw starts before the first vertex");
        if (fixedFunction) {
            if (!drawFixedFunction(uint32_t(mode), first, count, 0, 0)) return false;
            draws++;
            return true;
        }
        auto f = fn<void (*)(uint32_t, int, int)>("glDrawArrays");
        if (!f) return false;
        std::vector<std::vector<uint8_t>> storage;
        // The copy has to cover the last vertex the draw reaches, which with a non-zero
        // `first` is first + count: the driver indexes client arrays from the pointer, so a
        // copy of only `count` vertices has it reading past the end of a host buffer.
        if (size_t(first) + size_t(count) > VertexBudget)
            throw Error("GLES_ARRAY", "draw reaches more vertices than the copy budget");
        if (!bindClientArrays(size_t(first) + size_t(count), storage))
            throw Error("GLES_ARRAY", "client vertex array could not be read");
        f(uint32_t(mode), first, count);
        draws++;
        return true;
    }
    if (name == "glDrawElements") {
        const int mode = int(m.word()), count = int(m.word());
        const uint32_t type = m.word(), indices = m.word();
        if (count <= 0) return true;
        if (fixedFunction) {
            if (!drawFixedFunction(uint32_t(mode), 0, count, type, indices)) return false;
            draws++;
            return true;
        }
        auto f = fn<void (*)(uint32_t, int, uint32_t, const void*)>("glDrawElements");
        if (!f) return false;
        const size_t element = typeSize(type);
        if (!element && !elementBufferBound)
            throw Error("GLES_ARRAY", "index type is not a GL type");
        std::vector<uint8_t> copy;
        size_t vertices = size_t(count);
        if (!elementBufferBound) {
            copy.resize(size_t(count) * element);
            if (indices) memory.read(indices, copy.data(), copy.size());
            // An indexed draw reaches as far as its largest index, not as far as it has
            // indices: copying `count` vertices has the driver read past the copy whenever an
            // index is larger than the index count, which is the normal case.
            vertices = largestIndex(copy, element) + 1;
            if (vertices > VertexBudget)
                throw Error("GLES_ARRAY", "an index reaches more vertices than the copy budget");
        }
        std::vector<std::vector<uint8_t>> storage;
        if (!bindClientArrays(vertices, storage))
            throw Error("GLES_ARRAY", "client vertex array could not be read");
        if (elementBufferBound) {
            f(uint32_t(mode), count, type, reinterpret_cast<const void*>(uintptr_t(indices)));
        } else {
            storage.push_back(std::move(copy));
            f(uint32_t(mode), count, type, storage.back().data());
        }
        draws++;
        return true;
    }
    if (name == "glGenTextures" || name == "glGenBuffers" || name == "glGenFramebuffersOES" ||
        name == "glGenRenderbuffersOES") {
        const int count = int(m.word());
        const uint32_t out = m.word();
        auto f = fn<void (*)(int, uint32_t*)>(name.c_str());
        if (!f) return false;
        if (!count) return true;
        if (count > 4096 || !memory.permits(out, size_t(count) * 4, Write))
            throw Error("GLES_ARGUMENT", "name array is not writable", out);
        std::vector<uint32_t> names(static_cast<size_t>(count));
        f(count, names.data());
        memory.write(out, names.data(), names.size() * sizeof(uint32_t));
        return true;
    }
    if (name == "glDeleteTextures" || name == "glDeleteBuffers" ||
        name == "glDeleteFramebuffersOES" || name == "glDeleteRenderbuffersOES") {
        const int count = int(m.word());
        const uint32_t in = m.word();
        auto f = fn<void (*)(int, const uint32_t*)>(name.c_str());
        if (!f) return false;
        if (!count) return true;
        if (count > 4096 || !memory.permits(in, size_t(count) * 4, Read))
            throw Error("GLES_ARGUMENT", "name array is not readable", in);
        std::vector<uint32_t> names(static_cast<size_t>(count));
        memory.read(in, names.data(), names.size() * sizeof(uint32_t));
        f(count, names.data());
        if (name == "glDeleteFramebuffersOES") {
            bool reboundDefault = false;
            for (uint32_t deleted : names) {
                if (deleted == boundGuestFramebuffer) {
                    boundGuestFramebuffer = 0;
                    reboundDefault = true;
                }
                if (deleted == drawableFramebuffer) drawableFramebuffer = 0;
            }
            if (reboundDefault && framebuffer) {
                auto bind = glFn<void (*)(uint32_t, uint32_t)>("glBindFramebuffer");
                if (bind) bind(GL_FRAMEBUFFER, framebuffer);
            }
        } else if (name == "glDeleteRenderbuffersOES") {
            for (uint32_t deleted : names) {
                if (deleted == boundGuestRenderbuffer) boundGuestRenderbuffer = 0;
                if (deleted == drawableRenderbuffer) drawableRenderbuffer = 0;
            }
        }
        return true;
    }
    if (name == "glGetIntegerv" || name == "glGetRenderbufferParameterivOES") {
        std::vector<int32_t> values(32, 0);
        if (name == "glGetIntegerv") {
            const uint32_t pname = m.word(), out = m.word();
            auto f = fn<void (*)(uint32_t, int32_t*)>("glGetIntegerv");
            if (!f) return false;
            if (pname == GL_FRAMEBUFFER_BINDING) {
                if (!out || !memory.permits(out, 4, Write))
                    throw Error("GLES_ARGUMENT", "query result is not writable", out);
                memory.write32(out, boundGuestFramebuffer);
                return true;
            }
            // An ES 2.0-only driver answers these ES 1.1 limits with GL_INVALID_ENUM and
            // leaves the guest's word alone. Reading zero texture units out of that is how a
            // title that asked decides it cannot texture anything, so they are answered here.
            if (fixedFunction) {
                int answer = 0;
                bool known = true;
                switch (pname) {
                    case GL_MAX_TEXTURE_UNITS:
                        answer = int(FixedFunctionTextureUnits);
                        break;
                    case GL_MAX_LIGHTS:
                        answer = 8;
                        break;
                    case GL_MAX_MODELVIEW_STACK_DEPTH:
                    case GL_MAX_PROJECTION_STACK_DEPTH:
                    case GL_MAX_TEXTURE_STACK_DEPTH:
                        answer = 16;
                        break;
                    default:
                        known = false;
                        break;
                }
                if (known) {
                    if (!out || !memory.permits(out, 4, Write))
                        throw Error("GLES_ARGUMENT", "query result is not writable", out);
                    memory.write32(out, uint32_t(answer));
                    return true;
                }
            }
            f(pname, values.data());
            if (!out || !memory.permits(out, 4, Write))
                throw Error("GLES_ARGUMENT", "query result is not writable", out);
            memory.write32(out, uint32_t(values[0]));
            return true;
        }
        const uint32_t target = m.word(), pname = m.word(), out = m.word();
        auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>("glGetRenderbufferParameterivOES");
        if (!f) return false;
        f(target, pname, values.data());
        if (!out || !memory.permits(out, 4, Write))
            throw Error("GLES_ARGUMENT", "query result is not writable", out);
        memory.write32(out, uint32_t(values[0]));
        return true;
    }
    if (name == "glCheckFramebufferStatusOES") {
        auto f = fn<uint32_t (*)(uint32_t)>("glCheckFramebufferStatusOES");
        if (!f) return false;
        state.registers[0] = f(m.word());
        return true;
    }
    if (name == "glRenderbufferStorageOES") {
        auto f = fn<void (*)(uint32_t, uint32_t, int, int)>("glRenderbufferStorageOES");
        if (!f) return false;
        const uint32_t target = m.word(), format = m.word();
        const int w = int(m.word()), h = int(m.word());
        f(target, format, w, h);
        if (target == 0x8d41 && boundGuestRenderbuffer == drawableRenderbuffer && w > 0 && h > 0) {
            width = w;
            height = h;
        }
        return true;
    }
    if (name == "glFramebufferRenderbufferOES") {
        auto f =
            fn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>("glFramebufferRenderbufferOES");
        if (!f) return false;
        const uint32_t target = m.word(), attachment = m.word();
        const uint32_t renderbuffertarget = m.word(), renderbuffer = m.word();
        f(target, attachment, renderbuffertarget, renderbuffer);
        if (attachment == 0x8ce0 && renderbuffertarget == 0x8d41 &&
            renderbuffer == drawableRenderbuffer && boundGuestFramebuffer) {
            drawableFramebuffer = boundGuestFramebuffer;
        }
        return true;
    }
    if (name == "glFramebufferTexture2DOES") {
        auto f =
            fn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int)>("glFramebufferTexture2DOES");
        if (!f) return false;
        const uint32_t target = m.word(), attachment = m.word(), textarget = m.word();
        const uint32_t texture = m.word();
        f(target, attachment, textarget, texture, int(m.word()));
        return true;
    }
    if (name == "glLoadMatrixf") {
        const uint32_t source = m.word();
        float matrix[16] = {};
        if (source) {
            if (!memory.permits(source, sizeof(matrix), Read))
                throw Error("GLES_ARGUMENT", "matrix is not readable", source);
            memory.read(source, matrix, sizeof(matrix));
        }
        auto f = fn<void (*)(const float*)>("glLoadMatrixf");
        if (!f) return false;
        f(matrix);
        return true;
    }
    if (name == "glLightfv" || name == "glMaterialfv") {
        const uint32_t first = m.word(), pname = m.word(), source = m.word();
        float values[16] = {};
        if (source) {
            if (!memory.permits(source, 16, Read))
                throw Error("GLES_ARGUMENT", "light/material values are not readable", source);
            memory.read(source, values, 16);
        }
        auto f = fn<void (*)(uint32_t, uint32_t, const float*)>(name.c_str());
        if (!f) return false;
        f(first, pname, values);
        return true;
    }
    if (name == "glCompressedTexImage2D") {
        const uint32_t target = m.word(), level = m.word(), format = m.word();
        const int w = int(m.word()), h = int(m.word()), border = int(m.word());
        const uint32_t imageSize = m.word(), source = m.word();
        if (imageSize > 64 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "compressed texture exceeds budget", source);
        if (isPvrtcV1Format(format)) {
            if (border != 0)
                throw Error("GLES_PVRTC", "PVRTC1 textures require a zero border", source);
            size_t expectedSize = 0;
            std::string decodeError;
            if (!pvrtcV1ImageSize(format, w, h, expectedSize, decodeError))
                throw Error("GLES_PVRTC", decodeError, source);
            if (expectedSize != imageSize)
                throw Error("GLES_PVRTC",
                            "PVRTC1 compressed byte count does not match the texture dimensions",
                            source);
            auto upload = fn<void (*)(uint32_t, int, uint32_t, int, int, int, uint32_t, uint32_t,
                                      const void*)>("glTexImage2D");
            if (!upload) return false;
            if (!source || !imageSize) {
                upload(target, int(level), GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE,
                       nullptr);
                return true;
            }
            std::vector<uint8_t> compressed(imageSize);
            memory.read(source, compressed.data(), compressed.size());
            std::vector<uint8_t> rgba;
            if (!decodePvrtcV1(format, w, h, compressed.data(), compressed.size(), rgba,
                               decodeError))
                throw Error("GLES_PVRTC", decodeError, source);
            upload(target, int(level), GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE,
                   rgba.data());
            pvrtcDecodedUploads++;
            pvrtcDecodedPixels += size_t(w) * size_t(h);
            return true;
        }
        auto upload = fn<void (*)(uint32_t, int, uint32_t, int, int, int, int, const void*)>(
            "glCompressedTexImage2D");
        if (!upload) return false;
        std::vector<uint8_t> compressed;
        if (source && imageSize) {
            compressed.resize(imageSize);
            memory.read(source, compressed.data(), compressed.size());
        }
        upload(target, int(level), format, w, h, border, int(imageSize),
               compressed.empty() ? nullptr : compressed.data());
        return true;
    }
    if (name == "glTexImage2D" || name == "glTexSubImage2D") {
        const uint32_t target = m.word(), level = m.word();
        if (name == "glTexImage2D") {
            const uint32_t internal = m.word();
            const int w = int(m.word()), h = int(m.word()), border = int(m.word());
            const uint32_t format = m.word(), type = m.word(), source = m.word();
            auto f = fn<void (*)(uint32_t, int, uint32_t, int, int, int, uint32_t, uint32_t,
                                 const void*)>("glTexImage2D");
            if (!f) return false;
            std::vector<uint8_t> copy;
            if (source && w > 0 && h > 0) {
                const size_t perTexel = texelBytes(format, type);
                if (!perTexel)
                    throw Error("GLES_ARGUMENT", "texture format/type pair is not modelled");
                copy.resize(size_t(w) * size_t(h) * perTexel);
                memory.read(source, copy.data(), copy.size());
            }
            f(target, int(level), internal, w, h, border, format, type,
              copy.empty() ? nullptr : copy.data());
            return true;
        }
        const int x = int(m.word()), y = int(m.word()), w = int(m.word()), h = int(m.word());
        const uint32_t format = m.word(), type = m.word(), source = m.word();
        auto f = fn<void (*)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void*)>(
            "glTexSubImage2D");
        if (!f) return false;
        std::vector<uint8_t> copy;
        if (source && w > 0 && h > 0) {
            const size_t perTexel = texelBytes(format, type);
            if (!perTexel) throw Error("GLES_ARGUMENT", "texture format/type pair is not modelled");
            copy.resize(size_t(w) * size_t(h) * perTexel);
            memory.read(source, copy.data(), copy.size());
        }
        f(target, int(level), x, y, w, h, format, type, copy.empty() ? nullptr : copy.data());
        return true;
    }
    return false;
}

// ---- OpenGL ES 1.1 fixed function over an ES 2.0-only driver ----------------------------
//
// The titles this runtime runs are ES 1.1: they keep their vertices, colours and texture
// coordinates in client memory, transform them with a matrix stack, and never name a shader.
// ANGLE is ES 2.0 and has no ES 1.1 frontend, so those entry points do not exist in it. This
// is the part of the bridge that holds that state instead and turns a draw into an ES 2.0
// draw: the matrices the guest loaded become one model-view-projection uniform, the client
// arrays become buffer objects bound to attributes, and the colour/texture-environment state
// selects which of a handful of generated programs is used.
//
// What it does not implement is counted and reported by describe() rather than silently
// approximated: the ES 1.1 lighting model, texture environment modes other than modulate and
// replace, and simultaneous texture stages. Texture state and coordinates are tracked per unit,
// so a draw can select a nonzero unit, but this program still samples only that one unit.

bool GLES::fixedFunctionEntry(const std::string& name, Marshal& m) {
    if (name == "glVertexPointer" || name == "glColorPointer" || name == "glTexCoordPointer") {
        ClientArray& array = name == "glVertexPointer"  ? vertex
                             : name == "glColorPointer" ? colour
                                                        : texCoord[clientTextureUnit];
        array.size = int(m.word());
        array.type = m.word();
        array.stride = int(m.word());
        array.pointer = m.word();
        // Whether the pointer is an offset into a bound buffer object or a guest address
        // follows the binding at the time the pointer was set, exactly as in ES 1.1.
        array.buffered = arrayBufferBound;
        return true;
    }
    if (name == "glNormalPointer") {
        normal.size = 3;  // a normal is always three components; ES 1.1 has no size argument
        normal.type = m.word();
        normal.stride = int(m.word());
        normal.pointer = m.word();
        normal.buffered = arrayBufferBound;
        return true;
    }
    if (name == "glEnableClientState" || name == "glDisableClientState") {
        const uint32_t which = m.word();
        const bool enable = name == "glEnableClientState";
        if (which == GL_VERTEX_ARRAY)
            vertex.enabled = enable;
        else if (which == GL_COLOR_ARRAY)
            colour.enabled = enable;
        else if (which == GL_NORMAL_ARRAY)
            normal.enabled = enable;
        else if (which == GL_TEXTURE_COORD_ARRAY)
            texCoord[clientTextureUnit].enabled = enable;
        return true;
    }
    if (name == "glMatrixMode") {
        const uint32_t mode = m.word();
        // Only the three stacks an ES 1.1 title can select are held here. A fourth value is
        // either a guest bug or an enum this bridge has no meaning for, and quietly loading
        // the matrix into the modelview stack anyway is how a title ends up transformed by a
        // matrix it never set, with nothing in the report to say so.
        if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE_MATRIX) {
            setError("glMatrixMode was given a matrix mode this bridge does not hold");
            return true;
        }
        matrixMode = mode;
        matrix = mode == GL_PROJECTION       ? &projection
                 : mode == GL_TEXTURE_MATRIX ? &textureMatrixForUnit(activeTextureUnit)
                                             : &modelview;
        return true;
    }
    if (name == "glLoadMatrixf") {
        const uint32_t source = m.word();
        float values[16] = {};
        if (source) {
            if (!memory.permits(source, sizeof(values), Read))
                throw Error("GLES_ARGUMENT", "matrix is not readable", source);
            memory.read(source, values, sizeof(values));
        }
        std::memcpy(matrix->data(), values, sizeof(values));
        return true;
    }
    if (name == "glColor4f") {
        // Read into locals first: the order the arguments are evaluated in is unspecified,
        // and `f(m.f32(), ...)` can consume s0-s3 right-to-left and reverse the colour.
        const float red = m.f32(), green = m.f32(), blue = m.f32(), alpha = m.f32();
        currentColour[0] = red;
        currentColour[1] = green;
        currentColour[2] = blue;
        currentColour[3] = alpha;
        return true;
    }
    if (name == "glClientActiveTexture") {
        const uint32_t texture = m.word();
        if (texture < GL_TEXTURE0 || texture - GL_TEXTURE0 >= FixedFunctionTextureUnits) {
            setError("glClientActiveTexture selected a unit outside the advertised limit");
            return true;
        }
        clientTextureUnit = int(texture);
        if (clientTextureUnit != int(GL_TEXTURE0)) clientTextureUnitUses++;
        return true;
    }
    if (name == "glTexEnvi") {
        const uint32_t target = m.word(), pname = m.word();
        const int value = int(m.word());
        if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
            // Classified rather than stored blindly. Modulate and replace are what the
            // generated programs actually shade; decal, blend and add are approximated as
            // modulate and counted per draw; anything else has no meaning here, so it is
            // approximated once, counted and named in the report.
            const uint32_t mode = uint32_t(value);
            switch (mode) {
                case GL_MODULATE_TEXTURE_ENV:
                case GL_REPLACE_TEXTURE_ENV:
                case GL_DECAL_TEXTURE_ENV:
                case GL_BLEND_TEXTURE_ENV:
                case GL_ADD_TEXTURE_ENV:
                    if (activeTextureUnit == 0)
                        textureEnvMode = int(mode);
                    else
                        textureEnvModes[activeTextureUnit] = int(mode);
                    break;
                default:
                    if (activeTextureUnit == 0)
                        textureEnvMode = GL_MODULATE_TEXTURE_ENV;
                    else
                        textureEnvModes[activeTextureUnit] = GL_MODULATE_TEXTURE_ENV;
                    approximatedTexEnv++;
                    setError("an unknown texture environment mode was approximated as modulate");
                    break;
            }
        }
        return true;
    }
    if (name == "glLightfv" || name == "glMaterialfv") {
        const uint32_t first = m.word(), pname = m.word(), source = m.word();
        float values[16] = {};
        if (source) {
            if (!memory.permits(source, 16, Read))
                throw Error("GLES_ARGUMENT", "light/material values are not readable", source);
            memory.read(source, values, 16);
        }
        // Recorded, not implemented. The values are read so a bogus pointer is reported as
        // the guest error it is, and the call is counted so describe() says lighting was
        // asked for and is not emulated, instead of drawing flat and claiming success.
        lightingCalls++;
        (void)first;
        (void)pname;
        return true;
    }
    return false;
}

std::array<float, 16>& GLES::textureMatrixForUnit(int unit) {
    if (unit <= 0) return textureMatrix;
    auto [it, inserted] = textureMatrices.try_emplace(unit, Identity);
    (void)inserted;
    return it->second;
}

int GLES::textureEnvironmentForUnit(int unit) const {
    if (unit <= 0) return textureEnvMode;
    const auto found = textureEnvModes.find(unit);
    return found == textureEnvModes.end() ? int(GL_MODULATE_TEXTURE_ENV) : found->second;
}

bool GLES::textureEnabledForUnit(int unit) const {
    if (unit <= 0) return texture2dEnabled;
    const auto found = texture2dUnits.find(unit);
    return found != texture2dUnits.end() && found->second;
}

const GLES::Program& GLES::fixedFunctionProgram() {
    static const Program none{};
    const int textureUnitEnum = int(GL_TEXTURE0) + activeTextureUnit;
    const auto tex = texCoord.find(textureUnitEnum);
    const bool texArray = tex != texCoord.end() && tex->second.enabled;
    const bool textureEnabled = textureEnabledForUnit(activeTextureUnit);
    const int textureEnvironment = textureEnvironmentForUnit(activeTextureUnit);
    // 0 = not textured, 1 = texture coordinates from an array, 2 = from the ES 1.1 current
    // texture coordinate, whose default is (0, 0, 0, 1).
    const int texSource = !textureEnabled ? 0 : texArray ? 1 : 2;
    const bool replace = textureEnvironment == GL_REPLACE_TEXTURE_ENV;
    const uint32_t key =
        (colour.enabled ? 1u : 0u) | (uint32_t(texSource) << 1) | (replace ? 8u : 0u);
    if (auto found = programs.find(key); found != programs.end()) return found->second;

    auto createShader = glFn<uint32_t (*)(uint32_t)>("glCreateShader");
    auto shaderSource =
        glFn<void (*)(uint32_t, int, const char* const*, const int*)>("glShaderSource");
    auto compileShader = glFn<void (*)(uint32_t)>("glCompileShader");
    auto getShaderiv = glFn<void (*)(uint32_t, uint32_t, int*)>("glGetShaderiv");
    auto createProgram = glFn<uint32_t (*)()>("glCreateProgram");
    auto attachShader = glFn<void (*)(uint32_t, uint32_t)>("glAttachShader");
    auto bindAttrib = glFn<void (*)(uint32_t, uint32_t, const char*)>("glBindAttribLocation");
    auto linkProgram = glFn<void (*)(uint32_t)>("glLinkProgram");
    auto getProgramiv = glFn<void (*)(uint32_t, uint32_t, int*)>("glGetProgramiv");
    auto uniformLocation = glFn<int (*)(uint32_t, const char*)>("glGetUniformLocation");
    if (!createShader || !shaderSource || !compileShader || !getShaderiv || !createProgram ||
        !attachShader || !bindAttrib || !linkProgram || !getProgramiv || !uniformLocation) {
        setError(
            "the driver has no OpenGL ES 2.0 shader support, so ES 1.1 fixed function "
            "cannot be emulated over it");
        return none;
    }
    // A shader that fails to compile is reported with the driver's own info log: this runs on
    // devices nobody can attach a debugger to, so the message is the only evidence there is.
    auto infoLog = glFn<void (*)(uint32_t, int, int*, char*)>("glGetShaderInfoLog");
    const auto compile = [&](uint32_t type, const std::string& source) -> uint32_t {
        const uint32_t shader = createShader(type);
        if (!shader) return 0;
        const char* text = source.c_str();
        shaderSource(shader, 1, &text, nullptr);
        compileShader(shader);
        int ok = 0;
        getShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok) return shader;
        std::string detail = "a generated shader did not compile";
        if (infoLog) {
            char buffer[1024] = {};
            infoLog(shader, int(sizeof(buffer)), nullptr, buffer);
            detail += ": ";
            detail += buffer;
        }
        setError(detail);
        return 0;
    };

    std::string vertexSource = "precision highp float;\nattribute vec4 aPosition;\n";
    if (colour.enabled) vertexSource += "attribute vec4 aColor;\n";
    if (texSource == 1) vertexSource += "attribute vec2 aTexCoord;\n";
    vertexSource += "uniform mat4 uMvp;\nuniform vec4 uColor;\n";
    if (texSource) vertexSource += "uniform mat4 uTexMatrix;\n";
    vertexSource += "varying vec4 vColor;\n";
    if (texSource) vertexSource += "varying vec2 vTexCoord;\n";
    vertexSource += "void main() {\n  gl_Position = uMvp * aPosition;\n  vColor = ";
    vertexSource += colour.enabled ? "aColor;\n" : "uColor;\n";
    if (texSource == 1)
        vertexSource += "  vTexCoord = (uTexMatrix * vec4(aTexCoord, 0.0, 1.0)).st;\n";
    else if (texSource == 2)
        vertexSource += "  vTexCoord = (uTexMatrix * vec4(0.0, 0.0, 0.0, 1.0)).st;\n";
    vertexSource += "}\n";

    std::string fragmentSource = "precision highp float;\nvarying vec4 vColor;\n";
    if (texSource) {
        fragmentSource += "varying vec2 vTexCoord;\nuniform sampler2D uTexture;\n";
        fragmentSource += "void main() {\n  gl_FragColor = texture2D(uTexture, vTexCoord)";
        // ES 1.1 texture environment: replace ignores the incoming colour entirely, modulate
        // multiplies. Anything else is approximated by modulate and counted.
        fragmentSource += replace ? ";\n}\n" : " * vColor;\n}\n";
    } else {
        fragmentSource += "void main() {\n  gl_FragColor = vColor;\n}\n";
    }

    const uint32_t vertexShader = compile(GL_VERTEX_SHADER, vertexSource);
    const uint32_t fragmentShader = vertexShader ? compile(GL_FRAGMENT_SHADER, fragmentSource) : 0;
    if (!vertexShader || !fragmentShader) return none;
    const uint32_t id = createProgram();
    if (!id) return none;
    attachShader(id, vertexShader);
    attachShader(id, fragmentShader);
    // Bound by name before linking, so a draw never has to ask where an attribute landed.
    bindAttrib(id, AttributePosition, "aPosition");
    if (colour.enabled) bindAttrib(id, AttributeColour, "aColor");
    if (texSource == 1) bindAttrib(id, AttributeTexCoord, "aTexCoord");
    linkProgram(id);
    int linked = 0;
    getProgramiv(id, GL_LINK_STATUS, &linked);
    auto deleteShader = glFn<void (*)(uint32_t)>("glDeleteShader");
    if (deleteShader) {
        deleteShader(vertexShader);
        deleteShader(fragmentShader);
    }
    if (!linked) {
        setError("a generated program did not link");
        return none;
    }
    Program program;
    program.id = id;
    program.mvp = uniformLocation(id, "uMvp");
    program.colour = uniformLocation(id, "uColor");
    program.textureMatrix = uniformLocation(id, "uTexMatrix");
    program.texture = uniformLocation(id, "uTexture");
    auto inserted = programs.emplace(key, program);
    return inserted.first->second;
}

void GLES::useFixedFunctionProgram(uint32_t program) {
    if (activeProgram == program) return;
    auto useProgram = glFn<void (*)(uint32_t)>("glUseProgram");
    if (useProgram) useProgram(program);
    activeProgram = program;
}

bool GLES::bindFixedFunctionArray(const ClientArray& array, uint32_t location, uint32_t& buffer,
                                  bool normalise, size_t count) {
    auto bindBuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindBuffer");
    auto attrib = glFn<void (*)(uint32_t, int, uint32_t, uint32_t, int, const void*)>(
        "glVertexAttribPointer");
    auto enableAttrib = glFn<void (*)(uint32_t)>("glEnableVertexAttribArray");
    if (!bindBuffer || !attrib || !enableAttrib) return false;
    if (array.buffered && boundArrayBuffer) {
        // The guest's own buffer object holds the array and the pointer is an offset into it,
        // so nothing is copied: the driver already has the bytes.
        bindBuffer(GL_ARRAY_BUFFER, boundArrayBuffer);
        attrib(location, array.size, array.type, normalise ? 1u : 0u, array.stride,
               reinterpret_cast<const void*>(uintptr_t(array.pointer)));
        enableAttrib(location);
        return true;
    }
    const size_t element = typeSize(array.type);
    if (!element || !count || array.size < 1 || array.size > 4) return false;
    const size_t components = size_t(array.size);
    const size_t stride = array.stride ? size_t(array.stride) : components * element;
    if (stride < components * element) return false;
    const size_t bytes = stride * (count - 1) + components * element;
    if (bytes > 256 * 1024 * 1024) return false;
    std::vector<uint8_t> copy(bytes, 0);
    if (array.pointer) memory.read(array.pointer, copy.data(), bytes);
    uint32_t type = array.type;
    if (type == GL_FIXED) {
        // ES 1.1's 16.16 fixed point has no ES 2.0 attribute type, so it is widened here
        // rather than handed to a driver that would reject it.
        std::vector<float> widened(bytes / 4);
        for (size_t at = 0; at < widened.size(); at++) {
            int32_t fixed = 0;
            std::memcpy(&fixed, &copy[at * 4], 4);
            widened[at] = float(fixed) / 65536.0f;
        }
        std::memcpy(copy.data(), widened.data(), widened.size() * 4);
        type = GL_FLOAT;
    }
    if (!buffer) {
        auto gen = glFn<void (*)(int, uint32_t*)>("glGenBuffers");
        if (!gen) return false;
        gen(1, &buffer);
    }
    auto bufferData = glFn<void (*)(uint32_t, size_t, const void*, uint32_t)>("glBufferData");
    if (!bufferData || !buffer) return false;
    bindBuffer(GL_ARRAY_BUFFER, buffer);
    bufferData(GL_ARRAY_BUFFER, copy.size(), copy.data(), GL_STREAM_DRAW);
    attrib(location, array.size, type, normalise ? 1u : 0u, int(stride), nullptr);
    enableAttrib(location);
    return true;
}

bool GLES::drawFixedFunction(uint32_t mode, int first, int count, uint32_t indexType,
                             uint32_t indices) {
    const Program& program = fixedFunctionProgram();
    if (!program.id) return false;
    useFixedFunctionProgram(program.id);
    auto uniformMatrix = glFn<void (*)(int, int, uint32_t, const float*)>("glUniformMatrix4fv");
    auto uniform4f = glFn<void (*)(int, float, float, float, float)>("glUniform4f");
    auto uniform1i = glFn<void (*)(int, int)>("glUniform1i");
    auto disableAttrib = glFn<void (*)(uint32_t)>("glDisableVertexAttribArray");
    auto bindBuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindBuffer");
    if (!uniformMatrix || !uniform4f || !disableAttrib || !bindBuffer) return false;

    float mvp[16];
    multiplyMatrices(projection.data(), modelview.data(), mvp);
    if (program.mvp >= 0) uniformMatrix(program.mvp, 1, 0, mvp);
    if (program.colour >= 0)
        uniform4f(program.colour, currentColour[0], currentColour[1], currentColour[2],
                  currentColour[3]);
    if (program.textureMatrix >= 0)
        uniformMatrix(program.textureMatrix, 1, 0, textureMatrixForUnit(activeTextureUnit).data());
    if (program.texture >= 0 && uniform1i) uniform1i(program.texture, activeTextureUnit);
    if (activeTextureUnit != 0) nonDefaultTextureUnitDraws++;
    const int textureEnvironment = textureEnvironmentForUnit(activeTextureUnit);
    if (textureEnvironment != GL_MODULATE_TEXTURE_ENV &&
        textureEnvironment != GL_REPLACE_TEXTURE_ENV)
        approximatedTexEnv++;

    // An indexed draw reaches as far as its largest index; a non-indexed one to first + count.
    std::vector<uint8_t> indexCopy;
    size_t vertices = size_t(first) + size_t(count);
    if (indexType) {
        const size_t element = typeSize(indexType);
        if (!element) {
            setError("index type is not a GL type");
            return false;
        }
        if (boundElementBuffer) {
            // The driver holds the indices and, with a buffer bound, the arrays too: nothing
            // is copied on this path, so the vertex count is not needed.
            vertices = 0;
        } else {
            indexCopy.resize(size_t(count) * element);
            if (indices) memory.read(indices, indexCopy.data(), indexCopy.size());
            vertices = largestIndex(indexCopy, element) + 1;
            if (vertices > VertexBudget) {
                setError("an index reaches more vertices than the copy budget");
                return false;
            }
        }
    }
    if (!vertex.enabled) {
        setError("a draw was issued with no vertex array enabled");
        return false;
    }
    if (!bindFixedFunctionArray(vertex, AttributePosition, streamBuffers[0], false,
                                vertices ? vertices : 1))
        return false;
    if (colour.enabled && !bindFixedFunctionArray(colour, AttributeColour, streamBuffers[1], true,
                                                  vertices ? vertices : 1))
        return false;
    const int textureUnitEnum = int(GL_TEXTURE0) + activeTextureUnit;
    const auto tex = texCoord.find(textureUnitEnum);
    const bool textureEnabled = textureEnabledForUnit(activeTextureUnit);
    const bool texCoordEnabled = tex != texCoord.end() && tex->second.enabled;
    if (textureEnabled && texCoordEnabled &&
        !bindFixedFunctionArray(tex->second, AttributeTexCoord, streamBuffers[2], false,
                                vertices ? vertices : 1))
        return false;
    // Anything the chosen program does not read must not be left enabled from a previous draw
    // with a different variant: an enabled attribute with no array behind it is a driver
    // error, not a harmless leftover.
    if (!colour.enabled) disableAttrib(AttributeColour);
    if (!(textureEnabled && texCoordEnabled)) disableAttrib(AttributeTexCoord);
    disableAttrib(AttributeNormal);

    if (indexType) {
        auto draw = glFn<void (*)(uint32_t, int, uint32_t, const void*)>("glDrawElements");
        if (!draw) return false;
        if (boundElementBuffer) {
            bindBuffer(GL_ELEMENT_ARRAY_BUFFER, boundElementBuffer);
            draw(mode, count, indexType, reinterpret_cast<const void*>(uintptr_t(indices)));
        } else {
            auto bufferData =
                glFn<void (*)(uint32_t, size_t, const void*, uint32_t)>("glBufferData");
            if (!bufferData) return false;
            if (!indexBuffer) {
                auto gen = glFn<void (*)(int, uint32_t*)>("glGenBuffers");
                if (!gen) return false;
                gen(1, &indexBuffer);
            }
            bindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
            bufferData(GL_ELEMENT_ARRAY_BUFFER, indexCopy.size(), indexCopy.data(), GL_STREAM_DRAW);
            draw(mode, count, indexType, nullptr);
        }
    } else {
        // No element array is read by a non-indexed draw, and a stale binding would make the
        // driver look for one.
        bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        auto draw = glFn<void (*)(uint32_t, int, int)>("glDrawArrays");
        if (!draw) return false;
        draw(mode, first, count);
    }
    if (textureEnabledForUnit(0) && textureEnabledForUnit(1)) simultaneousTextureStageDraws++;
    return true;
}

void GLES::setClientApi(int api) {
    api = api == 2 ? 2 : 1;
    if (api == clientApi) return;
    clientApi = api;
    // An EGL context cannot change version. Detach and discard its pbuffer before the next
    // EAGL call creates a context/config for the newly requested API.
    if (display) {
        using Display = void*;
        using Surface = void*;
        using Context = void*;
        using Boolean = uint32_t;
        if (auto makeCurrentFn =
                egl<Boolean (*)(Display, Surface, Surface, Context)>("eglMakeCurrent"))
            makeCurrentFn(display, nullptr, nullptr, nullptr);
        if (surface) {
            if (auto destroySurface = egl<Boolean (*)(Display, Surface)>("eglDestroySurface"))
                destroySurface(display, surface);
        }
        if (context) {
            if (auto destroyContext = egl<Boolean (*)(Display, Context)>("eglDestroyContext"))
                destroyContext(display, context);
        }
    }
    surface = context = nullptr;
    framebuffer = renderbuffer = 0;
    boundGuestFramebuffer = boundGuestRenderbuffer = 0;
    drawableFramebuffer = drawableRenderbuffer = 0;
    width = height = fallbackWidth = fallbackHeight = 0;
    frame.clear();
    frameCallCount = 0;
}

bool GLES::makeCurrent() {
    if (!load()) return false;
    using Display = void*;
    using Config = void*;
    using Context = void*;
    using Surface = void*;
    using Boolean = uint32_t;
    using Int = int32_t;
    if (!display) {
        auto getPlatformDisplay =
            egl<Display (*)(uint32_t, void*, const int32_t*)>("eglGetPlatformDisplayEXT");
        auto getDisplay = egl<Display (*)(void*)>("eglGetDisplay");
        const bool bundledAngle = backendName == "libGLESv2_podnative.so";
        if (bundledAngle) {
            // The shipped ANGLE build is Vulkan-only. Match the display selection used by
            // the Android presentation backend instead of asking ANGLE for Mesa's
            // surfaceless platform or Android's process-default EGL display.
            if (getPlatformDisplay) {
                const Int angleAttributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                                               EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, EGL_NONE};
                display = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, nullptr, angleAttributes);
            }
            if (!display) {
                setError("bundled ANGLE could not create its Vulkan platform display");
                return false;
            }
        } else {
            // Device EGL commonly requires a native display on Android; try its default
            // first. The surfaceless Mesa platform remains useful for host drivers/tests.
            if (getDisplay) display = getDisplay(nullptr);
            if (!display && getPlatformDisplay)
                display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
        }
        if (!display) {
            setError("EGL has no display");
            return false;
        }
        auto initialize = egl<Boolean (*)(Display, Int*, Int*)>("eglInitialize");
        if (!initialize) {
            setError("EGL has no eglInitialize");
            display = nullptr;
            return false;
        }
        Int major = 0, minor = 0;
        if (!initialize(display, &major, &minor)) {
            setError("eglInitialize failed");
            display = nullptr;
            return false;
        }
    }
    // With no ES 1.1 frontend to hand over, the context is ES 2.0 and the fixed-function
    // state lives in this bridge; otherwise the title gets the API it was written for.
    const int contextApi = fixedFunction ? 2 : clientApi;
    if (!context) {
        // ES 1.1 and ES 2.0 are separate client APIs in EGL; a 1.1 title draws through
        // fixed-function state, so it must be given the API it was written for.
        auto bindApi = egl<Boolean (*)(uint32_t)>("eglBindAPI");
        if (bindApi) bindApi(EGL_OPENGL_ES_API);
        auto chooseConfig =
            egl<Boolean (*)(Display, const Int*, Config*, Int, Int*)>("eglChooseConfig");
        auto createContext =
            egl<Context (*)(Display, Config, Context, const Int*)>("eglCreateContext");
        if (!createContext) {
            setError("EGL has no eglCreateContext");
            return false;
        }
        Config config = nullptr;
        if (chooseConfig) {
            const Int attributes[] = {EGL_SURFACE_TYPE,
                                      EGL_PBUFFER_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      contextApi == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      EGL_DEPTH_SIZE,
                                      16,
                                      EGL_NONE};
            Int count = 0;
            chooseConfig(display, attributes, &config, 1, &count);
            if (!count) config = nullptr;
        }
        const Int contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, contextApi, EGL_NONE};
        context = createContext(display, config, nullptr, contextAttributes);
        if (!context) {
            setError("eglCreateContext failed");
            return false;
        }
    }
    auto makeCurrentFn = egl<Boolean (*)(Display, Surface, Surface, Context)>("eglMakeCurrent");
    auto getCurrentContext = egl<Context (*)()>("eglGetCurrentContext");
    if (!makeCurrentFn) {
        setError("EGL has no eglMakeCurrent");
        return false;
    }
    if (getCurrentContext && getCurrentContext() == context) return true;
    if (makeCurrentFn(display, nullptr, nullptr, context)) return true;
    // Not every driver accepts a surfaceless context (ANGLE's Android backend included).
    // The guest renders into a framebuffer object, so a one-pixel pbuffer is enough to make
    // the context current; -presentRenderbuffer: still reads pixels back from the drawable.
    if (!surface) {
        auto chooseConfig =
            egl<Boolean (*)(Display, const Int*, Config*, Int, Int*)>("eglChooseConfig");
        auto createPbuffer =
            egl<Surface (*)(Display, Config, const Int*)>("eglCreatePbufferSurface");
        if (chooseConfig && createPbuffer) {
            const Int configAttributes[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                contextApi == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT, EGL_NONE};
            Config config = nullptr;
            Int count = 0;
            chooseConfig(display, configAttributes, &config, 1, &count);
            if (count) {
                const Int surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
                surface = createPbuffer(display, config, surfaceAttributes);
            }
        }
    }
    if (!surface || !makeCurrentFn(display, surface, surface, context)) {
        setError("eglMakeCurrent failed");
        return false;
    }
    return true;
}

uint32_t GLES::drawableColorFormat() {
    auto getString = glFn<const uint8_t* (*)(uint32_t)>("glGetString");
    if (!getString) return GL_RGBA4;
    const auto* text = getString(0x1f03);  // GL_EXTENSIONS
    if (!text) return GL_RGBA4;
    const std::string extensions(reinterpret_cast<const char*>(text));
    constexpr char name[] = "GL_OES_rgb8_rgba8";
    size_t at = 0;
    while ((at = extensions.find(name, at)) != std::string::npos) {
        const bool left = at == 0 || extensions[at - 1] == ' ';
        const size_t end = at + sizeof(name) - 1;
        const bool right = end == extensions.size() || extensions[end] == ' ';
        if (left && right) return GL_RGBA8;
        at = end;
    }
    // RGBA4 is required by both ES 1.1 and ES 2.0; use RGBA8 only when the driver advertises
    // the extension that makes it a legal renderbuffer format.
    return GL_RGBA4;
}

bool GLES::createDrawable(int w, int h) {
    if (!makeCurrent()) return false;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        setError("drawable size is out of range");
        return false;
    }
    auto genRenderbuffers = glFn<void (*)(int, uint32_t*)>("glGenRenderbuffers");
    auto bindRenderbuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindRenderbuffer");
    auto renderbufferStorage =
        glFn<void (*)(uint32_t, uint32_t, int, int)>("glRenderbufferStorage");
    auto genFramebuffers = glFn<void (*)(int, uint32_t*)>("glGenFramebuffers");
    auto bindFramebuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindFramebuffer");
    auto framebufferRenderbuffer =
        glFn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>("glFramebufferRenderbuffer");
    if (!genRenderbuffers || !bindRenderbuffer || !renderbufferStorage || !genFramebuffers ||
        !bindFramebuffer || !framebufferRenderbuffer) {
        setError("the driver has no framebuffer object support");
        return false;
    }
    constexpr uint32_t Renderbuffer = 0x8d41, Framebuffer = 0x8d40, ColorAttachment0 = 0x8ce0;
    if (!renderbuffer) genRenderbuffers(1, &renderbuffer);
    bindRenderbuffer(Renderbuffer, renderbuffer);
    // RGBA8 is core to ES 2.0; an ES 1.1 device driver is only required to accept RGBA4.
    renderbufferStorage(Renderbuffer, drawableColorFormat(), w, h);
    if (!framebuffer) genFramebuffers(1, &framebuffer);
    bindFramebuffer(Framebuffer, framebuffer);
    framebufferRenderbuffer(Framebuffer, ColorAttachment0, Renderbuffer, renderbuffer);
    auto check = glFn<uint32_t (*)(uint32_t)>("glCheckFramebufferStatus");
    if (check && check(Framebuffer) != 0x8cd5) {
        setError("the fallback drawable framebuffer is incomplete");
        return false;
    }
    width = fallbackWidth = w;
    height = fallbackHeight = h;
    auto viewport = glFn<void (*)(int, int, int, int)>("glViewport");
    if (viewport) viewport(0, 0, w, h);
    return true;
}

bool GLES::renderbufferStorageFromDrawable(int w, int h) {
    if (!makeCurrent()) return false;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        setError("drawable size is out of range");
        return false;
    }
    // UIKit's EAGL method does not create an unrelated private renderbuffer: it allocates the
    // storage of the renderbuffer the app already generated and bound. Its framebuffer is
    // attached by the guest immediately afterwards, which lets present() capture the exact
    // framebuffer the title draws to.
    if (!boundGuestRenderbuffer) return createDrawable(w, h);
    auto bindRenderbuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindRenderbuffer");
    auto storage = glFn<void (*)(uint32_t, uint32_t, int, int)>("glRenderbufferStorage");
    if (!bindRenderbuffer || !storage) {
        setError("the driver cannot allocate EAGL renderbuffer storage");
        return false;
    }
    constexpr uint32_t Renderbuffer = 0x8d41;
    bindRenderbuffer(Renderbuffer, boundGuestRenderbuffer);
    storage(Renderbuffer, drawableColorFormat(), w, h);
    drawableRenderbuffer = boundGuestRenderbuffer;
    width = w;
    height = h;
    frame.clear();
    return true;
}

bool GLES::present() {
    if (!makeCurrent() || !framebuffer || width <= 0 || height <= 0) {
        setError("no drawable to present");
        return false;
    }
    if (width > 2048 || height > 2048) {
        setError("drawable exceeds the 2048-pixel host readback limit");
        return false;
    }
    auto bindFramebuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindFramebuffer");
    auto readPixels = glFn<void (*)(int, int, int, int, uint32_t, uint32_t, void*)>("glReadPixels");
    if (!bindFramebuffer || !readPixels) {
        setError("the driver cannot bind or read the drawable framebuffer");
        return false;
    }
    const uint32_t restore = boundGuestFramebuffer ? boundGuestFramebuffer : framebuffer;
    const uint32_t source = drawableFramebuffer     ? drawableFramebuffer
                            : boundGuestFramebuffer ? boundGuestFramebuffer
                                                    : framebuffer;
    bindFramebuffer(GL_FRAMEBUFFER, source);
    auto check = glFn<uint32_t (*)(uint32_t)>("glCheckFramebufferStatus");
    if (check && check(GL_FRAMEBUFFER) != 0x8cd5) {
        bindFramebuffer(GL_FRAMEBUFFER, restore);
        setError("the active EAGL framebuffer is incomplete at presentation");
        return false;
    }
    auto finish = glFn<void (*)()>("glFinish");
    if (finish) finish();
    frame.resize(size_t(width) * size_t(height) * 4);
    readPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    bindFramebuffer(GL_FRAMEBUFFER, restore);
    // GL's origin is bottom-left; keep the host copy top-row-first.
    std::vector<uint8_t> flipped(frame.size());
    for (int row = 0; row < height; row++)
        std::memcpy(&flipped[size_t(row) * size_t(width) * 4],
                    &frame[size_t(height - 1 - row) * size_t(width) * 4], size_t(width) * 4);
    frame.swap(flipped);
    frameCallCount = calls;
    // The guest owns a surfaceless EGL context (or a one-pixel pbuffer fallback), not an
    // Android window surface. The host UI consumes this RGBA frame on its own ANGLE surface;
    // swapping the pbuffer would not present the game and can mask a failed readback.
    return true;
}
}  // namespace podnative::runtime
