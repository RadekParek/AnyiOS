#include "gles.h"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "error.h"
#include "heap.h"
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
constexpr uint32_t GL_LIGHT0 = 0x4000;
constexpr uint32_t GL_NORMALIZE = 0x0ba1;
constexpr uint32_t GL_COLOR_MATERIAL = 0x0b57;
constexpr uint32_t GL_LIGHT_MODEL_AMBIENT = 0x0b53;
constexpr uint32_t GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0b51;
constexpr uint32_t GL_LIGHT_MODEL_TWO_SIDE = 0x0b52;
constexpr uint32_t GL_AMBIENT = 0x1200;
constexpr uint32_t GL_DIFFUSE = 0x1201;
constexpr uint32_t GL_SPECULAR = 0x1202;
constexpr uint32_t GL_POSITION = 0x1203;
constexpr uint32_t GL_SPOT_DIRECTION = 0x1204;
constexpr uint32_t GL_SPOT_EXPONENT = 0x1205;
constexpr uint32_t GL_SPOT_CUTOFF = 0x1206;
constexpr uint32_t GL_CONSTANT_ATTENUATION = 0x1207;
constexpr uint32_t GL_LINEAR_ATTENUATION = 0x1208;
constexpr uint32_t GL_QUADRATIC_ATTENUATION = 0x1209;
constexpr uint32_t GL_EMISSION = 0x1600;
constexpr uint32_t GL_SHININESS = 0x1601;
constexpr uint32_t GL_AMBIENT_AND_DIFFUSE = 0x1602;
constexpr uint32_t GL_FRONT = 0x0404;
constexpr uint32_t GL_BACK = 0x0405;
constexpr uint32_t GL_FRONT_AND_BACK = 0x0408;
constexpr uint32_t GL_INVALID_ENUM = 0x0500;
constexpr uint32_t GL_INVALID_VALUE = 0x0501;
constexpr uint32_t GL_INVALID_OPERATION = 0x0502;
constexpr uint32_t GL_OUT_OF_MEMORY = 0x0505;
constexpr float DegreesToRadians = 0.01745329251994329577f;
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
constexpr uint32_t GL_MODELVIEW_STACK_DEPTH = 0x0ba3;
constexpr uint32_t GL_PROJECTION_STACK_DEPTH = 0x0ba4;
constexpr uint32_t GL_TEXTURE_STACK_DEPTH = 0x0ba5;
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
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_STRIDE = 0x8624;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_TYPE = 0x8625;
constexpr uint32_t GL_CURRENT_VERTEX_ATTRIB = 0x8626;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x886a;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x889f;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645;
constexpr uint32_t GL_FRAMEBUFFER = 0x8d40;
constexpr uint32_t GL_FRAMEBUFFER_BINDING = 0x8ca6;
constexpr uint32_t GL_PACK_ALIGNMENT = 0x0d05;
constexpr uint32_t GL_UNPACK_ALIGNMENT = 0x0cf5;
constexpr uint32_t GL_VIEWPORT = 0x0ba2;
constexpr uint32_t GL_SCISSOR_BOX = 0x0c10;
constexpr uint32_t GL_COLOR_CLEAR_VALUE = 0x0c22;
constexpr uint32_t GL_COLOR_WRITEMASK = 0x0c23;
constexpr uint32_t GL_DEPTH_RANGE = 0x0b70;
constexpr uint32_t GL_MATRIX_MODE = 0x0ba0;
constexpr uint32_t GL_CURRENT_COLOR = 0x0b00;
constexpr uint32_t GL_CURRENT_TEXTURE_COORDS = 0x0b03;
constexpr uint32_t GL_MAX_VIEWPORT_DIMS = 0x0d3a;
constexpr uint32_t GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0x86a2;
constexpr uint32_t GL_COMPRESSED_TEXTURE_FORMATS = 0x86a3;
constexpr uint32_t GL_NUM_SHADER_BINARY_FORMATS = 0x8df9;
constexpr uint32_t GL_SHADER_BINARY_FORMATS = 0x8df8;
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
void transform4(const float* matrix, const float* source, float* destination) {
    for (int row = 0; row < 4; ++row)
        destination[row] = matrix[row] * source[0] + matrix[4 + row] * source[1] +
                           matrix[8 + row] * source[2] + matrix[12 + row] * source[3];
}
std::array<float, 9> inverseTranspose3x3(const float* matrix) {
    const float a = matrix[0], b = matrix[4], c = matrix[8];
    const float d = matrix[1], e = matrix[5], f = matrix[9];
    const float g = matrix[2], h = matrix[6], i = matrix[10];
    const float c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
    const float c10 = c * h - b * i, c11 = a * i - c * g, c12 = b * g - a * h;
    const float c20 = b * f - c * e, c21 = c * d - a * f, c22 = a * e - b * d;
    const float determinant = a * c00 + b * c01 + c * c02;
    if (std::fabs(determinant) < 1e-12f) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float inverseDeterminant = 1.0f / determinant;
    // GLSL mat3 uniforms are column-major; the cofactors are already inverse-transpose.
    return {c00 * inverseDeterminant, c10 * inverseDeterminant, c20 * inverseDeterminant,
            c01 * inverseDeterminant, c11 * inverseDeterminant, c21 * inverseDeterminant,
            c02 * inverseDeterminant, c12 * inverseDeterminant, c22 * inverseDeterminant};
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

bool pixelLayout(int width, int height, size_t bytesPerPixel, int alignment, size_t& rowBytes,
                 size_t& totalBytes) {
    if (width < 0 || height < 0 || !bytesPerPixel ||
        (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8))
        return false;
    const size_t row = size_t(width) * bytesPerPixel;
    if (width && row / size_t(width) != bytesPerPixel) return false;
    const size_t mask = size_t(alignment - 1);
    if (row > std::numeric_limits<size_t>::max() - mask) return false;
    rowBytes = (row + mask) & ~mask;
    if (!height) {
        totalBytes = 0;
        return true;
    }
    const size_t precedingRows = size_t(height - 1);
    if (precedingRows && rowBytes > std::numeric_limits<size_t>::max() / precedingRows)
        return false;
    const size_t precedingBytes = rowBytes * precedingRows;
    if (row > std::numeric_limits<size_t>::max() - precedingBytes) return false;
    // GL advances by the aligned stride between rows, but does not read or write padding
    // after the last row. Copying stride*height would cross the guest allocation boundary
    // whenever the final RGB row has alignment padding.
    totalBytes = precedingBytes + row;
    return totalBytes <= 64 * 1024 * 1024;
}

size_t queryVectorLength(uint32_t pname) {
    switch (pname) {
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_CURRENT_COLOR:
        case GL_CURRENT_TEXTURE_COORDS:
            return 4;
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            return 2;
        default:
            return 1;
    }
}

std::string readGuestString(const Memory& memory, uint32_t address, size_t limit = 1024 * 1024) {
    if (!address) throw Error("GLES_ARGUMENT", "a required GL string pointer is null");
    std::string text;
    for (size_t index = 0; index < limit; ++index) {
        if (index > std::numeric_limits<uint32_t>::max() - address)
            throw Error("GLES_ARGUMENT", "a guest GL string wraps the address space", address);
        char value = 0;
        memory.read(address + uint32_t(index), &value, sizeof(value));
        if (!value) return text;
        text.push_back(value);
    }
    throw Error("GLES_ARGUMENT", "a guest GL string exceeded the length limit", address);
}

// Every GL entry point this bridge implements. Keeping it in one place makes the
// unimplemented set explicit: anything absent stays an unimplemented-import trap.
const char* const ImportNames[] = {
    "_glActiveTexture",
    "_glAttachShader",
    "_glBindAttribLocation",
    "_glBindBuffer",
    "_glBindFramebuffer",
    "_glBindFramebufferOES",
    "_glBindRenderbuffer",
    "_glBindRenderbufferOES",
    "_glBindTexture",
    "_glBlendColor",
    "_glBlendEquation",
    "_glBlendEquationSeparate",
    "_glBlendFunc",
    "_glBlendFuncSeparate",
    "_glBufferData",
    "_glBufferSubData",
    "_glCheckFramebufferStatus",
    "_glCheckFramebufferStatusOES",
    "_glClear",
    "_glClearColor",
    "_glClearDepthf",
    "_glClearStencil",
    "_glClientActiveTexture",
    "_glCompileShader",
    "_glColor4f",
    "_glColorMask",
    "_glColorMaterial",
    "_glColorPointer",
    "_glCompressedTexImage2D",
    "_glCompressedTexSubImage2D",
    "_glCopyTexImage2D",
    "_glCopyTexSubImage2D",
    "_glCreateProgram",
    "_glCreateShader",
    "_glCullFace",
    "_glDeleteBuffers",
    "_glDeleteFramebuffers",
    "_glDeleteFramebuffersOES",
    "_glDeleteProgram",
    "_glDeleteRenderbuffers",
    "_glDeleteRenderbuffersOES",
    "_glDeleteShader",
    "_glDeleteTextures",
    "_glDetachShader",
    "_glDepthFunc",
    "_glDepthMask",
    "_glDepthRangef",
    "_glDisable",
    "_glDisableClientState",
    "_glDisableVertexAttribArray",
    "_glDrawArrays",
    "_glDrawElements",
    "_glEnable",
    "_glEnableClientState",
    "_glEnableVertexAttribArray",
    "_glFramebufferRenderbuffer",
    "_glFramebufferRenderbufferOES",
    "_glFramebufferTexture2D",
    "_glFramebufferTexture2DOES",
    "_glFinish",
    "_glFlush",
    "_glFrontFace",
    "_glGetFramebufferAttachmentParameteriv",
    "_glGetFramebufferAttachmentParameterivOES",
    "_glGenBuffers",
    "_glGenFramebuffers",
    "_glGenFramebuffersOES",
    "_glGenRenderbuffers",
    "_glGenRenderbuffersOES",
    "_glGenTextures",
    "_glGenerateMipmap",
    "_glGenerateMipmapOES",
    "_glGetActiveAttrib",
    "_glGetActiveUniform",
    "_glGetAttachedShaders",
    "_glGetAttribLocation",
    "_glGetBooleanv",
    "_glGetBufferParameteriv",
    "_glGetError",
    "_glGetFloatv",
    "_glGetIntegerv",
    "_glGetProgramInfoLog",
    "_glGetProgramiv",
    "_glGetRenderbufferParameteriv",
    "_glGetRenderbufferParameterivOES",
    "_glGetShaderInfoLog",
    "_glGetShaderPrecisionFormat",
    "_glGetShaderSource",
    "_glGetShaderiv",
    "_glGetString",
    "_glGetTexParameterfv",
    "_glGetTexParameteriv",
    "_glGetUniformLocation",
    "_glGetUniformfv",
    "_glGetUniformiv",
    "_glIsBuffer",
    "_glIsEnabled",
    "_glIsFramebuffer",
    "_glIsFramebufferOES",
    "_glIsProgram",
    "_glIsRenderbuffer",
    "_glIsRenderbufferOES",
    "_glIsShader",
    "_glIsTexture",
    "_glLightfv",
    "_glLightf",
    "_glLightModelfv",
    "_glLightModeli",
    "_glLineWidth",
    "_glLinkProgram",
    "_glLoadIdentity",
    "_glLoadMatrixf",
    "_glMultMatrixf",
    "_glPushMatrix",
    "_glPopMatrix",
    "_glRotatef",
    "_glScalef",
    "_glTranslatef",
    "_glFrustumf",
    "_glOrthof",
    "_glMaterialf",
    "_glMaterialfv",
    "_glMatrixMode",
    "_glNormal3f",
    "_glNormalPointer",
    "_glPixelStorei",
    "_glPolygonOffset",
    "_glReadPixels",
    "_glReleaseShaderCompiler",
    "_glRenderbufferStorage",
    "_glRenderbufferStorageOES",
    "_glSampleCoverage",
    "_glScissor",
    "_glShaderBinary",
    "_glShaderSource",
    "_glStencilFunc",
    "_glStencilFuncSeparate",
    "_glStencilMask",
    "_glStencilMaskSeparate",
    "_glStencilOp",
    "_glStencilOpSeparate",
    "_glTexCoordPointer",
    "_glTexEnvi",
    "_glTexImage2D",
    "_glTexParameterf",
    "_glTexParameterfv",
    "_glTexParameteri",
    "_glTexParameteriv",
    "_glTexSubImage2D",
    "_glUniform1f",
    "_glUniform1fv",
    "_glUniform1i",
    "_glUniform1iv",
    "_glUniform2f",
    "_glUniform2fv",
    "_glUniform2i",
    "_glUniform2iv",
    "_glUniform3f",
    "_glUniform3fv",
    "_glUniform3i",
    "_glUniform3iv",
    "_glUniform4f",
    "_glUniform4fv",
    "_glUniform4i",
    "_glUniform4iv",
    "_glUniformMatrix2fv",
    "_glUniformMatrix3fv",
    "_glUniformMatrix4fv",
    "_glUseProgram",
    "_glValidateProgram",
    "_glVertexAttrib1f",
    "_glVertexAttrib1fv",
    "_glVertexAttrib2f",
    "_glVertexAttrib2fv",
    "_glVertexAttrib3f",
    "_glVertexAttrib3fv",
    "_glVertexAttrib4f",
    "_glVertexAttrib4fv",
    "_glVertexAttribPointer",
    "_glGetVertexAttribfv",
    "_glGetVertexAttribiv",
    "_glGetVertexAttribPointerv",
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

GLES::GLES(Memory& memory, GuestHeap* guestHeap) : memory(memory), guestHeap(guestHeap) {
    lights[0].diffuse = {1, 1, 1, 1};
    lights[0].specular = {1, 1, 1, 1};
}
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
        if (lightingEnabled) {
            const size_t enabledLights = std::count_if(
                lights.begin(), lights.end(), [](const Light& light) { return light.enabled; });
            out << ", fixed-function lighting enabled with " << enabledLights << " light(s)";
        }
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
    if (!traceSuppressed.empty()) {
        out << "\nGLES_TRACE_SUMMARY: sampled calls";
        for (const auto& [name, suppressed] : traceSuppressed) {
            const auto found = traceCalls.find(name);
            const size_t total = found == traceCalls.end() ? suppressed : found->second;
            out << "\n  " << name << ": " << total << " call(s), " << (total - suppressed)
                << " shown, " << suppressed << " summarized";
        }
    }
    return out.str();
}

void GLES::setError(const std::string& text) { lastError = text; }

void GLES::setGlError(uint32_t error, const std::string& text) {
    if (!pendingGlError) pendingGlError = error;
    setError(text);
}

bool GLES::bindClientArrays(size_t count, std::vector<std::vector<uint8_t>>& storage) {
    auto bindBuffer = fn<void (*)(uint32_t, uint32_t)>("glBindBuffer");
    const auto hasClientArray = [](const ClientArray& array) {
        return array.enabled && !(array.buffered && array.buffer);
    };
    bool hasClientData = hasClientArray(vertex) || hasClientArray(colour) || hasClientArray(normal);
    for (const auto& [unit, array] : texCoord) {
        (void)unit;
        hasClientData = hasClientData || hasClientArray(array);
    }
    for (const auto& [index, array] : genericAttributes) {
        (void)index;
        hasClientData = hasClientData || hasClientArray(array);
    }
    if (hasClientData && boundArrayBuffer && !bindBuffer) return false;
    struct RestoreArrayBuffer {
        decltype(bindBuffer) bind;
        uint32_t name;
        bool restore;
        ~RestoreArrayBuffer() {
            if (restore) bind(GL_ARRAY_BUFFER, name);
        }
    } restore{bindBuffer, boundArrayBuffer, hasClientData && boundArrayBuffer};
    if (restore.restore) bindBuffer(GL_ARRAY_BUFFER, 0);

    auto upload = [&](ClientArray& array, const char* entry, bool hasSize) -> bool {
        if (!array.enabled || (array.buffered && array.buffer)) return true;
        const size_t element = typeSize(array.type);
        if (!element || count == 0) return false;
        const size_t components = hasSize ? size_t(array.size) : 3;
        if (!components || components > 4 || array.stride < 0) return false;
        const size_t packed = components * element;
        const size_t stride = array.stride ? size_t(array.stride) : packed;
        if (stride < packed || (count > 1 && stride > (256 * 1024 * 1024 - packed) / (count - 1)))
            return false;
        const size_t bytes = stride * (count - 1) + packed;
        storage.emplace_back(bytes, 0);
        const void* pointer = storage.back().data();
        if (array.pointer) {
            memory.read(array.pointer, storage.back().data(), bytes);
        } else {
            throw Error("GLES_ARRAY", "enabled client array points to null");
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

    auto attribPointer =
        fn<void (*)(uint32_t, int, uint32_t, uint8_t, int, const void*)>("glVertexAttribPointer");
    for (auto& [index, array] : genericAttributes) {
        if (!array.enabled || (array.buffered && array.buffer)) continue;
        if (!attribPointer || !array.pointer || !count || array.size < 1 || array.size > 4 ||
            array.stride < 0)
            return false;
        const size_t element = typeSize(array.type);
        if (!element) return false;
        const size_t packed = size_t(array.size) * element;
        const size_t stride = array.stride ? size_t(array.stride) : packed;
        if (stride < packed || (count > 1 && stride > (256 * 1024 * 1024 - packed) / (count - 1)))
            return false;
        const size_t bytes = stride * (count - 1) + packed;
        storage.emplace_back(bytes, 0);
        memory.read(array.pointer, storage.back().data(), bytes);
        attribPointer(index, array.size, array.type, uint8_t(array.normalized), int(stride),
                      storage.back().data());
    }
    return true;
}

bool GLES::invoke(const std::string& symbol, CpuState& state) {
    if (symbol.rfind("_gl", 0) != 0) return false;
    if (!available()) return false;
    if (!verbose) return invokeEntry(symbol, state);
    const CpuState before = state;
    const bool handled = invokeEntry(symbol, state);
    if (!handled) return false;

    const std::string name = symbol.substr(1);
    const size_t occurrence = ++traceCalls[name];
    size_t limit = 12;
    if (name == "glDrawArrays" || name == "glDrawElements" || name == "glClear" ||
        name == "glBindTexture" || name == "glBindBuffer")
        limit = 4;
    // Successful error polls are noise; the non-zero result is the evidence worth keeping.
    const bool emptyErrorPoll = name == "glGetError" && state.registers[0] == 0;
    const bool emit = occurrence <= limit && !emptyErrorPoll;
    if (!emit) {
        ++traceSuppressed[name];
    } else if (logSink) {
        logSink("gl: " + formatTraceCall(symbol, before, state));
    }
    return true;
}

std::string GLES::formatTraceCall(const std::string& symbol, const CpuState& before,
                                  const CpuState& after) const {
    const std::string name = symbol.substr(1);
    Marshal args{before, memory};
    const auto hex = [](uint32_t value) {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
        return out.str();
    };
    const auto enumName = [&hex](uint32_t value) -> std::string {
        if (value >= 0x84c0 && value <= 0x84df)
            return "GL_TEXTURE" + std::to_string(value - 0x84c0);
        switch (value) {
            case 0x0000:
                return "GL_POINTS";
            case 0x0001:
                return "GL_LINES";
            case 0x0004:
                return "GL_TRIANGLES";
            case 0x0005:
                return "GL_TRIANGLE_STRIP";
            case 0x0006:
                return "GL_TRIANGLE_FAN";
            case 0x0100:
                return "GL_DEPTH_BUFFER_BIT";
            case 0x0200:
                return "GL_NEVER";
            case 0x0201:
                return "GL_LESS";
            case 0x0202:
                return "GL_EQUAL";
            case 0x0203:
                return "GL_LEQUAL";
            case 0x0204:
                return "GL_GREATER";
            case 0x0205:
                return "GL_NOTEQUAL";
            case 0x0206:
                return "GL_GEQUAL";
            case 0x0207:
                return "GL_ALWAYS";
            case 0x0300:
                return "GL_SRC_COLOR";
            case 0x0301:
                return "GL_ONE_MINUS_SRC_COLOR";
            case 0x0302:
                return "GL_SRC_ALPHA";
            case 0x0303:
                return "GL_ONE_MINUS_SRC_ALPHA";
            case 0x0304:
                return "GL_DST_ALPHA";
            case 0x0305:
                return "GL_ONE_MINUS_DST_ALPHA";
            case 0x0306:
                return "GL_DST_COLOR";
            case 0x0307:
                return "GL_ONE_MINUS_DST_COLOR";
            case 0x0308:
                return "GL_SRC_ALPHA_SATURATE";
            case 0x0404:
                return "GL_FRONT";
            case 0x0405:
                return "GL_BACK";
            case 0x0408:
                return "GL_FRONT_AND_BACK";
            case 0x0500:
                return "GL_INVALID_ENUM";
            case 0x0501:
                return "GL_INVALID_VALUE";
            case 0x0502:
                return "GL_INVALID_OPERATION";
            case GL_OUT_OF_MEMORY:
                return "GL_OUT_OF_MEMORY";
            case 0x0506:
                return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case 0x0900:
                return "GL_CW";
            case 0x0901:
                return "GL_CCW";
            case 0x0b44:
                return "GL_CULL_FACE";
            case 0x0b50:
                return "GL_LIGHTING";
            case 0x0b57:
                return "GL_COLOR_MATERIAL";
            case 0x0b71:
                return "GL_DEPTH_TEST";
            case 0x0ba1:
                return "GL_NORMALIZE";
            case 0x0b90:
                return "GL_STENCIL_TEST";
            case 0x0b72:
                return "GL_DEPTH_WRITEMASK";
            case 0x0b73:
                return "GL_DEPTH_CLEAR_VALUE";
            case 0x0b74:
                return "GL_DEPTH_FUNC";
            case 0x0ba2:
                return "GL_VIEWPORT";
            case 0x0c10:
                return "GL_SCISSOR_BOX";
            case 0x0d33:
                return "GL_MAX_TEXTURE_SIZE";
            case 0x0d3a:
                return "GL_MAX_VIEWPORT_DIMS";
            case 0x0be2:
                return "GL_BLEND";
            case 0x0c11:
                return "GL_SCISSOR_TEST";
            case 0x0cf5:
                return "GL_UNPACK_ALIGNMENT";
            case 0x0d05:
                return "GL_PACK_ALIGNMENT";
            case 0x0de1:
                return "GL_TEXTURE_2D";
            case 0x1400:
                return "GL_BYTE";
            case 0x1401:
                return "GL_UNSIGNED_BYTE";
            case 0x1402:
                return "GL_SHORT";
            case 0x1403:
                return "GL_UNSIGNED_SHORT";
            case 0x1404:
                return "GL_INT";
            case 0x1405:
                return "GL_UNSIGNED_INT";
            case 0x1406:
                return "GL_FLOAT";
            case 0x1700:
                return "GL_MODELVIEW";
            case 0x1701:
                return "GL_PROJECTION";
            case 0x1702:
                return "GL_TEXTURE";
            case 0x0ba0:
                return "GL_MATRIX_MODE";
            case 0x0ba3:
                return "GL_MODELVIEW_STACK_DEPTH";
            case 0x0ba4:
                return "GL_PROJECTION_STACK_DEPTH";
            case 0x0ba5:
                return "GL_TEXTURE_STACK_DEPTH";
            case 0x0ba6:
                return "GL_MODELVIEW_MATRIX";
            case 0x0ba7:
                return "GL_PROJECTION_MATRIX";
            case 0x0ba8:
                return "GL_TEXTURE_MATRIX";
            case 0x1903:
                return "GL_RED";
            case 0x1906:
                return "GL_ALPHA";
            case 0x1907:
                return "GL_RGB";
            case 0x1908:
                return "GL_RGBA";
            case 0x1909:
                return "GL_LUMINANCE";
            case 0x190a:
                return "GL_LUMINANCE_ALPHA";
            case 0x1f00:
                return "GL_VENDOR";
            case 0x1f01:
                return "GL_RENDERER";
            case 0x1f02:
                return "GL_VERSION";
            case 0x1f03:
                return "GL_EXTENSIONS";
            case 0x2100:
                return "GL_MODULATE";
            case 0x2600:
                return "GL_NEAREST";
            case 0x2601:
                return "GL_LINEAR";
            case 0x2800:
                return "GL_TEXTURE_MAG_FILTER";
            case 0x2801:
                return "GL_TEXTURE_MIN_FILTER";
            case 0x2802:
                return "GL_TEXTURE_WRAP_S";
            case 0x2803:
                return "GL_TEXTURE_WRAP_T";
            case 0x1100:
                return "GL_DONT_CARE";
            case 0x1101:
                return "GL_FASTEST";
            case 0x1102:
                return "GL_NICEST";
            case 0x150a:
                return "GL_INVERT";
            case 0x1e00:
                return "GL_KEEP";
            case 0x1e01:
                return "GL_REPLACE";
            case 0x1e02:
                return "GL_INCR";
            case 0x1e03:
                return "GL_DECR";
            case 0x2101:
                return "GL_DECAL";
            case 0x0104:
                return "GL_ADD";
            case 0x2602:
                return "GL_NEAREST_MIPMAP_NEAREST";
            case 0x2701:
                return "GL_LINEAR_MIPMAP_NEAREST";
            case 0x2702:
                return "GL_NEAREST_MIPMAP_LINEAR";
            case 0x2703:
                return "GL_LINEAR_MIPMAP_LINEAR";
            case 0x2900:
                return "GL_CLAMP";
            case 0x2901:
                return "GL_REPEAT";
            case 0x812f:
                return "GL_CLAMP_TO_EDGE";
            case 0x4000:
                return "GL_COLOR_BUFFER_BIT";
            case 0x8074:
                return "GL_VERTEX_ARRAY";
            case 0x8075:
                return "GL_NORMAL_ARRAY";
            case 0x8076:
                return "GL_COLOR_ARRAY";
            case 0x8078:
                return "GL_TEXTURE_COORD_ARRAY";
            case 0x2200:
                return "GL_TEXTURE_ENV_MODE";
            case 0x0c50:
                return "GL_PERSPECTIVE_CORRECTION_HINT";
            case 0x8006:
                return "GL_FUNC_ADD";
            case 0x800a:
                return "GL_FUNC_SUBTRACT";
            case 0x800b:
                return "GL_FUNC_REVERSE_SUBTRACT";
            case 0x84e0:
                return "GL_ACTIVE_TEXTURE";
            case 0x8892:
                return "GL_ARRAY_BUFFER";
            case 0x8893:
                return "GL_ELEMENT_ARRAY_BUFFER";
            case 0x88e0:
                return "GL_STREAM_DRAW";
            case 0x88e4:
                return "GL_STATIC_DRAW";
            case 0x88e8:
                return "GL_DYNAMIC_DRAW";
            case 0x8d40:
                return "GL_FRAMEBUFFER";
            case 0x8d41:
                return "GL_RENDERBUFFER";
            case 0x8d42:
                return "GL_RENDERBUFFER_WIDTH";
            case 0x8d43:
                return "GL_RENDERBUFFER_HEIGHT";
            case 0x8b30:
                return "GL_FRAGMENT_SHADER";
            case 0x8b31:
                return "GL_VERTEX_SHADER";
            case 0x8b81:
                return "GL_COMPILE_STATUS";
            case 0x8b82:
                return "GL_LINK_STATUS";
            case 0x8b84:
                return "GL_INFO_LOG_LENGTH";
            case 0x8b8d:
                return "GL_CURRENT_PROGRAM";
            case 0x8ca6:
                return "GL_FRAMEBUFFER_BINDING";
            case 0x8cd0:
                return "GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE";
            case 0x8cd1:
                return "GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME";
            case 0x8cd2:
                return "GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL";
            case 0x8ce0:
                return "GL_COLOR_ATTACHMENT0";
            case 0x8cd5:
                return "GL_FRAMEBUFFER_COMPLETE";
            case 0x8cd6:
                return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
            case 0x8cd7:
                return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
            default:
                return hex(value);
        }
    };
    const auto blendFactor = [&hex](uint32_t value) -> std::string {
        switch (value) {
            case 0:
                return "GL_ZERO";
            case 1:
                return "GL_ONE";
            case 0x0300:
                return "GL_SRC_COLOR";
            case 0x0301:
                return "GL_ONE_MINUS_SRC_COLOR";
            case 0x0302:
                return "GL_SRC_ALPHA";
            case 0x0303:
                return "GL_ONE_MINUS_SRC_ALPHA";
            case 0x0304:
                return "GL_DST_ALPHA";
            case 0x0305:
                return "GL_ONE_MINUS_DST_ALPHA";
            case 0x0306:
                return "GL_DST_COLOR";
            case 0x0307:
                return "GL_ONE_MINUS_DST_COLOR";
            case 0x0308:
                return "GL_SRC_ALPHA_SATURATE";
            default:
                return hex(value);
        }
    };
    const auto stencilOpName = [&hex](uint32_t value) -> std::string {
        switch (value) {
            case 0:
                return "GL_ZERO";
            case 0x1e00:
                return "GL_KEEP";
            case 0x1e01:
                return "GL_REPLACE";
            case 0x1e02:
                return "GL_INCR";
            case 0x1e03:
                return "GL_DECR";
            case 0x150a:
                return "GL_INVERT";
            case 0x8507:
                return "GL_INCR_WRAP";
            case 0x8508:
                return "GL_DECR_WRAP";
            default:
                return hex(value);
        }
    };
    const auto clearMask = [&hex](uint32_t mask) {
        std::string text;
        const auto append = [&text](const char* name) {
            if (!text.empty()) text += "|";
            text += name;
        };
        if (mask & 0x4000) append("GL_COLOR_BUFFER_BIT");
        if (mask & 0x0100) append("GL_DEPTH_BUFFER_BIT");
        if (mask & 0x0400) append("GL_STENCIL_BUFFER_BIT");
        const uint32_t rest = mask & ~(0x4000u | 0x0100u | 0x0400u);
        if (rest || text.empty()) {
            if (!text.empty()) text += "|";
            text += hex(rest ? rest : mask);
        }
        return text;
    };
    const auto number = [](float value) {
        std::ostringstream out;
        out << std::setprecision(5) << value;
        return out.str();
    };
    std::ostringstream out;
    if (name == "glClear") {
        out << name << "(mask=" << clearMask(args.word()) << ')';
    } else if (name == "glClearColor" || name == "glColor4f" || name == "glBlendColor") {
        const float red = args.f32(), green = args.f32(), blue = args.f32(), alpha = args.f32();
        out << name << '(' << number(red) << ", " << number(green) << ", " << number(blue) << ", "
            << number(alpha) << ')';
    } else if (name == "glLineWidth" || name == "glClearDepthf") {
        out << name << '(' << number(args.f32()) << ')';
    } else if (name == "glClearStencil") {
        out << name << '(' << int(args.word()) << ')';
    } else if (name == "glFinish" || name == "glFlush" || name == "glReleaseShaderCompiler") {
        out << name << "()";
    } else if (name == "glPolygonOffset" || name == "glDepthRangef") {
        const float first = args.f32(), second = args.f32();
        out << name << '(' << number(first) << ", " << number(second) << ')';
    } else if (name == "glViewport" || name == "glScissor") {
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word());
        out << name << '(' << x << ", " << y << ", " << width << ", " << height << ')';
    } else if (name == "glActiveTexture" || name == "glClientActiveTexture" ||
               name == "glEnableClientState" || name == "glDisableClientState") {
        out << name << '(' << enumName(args.word()) << ')';
    } else if (name == "glEnable" || name == "glDisable" || name == "glFrontFace" ||
               name == "glCullFace" || name == "glDepthFunc" || name == "glBlendEquation" ||
               name == "glGenerateMipmap" || name == "glGenerateMipmapOES") {
        const uint32_t value = args.word();
        if ((name == "glEnable" || name == "glDisable") && value >= 0x4000 && value <= 0x4007)
            out << name << "(GL_LIGHT" << (value - 0x4000) << ')';
        else
            out << name << '(' << enumName(value) << ')';
    } else if (name == "glDepthMask") {
        out << name << '(' << (args.word() ? "GL_TRUE" : "GL_FALSE") << ')';
    } else if (name == "glBindFramebuffer" || name == "glBindFramebufferOES" ||
               name == "glBindRenderbuffer" || name == "glBindRenderbufferOES" ||
               name == "glBindTexture" || name == "glBindBuffer") {
        const uint32_t target = args.word(), object = args.word();
        out << name << "(target=" << enumName(target) << ", name=" << object << ')';
    } else if (name == "glDrawArrays") {
        const uint32_t mode = args.word();
        const int first = int(args.word()), count = int(args.word());
        out << name << "(mode=" << enumName(mode) << ", first=" << first << ", count=" << count
            << ')';
    } else if (name == "glDrawElements") {
        const uint32_t mode = args.word();
        const int count = int(args.word());
        const uint32_t type = args.word(), indices = args.word();
        out << name << "(mode=" << enumName(mode) << ", count=" << count
            << ", type=" << enumName(type) << ", indices=" << hex(indices) << ')';
    } else if (name == "glBufferData") {
        const uint32_t target = args.word(), bytes = args.word(), data = args.word();
        const uint32_t usage = args.word();
        out << name << "(target=" << enumName(target) << ", bytes=" << bytes
            << ", data=" << hex(data) << ", usage=" << enumName(usage) << ')';
    } else if (name == "glBufferSubData") {
        const uint32_t target = args.word(), offset = args.word(), bytes = args.word();
        const uint32_t data = args.word();
        out << name << "(target=" << enumName(target) << ", offset=" << offset
            << ", bytes=" << bytes << ", data=" << hex(data) << ')';
    } else if (name == "glPixelStorei") {
        out << name << '(' << enumName(args.word()) << ", alignment=" << int(args.word()) << ')';
    } else if (name == "glTexParameteri") {
        const uint32_t target = args.word(), pname = args.word();
        const int parameter = int(args.word());
        out << name << "(target=" << enumName(target) << ", pname=" << enumName(pname)
            << ", value=" << enumName(uint32_t(parameter)) << ')';
    } else if (name == "glTexEnvi") {
        out << name << "(target=" << enumName(args.word()) << ", pname=" << enumName(args.word())
            << ", value=" << enumName(args.word()) << ')';
    } else if (name == "glTexParameterf") {
        const uint32_t target = args.word(), pname = args.word();
        out << name << "(target=" << enumName(target) << ", pname=" << enumName(pname)
            << ", value=" << number(args.f32()) << ')';
    } else if (name == "glTexParameterfv" || name == "glTexParameteriv") {
        out << name << "(target=" << enumName(args.word()) << ", pname=" << enumName(args.word())
            << ", values=" << hex(args.word()) << ')';
    } else if (name == "glGetTexParameterfv" || name == "glGetTexParameteriv") {
        out << name << "(target=" << enumName(args.word()) << ", pname=" << enumName(args.word())
            << ", out=" << hex(args.word()) << ')';
    } else if (name == "glHint") {
        out << name << '(' << enumName(args.word()) << ", " << enumName(args.word()) << ')';
    } else if (name == "glBlendFunc") {
        out << name << '(' << blendFactor(args.word()) << ", " << blendFactor(args.word()) << ')';
    } else if (name == "glBlendFuncSeparate") {
        out << name << '(' << blendFactor(args.word()) << ", " << blendFactor(args.word()) << ", "
            << blendFactor(args.word()) << ", " << blendFactor(args.word()) << ')';
    } else if (name == "glBlendEquationSeparate") {
        out << name << '(' << enumName(args.word()) << ", " << enumName(args.word()) << ')';
    } else if (name == "glStencilMaskSeparate") {
        out << name << '(' << enumName(args.word()) << ", " << hex(args.word()) << ')';
    } else if (name == "glStencilFunc") {
        const uint32_t function = args.word();
        const int reference = int(args.word());
        const uint32_t mask = args.word();
        out << name << "(func=" << enumName(function) << ", ref=" << reference
            << ", mask=" << hex(mask) << ')';
    } else if (name == "glStencilFuncSeparate") {
        out << name << '(' << enumName(args.word()) << ", func=" << enumName(args.word())
            << ", ref=" << int(args.word()) << ", mask=" << hex(args.word()) << ')';
    } else if (name == "glStencilOpSeparate") {
        out << name << '(' << enumName(args.word()) << ", " << stencilOpName(args.word()) << ", "
            << stencilOpName(args.word()) << ", " << stencilOpName(args.word()) << ')';
    } else if (name == "glStencilOp") {
        out << name << '(' << stencilOpName(args.word()) << ", " << stencilOpName(args.word())
            << ", " << stencilOpName(args.word()) << ')';
    } else if (name == "glColorMask") {
        out << name << '(' << (args.word() ? "GL_TRUE" : "GL_FALSE") << ", "
            << (args.word() ? "GL_TRUE" : "GL_FALSE") << ", "
            << (args.word() ? "GL_TRUE" : "GL_FALSE") << ", "
            << (args.word() ? "GL_TRUE" : "GL_FALSE") << ')';
    } else if (name == "glSampleCoverage") {
        out << name << '(' << number(args.f32()) << ", invert=" << args.word() << ')';
    } else if (name == "glGetError") {
        const uint32_t result = after.registers[0];
        out << name << "() -> "
            << (result ? enumName(result) + " (" + hex(result) + ')' : "GL_NO_ERROR");
    } else if (name == "glGetString") {
        out << name << "(name=" << enumName(args.word()) << ") -> " << hex(after.registers[0]);
    } else if (name == "glGetIntegerv" || name == "glGetFloatv" || name == "glGetBooleanv") {
        out << name << "(pname=" << enumName(args.word()) << ", out=" << hex(args.word()) << ')';
    } else if (name == "glShaderSource") {
        const uint32_t shader = args.word();
        const int count = int(args.word());
        const uint32_t strings = args.word(), lengths = args.word();
        out << name << "(shader=" << shader << ", strings=" << count
            << ", pointers=" << hex(strings) << ", lengths=" << hex(lengths) << ')';
    } else if (name == "glAttachShader") {
        out << name << "(program=" << args.word() << ", shader=" << args.word() << ')';
    } else if (name == "glBindAttribLocation") {
        out << name << "(program=" << args.word() << ", index=" << args.word()
            << ", name=" << hex(args.word()) << ')';
    } else if (name == "glGetAttribLocation" || name == "glGetUniformLocation") {
        const uint32_t program = args.word(), string = args.word();
        out << name << "(program=" << program << ", name=" << hex(string) << ") -> "
            << int32_t(after.registers[0]);
    } else if (name == "glCreateShader") {
        out << name << "(type=" << enumName(args.word()) << ") -> " << after.registers[0];
    } else if (name == "glCreateProgram") {
        out << name << "() -> " << after.registers[0];
    } else if (name == "glCompileShader" || name == "glLinkProgram" || name == "glUseProgram" ||
               name == "glDeleteShader" || name == "glDeleteProgram") {
        out << name << "(object=" << args.word() << ')';
    } else if (name == "glGetShaderiv" || name == "glGetProgramiv") {
        out << name << "(object=" << args.word() << ", pname=" << enumName(args.word())
            << ", out=" << hex(args.word()) << ')';
    } else if (name == "glGetShaderInfoLog" || name == "glGetProgramInfoLog") {
        const uint32_t object = args.word();
        const int capacity = int(args.word());
        out << name << "(object=" << object << ", capacity=" << capacity
            << ", length=" << hex(args.word()) << ", log=" << hex(args.word()) << ')';
    } else if (name == "glGetActiveAttrib" || name == "glGetActiveUniform") {
        const uint32_t program = args.word(), index = args.word();
        const int capacity = int(args.word());
        const uint32_t length = args.word(), size = args.word(), type = args.word(),
                       output = args.word();
        out << name << "(program=" << program << ", index=" << index << ", capacity=" << capacity
            << ", length=" << hex(length) << ", size=" << hex(size) << ", type=" << hex(type)
            << ", name=" << hex(output) << ')';
    } else if (name == "glGetAttachedShaders") {
        out << name << "(program=" << args.word() << ", capacity=" << int(args.word())
            << ", count=" << hex(args.word()) << ", shaders=" << hex(args.word()) << ')';
    } else if (name == "glGetShaderPrecisionFormat") {
        out << name << "(shader=" << enumName(args.word())
            << ", precision=" << enumName(args.word()) << ", range=" << hex(args.word())
            << ", value=" << hex(args.word()) << ')';
    } else if (name == "glGetShaderSource") {
        out << name << "(shader=" << args.word() << ", capacity=" << int(args.word())
            << ", length=" << hex(args.word()) << ", source=" << hex(args.word()) << ')';
    } else if (name == "glGetUniformfv" || name == "glGetUniformiv") {
        out << name << "(program=" << args.word() << ", location=" << int(args.word())
            << ", out=" << hex(args.word()) << ')';
    } else if (name == "glMatrixMode") {
        out << name << '(' << enumName(args.word()) << ')';
    } else if (name == "glLoadIdentity" || name == "glPushMatrix" || name == "glPopMatrix") {
        out << name << "()";
    } else if (name == "glLoadMatrixf" || name == "glMultMatrixf") {
        out << name << "(matrix=" << hex(args.word()) << ')';
    } else if (name == "glTranslatef" || name == "glScalef") {
        out << name << '(' << number(args.f32()) << ", " << number(args.f32()) << ", "
            << number(args.f32()) << ')';
    } else if (name == "glRotatef") {
        out << name << '(' << number(args.f32()) << " degrees, axis=" << number(args.f32()) << ','
            << number(args.f32()) << ',' << number(args.f32()) << ')';
    } else if (name == "glOrthof" || name == "glFrustumf") {
        out << name << '(' << number(args.f32()) << ", " << number(args.f32()) << ", "
            << number(args.f32()) << ", " << number(args.f32()) << ", " << number(args.f32())
            << ", " << number(args.f32()) << ')';
    } else if (name == "glVertexPointer" || name == "glColorPointer" ||
               name == "glTexCoordPointer") {
        const int size = int(args.word());
        const uint32_t type = args.word();
        const int stride = int(args.word());
        const uint32_t pointer = args.word();
        out << name << "(size=" << size << ", type=" << enumName(type) << ", stride=" << stride
            << ", pointer=" << hex(pointer) << ')';
    } else if (name == "glNormalPointer") {
        const uint32_t type = args.word();
        const int stride = int(args.word());
        const uint32_t pointer = args.word();
        out << name << "(type=" << enumName(type) << ", stride=" << stride
            << ", pointer=" << hex(pointer) << ')';
    } else if (name == "glVertexAttribPointer") {
        const uint32_t index = args.word();
        const int size = int(args.word());
        const uint32_t type = args.word();
        const uint32_t normalized = args.word();
        const int stride = int(args.word());
        const uint32_t pointer = args.word();
        out << name << "(index=" << index << ", size=" << size << ", type=" << enumName(type)
            << ", normalized=" << (normalized ? "true" : "false") << ", stride=" << stride
            << ", pointer=" << hex(pointer) << ')';
    } else if (name == "glReadPixels") {
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word());
        const uint32_t format = args.word(), type = args.word(), pixels = args.word();
        out << name << "(x=" << x << ", y=" << y << ", width=" << width << ", height=" << height
            << ", format=" << enumName(format) << ", type=" << enumName(type)
            << ", pixels=" << hex(pixels) << ')';
    } else if (name == "glCopyTexImage2D") {
        const uint32_t target = args.word(), level = args.word(), internal = args.word();
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word()), border = int(args.word());
        out << name << "(target=" << enumName(target) << ", level=" << level
            << ", internal=" << enumName(internal) << ", rect=" << x << ',' << y << ' ' << width
            << 'x' << height << ", border=" << border << ')';
    } else if (name == "glCopyTexSubImage2D") {
        const uint32_t target = args.word(), level = args.word();
        const int xoffset = int(args.word()), yoffset = int(args.word());
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word());
        out << name << "(target=" << enumName(target) << ", level=" << level
            << ", offset=" << xoffset << ',' << yoffset << ", source=" << x << ',' << y << ' '
            << width << 'x' << height << ')';
    } else if (name == "glCompressedTexImage2D") {
        const uint32_t target = args.word(), level = args.word(), format = args.word();
        const int width = int(args.word()), height = int(args.word()), border = int(args.word());
        const uint32_t bytes = args.word(), data = args.word();
        out << name << "(target=" << enumName(target) << ", level=" << level
            << ", format=" << enumName(format) << ", size=" << width << 'x' << height
            << ", border=" << border << ", bytes=" << bytes << ", data=" << hex(data) << ')';
    } else if (name == "glCompressedTexSubImage2D") {
        const uint32_t target = args.word(), level = args.word();
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word());
        const uint32_t format = args.word(), bytes = args.word(), data = args.word();
        out << name << "(target=" << enumName(target) << ", level=" << level << ", rect=" << x
            << ',' << y << ' ' << width << 'x' << height << ", format=" << enumName(format)
            << ", bytes=" << bytes << ", data=" << hex(data) << ')';
    } else if (name == "glTexImage2D") {
        const uint32_t target = args.word(), level = args.word(), internal = args.word();
        const int width = int(args.word()), height = int(args.word()), border = int(args.word());
        const uint32_t format = args.word(), type = args.word(), pixels = args.word();
        out << name << "(target=" << enumName(target) << ", level=" << level
            << ", internal=" << enumName(internal) << ", size=" << width << 'x' << height
            << ", border=" << border << ", format=" << enumName(format)
            << ", type=" << enumName(type) << ", pixels=" << hex(pixels) << ')';
    } else if (name == "glTexSubImage2D") {
        const uint32_t target = args.word(), level = args.word();
        const int x = int(args.word()), y = int(args.word()), width = int(args.word());
        const int height = int(args.word());
        const uint32_t format = args.word(), type = args.word(), pixels = args.word();
        out << name << "(target=" << enumName(target) << ", level=" << level << ", rect=" << x
            << ',' << y << ' ' << width << 'x' << height << ", format=" << enumName(format)
            << ", type=" << enumName(type) << ", pixels=" << hex(pixels) << ')';
    } else if (name == "glGetFramebufferAttachmentParameteriv" ||
               name == "glGetFramebufferAttachmentParameterivOES") {
        const uint32_t target = args.word(), attachment = args.word(), pname = args.word();
        const uint32_t output = args.word();
        out << name << "(target=" << enumName(target) << ", attachment=" << enumName(attachment)
            << ", pname=" << enumName(pname) << ", out=" << hex(output) << ')';
    } else if (name == "glGetRenderbufferParameteriv" ||
               name == "glGetRenderbufferParameterivOES") {
        out << name << "(target=" << enumName(args.word()) << ", pname=" << enumName(args.word())
            << ", out=" << hex(args.word()) << ')';
    } else if (name == "glFramebufferRenderbuffer" || name == "glFramebufferRenderbufferOES") {
        out << name << "(target=" << enumName(args.word())
            << ", attachment=" << enumName(args.word())
            << ", renderbufferTarget=" << enumName(args.word()) << ", renderbuffer=" << args.word()
            << ')';
    } else if (name == "glFramebufferTexture2D" || name == "glFramebufferTexture2DOES") {
        out << name << "(target=" << enumName(args.word())
            << ", attachment=" << enumName(args.word()) << ", textarget=" << enumName(args.word())
            << ", texture=" << args.word() << ", level=" << int(args.word()) << ')';
    } else if (name == "glCheckFramebufferStatus" || name == "glCheckFramebufferStatusOES") {
        out << name << "(target=" << enumName(args.word()) << ") -> "
            << enumName(after.registers[0]);
    } else {
        out << name << "(arguments not decoded)";
    }
    return out.str();
}

bool GLES::invokeEntry(const std::string& symbol, CpuState& state) {
    Marshal m{state, memory};
    std::string name = symbol.substr(1);
    // ES 1.1 titles use the OES spellings and ES 2.0 titles use the core spellings. Both
    // names are owned by this bridge and converge here so drawable bookkeeping is identical.
    if (name == "glBindFramebuffer" || name == "glBindRenderbuffer" ||
        name == "glCheckFramebufferStatus" || name == "glDeleteFramebuffers" ||
        name == "glDeleteRenderbuffers" || name == "glFramebufferRenderbuffer" ||
        name == "glFramebufferTexture2D" || name == "glGenFramebuffers" ||
        name == "glGenRenderbuffers" || name == "glGetFramebufferAttachmentParameteriv" ||
        name == "glGetRenderbufferParameteriv" || name == "glIsFramebuffer" ||
        name == "glIsRenderbuffer" || name == "glRenderbufferStorage")
        name += "OES";
    else if (name == "glGenerateMipmapOES")
        name = "glGenerateMipmap";
    calls++;

    // With an ES 2.0-only driver the fixed-function entry points are answered here, from the
    // state this bridge keeps, before any attempt to forward them to a driver that does not
    // have them. Names ES 2.0 does have (glEnable, glDrawArrays, glBindBuffer) fall through
    // to the branches below, which record what the emulation needs and then forward.
    if (fixedFunction && clientApi == 1 && fixedFunctionEntry(name, m)) return true;

    if (name == "glClear") {
        auto f = fn<void (*)(uint32_t)>("glClear");
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glFinish" || name == "glFlush" || name == "glReleaseShaderCompiler") {
        auto f = fn<void (*)()>(name.c_str());
        if (!f) return false;
        f();
        return true;
    }
    if (name == "glClearStencil") {
        auto f = fn<void (*)(int)>(name.c_str());
        if (!f) return false;
        f(int(m.word()));
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
        if (fixedFunction && clientApi == 1) {
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
            if (which >= GL_LIGHT0 && which - GL_LIGHT0 < lights.size()) {
                lights[which - GL_LIGHT0].enabled = name == "glEnable";
                return true;
            }
            if (which == GL_NORMALIZE) {
                normalizeNormals = name == "glEnable";
                return true;
            }
            if (which == GL_COLOR_MATERIAL) {
                colorMaterialEnabled = name == "glEnable";
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
        array.buffer = arrayBufferBound ? boundArrayBuffer : 0;
        const void* pointer =
            array.buffered ? reinterpret_cast<const void*>(uintptr_t(array.pointer)) : nullptr;
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
        if (fixedFunction && clientApi == 1 && texture >= GL_TEXTURE0 &&
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
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), buffer = m.word();
        f(target, buffer);
        if (target == GL_ARRAY_BUFFER) {
            arrayBufferBound = buffer != 0;
            boundArrayBuffer = buffer;
        }
        if (target == GL_ELEMENT_ARRAY_BUFFER) {
            elementBufferBound = buffer != 0;
            boundElementBuffer = buffer;
        }
        return true;
    }
    if (name == "glBufferData") {
        const uint32_t target = m.word();
        const int size = int(m.word());
        const uint32_t data = m.word(), usage = m.word();
        auto f = fn<void (*)(uint32_t, size_t, const void*, uint32_t)>(name.c_str());
        if (!f) return false;
        if (size < 0 || size > 256 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "buffer upload size is outside the supported range");
        if (!data || !size) {
            f(target, size_t(size), nullptr, usage);
            if (target == GL_ELEMENT_ARRAY_BUFFER && boundElementBuffer)
                elementBufferData.erase(boundElementBuffer);
            return true;
        }
        std::vector<uint8_t> copy(static_cast<size_t>(size));
        memory.read(data, copy.data(), copy.size());
        f(target, copy.size(), copy.data(), usage);
        if (target == GL_ELEMENT_ARRAY_BUFFER && boundElementBuffer) {
            if (copy.size() <= 64 * 1024 * 1024)
                elementBufferData[boundElementBuffer] = std::move(copy);
            else
                elementBufferData.erase(boundElementBuffer);
        }
        return true;
    }
    if (name == "glBufferSubData") {
        const uint32_t target = m.word();
        const int offset = int(m.word()), size = int(m.word());
        const uint32_t data = m.word();
        auto f = fn<void (*)(uint32_t, std::ptrdiff_t, std::ptrdiff_t, const void*)>(name.c_str());
        if (!f) return false;
        if (offset < 0 || size < 0 || size > 256 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "buffer subupload range is outside the supported range");
        if (size && !data)
            throw Error("GLES_ARGUMENT", "a non-empty buffer subupload has a null source", data);
        std::vector<uint8_t> copy(static_cast<size_t>(size));
        if (size) memory.read(data, copy.data(), copy.size());
        f(target, std::ptrdiff_t(offset), std::ptrdiff_t(size),
          size ? static_cast<const void*>(copy.data()) : nullptr);
        if (target == GL_ELEMENT_ARRAY_BUFFER && boundElementBuffer) {
            auto found = elementBufferData.find(boundElementBuffer);
            if (found != elementBufferData.end() && size_t(offset) <= found->second.size() &&
                size_t(size) <= found->second.size() - size_t(offset))
                std::copy(copy.begin(), copy.end(), found->second.begin() + offset);
            else
                elementBufferData.erase(boundElementBuffer);
        }
        return true;
    }
    if (name == "glCreateShader") {
        auto f = fn<uint32_t (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        state.registers[0] = f(m.word());
        return true;
    }
    if (name == "glCreateProgram") {
        auto f = fn<uint32_t (*)()>(name.c_str());
        if (!f) return false;
        state.registers[0] = f();
        return true;
    }
    if (name == "glDeleteShader" || name == "glDeleteProgram" || name == "glCompileShader" ||
        name == "glLinkProgram" || name == "glUseProgram" || name == "glValidateProgram") {
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glAttachShader" || name == "glDetachShader") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t program = m.word(), shader = m.word();
        f(program, shader);
        return true;
    }
    if (name == "glBindAttribLocation") {
        auto f = fn<void (*)(uint32_t, uint32_t, const char*)>(name.c_str());
        if (!f) return false;
        const uint32_t program = m.word(), index = m.word(), stringAddress = m.word();
        const std::string attribute = readGuestString(memory, stringAddress);
        f(program, index, attribute.c_str());
        return true;
    }
    if (name == "glShaderBinary") {
        auto f = fn<void (*)(int, const uint32_t*, uint32_t, const void*, int)>(name.c_str());
        if (!f) return false;
        const int count = int(m.word());
        const uint32_t shaderArray = m.word(), format = m.word(), binaryAddress = m.word();
        const int length = int(m.word());
        if (count < 0 || count > 4096 || length < 0 || size_t(length) > 64 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "shader binary dimensions exceed supported limits");
        std::vector<uint32_t> shaders(static_cast<size_t>(count));
        if (count && (!shaderArray || !memory.permits(shaderArray, size_t(count) * 4, Read)))
            throw Error("GLES_ARGUMENT", "shader binary object array is not readable", shaderArray);
        if (count) memory.read(shaderArray, shaders.data(), size_t(count) * sizeof(uint32_t));
        std::vector<uint8_t> binary(static_cast<size_t>(length));
        if (length && (!binaryAddress || !memory.permits(binaryAddress, size_t(length), Read)))
            throw Error("GLES_ARGUMENT", "shader binary data is not readable", binaryAddress);
        if (length) memory.read(binaryAddress, binary.data(), binary.size());
        f(count, shaders.empty() ? nullptr : shaders.data(), format,
          binary.empty() ? nullptr : binary.data(), length);
        return true;
    }
    if (name == "glShaderSource") {
        auto f = fn<void (*)(uint32_t, int, const char* const*, const int*)>(name.c_str());
        if (!f) return false;
        const uint32_t shader = m.word();
        const int count = int(m.word());
        const uint32_t sourceArray = m.word(), lengthArray = m.word();
        if (count < 0 || count > 1024)
            throw Error("GLES_ARGUMENT", "shader source count is outside the supported range");
        if (!count) {
            f(shader, 0, nullptr, nullptr);
            return true;
        }
        const size_t arrayBytes = size_t(count) * sizeof(uint32_t);
        if (!sourceArray || !memory.permits(sourceArray, arrayBytes, Read))
            throw Error("GLES_ARGUMENT", "shader source pointer array is not readable",
                        sourceArray);
        std::vector<uint32_t> guestPointers(static_cast<size_t>(count));
        memory.read(sourceArray, guestPointers.data(), arrayBytes);
        std::vector<int32_t> lengths;
        if (lengthArray) {
            if (!memory.permits(lengthArray, size_t(count) * sizeof(int32_t), Read))
                throw Error("GLES_ARGUMENT", "shader source lengths are not readable", lengthArray);
            lengths.resize(size_t(count));
            memory.read(lengthArray, lengths.data(), lengths.size() * sizeof(int32_t));
        }
        std::vector<std::string> sources;
        sources.reserve(size_t(count));
        size_t totalBytes = 0;
        for (int index = 0; index < count; ++index) {
            if (lengthArray && lengths[size_t(index)] >= 0) {
                const size_t length = size_t(lengths[size_t(index)]);
                if (length > 1024 * 1024 || totalBytes > 16 * 1024 * 1024 - length)
                    throw Error("GLES_ARGUMENT", "shader source exceeds the copy budget",
                                guestPointers[size_t(index)]);
                std::string source(length, '\0');
                if (length) {
                    if (!guestPointers[size_t(index)] ||
                        !memory.permits(guestPointers[size_t(index)], length, Read))
                        throw Error("GLES_ARGUMENT", "shader source is not readable",
                                    guestPointers[size_t(index)]);
                    memory.read(guestPointers[size_t(index)], source.data(), length);
                }
                totalBytes += length;
                sources.push_back(std::move(source));
            } else {
                std::string source = readGuestString(memory, guestPointers[size_t(index)]);
                if (totalBytes > 16 * 1024 * 1024 - source.size())
                    throw Error("GLES_ARGUMENT", "shader source exceeds the copy budget");
                totalBytes += source.size();
                sources.push_back(std::move(source));
            }
        }
        std::vector<const char*> hostPointers;
        hostPointers.reserve(sources.size());
        for (const std::string& source : sources) hostPointers.push_back(source.c_str());
        f(shader, count, hostPointers.data(), lengthArray ? lengths.data() : nullptr);
        return true;
    }
    if (name == "glGetAttribLocation" || name == "glGetUniformLocation") {
        auto f = fn<int (*)(uint32_t, const char*)>(name.c_str());
        if (!f) return false;
        const uint32_t program = m.word(), stringAddress = m.word();
        const std::string text = readGuestString(memory, stringAddress);
        state.registers[0] = uint32_t(f(program, text.c_str()));
        return true;
    }
    if (name == "glGetActiveAttrib" || name == "glGetActiveUniform") {
        auto f = fn<void (*)(uint32_t, uint32_t, int, int32_t*, int32_t*, uint32_t*, char*)>(
            name.c_str());
        if (!f) return false;
        const uint32_t program = m.word(), index = m.word();
        const int capacity = int(m.word());
        const uint32_t lengthOut = m.word(), sizeOut = m.word(), typeOut = m.word(),
                       nameOut = m.word();
        if (capacity < 0 || capacity > 1024 * 1024)
            throw Error("GLES_ARGUMENT",
                        "active shader name buffer size is outside the supported range");
        if (lengthOut && !memory.permits(lengthOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "active shader name length is not writable", lengthOut);
        if (sizeOut && !memory.permits(sizeOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "active shader size is not writable", sizeOut);
        if (typeOut && !memory.permits(typeOut, sizeof(uint32_t), Write))
            throw Error("GLES_ARGUMENT", "active shader type is not writable", typeOut);
        if (capacity && (!nameOut || !memory.permits(nameOut, size_t(capacity), Write)))
            throw Error("GLES_ARGUMENT", "active shader name buffer is not writable", nameOut);
        std::vector<char> nameBuffer(size_t(capacity), '\0');
        int32_t length = 0, size = 0;
        uint32_t type = 0;
        f(program, index, capacity, lengthOut ? &length : nullptr, sizeOut ? &size : nullptr,
          typeOut ? &type : nullptr, capacity ? nameBuffer.data() : nullptr);
        if (lengthOut) memory.write(lengthOut, &length, sizeof(length));
        if (sizeOut) memory.write(sizeOut, &size, sizeof(size));
        if (typeOut) memory.write(typeOut, &type, sizeof(type));
        if (capacity) {
            const size_t written = std::min(size_t(capacity), size_t(std::max(length, 0)) + 1);
            memory.write(nameOut, nameBuffer.data(), written);
        }
        return true;
    }
    if (name == "glGetAttachedShaders") {
        auto f = fn<void (*)(uint32_t, int, int32_t*, uint32_t*)>(name.c_str());
        if (!f) return false;
        const uint32_t program = m.word();
        const int capacity = int(m.word());
        const uint32_t countOut = m.word(), shadersOut = m.word();
        if (capacity < 0 || capacity > 4096)
            throw Error("GLES_ARGUMENT", "attached shader capacity is outside the supported range");
        if (countOut && !memory.permits(countOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "attached shader count is not writable", countOut);
        if (capacity && (!shadersOut || !memory.permits(shadersOut, size_t(capacity) * 4, Write)))
            throw Error("GLES_ARGUMENT", "attached shader array is not writable", shadersOut);
        std::vector<uint32_t> shaders(static_cast<size_t>(capacity));
        int32_t count = 0;
        f(program, capacity, countOut ? &count : nullptr, capacity ? shaders.data() : nullptr);
        if (count < 0 || count > capacity)
            throw Error("GLES_DRIVER", "driver returned an invalid attached shader count");
        if (countOut) memory.write(countOut, &count, sizeof(count));
        if (count) memory.write(shadersOut, shaders.data(), size_t(count) * sizeof(uint32_t));
        return true;
    }
    if (name == "glGetShaderPrecisionFormat") {
        auto f = fn<void (*)(uint32_t, uint32_t, int32_t*, int32_t*)>(name.c_str());
        if (!f) return false;
        const uint32_t shaderType = m.word(), precisionType = m.word();
        const uint32_t rangeOut = m.word(), precisionOut = m.word();
        if (rangeOut && !memory.permits(rangeOut, 2 * sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "shader precision range is not writable", rangeOut);
        if (precisionOut && !memory.permits(precisionOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "shader precision value is not writable", precisionOut);
        int32_t range[2] = {}, precision = 0;
        f(shaderType, precisionType, rangeOut ? range : nullptr,
          precisionOut ? &precision : nullptr);
        if (rangeOut) memory.write(rangeOut, range, sizeof(range));
        if (precisionOut) memory.write(precisionOut, &precision, sizeof(precision));
        return true;
    }
    if (name == "glGetShaderSource") {
        auto f = fn<void (*)(uint32_t, int, int32_t*, char*)>(name.c_str());
        if (!f) return false;
        const uint32_t shader = m.word();
        const int capacity = int(m.word());
        const uint32_t lengthOut = m.word(), sourceOut = m.word();
        if (capacity < 0 || capacity > 1024 * 1024)
            throw Error("GLES_ARGUMENT",
                        "shader source buffer size is outside the supported range");
        if (lengthOut && !memory.permits(lengthOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "shader source length is not writable", lengthOut);
        if (capacity && (!sourceOut || !memory.permits(sourceOut, size_t(capacity), Write)))
            throw Error("GLES_ARGUMENT", "shader source buffer is not writable", sourceOut);
        std::vector<char> source(size_t(capacity), '\0');
        int32_t length = 0;
        f(shader, capacity, lengthOut ? &length : nullptr, capacity ? source.data() : nullptr);
        if (lengthOut) memory.write(lengthOut, &length, sizeof(length));
        if (capacity) {
            const size_t written = std::min(size_t(capacity), size_t(std::max(length, 0)) + 1);
            memory.write(sourceOut, source.data(), written);
        }
        return true;
    }
    if (name == "glGetUniformfv" || name == "glGetUniformiv") {
        const uint32_t program = m.word();
        const int location = int(m.word());
        const uint32_t out = m.word();
        if (location < 0) {
            setGlError(GL_INVALID_OPERATION, "uniform query used an invalid location");
            return true;
        }
        auto getProgramiv = fn<void (*)(uint32_t, uint32_t, int32_t*)>("glGetProgramiv");
        auto getActiveUniform =
            fn<void (*)(uint32_t, uint32_t, int, int32_t*, int32_t*, uint32_t*, char*)>(
                "glGetActiveUniform");
        auto getUniformLocation = fn<int (*)(uint32_t, const char*)>("glGetUniformLocation");
        if (!getProgramiv || !getActiveUniform || !getUniformLocation) return false;
        int32_t activeCount = 0, maxNameLength = 0;
        getProgramiv(program, 0x8b86, &activeCount);    // GL_ACTIVE_UNIFORMS
        getProgramiv(program, 0x8b87, &maxNameLength);  // GL_ACTIVE_UNIFORM_MAX_LENGTH
        if (activeCount < 0 || activeCount > 4096 || maxNameLength < 1 ||
            maxNameLength > 1024 * 1024)
            throw Error("GLES_DRIVER", "driver returned invalid active-uniform limits");
        const auto componentCount = [](uint32_t type) -> size_t {
            switch (type) {
                case 0x1406:  // GL_FLOAT
                case 0x1404:  // GL_INT
                case 0x8b56:  // GL_BOOL
                case 0x8b5e:  // GL_SAMPLER_2D
                case 0x8b60:  // GL_SAMPLER_CUBE
                    return 1;
                case 0x8b50:  // GL_FLOAT_VEC2
                case 0x8b53:  // GL_INT_VEC2
                case 0x8b57:  // GL_BOOL_VEC2
                    return 2;
                case 0x8b51:  // GL_FLOAT_VEC3
                case 0x8b54:  // GL_INT_VEC3
                case 0x8b58:  // GL_BOOL_VEC3
                    return 3;
                case 0x8b52:  // GL_FLOAT_VEC4
                case 0x8b55:  // GL_INT_VEC4
                case 0x8b59:  // GL_BOOL_VEC4
                    return 4;
                case 0x8b5a:  // GL_FLOAT_MAT2
                    return 4;
                case 0x8b5b:  // GL_FLOAT_MAT3
                    return 9;
                case 0x8b5c:  // GL_FLOAT_MAT4
                    return 16;
                default:
                    return 0;
            }
        };
        size_t components = 0;
        std::vector<char> uniformName(size_t(maxNameLength), '\0');
        for (int32_t index = 0; index < activeCount && !components; ++index) {
            int32_t length = 0, arraySize = 0;
            uint32_t type = 0;
            getActiveUniform(program, uint32_t(index), maxNameLength, &length, &arraySize, &type,
                             uniformName.data());
            if (length < 0 || length >= maxNameLength || arraySize < 1) continue;
            uniformName[size_t(length)] = '\0';
            const std::string reported(uniformName.data(), size_t(length));
            const bool array =
                arraySize > 1 ||
                (reported.size() >= 3 && reported.compare(reported.size() - 3, 3, "[0]") == 0);
            std::string base = reported;
            if (base.size() >= 3 && base.compare(base.size() - 3, 3, "[0]") == 0)
                base.resize(base.size() - 3);
            const int entries = array ? std::max(arraySize, 1) : 1;
            for (int element = 0; element < entries; ++element) {
                const std::string candidate =
                    array ? base + "[" + std::to_string(element) + "]" : base;
                if (getUniformLocation(program, candidate.c_str()) == location) {
                    components = componentCount(type);
                    break;
                }
            }
        }
        if (!components) {
            setGlError(GL_INVALID_OPERATION, "uniform query location is not active in the program");
            return true;
        }
        const size_t bytes = components * sizeof(uint32_t);
        if (!out || !memory.permits(out, bytes, Write))
            throw Error("GLES_ARGUMENT", "uniform query result is not writable", out);
        std::vector<uint32_t> values(components, 0);
        if (name == "glGetUniformfv") {
            auto f = fn<void (*)(uint32_t, int, float*)>(name.c_str());
            if (!f) return false;
            f(program, location, reinterpret_cast<float*>(values.data()));
        } else {
            auto f = fn<void (*)(uint32_t, int, int32_t*)>(name.c_str());
            if (!f) return false;
            f(program, location, reinterpret_cast<int32_t*>(values.data()));
        }
        memory.write(out, values.data(), bytes);
        return true;
    }
    if (name == "glGetShaderiv" || name == "glGetProgramiv") {
        auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>(name.c_str());
        if (!f) return false;
        const uint32_t object = m.word(), pname = m.word(), out = m.word();
        if (!out || !memory.permits(out, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "shader/program query result is not writable", out);
        int32_t value = 0;
        f(object, pname, &value);
        memory.write(out, &value, sizeof(value));
        return true;
    }
    if (name == "glGetShaderInfoLog" || name == "glGetProgramInfoLog") {
        auto f = fn<void (*)(uint32_t, int, int32_t*, char*)>(name.c_str());
        if (!f) return false;
        const uint32_t object = m.word();
        const int capacity = int(m.word());
        const uint32_t lengthOut = m.word(), logOut = m.word();
        if (capacity < 0 || capacity > 1024 * 1024)
            throw Error("GLES_ARGUMENT",
                        "shader/program log buffer size is outside the supported range");
        if (lengthOut && !memory.permits(lengthOut, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "shader/program log length is not writable", lengthOut);
        if (capacity && logOut && !memory.permits(logOut, size_t(capacity), Write))
            throw Error("GLES_ARGUMENT", "shader/program log buffer is not writable", logOut);
        std::vector<char> buffer(size_t(capacity), '\0');
        int32_t length = 0;
        f(object, capacity, lengthOut ? &length : nullptr,
          capacity && logOut ? buffer.data() : nullptr);
        if (lengthOut) memory.write(lengthOut, &length, sizeof(length));
        if (capacity && logOut) memory.write(logOut, buffer.data(), size_t(capacity));
        return true;
    }
    if (name == "glUniform1f" || name == "glUniform2f" || name == "glUniform3f" ||
        name == "glUniform4f") {
        const int location = int(m.word());
        if (name == "glUniform1f") {
            auto f = fn<void (*)(int, float)>(name.c_str());
            if (!f) return false;
            f(location, m.f32());
        } else if (name == "glUniform2f") {
            auto f = fn<void (*)(int, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32();
            f(location, x, y);
        } else if (name == "glUniform3f") {
            auto f = fn<void (*)(int, float, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32(), z = m.f32();
            f(location, x, y, z);
        } else {
            auto f = fn<void (*)(int, float, float, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32(), z = m.f32(), w = m.f32();
            f(location, x, y, z, w);
        }
        return true;
    }
    if (name == "glUniform1i" || name == "glUniform2i" || name == "glUniform3i" ||
        name == "glUniform4i") {
        const int location = int(m.word());
        if (name == "glUniform1i") {
            auto f = fn<void (*)(int, int)>(name.c_str());
            if (!f) return false;
            f(location, int(m.word()));
        } else if (name == "glUniform2i") {
            auto f = fn<void (*)(int, int, int)>(name.c_str());
            if (!f) return false;
            const int x = int(m.word()), y = int(m.word());
            f(location, x, y);
        } else if (name == "glUniform3i") {
            auto f = fn<void (*)(int, int, int, int)>(name.c_str());
            if (!f) return false;
            const int x = int(m.word()), y = int(m.word()), z = int(m.word());
            f(location, x, y, z);
        } else {
            auto f = fn<void (*)(int, int, int, int, int)>(name.c_str());
            if (!f) return false;
            const int x = int(m.word()), y = int(m.word()), z = int(m.word()), w = int(m.word());
            f(location, x, y, z, w);
        }
        return true;
    }
    if (name.size() == 12 && name.rfind("glUniform", 0) == 0 && name.back() == 'v' &&
        (name[9] >= '1' && name[9] <= '4') && (name[10] == 'f' || name[10] == 'i')) {
        const int location = int(m.word()), count = int(m.word());
        const uint32_t source = m.word();
        const size_t components = size_t(name[9] - '0');
        if (count < 0 || size_t(count) > (16 * 1024 * 1024) / (components * 4))
            throw Error("GLES_ARGUMENT", "uniform vector count is outside the supported range");
        const size_t bytes = size_t(count) * components * sizeof(uint32_t);
        if (count && (!source || !memory.permits(source, bytes, Read)))
            throw Error("GLES_ARGUMENT", "uniform vector is not readable", source);
        if (name[10] == 'f') {
            auto f = fn<void (*)(int, int, const float*)>(name.c_str());
            if (!f) return false;
            std::vector<float> values(size_t(count) * components);
            if (bytes) memory.read(source, values.data(), bytes);
            f(location, count, values.data());
        } else {
            auto f = fn<void (*)(int, int, const int32_t*)>(name.c_str());
            if (!f) return false;
            std::vector<int32_t> values(size_t(count) * components);
            if (bytes) memory.read(source, values.data(), bytes);
            f(location, count, values.data());
        }
        return true;
    }
    if (name == "glUniformMatrix2fv" || name == "glUniformMatrix3fv" ||
        name == "glUniformMatrix4fv") {
        const int location = int(m.word()), count = int(m.word());
        const uint8_t transpose = uint8_t(m.word() & 0xff);
        const uint32_t source = m.word();
        const size_t dimension = name == "glUniformMatrix2fv"   ? 2
                                 : name == "glUniformMatrix3fv" ? 3
                                                                : 4;
        const size_t elements = dimension * dimension;
        if (count < 0 || size_t(count) > (16 * 1024 * 1024) / (elements * sizeof(float)))
            throw Error("GLES_ARGUMENT", "uniform matrix count is outside the supported range");
        const size_t bytes = size_t(count) * elements * sizeof(float);
        if (count && (!source || !memory.permits(source, bytes, Read)))
            throw Error("GLES_ARGUMENT", "uniform matrix is not readable", source);
        auto f = fn<void (*)(int, int, uint8_t, const float*)>(name.c_str());
        if (!f) return false;
        std::vector<float> values(size_t(count) * elements);
        if (bytes) memory.read(source, values.data(), bytes);
        f(location, count, transpose, values.data());
        return true;
    }
    if (name == "glEnableVertexAttribArray" || name == "glDisableVertexAttribArray") {
        const uint32_t index = m.word();
        const bool enabled = name == "glEnableVertexAttribArray";
        genericAttributes[index].enabled = enabled;
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(index);
        return true;
    }
    if (name == "glVertexAttribPointer") {
        const uint32_t index = m.word();
        const int size = int(m.word());
        const uint32_t type = m.word();
        const uint8_t normalized = uint8_t(m.word() & 0xff);
        const int stride = int(m.word());
        const uint32_t pointer = m.word();
        auto f = fn<void (*)(uint32_t, int, uint32_t, uint8_t, int, const void*)>(name.c_str());
        if (!f) return false;
        ClientArray& array = genericAttributes[index];
        array.size = size;
        array.type = type;
        array.stride = stride;
        array.pointer = pointer;
        array.normalized = normalized != 0;
        array.buffered = arrayBufferBound;
        array.buffer = arrayBufferBound ? boundArrayBuffer : 0;
        f(index, size, type, normalized, stride,
          array.buffered ? reinterpret_cast<const void*>(uintptr_t(pointer)) : nullptr);
        return true;
    }
    if (name == "glVertexAttrib1f" || name == "glVertexAttrib2f" || name == "glVertexAttrib3f" ||
        name == "glVertexAttrib4f") {
        const uint32_t index = m.word();
        if (name == "glVertexAttrib1f") {
            auto f = fn<void (*)(uint32_t, float)>(name.c_str());
            if (!f) return false;
            f(index, m.f32());
        } else if (name == "glVertexAttrib2f") {
            auto f = fn<void (*)(uint32_t, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32();
            f(index, x, y);
        } else if (name == "glVertexAttrib3f") {
            auto f = fn<void (*)(uint32_t, float, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32(), z = m.f32();
            f(index, x, y, z);
        } else {
            auto f = fn<void (*)(uint32_t, float, float, float, float)>(name.c_str());
            if (!f) return false;
            const float x = m.f32(), y = m.f32(), z = m.f32(), w = m.f32();
            f(index, x, y, z, w);
        }
        return true;
    }

    if (name == "glVertexAttrib1fv" || name == "glVertexAttrib2fv" || name == "glVertexAttrib3fv" ||
        name == "glVertexAttrib4fv") {
        const uint32_t index = m.word(), source = m.word();
        const int components = name[14] - '0';
        if (components < 1 || components > 4 || !source ||
            !memory.permits(source, size_t(components) * sizeof(float), Read))
            throw Error("GLES_ARGUMENT", "generic vertex attribute values are not readable",
                        source);
        auto f = fn<void (*)(uint32_t, const float*)>(name.c_str());
        if (!f) return false;
        float values[4] = {};
        memory.read(source, values, size_t(components) * sizeof(float));
        f(index, values);
        return true;
    }
    if (name == "glGetVertexAttribfv" || name == "glGetVertexAttribiv") {
        const uint32_t index = m.word(), pname = m.word(), out = m.word();
        const size_t count = pname == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
        if (!out || !memory.permits(out, count * sizeof(uint32_t), Write))
            throw Error("GLES_ARGUMENT", "generic vertex attribute query result is not writable",
                        out);
        const auto found = genericAttributes.find(index);
        const ClientArray defaults{};
        const ClientArray& array = found == genericAttributes.end() ? defaults : found->second;
        bool known = true;
        int32_t integerValue = 0;
        switch (pname) {
            case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
                integerValue = array.enabled;
                break;
            case GL_VERTEX_ATTRIB_ARRAY_SIZE:
                integerValue = array.size ? array.size : 4;
                break;
            case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
                integerValue = array.stride;
                break;
            case GL_VERTEX_ATTRIB_ARRAY_TYPE:
                integerValue = array.type ? int32_t(array.type) : int32_t(GL_FLOAT);
                break;
            case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
                integerValue = array.normalized;
                break;
            case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
                integerValue = int32_t(array.buffer);
                break;
            default:
                known = false;
                break;
        }
        if (known) {
            if (name == "glGetVertexAttribiv") {
                memory.write(out, &integerValue, sizeof(integerValue));
            } else {
                const float value = float(integerValue);
                memory.write(out, &value, sizeof(value));
            }
            return true;
        }
        if (name == "glGetVertexAttribfv") {
            auto f = fn<void (*)(uint32_t, uint32_t, float*)>(name.c_str());
            if (!f) return false;
            std::vector<float> values(count, 0);
            f(index, pname, values.data());
            memory.write(out, values.data(), values.size() * sizeof(float));
        } else {
            auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>(name.c_str());
            if (!f) return false;
            std::vector<int32_t> values(count, 0);
            f(index, pname, values.data());
            memory.write(out, values.data(), values.size() * sizeof(int32_t));
        }
        return true;
    }
    if (name == "glGetVertexAttribPointerv") {
        const uint32_t index = m.word(), pname = m.word(), out = m.word();
        if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
            setGlError(GL_INVALID_ENUM, "glGetVertexAttribPointerv received an invalid pname");
            return true;
        }
        if (!out || !memory.permits(out, sizeof(uint32_t), Write))
            throw Error("GLES_ARGUMENT", "generic vertex attribute pointer result is not writable",
                        out);
        const auto found = genericAttributes.find(index);
        memory.write32(out, found == genericAttributes.end() ? 0 : found->second.pointer);
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
    if (name == "glBlendFunc" || name == "glPixelStorei" || name == "glHint" ||
        name == "glBlendEquationSeparate" || name == "glStencilMaskSeparate") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t first = m.word(), second = m.word();
        f(first, second);
        if (name == "glPixelStorei") {
            const int alignment = int(second);
            if (alignment == 1 || alignment == 2 || alignment == 4 || alignment == 8) {
                if (first == GL_PACK_ALIGNMENT) packAlignment = alignment;
                if (first == GL_UNPACK_ALIGNMENT) unpackAlignment = alignment;
            }
        }
        return true;
    }
    if (name == "glTexEnvi") {
        auto f = fn<void (*)(uint32_t, uint32_t, int)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), pname = m.word();
        const int parameter = int(m.word());
        f(target, pname, parameter);
        return true;
    }
    if (name == "glTexParameteri") {
        auto f = fn<void (*)(uint32_t, uint32_t, int)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), pname = m.word();
        const int parameter = int(m.word());
        f(target, pname, parameter);
        return true;
    }
    if (name == "glTexParameterf") {
        auto f = fn<void (*)(uint32_t, uint32_t, float)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), pname = m.word();
        const float parameter = m.f32();
        f(target, pname, parameter);
        return true;
    }
    if (name == "glTexParameterfv" || name == "glTexParameteriv") {
        const uint32_t target = m.word(), pname = m.word(), source = m.word();
        const size_t count = pname == 0x1004 || pname == 0x8b9d ? 4 : 1;
        const size_t bytes = count * sizeof(uint32_t);
        if (!source || !memory.permits(source, bytes, Read))
            throw Error("GLES_ARGUMENT", "texture parameter values are not readable", source);
        if (name == "glTexParameterfv") {
            auto f = fn<void (*)(uint32_t, uint32_t, const float*)>(name.c_str());
            if (!f) return false;
            float values[4] = {};
            memory.read(source, values, bytes);
            f(target, pname, values);
        } else {
            auto f = fn<void (*)(uint32_t, uint32_t, const int32_t*)>(name.c_str());
            if (!f) return false;
            int32_t values[4] = {};
            memory.read(source, values, bytes);
            f(target, pname, values);
        }
        return true;
    }
    if (name == "glBlendFuncSeparate") {
        auto f = fn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t srcRgb = m.word(), dstRgb = m.word(), srcAlpha = m.word(),
                       dstAlpha = m.word();
        f(srcRgb, dstRgb, srcAlpha, dstAlpha);
        return true;
    }
    if (name == "glStencilFunc") {
        auto f = fn<void (*)(uint32_t, int, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t function = m.word();
        const int reference = int(m.word());
        const uint32_t mask = m.word();
        f(function, reference, mask);
        return true;
    }
    if (name == "glStencilFuncSeparate" || name == "glStencilOpSeparate") {
        auto f = fn<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t face = m.word(), first = m.word(), second = m.word(), third = m.word();
        f(face, first, second, third);
        return true;
    }
    if (name == "glStencilOp") {
        auto f = fn<void (*)(uint32_t, uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t fail = m.word(), depthFail = m.word(), depthPass = m.word();
        f(fail, depthFail, depthPass);
        return true;
    }
    if (name == "glBlendColor") {
        auto f = fn<void (*)(float, float, float, float)>(name.c_str());
        if (!f) return false;
        const float red = m.f32(), green = m.f32(), blue = m.f32(), alpha = m.f32();
        f(red, green, blue, alpha);
        return true;
    }
    if (name == "glClearDepthf") {
        auto f = fn<void (*)(float)>(name.c_str());
        if (!f) return false;
        f(m.f32());
        return true;
    }
    if (name == "glPolygonOffset") {
        auto f = fn<void (*)(float, float)>(name.c_str());
        if (!f) return false;
        const float factor = m.f32(), units = m.f32();
        f(factor, units);
        return true;
    }
    if (name == "glDepthRangef") {
        auto f = fn<void (*)(float, float)>(name.c_str());
        if (!f) return false;
        const float nearValue = m.f32(), farValue = m.f32();
        f(nearValue, farValue);
        return true;
    }
    if (name == "glSampleCoverage") {
        auto f = fn<void (*)(float, uint8_t)>(name.c_str());
        if (!f) return false;
        const float value = m.f32();
        const uint8_t invert = uint8_t(m.word() & 0xff);
        f(value, invert);
        return true;
    }
    if (name == "glColorMask") {
        auto f = fn<void (*)(uint8_t, uint8_t, uint8_t, uint8_t)>(name.c_str());
        if (!f) return false;
        const uint8_t red = uint8_t(m.word() & 0xff), green = uint8_t(m.word() & 0xff);
        const uint8_t blue = uint8_t(m.word() & 0xff), alpha = uint8_t(m.word() & 0xff);
        f(red, green, blue, alpha);
        return true;
    }
    if (name == "glStencilMask") {
        auto f = fn<void (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        f(m.word());
        return true;
    }
    if (name == "glDepthFunc" || name == "glMatrixMode" || name == "glFrontFace" ||
        name == "glCullFace" || name == "glBlendEquation" || name == "glGenerateMipmap") {
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
        if (fixedFunction && clientApi == 1) {
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
        if (fixedFunction && clientApi == 1) {
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
        } else {
            vertices = 0;
            if (element) {
                const auto buffer = elementBufferData.find(boundElementBuffer);
                if (buffer != elementBufferData.end()) {
                    const size_t offset = size_t(indices), bytes = size_t(count) * element;
                    if (offset > buffer->second.size() || bytes > buffer->second.size() - offset)
                        throw Error("GLES_ARRAY", "element buffer range is outside captured data");
                    copy.assign(buffer->second.begin() + offset,
                                buffer->second.begin() + offset + bytes);
                    vertices = largestIndex(copy, element) + 1;
                    if (vertices > VertexBudget)
                        throw Error("GLES_ARRAY",
                                    "an index reaches more vertices than the copy budget");
                }
            }
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
        if (name == "glDeleteBuffers") {
            for (uint32_t deleted : names) {
                elementBufferData.erase(deleted);
                if (deleted == boundArrayBuffer) {
                    boundArrayBuffer = 0;
                    arrayBufferBound = false;
                }
                if (deleted == boundElementBuffer) {
                    boundElementBuffer = 0;
                    elementBufferBound = false;
                }
            }
        } else if (name == "glDeleteFramebuffersOES") {
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
        // An ES 2.0-only driver answers these ES 1.1 limits with GL_INVALID_ENUM and leaves
        // the guest's result alone. These limits describe the emulation, not ANGLE's native
        // fixed-function implementation.
        if (fixedFunction && clientApi == 1) {
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
                    answer = 16;
                    break;
                case GL_MAX_PROJECTION_STACK_DEPTH:
                case GL_MAX_TEXTURE_STACK_DEPTH:
                    answer = 2;
                    break;
                case GL_MODELVIEW_STACK_DEPTH:
                    answer = int(modelviewStack.size() + 1);
                    break;
                case GL_PROJECTION_STACK_DEPTH:
                    answer = int(projectionStack.size() + 1);
                    break;
                case GL_TEXTURE_STACK_DEPTH: {
                    const auto found = textureMatrixStacks.find(activeTextureUnit);
                    answer = int(found == textureMatrixStacks.end() ? 1 : found->second.size() + 1);
                    break;
                }
                case GL_MATRIX_MODE:
                    answer = int(matrixMode);
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
        size_t count = queryVectorLength(pname);
        if (pname == GL_COMPRESSED_TEXTURE_FORMATS || pname == GL_SHADER_BINARY_FORMATS) {
            const uint32_t countName = pname == GL_COMPRESSED_TEXTURE_FORMATS
                                           ? GL_NUM_COMPRESSED_TEXTURE_FORMATS
                                           : GL_NUM_SHADER_BINARY_FORMATS;
            int32_t available = 0;
            f(countName, &available);
            if (available < 0 || available > 4096)
                throw Error("GLES_QUERY", "driver returned an invalid GL query array length");
            count = size_t(available);
        }
        if (count > 4096 ||
            (count && (!out || !memory.permits(out, count * sizeof(int32_t), Write))))
            throw Error("GLES_ARGUMENT", "query result array is not writable", out);
        std::vector<int32_t> values(count, 0);
        f(pname, values.data());
        if (count) memory.write(out, values.data(), count * sizeof(int32_t));
        return true;
    }
    if (name == "glGetRenderbufferParameterivOES") {
        const uint32_t target = m.word(), pname = m.word(), out = m.word();
        auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>(name.c_str());
        if (!f) return false;
        if (!out || !memory.permits(out, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "renderbuffer query result is not writable", out);
        int32_t value = 0;
        f(target, pname, &value);
        memory.write(out, &value, sizeof(value));
        return true;
    }
    if (name == "glGetFramebufferAttachmentParameterivOES") {
        const uint32_t target = m.word(), attachment = m.word(), pname = m.word(), out = m.word();
        auto f = fn<void (*)(uint32_t, uint32_t, uint32_t, int32_t*)>(name.c_str());
        if (!f) return false;
        if (!out || !memory.permits(out, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "framebuffer attachment query result is not writable",
                        out);
        int32_t value = 0;
        f(target, attachment, pname, &value);
        memory.write(out, &value, sizeof(value));
        return true;
    }
    if (name == "glGetBufferParameteriv") {
        const uint32_t target = m.word(), pname = m.word(), out = m.word();
        auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>(name.c_str());
        if (!f) return false;
        if (!out || !memory.permits(out, sizeof(int32_t), Write))
            throw Error("GLES_ARGUMENT", "buffer query result is not writable", out);
        int32_t value = 0;
        f(target, pname, &value);
        memory.write(out, &value, sizeof(value));
        return true;
    }
    if (name == "glGetTexParameterfv" || name == "glGetTexParameteriv") {
        const uint32_t target = m.word(), pname = m.word(), out = m.word();
        const size_t count = pname == 0x1004 || pname == 0x8b9d ? 4 : 1;
        const size_t bytes = count * sizeof(uint32_t);
        if (!out || !memory.permits(out, bytes, Write))
            throw Error("GLES_ARGUMENT", "texture parameter query result is not writable", out);
        if (name == "glGetTexParameterfv") {
            auto f = fn<void (*)(uint32_t, uint32_t, float*)>(name.c_str());
            if (!f) return false;
            float values[4] = {};
            f(target, pname, values);
            memory.write(out, values, bytes);
        } else {
            auto f = fn<void (*)(uint32_t, uint32_t, int32_t*)>(name.c_str());
            if (!f) return false;
            int32_t values[4] = {};
            f(target, pname, values);
            memory.write(out, values, bytes);
        }
        return true;
    }
    if (name == "glGetFloatv") {
        const uint32_t pname = m.word(), out = m.word();
        const size_t count =
            pname == 0x0ba6 || pname == 0x0ba7 || pname == 0x0ba8 ? 16 : queryVectorLength(pname);
        if (!out || !memory.permits(out, count * sizeof(float), Write))
            throw Error("GLES_ARGUMENT", "query result array is not writable", out);
        if (fixedFunction && clientApi == 1 &&
            (pname == 0x0ba6 || pname == 0x0ba7 || pname == 0x0ba8 || pname == 0x0b00)) {
            const float* values = nullptr;
            if (pname == 0x0ba6)
                values = modelview.data();
            else if (pname == 0x0ba7)
                values = projection.data();
            else if (pname == 0x0ba8)
                values = textureMatrixForUnit(activeTextureUnit).data();
            else
                values = currentColour;
            memory.write(out, values, count * sizeof(float));
            return true;
        }
        auto f = fn<void (*)(uint32_t, float*)>(name.c_str());
        if (!f) return false;
        std::vector<float> values(count, 0.0f);
        f(pname, values.data());
        memory.write(out, values.data(), count * sizeof(float));
        return true;
    }
    if (name == "glGetBooleanv") {
        const uint32_t pname = m.word(), out = m.word();
        const size_t count = queryVectorLength(pname);
        if (!out || !memory.permits(out, count, Write))
            throw Error("GLES_ARGUMENT", "query result array is not writable", out);
        if (fixedFunction && clientApi == 1) {
            uint8_t value = 0;
            bool known = true;
            switch (pname) {
                case GL_TEXTURE_2D:
                    value = textureEnabledForUnit(activeTextureUnit);
                    break;
                case GL_LIGHTING:
                    value = lightingEnabled;
                    break;
                case GL_NORMALIZE:
                    value = normalizeNormals;
                    break;
                case GL_COLOR_MATERIAL:
                    value = colorMaterialEnabled;
                    break;
                case GL_VERTEX_ARRAY:
                    value = vertex.enabled;
                    break;
                case GL_COLOR_ARRAY:
                    value = colour.enabled;
                    break;
                case GL_NORMAL_ARRAY:
                    value = normal.enabled;
                    break;
                case GL_TEXTURE_COORD_ARRAY: {
                    const auto found = texCoord.find(clientTextureUnit);
                    value = found != texCoord.end() && found->second.enabled;
                    break;
                }
                default:
                    known = false;
                    break;
            }
            if (!known && pname >= GL_LIGHT0 && pname - GL_LIGHT0 < lights.size()) {
                value = lights[pname - GL_LIGHT0].enabled;
                known = true;
            }
            if (known) {
                memory.write(out, &value, sizeof(value));
                return true;
            }
        }
        auto f = fn<void (*)(uint32_t, uint8_t*)>(name.c_str());
        if (!f) return false;
        std::vector<uint8_t> values(count, 0);
        f(pname, values.data());
        memory.write(out, values.data(), count);
        return true;
    }
    if (name == "glGetError") {
        if (pendingGlError) {
            state.registers[0] = pendingGlError;
            pendingGlError = 0;
            return true;
        }
        auto f = fn<uint32_t (*)()>(name.c_str());
        if (!f) return false;
        state.registers[0] = f();
        return true;
    }
    if (name == "glGetString") {
        const uint32_t pname = m.word();
        auto f = fn<const uint8_t* (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        const uint8_t* source = f(pname);
        if (!source) {
            state.registers[0] = 0;
            return true;
        }
        if (const auto found = guestStrings.find(pname); found != guestStrings.end()) {
            state.registers[0] = found->second;
            return true;
        }
        std::string text;
        for (size_t index = 0; index < 65536; ++index) {
            if (!source[index]) break;
            text.push_back(char(source[index]));
        }
        if (pname == 0x1f03) {
            const auto appendExtension = [&text](const char* extension) {
                const std::string name(extension);
                size_t at = 0;
                while ((at = text.find(name, at)) != std::string::npos) {
                    const bool left = at == 0 || text[at - 1] == ' ';
                    const size_t end = at + name.size();
                    const bool right = end == text.size() || text[end] == ' ';
                    if (left && right) return;
                    at = end;
                }
                if (!text.empty()) text.push_back(' ');
                text += name;
            };
            if (function("glBindFramebufferOES") && function("glCheckFramebufferStatusOES"))
                appendExtension("GL_OES_framebuffer_object");
            appendExtension("GL_IMG_texture_compression_pvrtc");
        }
        if (!guestHeap) {
            setError("glGetString cannot expose a driver string without a guest heap");
            state.registers[0] = 0;
            return true;
        }
        const uint32_t address = guestHeap->allocate(uint32_t(text.size() + 1), 4);
        if (!address) {
            setError("guest heap could not hold a driver string returned by glGetString");
            state.registers[0] = 0;
            return true;
        }
        memory.write(address, text.c_str(), text.size() + 1);
        guestStrings.emplace(pname, address);
        state.registers[0] = address;
        return true;
    }
    if (name == "glIsEnabled") {
        const uint32_t capability = m.word();
        if (fixedFunction && clientApi == 1) {
            if (capability == GL_TEXTURE_2D) {
                state.registers[0] = textureEnabledForUnit(activeTextureUnit);
                return true;
            }
            if (capability == GL_LIGHTING) {
                state.registers[0] = lightingEnabled;
                return true;
            }
            if (capability == GL_NORMALIZE) {
                state.registers[0] = normalizeNormals;
                return true;
            }
            if (capability == GL_COLOR_MATERIAL) {
                state.registers[0] = colorMaterialEnabled;
                return true;
            }
            if (capability >= GL_LIGHT0 && capability - GL_LIGHT0 < lights.size()) {
                state.registers[0] = lights[capability - GL_LIGHT0].enabled;
                return true;
            }
        }
        auto f = fn<uint8_t (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        state.registers[0] = f(capability);
        return true;
    }
    if (name == "glIsBuffer" || name == "glIsTexture" || name == "glIsProgram" ||
        name == "glIsShader" || name == "glIsFramebufferOES" || name == "glIsRenderbufferOES") {
        auto f = fn<uint8_t (*)(uint32_t)>(name.c_str());
        if (!f) return false;
        state.registers[0] = f(m.word());
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
    if (name == "glReadPixels") {
        const int x = int(m.word()), y = int(m.word()), width = int(m.word()),
                  height = int(m.word());
        const uint32_t format = m.word(), type = m.word(), destination = m.word();
        auto f = fn<void (*)(int, int, int, int, uint32_t, uint32_t, void*)>(name.c_str());
        if (!f) return false;
        const size_t bytesPerPixel = texelBytes(format, type);
        size_t rowBytes = 0, totalBytes = 0;
        if (!bytesPerPixel ||
            !pixelLayout(width, height, bytesPerPixel, packAlignment, rowBytes, totalBytes)) {
            // Let the driver set the GL error for an unsupported enum or invalid dimensions;
            // there is no safe host-side copy size for those arguments.
            f(x, y, width, height, format, type, nullptr);
            return true;
        }
        if (!totalBytes) {
            f(x, y, width, height, format, type, nullptr);
            return true;
        }
        if (!destination || !memory.permits(destination, totalBytes, Write))
            throw Error("GLES_ARGUMENT", "glReadPixels destination is not writable", destination);
        std::vector<uint8_t> pixels(totalBytes, 0);
        f(x, y, width, height, format, type, pixels.data());
        memory.write(destination, pixels.data(), pixels.size());
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
    if (name == "glColorMaterial") {
        auto f = fn<void (*)(uint32_t, uint32_t)>(name.c_str());
        if (!f) return false;
        const uint32_t face = m.word(), mode = m.word();
        f(face, mode);
        return true;
    }
    if (name == "glNormal3f") {
        auto f = fn<void (*)(float, float, float)>(name.c_str());
        if (!f) return false;
        const float x = m.f32(), y = m.f32(), z = m.f32();
        f(x, y, z);
        return true;
    }
    if (name == "glLightModeli") {
        auto f = fn<void (*)(uint32_t, int)>(name.c_str());
        if (!f) return false;
        const uint32_t pname = m.word();
        const int value = int(m.word());
        f(pname, value);
        return true;
    }
    if (name == "glLightf" || name == "glMaterialf") {
        auto f = fn<void (*)(uint32_t, uint32_t, float)>(name.c_str());
        if (!f) return false;
        const uint32_t first = m.word(), pname = m.word();
        const float value = m.f32();
        f(first, pname, value);
        return true;
    }
    if (name == "glLightModelfv") {
        const uint32_t pname = m.word(), source = m.word();
        const size_t count = pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1;
        auto f = fn<void (*)(uint32_t, const float*)>(name.c_str());
        if (!f) return false;
        if (!source || !memory.permits(source, count * sizeof(float), Read))
            throw Error("GLES_ARGUMENT", "light-model values are not readable", source);
        std::vector<float> values(count);
        memory.read(source, values.data(), values.size() * sizeof(float));
        f(pname, values.data());
        return true;
    }
    if (name == "glLightfv" || name == "glMaterialfv") {
        const uint32_t first = m.word(), pname = m.word(), source = m.word();
        size_t count = 0;
        if (name == "glLightfv") {
            switch (pname) {
                case GL_AMBIENT:
                case GL_DIFFUSE:
                case GL_SPECULAR:
                case GL_POSITION:
                    count = 4;
                    break;
                case GL_SPOT_DIRECTION:
                    count = 3;
                    break;
                case GL_SPOT_EXPONENT:
                case GL_SPOT_CUTOFF:
                case GL_CONSTANT_ATTENUATION:
                case GL_LINEAR_ATTENUATION:
                case GL_QUADRATIC_ATTENUATION:
                    count = 1;
                    break;
                default:
                    break;
            }
        } else {
            switch (pname) {
                case GL_AMBIENT:
                case GL_DIFFUSE:
                case GL_SPECULAR:
                case GL_EMISSION:
                case GL_AMBIENT_AND_DIFFUSE:
                    count = 4;
                    break;
                case GL_SHININESS:
                    count = 1;
                    break;
                default:
                    break;
            }
        }
        auto f = fn<void (*)(uint32_t, uint32_t, const float*)>(name.c_str());
        if (!f) return false;
        if (!count) {
            f(first, pname, nullptr);
            return true;
        }
        if (!source || !memory.permits(source, count * sizeof(float), Read))
            throw Error("GLES_ARGUMENT", "light/material values are not readable", source);
        std::vector<float> values(count);
        memory.read(source, values.data(), values.size() * sizeof(float));
        f(first, pname, values.data());
        return true;
    }
    if (name == "glCopyTexImage2D") {
        auto f = fn<void (*)(uint32_t, int, uint32_t, int, int, int, int, int)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), level = m.word(), internalFormat = m.word();
        const int x = int(m.word()), y = int(m.word()), width = int(m.word());
        const int height = int(m.word()), border = int(m.word());
        f(target, int(level), internalFormat, x, y, width, height, border);
        return true;
    }
    if (name == "glCopyTexSubImage2D") {
        auto f = fn<void (*)(uint32_t, int, int, int, int, int, int, int)>(name.c_str());
        if (!f) return false;
        const uint32_t target = m.word(), level = m.word();
        const int xoffset = int(m.word()), yoffset = int(m.word());
        const int x = int(m.word()), y = int(m.word()), width = int(m.word()),
                  height = int(m.word());
        f(target, int(level), xoffset, yoffset, x, y, width, height);
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
    if (name == "glCompressedTexSubImage2D") {
        const uint32_t target = m.word(), level = m.word();
        const int xoffset = int(m.word()), yoffset = int(m.word()), width = int(m.word());
        const int height = int(m.word());
        const uint32_t format = m.word();
        const int imageSize = int(m.word());
        const uint32_t source = m.word();
        if (imageSize < 0 || size_t(imageSize) > 64 * 1024 * 1024)
            throw Error("GLES_ARGUMENT", "compressed texture subimage exceeds its copy budget",
                        source);
        auto f = fn<void (*)(uint32_t, int, int, int, int, int, uint32_t, int, const void*)>(
            name.c_str());
        if (!f) return false;
        std::vector<uint8_t> compressed(static_cast<size_t>(imageSize));
        if (imageSize && source && !memory.permits(source, size_t(imageSize), Read))
            throw Error("GLES_ARGUMENT", "compressed texture subimage is not readable", source);
        if (imageSize && source) memory.read(source, compressed.data(), compressed.size());
        f(target, int(level), xoffset, yoffset, width, height, format, imageSize,
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
                size_t rowBytes = 0, totalBytes = 0;
                if (!perTexel ||
                    !pixelLayout(w, h, perTexel, unpackAlignment, rowBytes, totalBytes))
                    throw Error("GLES_ARGUMENT", "texture upload layout is not supported");
                copy.resize(totalBytes);
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
            size_t rowBytes = 0, totalBytes = 0;
            if (!perTexel || !pixelLayout(w, h, perTexel, unpackAlignment, rowBytes, totalBytes))
                throw Error("GLES_ARGUMENT", "texture upload layout is not supported");
            copy.resize(totalBytes);
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
// Lighting, current colour, matrices and the common modulate/replace texture modes are
// translated into shader state. The remaining texture-environment modes and simultaneous
// texture stages are counted and reported rather than silently claimed as exact: a draw can
// select a nonzero unit, but this shader samples only that active unit.

bool GLES::fixedFunctionEntry(const std::string& name, Marshal& m) {
    const auto multiplyCurrent = [this](const std::array<float, 16>& transform) {
        std::array<float, 16> result{};
        multiplyMatrices(matrix->data(), transform.data(), result.data());
        *matrix = result;
    };
    const auto currentStack = [this]() -> std::vector<std::array<float, 16>>* {
        if (matrixMode == GL_MODELVIEW) return &modelviewStack;
        if (matrixMode == GL_PROJECTION) return &projectionStack;
        return &textureMatrixStacks[activeTextureUnit];
    };
    const auto maximumStackDepth = [this]() -> size_t {
        if (matrixMode == GL_MODELVIEW) return 16;
        return 2;
    };
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
        array.buffer = arrayBufferBound ? boundArrayBuffer : 0;
        return true;
    }
    if (name == "glNormalPointer") {
        normal.size = 3;  // a normal is always three components; ES 1.1 has no size argument
        normal.type = m.word();
        normal.stride = int(m.word());
        normal.pointer = m.word();
        normal.buffered = arrayBufferBound;
        normal.buffer = arrayBufferBound ? boundArrayBuffer : 0;
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
    if (name == "glLoadIdentity") {
        *matrix = Identity;
        return true;
    }
    if (name == "glLoadMatrixf" || name == "glMultMatrixf") {
        const uint32_t source = m.word();
        float values[16] = {};
        if (!source || !memory.permits(source, sizeof(values), Read))
            throw Error("GLES_ARGUMENT", "matrix is not readable", source);
        memory.read(source, values, sizeof(values));
        if (name == "glLoadMatrixf") {
            std::copy_n(values, 16, matrix->begin());
        } else {
            std::array<float, 16> transform{};
            std::copy_n(values, 16, transform.begin());
            multiplyCurrent(transform);
        }
        return true;
    }
    if (name == "glPushMatrix" || name == "glPopMatrix") {
        auto* stack = currentStack();
        if (name == "glPushMatrix") {
            if (stack->size() + 1 >= maximumStackDepth()) {
                setGlError(GL_INVALID_OPERATION, "OpenGL ES matrix stack overflow");
                return true;
            }
            stack->push_back(*matrix);
        } else {
            if (stack->empty()) {
                setGlError(GL_INVALID_OPERATION, "OpenGL ES matrix stack underflow");
                return true;
            }
            *matrix = stack->back();
            stack->pop_back();
        }
        return true;
    }
    if (name == "glTranslatef" || name == "glScalef") {
        const float x = m.f32(), y = m.f32(), z = m.f32();
        std::array<float, 16> transform = Identity;
        if (name == "glTranslatef") {
            transform[12] = x;
            transform[13] = y;
            transform[14] = z;
        } else {
            transform[0] = x;
            transform[5] = y;
            transform[10] = z;
        }
        multiplyCurrent(transform);
        return true;
    }
    if (name == "glRotatef") {
        const float degrees = m.f32(), x = m.f32(), y = m.f32(), z = m.f32();
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length == 0 || !std::isfinite(length)) return true;
        const float axisX = x / length, axisY = y / length, axisZ = z / length;
        const float radians = degrees * 0.01745329251994329577f;
        const float cosine = std::cos(radians), sine = std::sin(radians), oneMinus = 1 - cosine;
        std::array<float, 16> transform = Identity;
        transform[0] = axisX * axisX * oneMinus + cosine;
        transform[4] = axisX * axisY * oneMinus - axisZ * sine;
        transform[8] = axisX * axisZ * oneMinus + axisY * sine;
        transform[1] = axisY * axisX * oneMinus + axisZ * sine;
        transform[5] = axisY * axisY * oneMinus + cosine;
        transform[9] = axisY * axisZ * oneMinus - axisX * sine;
        transform[2] = axisZ * axisX * oneMinus - axisY * sine;
        transform[6] = axisZ * axisY * oneMinus + axisX * sine;
        transform[10] = axisZ * axisZ * oneMinus + cosine;
        multiplyCurrent(transform);
        return true;
    }
    if (name == "glOrthof" || name == "glFrustumf") {
        const float left = m.f32(), right = m.f32(), bottom = m.f32(), top = m.f32();
        const float nearValue = m.f32(), farValue = m.f32();
        const float width = right - left, height = top - bottom, depth = farValue - nearValue;
        if (width == 0 || height == 0 || depth == 0 ||
            (name == "glFrustumf" && (nearValue <= 0 || farValue <= 0))) {
            setGlError(GL_INVALID_VALUE,
                       "projection volume has a zero extent or invalid clipping plane");
            return true;
        }
        std::array<float, 16> transform{};
        if (name == "glOrthof") {
            transform = Identity;
            transform[0] = 2 / width;
            transform[5] = 2 / height;
            transform[10] = -2 / depth;
            transform[12] = -(right + left) / width;
            transform[13] = -(top + bottom) / height;
            transform[14] = -(farValue + nearValue) / depth;
        } else {
            transform[0] = 2 * nearValue / width;
            transform[5] = 2 * nearValue / height;
            transform[8] = (right + left) / width;
            transform[9] = (top + bottom) / height;
            transform[10] = -(farValue + nearValue) / depth;
            transform[11] = -1;
            transform[14] = -(2 * farValue * nearValue) / depth;
        }
        multiplyCurrent(transform);
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
    if (name == "glNormal3f") {
        currentNormal[0] = m.f32();
        currentNormal[1] = m.f32();
        currentNormal[2] = m.f32();
        currentNormal[3] = 0;
        return true;
    }
    if (name == "glColorMaterial") {
        const uint32_t face = m.word(), mode = m.word();
        if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
            (mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR &&
             mode != GL_EMISSION && mode != GL_AMBIENT_AND_DIFFUSE)) {
            setGlError(GL_INVALID_ENUM, "glColorMaterial received an unsupported face or mode");
            return true;
        }
        colorMaterialFace = face;
        colorMaterialMode = mode;
        return true;
    }
    if (name == "glLightModeli") {
        const uint32_t pname = m.word();
        const int value = int(m.word());
        if (pname == GL_LIGHT_MODEL_LOCAL_VIEWER)
            localViewer = value != 0;
        else if (pname == GL_LIGHT_MODEL_TWO_SIDE)
            twoSidedLighting = value != 0;
        else
            setGlError(GL_INVALID_ENUM, "glLightModeli received an unsupported pname");
        return true;
    }
    if (name == "glLightModelfv") {
        const uint32_t pname = m.word(), source = m.word();
        const size_t count = pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1;
        if (!source || !memory.permits(source, count * sizeof(float), Read))
            throw Error("GLES_ARGUMENT", "light-model values are not readable", source);
        float values[4] = {};
        memory.read(source, values, count * sizeof(float));
        if (pname == GL_LIGHT_MODEL_AMBIENT)
            std::copy_n(values, 4, globalAmbient.begin());
        else if (pname == GL_LIGHT_MODEL_LOCAL_VIEWER)
            localViewer = values[0] != 0;
        else if (pname == GL_LIGHT_MODEL_TWO_SIDE)
            twoSidedLighting = values[0] != 0;
        else
            setGlError(GL_INVALID_ENUM, "glLightModelfv received an unsupported pname");
        return true;
    }
    if (name == "glLightf" || name == "glMaterialf") {
        const uint32_t first = m.word(), pname = m.word();
        const float value = m.f32();
        if (name == "glLightf") {
            if (first < GL_LIGHT0 || first - GL_LIGHT0 >= lights.size()) {
                setGlError(GL_INVALID_ENUM,
                           "glLightf received a light outside GL_LIGHT0..GL_LIGHT7");
                return true;
            }
            Light& light = lights[first - GL_LIGHT0];
            if (pname == GL_SPOT_EXPONENT) {
                if (value < 0 || value > 128)
                    setGlError(GL_INVALID_VALUE, "light spot exponent is outside 0..128");
                else
                    light.spotExponent = value;
            } else if (pname == GL_SPOT_CUTOFF) {
                if (value != 180 && (value < 0 || value > 90))
                    setGlError(GL_INVALID_VALUE, "light spot cutoff is outside 0..90 or 180");
                else {
                    light.spotCutoff = value;
                    light.spotCutoffCos = value == 180 ? -1.0f : std::cos(value * DegreesToRadians);
                }
            } else if (pname == GL_CONSTANT_ATTENUATION || pname == GL_LINEAR_ATTENUATION ||
                       pname == GL_QUADRATIC_ATTENUATION) {
                if (value < 0)
                    setGlError(GL_INVALID_VALUE, "light attenuation cannot be negative");
                else if (pname == GL_CONSTANT_ATTENUATION)
                    light.constantAttenuation = value;
                else if (pname == GL_LINEAR_ATTENUATION)
                    light.linearAttenuation = value;
                else
                    light.quadraticAttenuation = value;
            } else {
                setGlError(GL_INVALID_ENUM, "glLightf received a non-scalar light pname");
            }
        } else if (pname == GL_SHININESS) {
            if (first != GL_FRONT && first != GL_BACK && first != GL_FRONT_AND_BACK) {
                setGlError(GL_INVALID_ENUM, "glMaterialf received an invalid face");
                return true;
            }
            if (value < 0 || value > 128) {
                setGlError(GL_INVALID_VALUE, "material shininess is outside 0..128");
                return true;
            }
            if (first != GL_BACK) materials[0].shininess = value;
            if (first != GL_FRONT) materials[1].shininess = value;
        } else {
            setGlError(GL_INVALID_ENUM, "glMaterialf only supports GL_SHININESS");
        }
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
        size_t count = 0;
        if (name == "glLightfv") {
            if (first < GL_LIGHT0 || first - GL_LIGHT0 >= lights.size()) {
                setGlError(GL_INVALID_ENUM,
                           "glLightfv received a light outside GL_LIGHT0..GL_LIGHT7");
                return true;
            }
            switch (pname) {
                case GL_AMBIENT:
                case GL_DIFFUSE:
                case GL_SPECULAR:
                case GL_POSITION:
                    count = 4;
                    break;
                case GL_SPOT_DIRECTION:
                    count = 3;
                    break;
                case GL_SPOT_EXPONENT:
                case GL_SPOT_CUTOFF:
                case GL_CONSTANT_ATTENUATION:
                case GL_LINEAR_ATTENUATION:
                case GL_QUADRATIC_ATTENUATION:
                    count = 1;
                    break;
                default:
                    setGlError(GL_INVALID_ENUM, "glLightfv received an unsupported pname");
                    return true;
            }
        } else {
            if (first != GL_FRONT && first != GL_BACK && first != GL_FRONT_AND_BACK) {
                setGlError(GL_INVALID_ENUM, "glMaterialfv received an invalid face");
                return true;
            }
            switch (pname) {
                case GL_AMBIENT:
                case GL_DIFFUSE:
                case GL_SPECULAR:
                case GL_EMISSION:
                case GL_AMBIENT_AND_DIFFUSE:
                    count = 4;
                    break;
                case GL_SHININESS:
                    count = 1;
                    break;
                default:
                    setGlError(GL_INVALID_ENUM, "glMaterialfv received an unsupported pname");
                    return true;
            }
        }
        if (!source || !memory.permits(source, count * sizeof(float), Read))
            throw Error("GLES_ARGUMENT", "light/material values are not readable", source);
        float values[4] = {};
        memory.read(source, values, count * sizeof(float));
        if (name == "glLightfv") {
            Light& light = lights[first - GL_LIGHT0];
            switch (pname) {
                case GL_AMBIENT:
                    std::copy_n(values, 4, light.ambient.begin());
                    break;
                case GL_DIFFUSE:
                    std::copy_n(values, 4, light.diffuse.begin());
                    break;
                case GL_SPECULAR:
                    std::copy_n(values, 4, light.specular.begin());
                    break;
                case GL_POSITION:
                    transform4(modelview.data(), values, light.position.data());
                    break;
                case GL_SPOT_DIRECTION: {
                    const auto normalMatrix = inverseTranspose3x3(modelview.data());
                    const float x = normalMatrix[0] * values[0] + normalMatrix[3] * values[1] +
                                    normalMatrix[6] * values[2];
                    const float y = normalMatrix[1] * values[0] + normalMatrix[4] * values[1] +
                                    normalMatrix[7] * values[2];
                    const float z = normalMatrix[2] * values[0] + normalMatrix[5] * values[1] +
                                    normalMatrix[8] * values[2];
                    const float length = std::sqrt(x * x + y * y + z * z);
                    if (length > 0) {
                        light.spotDirection = {x / length, y / length, z / length, 0};
                    } else {
                        setGlError(GL_INVALID_VALUE, "spot direction cannot be a zero vector");
                    }
                    break;
                }
                case GL_SPOT_EXPONENT:
                case GL_SPOT_CUTOFF:
                case GL_CONSTANT_ATTENUATION:
                case GL_LINEAR_ATTENUATION:
                case GL_QUADRATIC_ATTENUATION: {
                    const float value = values[0];
                    if (pname == GL_SPOT_EXPONENT && (value < 0 || value > 128))
                        setGlError(GL_INVALID_VALUE, "light spot exponent is outside 0..128");
                    else if (pname == GL_SPOT_CUTOFF && value != 180 && (value < 0 || value > 90))
                        setGlError(GL_INVALID_VALUE, "light spot cutoff is outside 0..90 or 180");
                    else if ((pname == GL_CONSTANT_ATTENUATION || pname == GL_LINEAR_ATTENUATION ||
                              pname == GL_QUADRATIC_ATTENUATION) &&
                             value < 0)
                        setGlError(GL_INVALID_VALUE, "light attenuation cannot be negative");
                    else if (pname == GL_SPOT_EXPONENT)
                        light.spotExponent = value;
                    else if (pname == GL_SPOT_CUTOFF) {
                        light.spotCutoff = value;
                        light.spotCutoffCos =
                            value == 180 ? -1.0f : std::cos(value * DegreesToRadians);
                    } else if (pname == GL_CONSTANT_ATTENUATION)
                        light.constantAttenuation = value;
                    else if (pname == GL_LINEAR_ATTENUATION)
                        light.linearAttenuation = value;
                    else
                        light.quadraticAttenuation = value;
                    break;
                }
            }
        } else {
            const auto update = [&](Material& material) {
                switch (pname) {
                    case GL_AMBIENT:
                        std::copy_n(values, 4, material.ambient.begin());
                        break;
                    case GL_DIFFUSE:
                        std::copy_n(values, 4, material.diffuse.begin());
                        break;
                    case GL_SPECULAR:
                        std::copy_n(values, 4, material.specular.begin());
                        break;
                    case GL_EMISSION:
                        std::copy_n(values, 4, material.emission.begin());
                        break;
                    case GL_AMBIENT_AND_DIFFUSE:
                        std::copy_n(values, 4, material.ambient.begin());
                        std::copy_n(values, 4, material.diffuse.begin());
                        break;
                    case GL_SHININESS:
                        if (values[0] < 0 || values[0] > 128)
                            setGlError(GL_INVALID_VALUE, "material shininess is outside 0..128");
                        else
                            material.shininess = values[0];
                        break;
                }
            };
            if (first != GL_BACK) update(materials[0]);
            if (first != GL_FRONT) update(materials[1]);
        }
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
    const bool useColourArray = colour.enabled && (!lightingEnabled || colorMaterialEnabled);
    const uint32_t key = (useColourArray ? 1u : 0u) | (uint32_t(texSource) << 1) |
                         (replace ? 1u << 8 : 0u) | (lightingEnabled ? 1u << 9 : 0u);
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
    auto deleteShader = glFn<void (*)(uint32_t)>("glDeleteShader");
    auto deleteProgram = glFn<void (*)(uint32_t)>("glDeleteProgram");
    if (!createShader || !shaderSource || !compileShader || !getShaderiv || !createProgram ||
        !attachShader || !bindAttrib || !linkProgram || !getProgramiv || !uniformLocation) {
        setGlError(GL_INVALID_OPERATION,
                   "the driver has no OpenGL ES 2.0 shader support, so ES 1.1 fixed function "
                   "cannot be emulated over it");
        return none;
    }
    // A shader that fails to compile is reported with the driver's own info log: this runs on
    // devices nobody can attach a debugger to, so the message is the only evidence there is.
    auto shaderInfoLog = glFn<void (*)(uint32_t, int, int*, char*)>("glGetShaderInfoLog");
    auto programInfoLog = glFn<void (*)(uint32_t, int, int*, char*)>("glGetProgramInfoLog");
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
        if (shaderInfoLog) {
            char buffer[2048] = {};
            shaderInfoLog(shader, int(sizeof(buffer)), nullptr, buffer);
            detail += ": ";
            detail += buffer;
        }
        setGlError(GL_INVALID_OPERATION, detail);
        if (deleteShader) deleteShader(shader);
        return 0;
    };

    std::string vertexSource = "precision highp float;\nattribute vec4 aPosition;\n";
    if (useColourArray) vertexSource += "attribute vec4 aColor;\n";
    if (texSource == 1) vertexSource += "attribute vec2 aTexCoord;\n";
    if (lightingEnabled) vertexSource += "attribute vec3 aNormal;\n";
    vertexSource += "uniform mat4 uMvp;\nuniform vec4 uColor;\n";
    if (texSource) vertexSource += "uniform mat4 uTexMatrix;\n";
    if (lightingEnabled) {
        vertexSource +=
            "uniform mat4 uModelView;\nuniform mat3 uNormalMatrix;\n"
            "uniform vec4 uGlobalAmbient;\nuniform vec4 uMaterialAmbient[2];\n"
            "uniform vec4 uMaterialDiffuse[2];\nuniform vec4 uMaterialSpecular[2];\n"
            "uniform vec4 uMaterialEmission[2];\nuniform float uMaterialShininess[2];\n"
            "uniform vec4 uLightAmbient[8];\nuniform vec4 uLightDiffuse[8];\n"
            "uniform vec4 uLightSpecular[8];\nuniform vec4 uLightPosition[8];\n"
            "uniform vec4 uLightSpotDirection[8];\nuniform vec4 uLightParameters[8];\n"
            "uniform float uLightExponent[8];\nuniform int uColorMaterialEnabled;\n"
            "uniform int uColorMaterialFace;\nuniform int uColorMaterialMode;\n"
            "uniform int uLocalViewer;\nuniform int uTwoSided;\nuniform int uNormalizeNormals;\n"
            "varying vec4 vFrontColor;\nvarying vec4 vBackColor;\n";
    } else {
        vertexSource += "varying vec4 vColor;\n";
    }
    if (texSource) vertexSource += "varying vec2 vTexCoord;\n";
    vertexSource += "void main() {\n  gl_Position = uMvp * aPosition;\n";
    if (lightingEnabled) {
        vertexSource += "  vec4 baseColor = ";
        vertexSource += useColourArray ? "aColor;\n" : "uColor;\n";
        vertexSource +=
            "  vec4 eyePosition = uModelView * aPosition;\n"
            "  vec3 transformedNormal = uNormalMatrix * aNormal;\n";

        const auto appendSide = [&](int side, const char* suffix, uint32_t faceEnum,
                                    const char* normalExpression) {
            const std::string id(suffix);
            const std::string sideIndex = std::to_string(side);
            const std::string face = std::to_string(faceEnum);
            vertexSource += "  vec4 matAmbient" + id + " = uMaterialAmbient[" + sideIndex + "];\n";
            vertexSource += "  vec4 matDiffuse" + id + " = uMaterialDiffuse[" + sideIndex + "];\n";
            vertexSource +=
                "  vec4 matSpecular" + id + " = uMaterialSpecular[" + sideIndex + "];\n";
            vertexSource +=
                "  vec4 matEmission" + id + " = uMaterialEmission[" + sideIndex + "];\n";
            vertexSource +=
                "  float matShininess" + id + " = uMaterialShininess[" + sideIndex + "];\n";
            vertexSource += "  if (uColorMaterialEnabled != 0 && (uColorMaterialFace == " + face +
                            " || uColorMaterialFace == " + std::to_string(GL_FRONT_AND_BACK) +
                            ")) {\n";
            vertexSource += "    if (uColorMaterialMode == " + std::to_string(GL_AMBIENT) +
                            " || uColorMaterialMode == " + std::to_string(GL_AMBIENT_AND_DIFFUSE) +
                            ") matAmbient" + id + " = baseColor;\n";
            vertexSource += "    if (uColorMaterialMode == " + std::to_string(GL_DIFFUSE) +
                            " || uColorMaterialMode == " + std::to_string(GL_AMBIENT_AND_DIFFUSE) +
                            ") matDiffuse" + id + " = baseColor;\n";
            vertexSource += "    if (uColorMaterialMode == " + std::to_string(GL_SPECULAR) +
                            ") matSpecular" + id + " = baseColor;\n";
            vertexSource += "    if (uColorMaterialMode == " + std::to_string(GL_EMISSION) +
                            ") matEmission" + id + " = baseColor;\n  }\n";
            vertexSource += "  vec3 N" + id + " = " + normalExpression + ";\n";
            vertexSource += "  vec3 V" + id +
                            " = uLocalViewer != 0 ? normalize(-eyePosition.xyz) : "
                            "vec3(0.0, 0.0, 1.0);\n";
            vertexSource += "  vec4 lit" + id + " = matEmission" + id +
                            " + uGlobalAmbient * matAmbient" + id + ";\n";
            for (int index = 0; index < 8; ++index) {
                const std::string light = std::to_string(index);
                vertexSource += "  {\n    vec4 lp = uLightPosition[" + light +
                                "];\n"
                                "    vec3 delta = lp.xyz - eyePosition.xyz * lp.w;\n"
                                "    float distanceToLight = lp.w == 0.0 ? 0.0 : length(delta);\n"
                                "    vec3 L = lp.w == 0.0 ? normalize(lp.xyz) : "
                                "(distanceToLight > 0.0 ? delta / distanceToLight : "
                                "vec3(0.0, 0.0, 1.0));\n"
                                "    vec4 attenuationParams = uLightParameters[" +
                                light +
                                "];\n"
                                "    float attenuation = lp.w == 0.0 ? 1.0 : "
                                "1.0 / max(attenuationParams.x + attenuationParams.y * "
                                "distanceToLight + attenuationParams.z * distanceToLight * "
                                "distanceToLight, 0.000001);\n"
                                "    float spot = 1.0;\n"
                                "    if (attenuationParams.w > -0.99999) {\n"
                                "      float spotCos = dot(-L, normalize(uLightSpotDirection[" +
                                light +
                                "].xyz));\n"
                                "      spot = spotCos < attenuationParams.w ? 0.0 : "
                                "pow(max(spotCos, 0.0), uLightExponent[" +
                                light +
                                "]);\n"
                                "    }\n"
                                "    float diffuseTerm = max(dot(N" +
                                id +
                                ", L), 0.0);\n"
                                "    vec3 H = normalize(L + V" +
                                id +
                                ");\n"
                                "    float specularTerm = diffuseTerm > 0.0 && matShininess" +
                                id + " > 0.0 ? pow(max(dot(N" + id + ", H), 0.0), matShininess" +
                                id +
                                ") : 0.0;\n"
                                "    lit" +
                                id + " += attenuation * spot * (matAmbient" + id +
                                " * uLightAmbient[" + light + "] + matDiffuse" + id +
                                " * uLightDiffuse[" + light + "] * diffuseTerm + matSpecular" + id +
                                " * uLightSpecular[" + light + "] * specularTerm);\n  }\n";
            }
            vertexSource += "  v" + id + "Color = lit" + id + ";\n";
        };
        vertexSource +=
            "  vec3 lightNormal = uNormalizeNormals != 0 ? "
            "normalize(transformedNormal) : transformedNormal;\n";
        appendSide(0, "Front", GL_FRONT, "lightNormal");
        vertexSource += "  if (uTwoSided != 0) {\n";
        appendSide(1, "Back", GL_BACK, "-lightNormal");
        vertexSource += "  } else {\n    vBackColor = vFrontColor;\n  }\n";
    } else {
        vertexSource += "  vColor = ";
        vertexSource += useColourArray ? "aColor;\n" : "uColor;\n";
    }
    if (texSource == 1)
        vertexSource += "  vTexCoord = (uTexMatrix * vec4(aTexCoord, 0.0, 1.0)).st;\n";
    else if (texSource == 2)
        vertexSource += "  vTexCoord = (uTexMatrix * vec4(0.0, 0.0, 0.0, 1.0)).st;\n";
    vertexSource += "}\n";

    std::string fragmentSource = lightingEnabled
                                     ? "precision mediump float;\nvarying vec4 vFrontColor;\n"
                                       "varying vec4 vBackColor;\n"
                                     : "precision mediump float;\nvarying vec4 vColor;\n";
    if (texSource) {
        fragmentSource += "varying vec2 vTexCoord;\nuniform sampler2D uTexture;\n";
        if (lightingEnabled) {
            fragmentSource +=
                "void main() {\n  vec4 surfaceColor = "
                "gl_FrontFacing ? vFrontColor : vBackColor;\n"
                "  gl_FragColor = texture2D(uTexture, vTexCoord)";
            // ES 1.1 texture environment: replace ignores the incoming colour entirely, modulate
            // multiplies. Anything else is approximated by modulate and counted.
            fragmentSource += replace ? ";\n}\n" : " * surfaceColor;\n}\n";
        } else {
            fragmentSource += "void main() {\n  gl_FragColor = texture2D(uTexture, vTexCoord)";
            fragmentSource += replace ? ";\n}\n" : " * vColor;\n}\n";
        }
    } else if (lightingEnabled) {
        fragmentSource +=
            "void main() {\n  gl_FragColor = "
            "gl_FrontFacing ? vFrontColor : vBackColor;\n}\n";
    } else {
        fragmentSource += "void main() {\n  gl_FragColor = vColor;\n}\n";
    }

    const uint32_t vertexShader = compile(GL_VERTEX_SHADER, vertexSource);
    const uint32_t fragmentShader = vertexShader ? compile(GL_FRAGMENT_SHADER, fragmentSource) : 0;
    if (!vertexShader || !fragmentShader) {
        if (vertexShader && deleteShader) deleteShader(vertexShader);
        if (fragmentShader && deleteShader) deleteShader(fragmentShader);
        return none;
    }
    const uint32_t id = createProgram();
    if (!id) {
        if (deleteShader) {
            deleteShader(vertexShader);
            deleteShader(fragmentShader);
        }
        return none;
    }
    attachShader(id, vertexShader);
    attachShader(id, fragmentShader);
    // Bound by name before linking, so a draw never has to ask where an attribute landed.
    bindAttrib(id, AttributePosition, "aPosition");
    if (useColourArray) bindAttrib(id, AttributeColour, "aColor");
    if (texSource == 1) bindAttrib(id, AttributeTexCoord, "aTexCoord");
    if (lightingEnabled) bindAttrib(id, AttributeNormal, "aNormal");
    linkProgram(id);
    int linked = 0;
    getProgramiv(id, GL_LINK_STATUS, &linked);
    if (deleteShader) {
        deleteShader(vertexShader);
        deleteShader(fragmentShader);
    }
    if (!linked) {
        std::string detail = "a generated program did not link";
        if (programInfoLog) {
            char buffer[2048] = {};
            programInfoLog(id, int(sizeof(buffer)), nullptr, buffer);
            if (buffer[0]) detail += ": " + std::string(buffer);
        }
        setGlError(GL_INVALID_OPERATION, detail);
        if (deleteProgram) deleteProgram(id);
        return none;
    }
    Program program;
    program.id = id;
    program.mvp = uniformLocation(id, "uMvp");
    program.colour = uniformLocation(id, "uColor");
    program.textureMatrix = uniformLocation(id, "uTexMatrix");
    program.texture = uniformLocation(id, "uTexture");
    if (lightingEnabled) {
        program.modelview = uniformLocation(id, "uModelView");
        program.normalMatrix = uniformLocation(id, "uNormalMatrix");
        program.globalAmbient = uniformLocation(id, "uGlobalAmbient");
        program.materialAmbient = uniformLocation(id, "uMaterialAmbient[0]");
        program.materialDiffuse = uniformLocation(id, "uMaterialDiffuse[0]");
        program.materialSpecular = uniformLocation(id, "uMaterialSpecular[0]");
        program.materialEmission = uniformLocation(id, "uMaterialEmission[0]");
        program.materialShininess = uniformLocation(id, "uMaterialShininess[0]");
        program.colorMaterialEnabled = uniformLocation(id, "uColorMaterialEnabled");
        program.colorMaterialFace = uniformLocation(id, "uColorMaterialFace");
        program.colorMaterialMode = uniformLocation(id, "uColorMaterialMode");
        program.localViewer = uniformLocation(id, "uLocalViewer");
        program.twoSided = uniformLocation(id, "uTwoSided");
        program.normalizeNormals = uniformLocation(id, "uNormalizeNormals");
        program.lightAmbient = uniformLocation(id, "uLightAmbient[0]");
        program.lightDiffuse = uniformLocation(id, "uLightDiffuse[0]");
        program.lightSpecular = uniformLocation(id, "uLightSpecular[0]");
        program.lightPosition = uniformLocation(id, "uLightPosition[0]");
        program.lightSpotDirection = uniformLocation(id, "uLightSpotDirection[0]");
        program.lightParameters = uniformLocation(id, "uLightParameters[0]");
        program.lightExponent = uniformLocation(id, "uLightExponent[0]");
    }
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
    if (array.buffered && array.buffer) {
        // The buffer binding is captured when glVertexPointer is called, not whatever
        // GL_ARRAY_BUFFER happens to be bound later.
        bindBuffer(GL_ARRAY_BUFFER, array.buffer);
        attrib(location, array.size, array.type, normalise ? 1u : 0u, array.stride,
               reinterpret_cast<const void*>(uintptr_t(array.pointer)));
        enableAttrib(location);
        return true;
    }
    const size_t element = typeSize(array.type);
    if (!element || !count || array.size < 1 || array.size > 4) return false;
    const size_t components = size_t(array.size);
    const size_t packed = components * element;
    if (array.stride < 0) return false;
    const size_t stride = array.stride ? size_t(array.stride) : packed;
    if (stride < packed || (count > 1 && stride > (256 * 1024 * 1024 - packed) / (count - 1)))
        return false;
    const size_t bytes = stride * (count - 1) + packed;
    if (!array.pointer) throw Error("GLES_ARRAY", "enabled client array points to null");
    std::vector<uint8_t> copy(bytes, 0);
    memory.read(array.pointer, copy.data(), bytes);
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
    auto uniformMatrix3 = glFn<void (*)(int, int, uint32_t, const float*)>("glUniformMatrix3fv");
    auto uniform4f = glFn<void (*)(int, float, float, float, float)>("glUniform4f");
    auto uniform4fv = glFn<void (*)(int, int, const float*)>("glUniform4fv");
    auto uniform1fv = glFn<void (*)(int, int, const float*)>("glUniform1fv");
    auto uniform1i = glFn<void (*)(int, int)>("glUniform1i");
    auto vertexAttrib3f = glFn<void (*)(uint32_t, float, float, float)>("glVertexAttrib3f");
    auto disableAttrib = glFn<void (*)(uint32_t)>("glDisableVertexAttribArray");
    auto bindBuffer = glFn<void (*)(uint32_t, uint32_t)>("glBindBuffer");
    if (!uniformMatrix || !uniform4f || !disableAttrib || !bindBuffer) return false;
    if (lightingEnabled &&
        (!uniformMatrix3 || !uniform4fv || !uniform1fv || !uniform1i || !vertexAttrib3f))
        return false;
    struct RestoreBufferBindings {
        decltype(bindBuffer) bind;
        uint32_t array;
        uint32_t element;
        ~RestoreBufferBindings() {
            bind(GL_ARRAY_BUFFER, array);
            bind(GL_ELEMENT_ARRAY_BUFFER, element);
        }
    } restore{bindBuffer, boundArrayBuffer, boundElementBuffer};

    float mvp[16];
    multiplyMatrices(projection.data(), modelview.data(), mvp);
    if (program.mvp >= 0) uniformMatrix(program.mvp, 1, 0, mvp);
    if (program.colour >= 0)
        uniform4f(program.colour, currentColour[0], currentColour[1], currentColour[2],
                  currentColour[3]);
    if (program.textureMatrix >= 0)
        uniformMatrix(program.textureMatrix, 1, 0, textureMatrixForUnit(activeTextureUnit).data());
    if (program.texture >= 0 && uniform1i) uniform1i(program.texture, activeTextureUnit);
    if (lightingEnabled) {
        const auto normal = inverseTranspose3x3(modelview.data());
        if (program.modelview >= 0) uniformMatrix(program.modelview, 1, 0, modelview.data());
        if (program.normalMatrix >= 0) uniformMatrix3(program.normalMatrix, 1, 0, normal.data());
        if (program.globalAmbient >= 0) uniform4fv(program.globalAmbient, 1, globalAmbient.data());
        const auto uploadMaterials = [&](int location, auto select) {
            if (location < 0) return;
            float values[8];
            for (size_t side = 0; side < materials.size(); ++side)
                std::copy_n(select(materials[side]).data(), 4, values + side * 4);
            uniform4fv(location, 2, values);
        };
        uploadMaterials(program.materialAmbient,
                        [](const Material& material) -> const auto& { return material.ambient; });
        uploadMaterials(program.materialDiffuse,
                        [](const Material& material) -> const auto& { return material.diffuse; });
        uploadMaterials(program.materialSpecular,
                        [](const Material& material) -> const auto& { return material.specular; });
        uploadMaterials(program.materialEmission,
                        [](const Material& material) -> const auto& { return material.emission; });
        if (program.materialShininess >= 0) {
            const float values[] = {materials[0].shininess, materials[1].shininess};
            uniform1fv(program.materialShininess, 2, values);
        }
        if (program.colorMaterialEnabled >= 0)
            uniform1i(program.colorMaterialEnabled, colorMaterialEnabled ? 1 : 0);
        if (program.colorMaterialFace >= 0)
            uniform1i(program.colorMaterialFace, int(colorMaterialFace));
        if (program.colorMaterialMode >= 0)
            uniform1i(program.colorMaterialMode, int(colorMaterialMode));
        if (program.localViewer >= 0) uniform1i(program.localViewer, localViewer ? 1 : 0);
        if (program.twoSided >= 0) uniform1i(program.twoSided, twoSidedLighting ? 1 : 0);
        if (program.normalizeNormals >= 0)
            uniform1i(program.normalizeNormals, normalizeNormals ? 1 : 0);

        float ambient[32] = {}, diffuse[32] = {}, specular[32] = {};
        float positions[32] = {}, spotDirections[32] = {}, parameters[32] = {};
        float exponents[8] = {};
        for (size_t index = 0; index < lights.size(); ++index) {
            const Light& light = lights[index];
            const size_t offset = index * 4;
            if (light.enabled) {
                std::copy(light.ambient.begin(), light.ambient.end(), ambient + offset);
                std::copy(light.diffuse.begin(), light.diffuse.end(), diffuse + offset);
                std::copy(light.specular.begin(), light.specular.end(), specular + offset);
            }
            std::copy(light.position.begin(), light.position.end(), positions + offset);
            std::copy(light.spotDirection.begin(), light.spotDirection.end(),
                      spotDirections + offset);
            parameters[offset] = light.constantAttenuation;
            parameters[offset + 1] = light.linearAttenuation;
            parameters[offset + 2] = light.quadraticAttenuation;
            parameters[offset + 3] = light.spotCutoffCos;
            exponents[index] = light.spotExponent;
        }
        if (program.lightAmbient >= 0) uniform4fv(program.lightAmbient, 8, ambient);
        if (program.lightDiffuse >= 0) uniform4fv(program.lightDiffuse, 8, diffuse);
        if (program.lightSpecular >= 0) uniform4fv(program.lightSpecular, 8, specular);
        if (program.lightPosition >= 0) uniform4fv(program.lightPosition, 8, positions);
        if (program.lightSpotDirection >= 0)
            uniform4fv(program.lightSpotDirection, 8, spotDirections);
        if (program.lightParameters >= 0) uniform4fv(program.lightParameters, 8, parameters);
        if (program.lightExponent >= 0) uniform1fv(program.lightExponent, 8, exponents);
    }
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
            vertices = 0;
            const auto buffer = elementBufferData.find(boundElementBuffer);
            if (buffer != elementBufferData.end()) {
                const size_t offset = size_t(indices), bytes = size_t(count) * element;
                if (offset > buffer->second.size() || bytes > buffer->second.size() - offset)
                    throw Error("GLES_ARRAY", "element buffer range is outside captured data");
                indexCopy.assign(buffer->second.begin() + offset,
                                 buffer->second.begin() + offset + bytes);
                vertices = largestIndex(indexCopy, element) + 1;
                if (vertices > VertexBudget) {
                    setError("an index reaches more vertices than the copy budget");
                    return false;
                }
            }
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
    if (indexType && boundElementBuffer && !vertices) {
        const auto needsIndexExtent = [](const ClientArray& array) {
            return array.enabled && !(array.buffered && array.buffer);
        };
        const int textureUnitEnum = int(GL_TEXTURE0) + activeTextureUnit;
        const auto texArray = texCoord.find(textureUnitEnum);
        if (needsIndexExtent(vertex) || (colour.enabled && needsIndexExtent(colour)) ||
            (lightingEnabled && needsIndexExtent(normal)) ||
            (textureEnabledForUnit(activeTextureUnit) && texArray != texCoord.end() &&
             needsIndexExtent(texArray->second)))
            throw Error("GLES_ARRAY",
                        "client arrays cannot be bounded without captured element data");
    }
    if (!vertex.enabled) {
        setError("a draw was issued with no vertex array enabled");
        return false;
    }
    if (!bindFixedFunctionArray(vertex, AttributePosition, streamBuffers[0], false,
                                vertices ? vertices : 1))
        return false;
    const bool useColourArray = colour.enabled && (!lightingEnabled || colorMaterialEnabled);
    if (useColourArray && !bindFixedFunctionArray(colour, AttributeColour, streamBuffers[1], true,
                                                  vertices ? vertices : 1))
        return false;
    if (lightingEnabled) {
        if (normal.enabled) {
            const bool normalized = normal.type == GL_BYTE || normal.type == GL_SHORT;
            if (!bindFixedFunctionArray(normal, AttributeNormal, streamBuffers[3], normalized,
                                        vertices ? vertices : 1))
                return false;
        } else {
            disableAttrib(AttributeNormal);
            vertexAttrib3f(AttributeNormal, currentNormal[0], currentNormal[1], currentNormal[2]);
        }
    }
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
    if (!useColourArray) disableAttrib(AttributeColour);
    if (!(textureEnabled && texCoordEnabled)) disableAttrib(AttributeTexCoord);
    if (!lightingEnabled) disableAttrib(AttributeNormal);

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
