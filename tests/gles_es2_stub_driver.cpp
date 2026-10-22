// A recording OpenGL ES 2.0 driver with no ES 1.1 frontend, used by gles_emulation_test.
//
// gles_stub_driver speaks ES 1.1, so with it the bridge forwards everything and the
// fixed-function emulation never runs. This double is the opposite on purpose: it exports
// every ES 2.0 name the emulation needs and *none* of the ES 1.1 ones, which is exactly the
// shape of ANGLE and of most modern mobile drivers. A guest written for ES 1.1 against it has
// no glVertexPointer to call, so the runtime must translate. What this file records is the
// translation: the shader pair it was given, the attribute bindings, the model-view-projection
// matrix, the vertex bytes lifted out of guest memory, and the draws that came out the other
// side. No rasterisation, and nothing here is linked into the app.
#include "gles_es2_stub_driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using podnative::testing::Es2StubState;
Es2StubState state;

// Named rather than hex, so a failure reads as GL and not as a number to decode.
constexpr uint32_t GL_VERTEX_SHADER = 0x8b31;
constexpr uint32_t GL_FRAGMENT_SHADER = 0x8b30;
constexpr uint32_t GL_COMPILE_STATUS = 0x8b81;
constexpr uint32_t GL_LINK_STATUS = 0x8b82;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892;
constexpr uint32_t GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t GL_FLOAT = 0x1406;
constexpr uint32_t GL_MAX_TEXTURE_SIZE = 0x0d33;
constexpr uint32_t GL_MAX_VIEWPORT_DIMS = 0x0d3a;
constexpr uint32_t GL_MAX_VERTEX_ATTRIBS = 0x8869;
constexpr uint32_t GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
constexpr uint32_t GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8b4d;
constexpr uint32_t GL_MAX_RENDERBUFFER_SIZE = 0x84e8;
constexpr uint32_t GL_VERSION = 0x1f02;
constexpr uint32_t GL_SHADING_LANGUAGE_VERSION = 0x8b8c;

unsigned nextObject = 1;
// Object ids are handed out in one counter, so a shader's type and a uniform's name have to be
// remembered against the id the bridge was given; that is what these two small tables are for.
unsigned shaderTypes[32] = {};
char uniformNames[16][32] = {};
int uniformCount = 0;

void recordSource(unsigned shader, const char* const* strings, const int* lengths, int count) {
    char* destination = nullptr;
    for (unsigned index = 0; index < 32; ++index) {
        if (shaderTypes[index] == 0) continue;
        if (shaderTypes[index] >> 8 != shader) continue;
        // Only the low byte of the type was kept, so only the low byte is compared.
        destination = (shaderTypes[index] & 0xff) == (GL_VERTEX_SHADER & 0xff)
                          ? state.vertexSource
                          : state.fragmentSource;
    }
    if (!destination) return;
    size_t used = 0;
    for (int index = 0; index < count && strings && strings[index]; ++index) {
        const size_t length =
            lengths && lengths[index] > 0 ? size_t(lengths[index]) : std::strlen(strings[index]);
        const size_t copy = std::min(length, sizeof(state.vertexSource) - 1 - used);
        std::memcpy(destination + used, strings[index], copy);
        used += copy;
        destination[used] = '\0';
    }
}

int uniformLocation(const char* name) {
    for (int index = 0; index < uniformCount; ++index)
        if (std::strncmp(uniformNames[index], name, sizeof(uniformNames[index]) - 1) == 0)
            return index;
    if (uniformCount >= 16) return -1;
    std::snprintf(uniformNames[uniformCount], sizeof(uniformNames[0]), "%s", name ? name : "");
    return uniformCount++;
}

void recordDraw(uint32_t mode, int first, int count, uint32_t type, uint32_t indices) {
    if (state.draws < int(sizeof(state.drawLog) / sizeof(state.drawLog[0]))) {
        auto& draw = state.drawLog[state.draws];
        draw.mode = mode;
        draw.first = first;
        draw.count = count;
        draw.type = type;
        draw.indices = indices;
    }
    ++state.draws;
    if (type == 0)
        ++state.drawArraysCalls;
    else
        ++state.drawElementsCalls;
}

// --- EGL: a single ES 2.0 context, which is what the emulation has to ask for ---
uintptr_t displayToken = 1;
uintptr_t contextToken = 0;
uintptr_t currentContext = 0;

}  // namespace

extern "C" podnative::testing::Es2StubState* es2_stub_state() { return &state; }

extern "C" void es2_stub_reset() {
    state = podnative::testing::Es2StubState();
    nextObject = 1;
    uniformCount = 0;
    std::memset(shaderTypes, 0, sizeof(shaderTypes));
    std::memset(uniformNames, 0, sizeof(uniformNames));
    displayToken = 1;
    contextToken = 0;
    currentContext = 0;
}

extern "C" {

void* eglGetDisplay(void*) { return &displayToken; }
uint32_t eglInitialize(void*, int* major, int* minor) {
    *major = 1;
    *minor = 4;
    return 1;
}
uint32_t eglBindAPI(uint32_t) { return 1; }
uint32_t eglChooseConfig(void*, const int*, void** configs, int count, int* found) {
    static int config = 1;
    if (count > 0 && configs) configs[0] = &config;
    *found = 1;
    return 1;
}
void* eglCreateContext(void*, void*, void*, const int* attributes) {
    ++state.createContextCalls;
    for (int i = 0; attributes && attributes[i] != 0x3038; i += 2)
        if (attributes[i] == 0x3098) state.contextVersion = uint32_t(attributes[i + 1]);
    return ++contextToken ? &contextToken : nullptr;
}
void* eglGetCurrentContext() { return currentContext ? &contextToken : nullptr; }
uint32_t eglMakeCurrent(void*, void* draw, void*, void* context) {
    if (draw) ++state.makeCurrentCalls;
    currentContext = context ? 1 : 0;
    return 1;
}
void* eglCreatePbufferSurface(void*, void*, const int*) { return &contextToken; }
uint32_t eglDestroyContext(void*, void*) { return 1; }
uint32_t eglSwapBuffers(void*, void*) { return 1; }
uint32_t eglSwapInterval(void*, int) { return 1; }
uint32_t eglGetError() { return 0x3000; }
void* eglGetProcAddress(const char*) { return nullptr; }

// --- the shader pair the emulation generates ---
uint32_t glCreateShader(uint32_t type) {
    const unsigned object = nextObject++;
    for (unsigned index = 0; index < 32; ++index)
        if (shaderTypes[index] == 0) {
            // The id is kept in the high bits so recordSource can find the type again.
            shaderTypes[index] = (object << 8) | (type & 0xff);
            break;
        }
    return object;
}
void glShaderSource(uint32_t shader, int count, const char* const* strings, const int* lengths) {
    recordSource(shader, strings, lengths, count);
}
void glCompileShader(uint32_t) { ++state.compileCalls; }
void glGetShaderiv(uint32_t, uint32_t pname, int* value) {
    *value = (pname == GL_COMPILE_STATUS && state.forceCompileFailure) ? 0 : 1;
}
void glGetShaderInfoLog(uint32_t, int size, int* written, char* log) {
    const char* message = state.forceCompileFailure ? "forced compile failure" : "";
    if (written) *written = int(std::strlen(message));
    if (log && size > 0) std::snprintf(log, size_t(size), "%s", message);
}
void glDeleteShader(uint32_t) {}
uint32_t glCreateProgram() { return nextObject++; }
void glAttachShader(uint32_t, uint32_t) {}
void glBindAttribLocation(uint32_t, uint32_t index, const char* name) {
    if (state.attribBindings < 4) {
        std::snprintf(state.attribName[state.attribBindings], sizeof(state.attribName[0]), "%s",
                      name ? name : "");
        state.attribIndex[state.attribBindings] = int(index);
    }
    ++state.attribBindings;
}
void glLinkProgram(uint32_t) { ++state.linkCalls; }
void glGetProgramiv(uint32_t, uint32_t pname, int* value) {
    *value = (pname == GL_LINK_STATUS && state.forceCompileFailure) ? 0 : 1;
}
void glGetProgramInfoLog(uint32_t, int size, int* written, char* log) {
    if (written) *written = 0;
    if (log && size > 0) log[0] = '\0';
}
void glDeleteProgram(uint32_t) {}
void glUseProgram(uint32_t) { ++state.useProgramCalls; }
int glGetUniformLocation(uint32_t, const char* name) { return uniformLocation(name); }

void glUniformMatrix4fv(int location, int count, uint32_t transpose, const float* value) {
    if (location < 0 || location >= uniformCount || !value || count < 1) return;
    const char* name = uniformNames[location];
    float* destination = nullptr;
    int* uploads = nullptr;
    if (std::strcmp(name, "uMvp") == 0) {
        destination = state.mvp;
        uploads = &state.mvpUploads;
    } else if (std::strcmp(name, "uTexMatrix") == 0) {
        destination = state.textureMatrix;
        uploads = &state.textureMatrixUploads;
    }
    if (!destination) return;
    if (transpose) state.errors++, state.lastError = 0x0500;  // ES 2.0 rejects transpose
    std::memcpy(destination, value, sizeof(float) * 16);
    ++*uploads;
}
void glUniform4f(int location, float r, float g, float b, float a) {
    if (location < 0 || location >= uniformCount) return;
    if (std::strcmp(uniformNames[location], "uColor") != 0) return;
    state.colour[0] = r;
    state.colour[1] = g;
    state.colour[2] = b;
    state.colour[3] = a;
    ++state.colourUploads;
}
void glUniform1i(int location, int value) {
    if (location < 0 || location >= uniformCount) return;
    if (std::strcmp(uniformNames[location], "uTexture") != 0) return;
    state.samplerUnit = value;
    ++state.samplerUploads;
}

// --- client arrays, lifted into vertex buffers ---
void glGenBuffers(int count, uint32_t* names) {
    for (int index = 0; index < count; ++index) names[index] = nextObject++;
}
void glDeleteBuffers(int, const uint32_t*) {}
void glBindBuffer(uint32_t, uint32_t) {}
void glBufferData(uint32_t target, size_t bytes, const void* data, uint32_t) {
    if (state.bufferUploads < int(sizeof(state.uploadTargets) / sizeof(state.uploadTargets[0]))) {
        state.uploadTargets[state.bufferUploads] = target;
        state.uploadBytes[state.bufferUploads] = unsigned(bytes);
    }
    ++state.bufferUploads;
    state.lastUploadTarget = target;
    state.lastUploadBytes = unsigned(bytes);
    state.lastUploadVertex[0] = state.lastUploadVertex[1] = state.lastUploadVertex[2] =
        state.lastUploadVertex[3] = 0.0f;
    // The first vertex is what proves the stride, the component count and the GL_FIXED
    // widening were worked out from the guest's own pointer rather than guessed.
    if (target == GL_ARRAY_BUFFER && data && bytes >= 4 * sizeof(float))
        std::memcpy(state.lastUploadVertex, data, sizeof(state.lastUploadVertex));
}
void glVertexAttribPointer(uint32_t index, int size, uint32_t type, uint32_t normalized, int stride,
                           const void* pointer) {
    if (state.attribPointerCalls < int(sizeof(state.pointers) / sizeof(state.pointers[0]))) {
        auto& recorded = state.pointers[state.attribPointerCalls];
        recorded.index = index;
        recorded.size = size;
        recorded.type = type;
        recorded.normalized = int(normalized);
        recorded.stride = stride;
        recorded.offset = unsigned(uintptr_t(pointer));
    }
    ++state.attribPointerCalls;
}
void glEnableVertexAttribArray(uint32_t) { ++state.enabledAttribs; }
void glDisableVertexAttribArray(uint32_t) { ++state.disabledAttribs; }

// --- draws ---
void glDrawArrays(uint32_t mode, int first, int count) { recordDraw(mode, first, count, 0, 0); }
void glDrawElements(uint32_t mode, int count, uint32_t type, const void* indices) {
    recordDraw(mode, 0, count, type, unsigned(uintptr_t(indices)));
}

// --- textures, state and queries ---
void glActiveTexture(uint32_t texture) {
    ++state.activeTextureCalls;
    state.lastActiveTexture = texture;
}
void glGenTextures(int count, uint32_t* names) {
    for (int index = 0; index < count; ++index) names[index] = nextObject++;
}
void glDeleteTextures(int, const uint32_t*) {}
void glBindTexture(uint32_t, uint32_t) {}
void glTexParameteri(uint32_t, uint32_t, int) {}
void glPixelStorei(uint32_t, int) {}
void glTexImage2D(uint32_t, int, uint32_t, int, int, int, uint32_t, uint32_t, const void*) {
    ++state.textureUploads;
}
void glTexSubImage2D(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void*) {
    ++state.textureUploads;
}
void glCompressedTexImage2D(uint32_t, int, uint32_t, int, int, int, int, const void*) {
    ++state.textureUploads;
}
void glEnable(uint32_t) {}
void glDisable(uint32_t) {}
void glBlendFunc(uint32_t, uint32_t) {}
void glClearColor(float, float, float, float) {}
void glClear(uint32_t) {}
void glDepthFunc(uint32_t) {}
void glDepthMask(uint8_t) {}
void glFrontFace(uint32_t) {}
void glLineWidth(float) {}
void glViewport(int, int, int, int) {}
void glScissor(int, int, int, int) {}
void glFlush() {}
void glFinish() {}
void glGetIntegerv(uint32_t pname, int32_t* values) {
    if (!values) return;
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE:
        case GL_MAX_RENDERBUFFER_SIZE:
            values[0] = 4096;
            break;
        case GL_MAX_VIEWPORT_DIMS:
            values[0] = 4096;
            values[1] = 4096;
            break;
        case GL_MAX_VERTEX_ATTRIBS:
            values[0] = 16;
            break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
            values[0] = 8;
            break;
        default:
            values[0] = 0;
            break;
    }
}
const char* glGetString(uint32_t name) {
    switch (name) {
        case GL_VERSION:
            return "OpenGL ES 2.0 (AnyiOS ES2 recording double)";
        case GL_SHADING_LANGUAGE_VERSION:
            return "OpenGL ES GLSL ES 1.00";
        default:
            return "AnyiOS";
    }
}
uint32_t glGetError() {
    const uint32_t error = state.lastError;
    state.lastError = 0;
    return error;
}

// --- framebuffer objects, core spellings only: no OES suffixes, as on a real ES 2.0 driver ---
void glGenFramebuffers(int count, uint32_t* names) {
    for (int index = 0; index < count; ++index) names[index] = nextObject++;
}
void glBindFramebuffer(uint32_t, uint32_t) {}
void glDeleteFramebuffers(int, const uint32_t*) {}
void glFramebufferTexture2D(uint32_t, uint32_t, uint32_t, uint32_t, int) {}
void glFramebufferRenderbuffer(uint32_t, uint32_t, uint32_t, uint32_t) {}
uint32_t glCheckFramebufferStatus(uint32_t) { return 0x8cd5; }
void glGenRenderbuffers(int count, uint32_t* names) {
    for (int index = 0; index < count; ++index) names[index] = nextObject++;
}
void glBindRenderbuffer(uint32_t, uint32_t) {}
void glDeleteRenderbuffers(int, const uint32_t*) {}
void glRenderbufferStorage(uint32_t, uint32_t, int, int) {}
void glGetRenderbufferParameteriv(uint32_t, uint32_t pname, int32_t* values) {
    if (!values) return;
    // GL_RENDERBUFFER_WIDTH/HEIGHT: a present is only meaningful with a size behind it.
    values[0] = (pname == 0x8cd4 || pname == 0x8cd3) ? 320 : 0;
}
void glReadPixels(int, int, int, int, uint32_t, uint32_t, void* pixels) {
    if (pixels) std::memset(pixels, 0, 4);
}

}  // extern "C"
