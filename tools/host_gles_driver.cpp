// A GLES 1.1 + EGL state machine for running real iOS titles on the host.
//
// This is a *diagnostic harness*, not a renderer: it tracks GL state the way the API
// defines it (matrix stacks, enables, textures, buffers, framebuffer objects), answers
// glGet* with real values, and counts draws. It rasterises nothing, so a frame read back
// from it is a deterministic test pattern and must never be presented as game output.
//
// It exists because the host has no GL driver to test against, and the only way to find
// the *next* blocker behind the GL boundary is to let the guest keep executing. On a
// device the app loads the real ANGLE libraries instead; nothing here is compiled into
// the app.
//
// Build it as libGLESv2_podnative.so in a directory of its own and run the guest runner
// with LD_LIBRARY_PATH pointing there; the bridge resolves that soname first.
//   c++ -std=c++20 -O1 -fPIC -shared tools/host_gles_driver.cpp -o <dir>/libGLESv2_podnative.so
//   LD_LIBRARY_PATH=<dir> guest_runner <bundle.app> <container>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr uint32_t GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402;
constexpr uint32_t GL_UNSIGNED_SHORT = 0x1403, GL_INT = 0x1404, GL_UNSIGNED_INT = 0x1405;
constexpr uint32_t GL_FLOAT = 0x1406, GL_FIXED = 0x140C;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075;
constexpr uint32_t GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702;
constexpr uint32_t GL_NO_ERROR = 0;

struct MatrixStack {
    std::vector<float> stack;  // 16 floats per entry, back() is the current matrix
};
struct ArrayState {
    bool enabled = false;
    int size = 4;
    uint32_t type = GL_FLOAT;
    int stride = 0;
    const void* pointer = nullptr;
};
struct Texture {
    int width = 0, height = 0;
    uint32_t format = 0, type = 0;
};
struct Buffer {
    size_t size = 0;
    uint32_t target = 0;
};
struct Renderbuffer {
    int width = 0, height = 0;
    uint32_t format = 0;
};
struct Framebuffer {
    uint32_t colourRenderbuffer = 0;
    uint32_t depthRenderbuffer = 0;
    uint32_t texture = 0;
};

struct Driver {
    uint32_t error = GL_NO_ERROR;
    uint32_t matrixMode = GL_MODELVIEW;
    MatrixStack modelview{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    MatrixStack projection{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    MatrixStack textureMatrix{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    std::map<uint32_t, bool> capabilities;
    int viewport[4] = {0, 0, 320, 480};
    int scissor[4] = {0, 0, 320, 480};
    float clearColour[4] = {0, 0, 0, 1};
    float lineWidth = 1;
    uint32_t blendSrc = 1, blendDst = 0, depthFunc = 0x0201, frontFace = 0x0901;
    bool depthMask = true;
    uint32_t activeTexture = 0x84C0;  // GL_TEXTURE0
    uint32_t boundTexture[4] = {0, 0, 0, 0};
    std::map<uint32_t, Texture> textures;
    uint32_t nextTexture = 1;
    uint32_t boundBuffer[2] = {0, 0};  // ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER
    std::map<uint32_t, Buffer> buffers;
    uint32_t nextBuffer = 1;
    uint32_t boundFramebuffer = 0, boundRenderbuffer = 0;
    std::map<uint32_t, Renderbuffer> renderbuffers;
    std::map<uint32_t, Framebuffer> framebuffers;
    uint32_t nextRenderbuffer = 1, nextFramebuffer = 1;
    ArrayState vertex, colour, normal;
    std::map<uint32_t, ArrayState> texCoord;
    size_t drawCalls = 0, vertices = 0, texturesUploaded = 0, buffersUploaded = 0;
    size_t badPointers = 0;
    // Not GL state: the harness keeps the drawable so glReadPixels can answer.
    int drawableWidth = 0, drawableHeight = 0;
    uint32_t drawableFramebuffer = 0;
};
Driver* driver() {
    static Driver state;
    return &state;
}
void setError(uint32_t code) {
    if (!driver()->error) driver()->error = code;
}
MatrixStack& current() {
    switch (driver()->matrixMode) {
        case GL_PROJECTION:
            return driver()->projection;
        case GL_TEXTURE:
            return driver()->textureMatrix;
        default:
            return driver()->modelview;
    }
}
// Column-major 4x4 multiply, matching GL's convention: result = current * operand.
void multiply(const float* operand) {
    float result[16];
    const float* a = current().stack.data() + current().stack.size() - 16;
    for (int column = 0; column < 4; column++)
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * operand[column * 4 + k];
            result[column * 4 + row] = sum;
        }
    std::memcpy(current().stack.data() + current().stack.size() - 16, result, sizeof(result));
}
size_t typeSize(uint32_t type) {
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
            return 2;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
        case GL_FIXED:
            return 4;
        default:
            return 0;
    }
}
// The bridge hands down host pointers; anything that is not a plausible host address means
// the marshalling is wrong, which is exactly what a harness must notice.
bool plausible(const void* pointer) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    return address == 0 || address > 0x10000;
}
}  // namespace

extern "C" {
// ---- EGL ----
static uintptr_t displayToken = 1;
static uintptr_t surfaceToken = 2;
static bool contextCurrent = false;
static uint32_t contextVersion = 1;
void* eglGetDisplay(void*) { return &displayToken; }
void* eglGetPlatformDisplayEXT(uint32_t, void*, const int32_t*) { return &displayToken; }
uint32_t eglInitialize(void*, int* major, int* minor) {
    *major = 1;
    *minor = 4;
    return 1;
}
uint32_t eglBindAPI(uint32_t) { return 1; }
uint32_t eglChooseConfig(void*, const int* attributes, void** configs, int count, int* found) {
    static int config = 1;
    if (count > 0 && configs) configs[0] = &config;
    if (found) *found = 1;
    (void)attributes;
    return 1;
}
void* eglCreateContext(void*, void*, void*, const int* attributes) {
    for (int i = 0; attributes && attributes[i] != 0x3038; i += 2)
        if (attributes[i] == 0x3098) contextVersion = uint32_t(attributes[i + 1]);
    static int context = 3;
    return &context;
}
void* eglGetCurrentContext() { return contextCurrent ? &surfaceToken : nullptr; }
uint32_t eglMakeCurrent(void*, void*, void*, void* context) {
    contextCurrent = context != nullptr;
    return 1;
}
uint32_t eglDestroyContext(void*, void*) {
    contextCurrent = false;
    return 1;
}
void* eglCreatePbufferSurface(void*, void*, const int*) { return &surfaceToken; }
uint32_t eglSwapBuffers(void*, void*) { return 1; }
void* eglGetProcAddress(const char*) { return nullptr; }

// ---- GL state ----
void glEnable(uint32_t capability) { driver()->capabilities[capability] = true; }
void glDisable(uint32_t capability) { driver()->capabilities[capability] = false; }
uint32_t glIsEnabled(uint32_t capability) {
    auto found = driver()->capabilities.find(capability);
    return found != driver()->capabilities.end() && found->second;
}
void glClear(uint32_t) {}
void glClearColor(float r, float g, float b, float a) {
    float* colour = driver()->clearColour;
    colour[0] = r;
    colour[1] = g;
    colour[2] = b;
    colour[3] = a;
}
void glViewport(int x, int y, int w, int h) {
    int* viewport = driver()->viewport;
    viewport[0] = x;
    viewport[1] = y;
    viewport[2] = w;
    viewport[3] = h;
}
void glScissor(int x, int y, int w, int h) {
    int* scissor = driver()->scissor;
    scissor[0] = x;
    scissor[1] = y;
    scissor[2] = w;
    scissor[3] = h;
}
void glBlendFunc(uint32_t source, uint32_t destination) {
    driver()->blendSrc = source;
    driver()->blendDst = destination;
}
void glDepthFunc(uint32_t function) { driver()->depthFunc = function; }
void glDepthMask(uint8_t flag) { driver()->depthMask = flag != 0; }
void glFrontFace(uint32_t winding) { driver()->frontFace = winding; }
void glLineWidth(float width) { driver()->lineWidth = width; }
void glPixelStorei(uint32_t pname, int value) {
    if (pname != 0x0CF5 && pname != 0x0D05) setError(0x0500);  // GL_INVALID_ENUM
    (void)value;
}
uint32_t glGetError() {
    const uint32_t code = driver()->error;
    driver()->error = GL_NO_ERROR;
    return code;
}
void glGetIntegerv(uint32_t pname, int32_t* values) {
    Driver& state = *driver();
    if (!values) {
        setError(0x0500);
        return;
    }
    switch (pname) {
        case 0x0D3A:  // GL_VIEWPORT
            for (int i = 0; i < 4; i++) values[i] = state.viewport[i];
            return;
        case 0x0C10:  // GL_SCISSOR_BOX
            for (int i = 0; i < 4; i++) values[i] = state.scissor[i];
            return;
        case 0x0BA0:  // GL_MATRIX_MODE
            values[0] = int32_t(state.matrixMode);
            return;
        case 0x0D33:  // GL_MAX_TEXTURE_SIZE
            values[0] = 2048;
            return;
        case 0x8872:  // GL_MAX_TEXTURE_IMAGE_UNITS
            values[0] = 4;
            return;
        case 0x0D3B:  // GL_MAX_VIEWPORT_DIMS
            values[0] = 2048;
            values[1] = 2048;
            return;
        case 0x1F02:        // GL_VERSION
            values[0] = 0;  // strings are not modelled; the bridge refuses this as a pointer read
            setError(0x0500);
            return;
        case 0x0B44:  // GL_CULL_FACE_MODE
            values[0] = 0x0405;
            return;
        case 0x0B45:  // GL_FRONT_FACE
            values[0] = int32_t(state.frontFace);
            return;
        case 0x0D56:  // GL_DEPTH_FUNC
            values[0] = int32_t(state.depthFunc);
            return;
        case 0x0B71:  // GL_DEPTH_TEST
            values[0] = glIsEnabled(0x0B71);
            return;
        case 0x0BE2:  // GL_BLEND
            values[0] = glIsEnabled(0x0BE2);
            return;
        case 0x0B70:  // GL_TEXTURE_2D
            values[0] = glIsEnabled(0x0DE1);
            return;
        case 0x0DE1:  // GL_TEXTURE_BINDING_2D
            values[0] = int32_t(state.boundTexture[state.activeTexture - 0x84C0]);
            return;
        case 0x8894:  // GL_ARRAY_BUFFER_BINDING
            values[0] = int32_t(state.boundBuffer[0]);
            return;
        case 0x8895:  // GL_ELEMENT_ARRAY_BUFFER_BINDING
            values[0] = int32_t(state.boundBuffer[1]);
            return;
        case 0x8CA6:  // GL_FRAMEBUFFER_BINDING_OES
            values[0] = int32_t(state.boundFramebuffer);
            return;
        case 0x8CA7:  // GL_RENDERBUFFER_BINDING_OES
            values[0] = int32_t(state.boundRenderbuffer);
            return;
        case 0x84E0:  // GL_ACTIVE_TEXTURE
            values[0] = int32_t(state.activeTexture);
            return;
        case 0x1F03:  // GL_EXTENSIONS
            values[0] = 0;
            setError(0x0500);
            return;
        default:
            values[0] = 0;
            return;
    }
}
void glGetRenderbufferParameterivOES(uint32_t target, uint32_t pname, int32_t* value) {
    Driver& state = *driver();
    if (target != 0x8D41 || !value) {
        setError(0x0500);
        return;
    }
    auto found = state.renderbuffers.find(state.boundRenderbuffer);
    const Renderbuffer storage =
        found == state.renderbuffers.end() ? Renderbuffer{} : found->second;
    *value = pname == 0x8D42 ? storage.width : pname == 0x8D43 ? storage.height : 0;
}

// ---- matrices ----
void glMatrixMode(uint32_t mode) { driver()->matrixMode = mode; }
void glLoadIdentity() {
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MatrixStack& stack = current();
    std::memcpy(stack.stack.data() + stack.stack.size() - 16, identity, sizeof(identity));
}
void glLoadMatrixf(const float* matrix) {
    if (!plausible(matrix) || !matrix) {
        setError(0x0500);
        return;
    }
    MatrixStack& stack = current();
    std::memcpy(stack.stack.data() + stack.stack.size() - 16, matrix, sizeof(float) * 16);
}
void glMultMatrixf(const float* matrix) {
    if (!plausible(matrix) || !matrix) {
        setError(0x0500);
        return;
    }
    multiply(matrix);
}
void glPushMatrix() {
    MatrixStack& stack = current();
    const float* top = stack.stack.data() + stack.stack.size() - 16;
    stack.stack.insert(stack.stack.end(), top, top + 16);
}
void glPopMatrix() {
    MatrixStack& stack = current();
    if (stack.stack.size() <= 16) {
        setError(0x0503);  // GL_STACK_UNDERFLOW
        return;
    }
    stack.stack.resize(stack.stack.size() - 16);
}
void glTranslatef(float x, float y, float z) {
    const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
    multiply(matrix);
}
void glScalef(float x, float y, float z) {
    const float matrix[16] = {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1};
    multiply(matrix);
}
void glRotatef(float degrees, float x, float y, float z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length == 0) {
        setError(0x0500);
        return;
    }
    const float radians = degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(radians), s = std::sin(radians);
    const float nx = x / length, ny = y / length, nz = z / length;
    const float matrix[16] = {c + nx * nx * (1 - c),
                              ny * nx * (1 - c) + nz * s,
                              nz * nx * (1 - c) - ny * s,
                              0,
                              nx * ny * (1 - c) - nz * s,
                              c + ny * ny * (1 - c),
                              nz * ny * (1 - c) + nx * s,
                              0,
                              nx * nz * (1 - c) + ny * s,
                              ny * nz * (1 - c) - nx * s,
                              c + nz * nz * (1 - c),
                              0,
                              0,
                              0,
                              0,
                              1};
    multiply(matrix);
}
void glOrthof(float left, float right, float bottom, float top, float near, float far) {
    if (right == left || top == bottom || far == near) {
        setError(0x0501);  // GL_INVALID_VALUE
        return;
    }
    const float matrix[16] = {2 / (right - left),
                              0,
                              0,
                              0,
                              0,
                              2 / (top - bottom),
                              0,
                              0,
                              0,
                              0,
                              -2 / (far - near),
                              0,
                              -(right + left) / (right - left),
                              -(top + bottom) / (top - bottom),
                              -(far + near) / (far - near),
                              1};
    multiply(matrix);
}

// ---- lighting and material state ----
struct VectorState {
    std::map<uint32_t, std::vector<float>> values;
};
std::map<uint32_t, VectorState>& lightState() {
    static std::map<uint32_t, VectorState> lights;
    return lights;
}
VectorState& material() {
    static VectorState state;
    return state;
}
void glLightfv(uint32_t light, uint32_t pname, const float* values) {
    if (!plausible(values) || !values) {
        setError(0x0500);
        return;
    }
    lightState()[light].values[pname] = std::vector<float>(values, values + 4);
}
void glLightModelfv(uint32_t pname, const float* values) {
    if (!plausible(values) || !values) {
        setError(0x0500);
        return;
    }
    lightState()[0].values[pname] = std::vector<float>(values, values + 4);
}
void glMaterialfv(uint32_t /*face*/, uint32_t pname, const float* values) {
    if (!plausible(values) || !values) {
        setError(0x0500);
        return;
    }
    material().values[pname] = std::vector<float>(values, values + 4);
}
void glColor4f(float r, float g, float b, float a) { (void)r, (void)g, (void)b, (void)a; }
void glTexEnvi(uint32_t, uint32_t pname, int value) { (void)pname, (void)value; }
void glTexEnvf(uint32_t, uint32_t, float) {}

// ---- textures ----
void glGenTextures(int count, uint32_t* names) {
    if (count < 0 || (count && !names)) {
        setError(0x0501);
        return;
    }
    for (int i = 0; i < count; i++) names[i] = driver()->nextTexture++;
}
void glDeleteTextures(int count, const uint32_t* names) {
    for (int i = 0; i < count; i++) driver()->textures.erase(names[i]);
}
void glBindTexture(uint32_t target, uint32_t texture) {
    if (target != 0x0DE1) {
        setError(0x0500);
        return;
    }
    driver()->boundTexture[driver()->activeTexture - 0x84C0] = texture;
}
void glActiveTexture(uint32_t unit) {
    if (unit < 0x84C0 || unit > 0x84C3) {
        setError(0x0500);
        return;
    }
    driver()->activeTexture = unit;
}
void glClientActiveTexture(uint32_t unit) { glActiveTexture(unit); }
void glTexParameteri(uint32_t, uint32_t pname, int value) { (void)pname, (void)value; }
void glTexImage2D(uint32_t, int level, uint32_t, int width, int height, int, uint32_t format,
                  uint32_t type, const void* pixels) {
    Driver& state = *driver();
    if (!plausible(pixels)) {
        state.badPointers++;
        setError(0x0500);
        return;
    }
    Texture& texture = state.textures[state.boundTexture[state.activeTexture - 0x84C0]];
    if (level == 0) {
        texture.width = width;
        texture.height = height;
        texture.format = format;
        texture.type = type;
    }
    if (pixels) state.texturesUploaded++;
}
void glTexSubImage2D(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void* pixels) {
    if (!plausible(pixels)) {
        driver()->badPointers++;
        setError(0x0500);
        return;
    }
    if (pixels) driver()->texturesUploaded++;
}
void glCompressedTexImage2D(uint32_t, int, uint32_t, int width, int height, int, int,
                            const void* data) {
    Driver& state = *driver();
    if (!plausible(data)) {
        state.badPointers++;
        setError(0x0500);
        return;
    }
    Texture& texture = state.textures[state.boundTexture[state.activeTexture - 0x84C0]];
    texture.width = width;
    texture.height = height;
    state.texturesUploaded++;
}

// ---- buffer objects ----
void glGenBuffers(int count, uint32_t* names) {
    if (count < 0 || (count && !names)) {
        setError(0x0501);
        return;
    }
    for (int i = 0; i < count; i++) names[i] = driver()->nextBuffer++;
}
void glDeleteBuffers(int count, const uint32_t* names) {
    for (int i = 0; i < count; i++) driver()->buffers.erase(names[i]);
}
void glBindBuffer(uint32_t target, uint32_t buffer) {
    if (target != 0x8892 && target != 0x8893) {
        setError(0x0500);
        return;
    }
    driver()->boundBuffer[target == 0x8892 ? 0 : 1] = buffer;
}
void glBufferData(uint32_t target, int size, const void* data, uint32_t usage) {
    Driver& state = *driver();
    if (size < 0 || !plausible(data)) {
        state.badPointers++;
        setError(0x0501);
        return;
    }
    Buffer& buffer = state.buffers[state.boundBuffer[target == 0x8892 ? 0 : 1]];
    buffer.size = size_t(size);
    buffer.target = target;
    if (data) state.buffersUploaded++;
    (void)usage;
}

// ---- framebuffer objects ----
void glGenFramebuffersOES(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = driver()->nextFramebuffer++;
}
void glDeleteFramebuffersOES(int count, const uint32_t* names) {
    for (int i = 0; i < count; i++) driver()->framebuffers.erase(names[i]);
}
void glBindFramebufferOES(uint32_t target, uint32_t framebuffer) {
    if (target != 0x8D40) {
        setError(0x0500);
        return;
    }
    driver()->boundFramebuffer = framebuffer;
}
void glGenRenderbuffersOES(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = driver()->nextRenderbuffer++;
}
void glDeleteRenderbuffersOES(int count, const uint32_t* names) {
    for (int i = 0; i < count; i++) driver()->renderbuffers.erase(names[i]);
}
void glBindRenderbufferOES(uint32_t target, uint32_t renderbuffer) {
    if (target != 0x8D41) {
        setError(0x0500);
        return;
    }
    driver()->boundRenderbuffer = renderbuffer;
}
void glRenderbufferStorageOES(uint32_t target, uint32_t format, int width, int height) {
    Driver& state = *driver();
    if (target != 0x8D41) {
        setError(0x0500);
        return;
    }
    Renderbuffer& storage = state.renderbuffers[state.boundRenderbuffer];
    storage.width = width;
    storage.height = height;
    storage.format = format;
}
void glFramebufferRenderbufferOES(uint32_t target, uint32_t attachment, uint32_t,
                                  uint32_t renderbuffer) {
    Driver& state = *driver();
    if (target != 0x8D40) {
        setError(0x0500);
        return;
    }
    Framebuffer& framebuffer = state.framebuffers[state.boundFramebuffer];
    if (attachment == 0x8CE0)
        framebuffer.colourRenderbuffer = renderbuffer;
    else
        framebuffer.depthRenderbuffer = renderbuffer;
}
void glFramebufferTexture2DOES(uint32_t target, uint32_t attachment, uint32_t, uint32_t texture,
                               int level) {
    Driver& state = *driver();
    if (target != 0x8D40 || level != 0) {
        setError(0x0500);
        return;
    }
    Framebuffer& framebuffer = state.framebuffers[state.boundFramebuffer];
    if (attachment == 0x8CE0) framebuffer.texture = texture;
}
uint32_t glCheckFramebufferStatusOES(uint32_t target) {
    Driver& state = *driver();
    if (target != 0x8D40) {
        setError(0x0500);
        return 0;
    }
    if (state.boundFramebuffer == 0) return 0x8CD5;  // the default drawable is complete
    auto found = state.framebuffers.find(state.boundFramebuffer);
    if (found == state.framebuffers.end()) return 0x8CD5;
    const Framebuffer& framebuffer = found->second;
    if (!framebuffer.colourRenderbuffer && !framebuffer.texture) return 0x8CD6;
    auto storage = state.renderbuffers.find(framebuffer.colourRenderbuffer);
    if (framebuffer.colourRenderbuffer && storage != state.renderbuffers.end() &&
        storage->second.width == 0)
        return 0x8CD6;
    return 0x8CD5;
}

// ---- client arrays and draws ----
void glEnableClientState(uint32_t which) {
    Driver& state = *driver();
    if (which == GL_VERTEX_ARRAY)
        state.vertex.enabled = true;
    else if (which == GL_COLOR_ARRAY)
        state.colour.enabled = true;
    else if (which == GL_NORMAL_ARRAY)
        state.normal.enabled = true;
    else if (which == GL_TEXTURE_COORD_ARRAY)
        state.texCoord[state.activeTexture].enabled = true;
    else
        setError(0x0500);
}
void glDisableClientState(uint32_t which) {
    Driver& state = *driver();
    if (which == GL_VERTEX_ARRAY)
        state.vertex.enabled = false;
    else if (which == GL_COLOR_ARRAY)
        state.colour.enabled = false;
    else if (which == GL_NORMAL_ARRAY)
        state.normal.enabled = false;
    else if (which == GL_TEXTURE_COORD_ARRAY)
        state.texCoord[state.activeTexture].enabled = false;
    else
        setError(0x0500);
}
void glVertexPointer(int size, uint32_t type, int stride, const void* pointer) {
    ArrayState& array = driver()->vertex;
    array.size = size;
    array.type = type;
    array.stride = stride;
    array.pointer = pointer;
    if (!plausible(pointer)) driver()->badPointers++;
}
void glColorPointer(int size, uint32_t type, int stride, const void* pointer) {
    ArrayState& array = driver()->colour;
    array.size = size;
    array.type = type;
    array.stride = stride;
    array.pointer = pointer;
    if (!plausible(pointer)) driver()->badPointers++;
}
void glNormalPointer(uint32_t type, int stride, const void* pointer) {
    ArrayState& array = driver()->normal;
    array.size = 3;
    array.type = type;
    array.stride = stride;
    array.pointer = pointer;
    if (!plausible(pointer)) driver()->badPointers++;
}
void glTexCoordPointer(int size, uint32_t type, int stride, const void* pointer) {
    ArrayState& array = driver()->texCoord[driver()->activeTexture];
    array.size = size;
    array.type = type;
    array.stride = stride;
    array.pointer = pointer;
    if (!plausible(pointer)) driver()->badPointers++;
}
// A draw is only meaningful if the enabled arrays and the index data are readable host
// memory (the bridge copies them there) — this is what catches a marshalling regression.
bool checkArray(const ArrayState& array, size_t count) {
    if (!array.enabled) return true;
    const size_t element = typeSize(array.type);
    if (!element && array.pointer) return false;
    if (!array.pointer) return true;  // client-side arrays may legally be null
    const size_t stride = array.stride ? size_t(array.stride) : size_t(array.size) * element;
    const auto* bytes = static_cast<const uint8_t*>(array.pointer);
    uint32_t checksum = 0;
    for (size_t i = 0; i < count; i++) checksum += bytes[i * stride];
    if (checksum == 0xdeadbeef) return false;  // keeps the read; a real array never sums so
    return true;
}
void glDrawArrays(uint32_t mode, int first, int count) {
    Driver& state = *driver();
    if (count < 0 || first < 0) {
        setError(0x0501);
        return;
    }
    bool ok = checkArray(state.vertex, size_t(count));
    if (state.colour.enabled) ok = ok && checkArray(state.colour, size_t(count));
    if (state.normal.enabled) ok = ok && checkArray(state.normal, size_t(count));
    for (auto& [unit, array] : state.texCoord)
        if (array.enabled) ok = ok && checkArray(array, size_t(count));
    if (!ok) {
        state.badPointers++;
        setError(0x0500);
        return;
    }
    (void)mode;
    state.drawCalls++;
    state.vertices += size_t(count);
}
void glDrawElements(uint32_t mode, int count, uint32_t type, const void* indices) {
    Driver& state = *driver();
    if (count < 0) {
        setError(0x0501);
        return;
    }
    const size_t element = typeSize(type);
    const bool indexed = state.boundBuffer[1] == 0;
    if (indexed && element && indices) {
        const auto* index = static_cast<const uint16_t*>(indices);
        uint32_t checksum = 0;
        for (int i = 0; i < count; i++) checksum += index[i * (element / 2)];
        if (checksum == 0xdeadbeef) return;
    }
    if (!checkArray(state.vertex, 1) && indexed) {
        state.badPointers++;
        return;
    }
    (void)mode;
    state.drawCalls++;
    state.vertices += size_t(count);
}
void glReadPixels(int x, int y, int width, int height, uint32_t format, uint32_t type,
                  void* pixels) {
    if (!pixels || width <= 0 || height <= 0) return;
    if (format != 0x1908 || type != GL_UNSIGNED_BYTE) {
        setError(0x0500);
        return;
    }
    // A test pattern, not a frame: it proves the readback path and the row flip.
    auto* bytes = static_cast<uint8_t*>(pixels);
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++) {
            uint8_t* texel = bytes + (size_t(row) * size_t(width) + size_t(column)) * 4;
            texel[0] = uint8_t(x + column);
            texel[1] = uint8_t(y + row);
            texel[2] = 0x5a;
            texel[3] = 0xff;
        }
}
void glFinish() {}
void glFlush() {}

// ---- diagnostics for a run (not GL API) ----
const char* host_gles_summary() {
    static std::string summary;
    Driver& state = *driver();
    summary = "host harness: " + std::to_string(state.drawCalls) + " draws, " +
              std::to_string(state.vertices) + " vertices, " +
              std::to_string(state.texturesUploaded) + " texture uploads, " +
              std::to_string(state.buffersUploaded) + " buffer uploads, " +
              std::to_string(state.badPointers) + " unreadable pointers (no rasterisation)";
    return summary.c_str();
}
}
