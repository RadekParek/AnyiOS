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
constexpr uint32_t GL_COMPILE_STATUS = 0x8b81;
constexpr uint32_t GL_LINK_STATUS = 0x8b82;
constexpr uint32_t GL_ACTIVE_UNIFORMS = 0x8b86;
constexpr uint32_t GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8b87;
constexpr uint32_t GL_FLOAT_VEC2 = 0x8b50;
constexpr uint32_t GL_FLOAT_VEC4 = 0x8b52;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892;
constexpr uint32_t GL_MAX_TEXTURE_SIZE = 0x0d33;
constexpr uint32_t GL_MAX_VIEWPORT_DIMS = 0x0d3a;
constexpr uint32_t GL_MAX_VERTEX_ATTRIBS = 0x8869;
constexpr uint32_t GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
constexpr uint32_t GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8b4d;
constexpr uint32_t GL_MAX_RENDERBUFFER_SIZE = 0x84e8;
constexpr uint32_t GL_UNPACK_ALIGNMENT = 0x0cf5;
constexpr uint32_t GL_PACK_ALIGNMENT = 0x0d05;
constexpr uint32_t GL_RGB = 0x1907;
constexpr uint32_t GL_RGBA = 0x1908;
constexpr uint32_t GL_UNSIGNED_BYTE = 0x1401;
constexpr uint32_t GL_VIEWPORT = 0x0ba2;
constexpr uint32_t GL_EXTENSIONS = 0x1f03;
constexpr uint32_t GL_VERSION = 0x1f02;
constexpr uint32_t GL_SHADING_LANGUAGE_VERSION = 0x8b8c;

unsigned nextObject = 1;
// Object ids are handed out in one counter, so a shader's type and a uniform's name have to be
// remembered against the id the bridge was given; that is what these two small tables are for.
unsigned shaderTypes[32] = {};
char uniformNames[64][32] = {};
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
    if (uniformCount >= 64) return -1;
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

extern "C" void es2_stub_set_error(unsigned error) { state.lastError = error; }

extern "C" {

void* eglGetDisplay(void*) { return &displayToken; }
void* eglGetPlatformDisplayEXT(uint32_t, void*, const int*) { return &displayToken; }
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
uint32_t eglDestroySurface(void*, void*) { return 1; }
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
    ++state.shaderSourceCalls;
    recordSource(shader, strings, lengths, count);
}
void glCompileShader(uint32_t) { ++state.compileCalls; }
void glGetShaderiv(uint32_t, uint32_t pname, int* value) {
    ++state.getShaderCalls;
    *value = (pname == GL_COMPILE_STATUS && state.forceCompileFailure) ? 0 : 1;
}
void glGetShaderInfoLog(uint32_t, int size, int* written, char* log) {
    ++state.shaderInfoLogCalls;
    const char* message = state.forceCompileFailure ? "forced compile failure" : "";
    if (written) *written = int(std::strlen(message));
    if (log && size > 0) std::snprintf(log, size_t(size), "%s", message);
}
void glGetShaderSource(uint32_t shader, int capacity, int* length, char* source) {
    const char* text = "";
    for (unsigned index = 0; index < 32; ++index) {
        if ((shaderTypes[index] >> 8) != shader || !shaderTypes[index]) continue;
        text = (shaderTypes[index] & 0xff) == (GL_VERTEX_SHADER & 0xff) ? state.vertexSource
                                                                        : state.fragmentSource;
        break;
    }
    const size_t chars = std::strlen(text);
    const size_t copied = capacity > 0 ? std::min(chars, size_t(capacity - 1)) : 0;
    if (length) *length = int(copied);
    if (source && capacity > 0) {
        std::memcpy(source, text, copied);
        source[copied] = '\0';
    }
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
    ++state.getProgramCalls;
    if (pname == GL_ACTIVE_UNIFORMS)
        *value = 1;
    else if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH)
        *value = 16;
    else
        *value = (pname == GL_LINK_STATUS && state.forceCompileFailure) ? 0 : 1;
}
void glGetActiveAttrib(uint32_t, uint32_t index, int capacity, int* length, int* size,
                       uint32_t* type, char* name) {
    ++state.activeAttribQueries;
    const char* text = index == 0 ? "aPosition" : "";
    const int chars = int(std::strlen(text));
    if (length) *length = chars;
    if (size) *size = index == 0 ? 1 : 0;
    if (type) *type = index == 0 ? GL_FLOAT_VEC4 : 0;
    if (name && capacity > 0) std::snprintf(name, size_t(capacity), "%s", text);
}
void glGetActiveUniform(uint32_t, uint32_t index, int capacity, int* length, int* size,
                        uint32_t* type, char* name) {
    ++state.activeUniformQueries;
    const char* text = index == 0 ? "uVec[0]" : "";
    const int chars = int(std::strlen(text));
    if (length) *length = chars;
    if (size) *size = index == 0 ? 2 : 0;
    if (type) *type = index == 0 ? GL_FLOAT_VEC2 : 0;
    if (name && capacity > 0) std::snprintf(name, size_t(capacity), "%s", text);
}
void glGetAttachedShaders(uint32_t, int capacity, int* count, uint32_t* shaders) {
    ++state.attachedShaderQueries;
    const int copied = std::min(capacity, 2);
    if (count) *count = copied;
    if (shaders)
        for (int index = 0; index < copied; ++index) shaders[index] = uint32_t(100 + index);
}
void glGetShaderPrecisionFormat(uint32_t, uint32_t, int* range, int* precision) {
    ++state.precisionQueries;
    if (range) {
        range[0] = -126;
        range[1] = 127;
    }
    if (precision) *precision = 23;
}
void glGetProgramInfoLog(uint32_t, int size, int* written, char* log) {
    ++state.programInfoLogCalls;
    if (written) *written = 0;
    if (log && size > 0) log[0] = '\0';
}
void glDeleteProgram(uint32_t) {}
void glUseProgram(uint32_t) { ++state.useProgramCalls; }
int glGetAttribLocation(uint32_t, const char* name) {
    ++state.attribLocationCalls;
    state.lastAttribLocation = name && std::strcmp(name, "aPosition") == 0 ? 5 : -1;
    return state.lastAttribLocation;
}
int glGetUniformLocation(uint32_t, const char* name) {
    if (!name) return -1;
    // A real linker returns -1 for uniforms optimised out of a shader. Keep that distinction:
    // otherwise a plain untextured program appears to upload texture state it never reads.
    if (std::strcmp(name, "uColor") == 0 &&
        std::strstr(state.vertexSource, "vColor = uColor;") == nullptr &&
        std::strstr(state.vertexSource, "baseColor = uColor;") == nullptr)
        return -1;
    if (std::strcmp(name, "uTexMatrix") == 0 &&
        std::strstr(state.vertexSource, "uTexMatrix") == nullptr)
        return -1;
    if (std::strcmp(name, "uTexture") == 0 &&
        std::strstr(state.fragmentSource, "uTexture") == nullptr)
        return -1;
    return uniformLocation(name);
}
void glGetUniformfv(uint32_t, int, float* values) {
    ++state.uniformQueries;
    if (values) {
        values[0] = 1.5f;
        values[1] = 2.5f;
    }
}
void glGetUniformiv(uint32_t, int, int32_t* values) {
    ++state.uniformQueries;
    if (values) {
        values[0] = 5;
        values[1] = 6;
    }
}

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
void recordScalarUniform(int components, int integer, const float* values) {
    ++state.scalarUniformCalls;
    state.scalarUniformComponents = components;
    state.scalarUniformInteger = integer;
    for (int index = 0; index < components; ++index) state.scalarUniformValues[index] = values[index];
}
void glUniform1f(int, float x) { recordScalarUniform(1, 0, &x); }
void glUniform2f(int, float x, float y) {
    const float values[] = {x, y};
    recordScalarUniform(2, 0, values);
}
void glUniform3f(int, float x, float y, float z) {
    const float values[] = {x, y, z};
    recordScalarUniform(3, 0, values);
}
void glUniform2i(int, int x, int y) {
    const float values[] = {float(x), float(y)};
    recordScalarUniform(2, 1, values);
}
void glUniform3i(int, int x, int y, int z) {
    const float values[] = {float(x), float(y), float(z)};
    recordScalarUniform(3, 1, values);
}
void glUniform4i(int, int x, int y, int z, int w) {
    const float values[] = {float(x), float(y), float(z), float(w)};
    recordScalarUniform(4, 1, values);
}
void recordFloatVector(int components, int count, const float* values) {
    ++state.vectorUniformCalls;
    state.vectorUniformComponents = components;
    state.vectorUniformInteger = 0;
    state.vectorUniformCount = count;
    const size_t elements = count > 0 ? size_t(count) * size_t(components) : 0;
    if (elements && values)
        std::copy_n(values, std::min(elements, size_t(16)), state.vectorUniformValues);
}
void recordIntVector(int components, int count, const int32_t* values) {
    ++state.vectorUniformCalls;
    state.vectorUniformComponents = components;
    state.vectorUniformInteger = 1;
    state.vectorUniformCount = count;
    const size_t elements = count > 0 ? size_t(count) * size_t(components) : 0;
    if (elements && values)
        std::copy_n(values, std::min(elements, size_t(16)), state.vectorUniformInts);
}
void glUniform1fv(int, int count, const float* values) { recordFloatVector(1, count, values); }
void glUniform2fv(int, int count, const float* values) { recordFloatVector(2, count, values); }
void glUniform3fv(int, int count, const float* values) { recordFloatVector(3, count, values); }
void glUniform4fv(int location, int count, const float* values) {
    recordFloatVector(4, count, values);
    ++state.uniform4fvCalls;
    if (location < 0 || location >= uniformCount || !values) return;
    const char* name = uniformNames[location];
    float* destination = nullptr;
    int* uploads = nullptr;
    if (std::strcmp(name, "uLightAmbient[0]") == 0) {
        destination = state.lightAmbientValues;
        uploads = &state.lightAmbientUploads;
    } else if (std::strcmp(name, "uLightDiffuse[0]") == 0) {
        destination = state.lightDiffuseValues;
        uploads = &state.lightDiffuseUploads;
    } else if (std::strcmp(name, "uLightPosition[0]") == 0) {
        destination = state.lightPositionValues;
        uploads = &state.lightPositionUploads;
    }
    if (!destination) return;
    std::copy_n(values, 4, destination);
    ++*uploads;
}
void glUniform1iv(int, int count, const int32_t* values) { recordIntVector(1, count, values); }
void glUniform2iv(int, int count, const int32_t* values) { recordIntVector(2, count, values); }
void glUniform3iv(int, int count, const int32_t* values) { recordIntVector(3, count, values); }
void glUniform4iv(int, int count, const int32_t* values) { recordIntVector(4, count, values); }
void recordMatrixUniform(int dimension, int count, uint32_t transpose, const float* values) {
    ++state.matrixUniformCalls;
    state.matrixUniformDimension = dimension;
    state.matrixUniformCount = count;
    state.matrixUniformTranspose = int(transpose);
    const size_t elements = count > 0 ? size_t(count) * size_t(dimension * dimension) : 0;
    std::copy_n(values, std::min(elements, size_t(64)), state.matrixUniformValues);
}
void glUniformMatrix2fv(int, int count, uint32_t transpose, const float* values) {
    recordMatrixUniform(2, count, transpose, values);
}
void glUniformMatrix3fv(int, int count, uint32_t transpose, const float* values) {
    recordMatrixUniform(3, count, transpose, values);
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
void glBufferSubData(uint32_t target, std::ptrdiff_t offset, std::ptrdiff_t bytes, const void*) {
    ++state.bufferSubDataCalls;
    state.lastSubDataTarget = target;
    state.lastSubDataOffset = size_t(offset);
    state.lastSubDataBytes = size_t(bytes);
}
void glGetBufferParameteriv(uint32_t target, uint32_t pname, int32_t* values) {
    ++state.bufferParameterQueries;
    state.lastBufferParameterTarget = target;
    state.lastBufferParameterPname = pname;
    if (!values) return;
    values[0] = pname == 0x8764 ? int32_t(state.lastUploadBytes) : 0;
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
    if (pointer && uintptr_t(pointer) > 0x100000) {
        ++state.clientAttribPointerCalls;
        state.lastClientAttribIndex = index;
        state.clientAttribValues[0] = state.clientAttribValues[1] =
            state.clientAttribValues[2] = state.clientAttribValues[3] = 0.0f;
        if (type == 0x1406 && size > 0)
            std::memcpy(state.clientAttribValues, pointer,
                        std::min<size_t>(size_t(size), 4) * sizeof(float));
    }
}
void recordAttribVector(uint32_t index, int components, const float* values) {
    ++state.attribVectorCalls;
    state.lastAttribVectorIndex = index;
    state.lastAttribVectorComponents = components;
    std::copy_n(values, components, state.lastAttribVector);
}
void glVertexAttrib1fv(uint32_t index, const float* values) { recordAttribVector(index, 1, values); }
void glVertexAttrib2fv(uint32_t index, const float* values) { recordAttribVector(index, 2, values); }
void glVertexAttrib3fv(uint32_t index, const float* values) { recordAttribVector(index, 3, values); }
void glVertexAttrib4fv(uint32_t index, const float* values) { recordAttribVector(index, 4, values); }
void glVertexAttrib3f(uint32_t, float, float, float) {}
void glGetVertexAttribfv(uint32_t, uint32_t pname, float* values) {
    if (!values) return;
    if (pname == 0x8626) {
        values[0] = values[1] = values[2] = 0.0f;
        values[3] = 1.0f;
    } else {
        values[0] = 0.0f;
    }
}
void glGetVertexAttribiv(uint32_t, uint32_t, int32_t* values) {
    if (values) values[0] = 0;
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
void glTexParameteri(uint32_t target, uint32_t pname, int value) {
    ++state.texParameterIntCalls;
    state.lastTexParameterTarget = target;
    state.lastTexParameterName = pname;
    state.lastTexParameterInt = value;
}
void glTexParameterf(uint32_t target, uint32_t pname, float value) {
    ++state.texParameterFloatCalls;
    state.lastTexParameterTarget = target;
    state.lastTexParameterName = pname;
    state.lastTexParameterFloat = value;
}
void glTexParameterfv(uint32_t target, uint32_t pname, const float* values) {
    ++state.texParameterVectorCalls;
    state.lastTexParameterTarget = target;
    state.lastTexParameterName = pname;
    if (values) state.lastTexParameterFloat = values[0];
}
void glTexParameteriv(uint32_t target, uint32_t pname, const int32_t* values) {
    ++state.texParameterVectorCalls;
    state.lastTexParameterTarget = target;
    state.lastTexParameterName = pname;
    if (values) state.lastTexParameterInt = values[0];
}
void glGetTexParameteriv(uint32_t, uint32_t, int32_t* values) {
    if (values) values[0] = state.lastTexParameterInt;
}
void glGetTexParameterfv(uint32_t, uint32_t, float* values) {
    if (values) values[0] = state.lastTexParameterFloat;
}
void glPixelStorei(uint32_t pname, int value) {
    if (pname == GL_UNPACK_ALIGNMENT) state.unpackAlignment = value;
    if (pname == GL_PACK_ALIGNMENT) state.packAlignment = value;
}
void glTexImage2D(uint32_t, int, uint32_t, int width, int height, int, uint32_t format,
                  uint32_t type, const void* pixels) {
    ++state.textureUploads;
    ++state.texImageCalls;
    state.texImageWidth = width;
    state.texImageHeight = height;
    state.texImageFormat = format;
    state.texImageType = type;
    const size_t bytesPerPixel = type == GL_UNSIGNED_BYTE
                                     ? (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0)
                                     : 0;
    if (!pixels || !bytesPerPixel || width <= 0 || height <= 0) return;
    const size_t row = size_t(width) * bytesPerPixel;
    const size_t alignment = size_t(state.unpackAlignment);
    const size_t stride = (row + alignment - 1) & ~(alignment - 1);
    state.texImageBytes = stride * size_t(height - 1) + row;
    std::memcpy(state.texImageData, pixels,
                std::min(state.texImageBytes, sizeof(state.texImageData)));
}
void glTexSubImage2D(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void*) {
    ++state.textureUploads;
}
void glCompressedTexImage2D(uint32_t, int, uint32_t, int, int, int, int, const void*) {
    ++state.textureUploads;
}
void glEnable(uint32_t capability) {
    ++state.enableCalls;
    state.lastCapability = capability;
    if (capability == 0x0de1 || capability == 0x0b50) {
        ++state.errors;
        state.lastError = 0x0500;  // ES 2.0 rejects ES 1.1 fixed-function capabilities
    }
}
void glDisable(uint32_t capability) {
    ++state.disableCalls;
    state.lastCapability = capability;
    if (capability == 0x0de1 || capability == 0x0b50) {
        ++state.errors;
        state.lastError = 0x0500;
    }
}
void glBlendFunc(uint32_t, uint32_t) {}
void glClearColor(float, float, float, float) {}
void glClear(uint32_t) {}
void glDepthFunc(uint32_t) {}
void glDepthMask(uint8_t) {}
void glFrontFace(uint32_t mode) {
    ++state.frontFaceCalls;
    state.lastFrontFace = mode;
}
void glCullFace(uint32_t mode) {
    ++state.cullFaceCalls;
    state.lastCullFace = mode;
}
void glBlendEquation(uint32_t mode) {
    ++state.blendEquationCalls;
    state.lastBlendEquation = mode;
}
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
        case GL_VIEWPORT:
            values[0] = 0;
            values[1] = 0;
            values[2] = 320;
            values[3] = 480;
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
void glGetFloatv(uint32_t pname, float* values) {
    if (!values) return;
    if (pname == GL_VIEWPORT) {
        values[0] = 0.0f;
        values[1] = 0.0f;
        values[2] = 320.0f;
        values[3] = 480.0f;
    } else if (pname == 0x0ba6 || pname == 0x0ba7 || pname == 0x0ba8) {
        std::fill(values, values + 16, 0.0f);
        values[0] = values[5] = values[10] = values[15] = 1.0f;
    } else {
        values[0] = 1.0f;
    }
}
void glGetBooleanv(uint32_t, uint8_t* values) {
    if (values) values[0] = 1;
}
const char* glGetString(uint32_t name) {
    if (name == GL_EXTENSIONS) return "GL_TEST_recording";
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
void glBindFramebuffer(uint32_t, uint32_t name) {
    ++state.framebufferBindCalls;
    state.boundFramebuffer = name;
}
void glDeleteFramebuffers(int, const uint32_t*) {}
void glFramebufferTexture2D(uint32_t target, uint32_t attachment, uint32_t textarget,
                           uint32_t texture, int level) {
    ++state.framebufferTextureCalls;
    state.lastFramebufferTarget = target;
    state.lastAttachment = attachment;
    state.lastAttachmentTextureTarget = textarget;
    state.lastAttachmentObjectType = texture ? 0x1702 : 0;
    state.lastAttachmentObjectName = texture;
    state.lastAttachmentLevel = level;
}
void glFramebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbufferTarget,
                               uint32_t renderbuffer) {
    ++state.framebufferRenderbufferCalls;
    state.lastFramebufferTarget = target;
    state.lastAttachment = attachment;
    state.lastAttachmentRenderbufferTarget = renderbufferTarget;
    state.lastAttachmentObjectType = renderbuffer ? 0x8d41 : 0;
    state.lastAttachmentObjectName = renderbuffer;
    state.lastAttachmentLevel = 0;
}
void glGetFramebufferAttachmentParameteriv(uint32_t, uint32_t, uint32_t pname, int32_t* values) {
    ++state.framebufferAttachmentQueries;
    if (!values) return;
    if (pname == 0x8cd0)
        values[0] = int32_t(state.lastAttachmentObjectType);
    else if (pname == 0x8cd1)
        values[0] = int32_t(state.lastAttachmentObjectName);
    else if (pname == 0x8cd2)
        values[0] = state.lastAttachmentLevel;
    else
        values[0] = 0;
}
uint32_t glCheckFramebufferStatus(uint32_t) {
    ++state.framebufferStatusCalls;
    return 0x8cd5;
}
void glGenRenderbuffers(int count, uint32_t* names) {
    for (int index = 0; index < count; ++index) names[index] = nextObject++;
}
void glBindRenderbuffer(uint32_t, uint32_t name) { state.boundRenderbuffer = name; }
void glDeleteRenderbuffers(int, const uint32_t*) {}
void glRenderbufferStorage(uint32_t, uint32_t format, int width, int height) {
    ++state.renderbufferStorageCalls;
    state.renderbufferInternalFormat = format;
    state.renderbufferWidth = width;
    state.renderbufferHeight = height;
}
void glGetRenderbufferParameteriv(uint32_t, uint32_t pname, int32_t* values) {
    if (!values) return;
    // GL_RENDERBUFFER_WIDTH / HEIGHT / INTERNAL_FORMAT.
    if (pname == 0x8d42)
        values[0] = state.renderbufferWidth;
    else if (pname == 0x8d43)
        values[0] = state.renderbufferHeight;
    else if (pname == 0x8d44)
        values[0] = int32_t(state.renderbufferInternalFormat);
    else
        values[0] = 0;
}
void glReadPixels(int x, int y, int width, int height, uint32_t format, uint32_t type,
                  void* pixels) {
    ++state.readPixelsCalls;
    state.readPixelsX = x;
    state.readPixelsY = y;
    state.readPixelsWidth = width;
    state.readPixelsHeight = height;
    state.readPixelsFormat = int(format);
    state.readPixelsType = int(type);
    const size_t bytesPerPixel = type == GL_UNSIGNED_BYTE
                                     ? (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 0)
                                     : 0;
    if (!pixels || !bytesPerPixel || width <= 0 || height <= 0) return;
    const size_t row = size_t(width) * bytesPerPixel;
    const size_t alignment = size_t(state.packAlignment);
    const size_t stride = (row + alignment - 1) & ~(alignment - 1);
    const size_t packedBytes = stride * size_t(height - 1) + row;
    state.readPixelsBytes = packedBytes;
    auto* destination = static_cast<unsigned char*>(pixels);
    for (int rowIndex = 0; rowIndex < height; ++rowIndex)
        for (size_t column = 0; column < row; ++column)
            destination[size_t(rowIndex) * stride + column] =
                static_cast<unsigned char>(size_t(rowIndex) * row + column + 1);
    std::memcpy(state.readPixelsData, destination,
                std::min(state.readPixelsBytes, sizeof(state.readPixelsData)));
}

}  // extern "C"
