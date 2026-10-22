#include "gles.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "error.h"

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
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074;
constexpr uint32_t GL_NORMAL_ARRAY = 0x8075;
constexpr uint32_t GL_COLOR_ARRAY = 0x8076;
constexpr uint32_t GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892;
constexpr uint32_t GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t GL_RGBA8 = 0x8058;
// EGL bits used by the drawable path; spelled out so this file needs no EGL headers and
// still compiles where the driver is loaded at run time.
constexpr int32_t EGL_NONE = 0x3038;
constexpr int32_t EGL_RENDERABLE_TYPE = 0x3040;
constexpr int32_t EGL_OPENGL_ES_BIT = 0x0001;
constexpr int32_t EGL_OPENGL_ES2_BIT = 0x0004;
constexpr int32_t EGL_OPENGL_ES3_BIT = 0x00000040;
constexpr int32_t EGL_CONTEXT_CLIENT_VERSION = 0x3098;
constexpr int32_t EGL_SURFACE_TYPE = 0x3033;
constexpr int32_t EGL_PBUFFER_BIT = 0x0001;
constexpr int32_t EGL_RED_SIZE = 0x3024;
constexpr int32_t EGL_GREEN_SIZE = 0x3023;
constexpr int32_t EGL_BLUE_SIZE = 0x3022;
constexpr int32_t EGL_ALPHA_SIZE = 0x3021;
constexpr int32_t EGL_DEPTH_SIZE = 0x3025;
constexpr int32_t EGL_STENCIL_SIZE = 0x3026;
constexpr int32_t EGL_PLATFORM_SURFACELESS_MESA = 0x31dd;
constexpr uint32_t EGL_OPENGL_ES_API = 0x30a0;
constexpr int32_t EGL_WIDTH = 0x3057;
constexpr int32_t EGL_HEIGHT = 0x3056;

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
GLES::~GLES() = default;

GLES::Entry GLES::function(const char* name) const {
    auto found = functions.find(name);
    return found == functions.end() ? nullptr : found->second;
}

bool GLES::resolve(const char* name) {
    if (function(name)) return true;
    void* symbol = dlsym(RTLD_DEFAULT, name);
    auto getProc = fn<void* (*)(const char*)>("eglGetProcAddress");
    if (!symbol && getProc) symbol = getProc(name);
    if (!symbol) {
        // The framebuffer/renderbuffer entry points exist twice: as the OES extension an
        // ES 1.1 title imports, and as the core ES 2.0 names. The runtime asks for the core
        // one when it drives a drawable, so both directions are tried: an ES 1.1 context
        // only answers the OES spelling, an ES 2.0 one only the core spelling.
        const std::string full(name);
        const bool oes = full.size() > 3 && full.compare(full.size() - 3, 3, "OES") == 0;
        const std::string alternate = oes ? full.substr(0, full.size() - 3) : full + "OES";
        symbol = dlsym(RTLD_DEFAULT, alternate.c_str());
        if (!symbol && getProc) symbol = getProc(alternate.c_str());
    }
    if (!symbol) return false;
    functions.emplace(name, reinterpret_cast<Entry>(symbol));
    return true;
}

bool GLES::load() {
    if (driverLoaded) return available();
    driverLoaded = true;
    // The driver is a real shared library. Android links ANGLE into the app; elsewhere
    // the system GLES is used. If neither exists the bridge stays empty and every GL
    // import keeps its unimplemented trap, which is the honest result.
    const char* candidates[] = {
#ifdef __ANDROID__
        "libGLESv2_podnative.so",
        "libGLESv2.so",
        "libGLESv2.so.2",
#else
        "libGLESv2.so.2",
        "libGLESv2.so",
        "libGLESv2_podnative.so",
#endif
    };
    for (const char* candidate : candidates) {
        void* handle = dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) continue;
        backendName = candidate;
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
    if (width && height) out << ", drawable " << width << 'x' << height;
    if (!lastError.empty()) out << ", last error: " << lastError;
    return out.str();
}

void GLES::setError(const std::string& text) { lastError = text; }

bool GLES::bindClientArrays(size_t count, std::vector<std::vector<uint8_t>>& storage) {
    auto upload = [&](ClientArray& array, const char* entry, bool hasSize) -> bool {
        if (!array.enabled) return true;
        const bool buffered = array.buffered;
        if (buffered) return true;  // the driver already holds this array in a buffer object
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
    Marshal m{state, memory};
    const std::string name = symbol.substr(1);
    calls++;

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
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word());
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
        // A buffer object's data was already uploaded; its pointer is an offset, so the
        // driver is told directly and the draw-time copy is skipped.
        array.buffered = arrayBufferBound;
        const void* pointer = reinterpret_cast<const void*>(uintptr_t(array.pointer));
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
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glBindBuffer") {
        const uint32_t target = m.word(), buffer = m.word();
        if (target == GL_ARRAY_BUFFER) arrayBufferBound = buffer != 0;
        if (target == GL_ELEMENT_ARRAY_BUFFER) elementBufferBound = buffer != 0;
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
    if (name == "glBindTexture" || name == "glBindFramebufferOES" ||
        name == "glBindRenderbufferOES") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t first = m.word(), second = m.word();
        f(first, second);
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
        auto f = fn<void (*)(uint32_t, int, int)>("glDrawArrays");
        if (!f) return false;
        std::vector<std::vector<uint8_t>> storage;
        if (!bindClientArrays(size_t(count), storage))
            throw Error("GLES_ARRAY", "client vertex array could not be read");
        f(uint32_t(mode), first, count);
        draws++;
        return true;
    }
    if (name == "glDrawElements") {
        const int mode = int(m.word()), count = int(m.word());
        const uint32_t type = m.word(), indices = m.word();
        auto f = fn<void (*)(uint32_t, int, uint32_t, const void*)>("glDrawElements");
        if (!f) return false;
        std::vector<std::vector<uint8_t>> storage;
        if (!bindClientArrays(size_t(count), storage))
            throw Error("GLES_ARRAY", "client vertex array could not be read");
        const size_t element = typeSize(type);
        if (!element && !elementBufferBound)
            throw Error("GLES_ARRAY", "index type is not a GL type");
        if (elementBufferBound) {
            f(uint32_t(mode), count, type, reinterpret_cast<const void*>(uintptr_t(indices)));
        } else {
            std::vector<uint8_t> copy(size_t(count) * element);
            if (indices) memory.read(indices, copy.data(), copy.size());
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
        return true;
    }
    if (name == "glGetIntegerv" || name == "glGetRenderbufferParameterivOES") {
        std::vector<int32_t> values(32, 0);
        if (name == "glGetIntegerv") {
            const uint32_t pname = m.word(), out = m.word();
            auto f = fn<void (*)(uint32_t, int32_t*)>("glGetIntegerv");
            if (!f) return false;
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
        return true;
    }
    if (name == "glFramebufferRenderbufferOES") {
        auto f =
            fn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>("glFramebufferRenderbufferOES");
        if (!f) return false;
        const uint32_t target = m.word(), attachment = m.word();
        const uint32_t renderbuffertarget = m.word(), renderbuffer = m.word();
        f(target, attachment, renderbuffertarget, renderbuffer);
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
        auto f = fn<void (*)(uint32_t, int, uint32_t, int, int, int, int, const void*)>(
            "glCompressedTexImage2D");
        if (!f) return false;
        if (imageSize > 64 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "compressed texture exceeds budget", source);
        std::vector<uint8_t> copy(imageSize);
        if (source && imageSize) memory.read(source, copy.data(), copy.size());
        f(target, int(level), format, w, h, border, int(imageSize),
          copy.empty() ? nullptr : copy.data());
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

void GLES::setClientApi(int api) {
    api = api == 2 ? 2 : 1;
    if (api == clientApi) return;
    clientApi = api;
    // A context cannot change version; the next EAGL call makes a new one. The drawable
    // objects belong to the guest, so the ids it holds stay valid names in the new context
    // only after it recreates them, exactly as on iOS.
    auto destroyContext = egl<void (*)(void*)>("eglDestroyContext");
    if (context && destroyContext) destroyContext(context);
    context = nullptr;
    framebuffer = renderbuffer = 0;
    width = height = 0;
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
        if (getPlatformDisplay)
            display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
        if (!display && getDisplay) display = getDisplay(nullptr);
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
                                      clientApi == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT,
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
        const Int contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, clientApi, EGL_NONE};
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
            const Int configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                            clientApi == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT,
                                            EGL_NONE};
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
    renderbufferStorage(Renderbuffer, GL_RGBA8, w, h);
    if (!framebuffer) genFramebuffers(1, &framebuffer);
    bindFramebuffer(Framebuffer, framebuffer);
    framebufferRenderbuffer(Framebuffer, ColorAttachment0, Renderbuffer, renderbuffer);
    auto check = glFn<uint32_t (*)(uint32_t)>("glCheckFramebufferStatus");
    if (check && check(Framebuffer) != 0x8cd5) {
        setError("the drawable framebuffer is incomplete");
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool GLES::present() {
    if (!makeCurrent() || !framebuffer) {
        setError("no drawable to present");
        return false;
    }
    auto readPixels = glFn<void (*)(int, int, int, int, uint32_t, uint32_t, void*)>("glReadPixels");
    auto finish = glFn<void (*)()>("glFinish");
    if (finish) finish();
    if (readPixels) {
        frame.resize(size_t(width) * size_t(height) * 4);
        readPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
        // GL's origin is bottom-left; keep the host copy top-row-first.
        std::vector<uint8_t> flipped(frame.size());
        for (int row = 0; row < height; row++)
            std::memcpy(&flipped[size_t(row) * size_t(width) * 4],
                        &frame[size_t(height - 1 - row) * size_t(width) * 4], size_t(width) * 4);
        frame.swap(flipped);
    }
    if (surface) {
        auto swap = egl<uint32_t (*)(void*, void*)>("eglSwapBuffers");
        if (swap) swap(display, surface);
    }
    return true;
}
}  // namespace podnative::runtime
