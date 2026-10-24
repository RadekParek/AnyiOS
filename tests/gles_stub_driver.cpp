// A recording GLES/EGL driver used by gles_driver_test.
//
// This is a *test double*, not a renderer: it does no rasterisation, it records what the
// bridge passed down. That is the point — the bridge's job is argument marshalling and
// resource transitions, and a driver that records is the only way to prove the guest's
// float registers, stack words, client arrays and pixel copies arrive intact without a
// GPU. Nothing here is linked into the app.
#include "gles_stub_driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

static StubState state;

extern "C" StubState* stub_state() { return &state; }

static int refuseSurfaceless = 0;
static uint32_t nextTexture = 1, nextBuffer = 1;
static uint32_t nextFramebuffer = 3, nextRenderbuffer = 7;
static uintptr_t displayToken = 1;
static uintptr_t contextToken = 0;
static uintptr_t currentContext = 0;
static uintptr_t surfaceToken = 1;
static uintptr_t contextConfigToken = 1, alternateConfigToken = 2;
static void* contextConfig = nullptr;
static void enableViaProc(uint32_t capability);
extern "C" void stub_refuse_surfaceless(int refuse) { refuseSurfaceless = refuse; }

extern "C" void stub_reset() {
    std::memset(&state, 0, sizeof(state));
    state.getIntegerValue = 0;
    refuseSurfaceless = 0;
    nextTexture = 1;
    nextBuffer = 1;
    nextFramebuffer = 3;
    nextRenderbuffer = 7;
    contextToken = currentContext = 0;
    surfaceToken = 1;
    contextConfig = nullptr;
}

static void enableViaProc(uint32_t capability) {
    state.enableCalls++;
    state.enabledCapability = capability;
}

extern "C" {
// --- EGL: surfaceless, as on Android when the guest draws into a renderbuffer ---
void* eglGetDisplay(void*) { return &displayToken; }
void* eglGetPlatformDisplayEXT(uint32_t platform, void*, const int* attributes) {
    state.platformDisplayCalls++;
    state.platformDisplay = platform;
    for (int i = 0; attributes && attributes[i] != 0x3038; i += 2)
        if (attributes[i] == 0x3203) state.angleDisplayType = uint32_t(attributes[i + 1]);
    return &displayToken;
}
uint32_t eglInitialize(void*, int* major, int* minor) {
    *major = 1;
    *minor = 4;
    return 1;
}
uint32_t eglBindAPI(uint32_t) { return 1; }
uint32_t eglChooseConfig(void*, const int* attributes, void** configs, int count, int* found) {
    bool requestsDepth = false;
    for (int i = 0; attributes && attributes[i] != 0x3038; i += 2)
        if (attributes[i] == 0x3025 && attributes[i + 1] > 0) requestsDepth = true;
    if (count > 0 && configs)
        configs[0] = requestsDepth ? static_cast<void*>(&contextConfigToken)
                                   : static_cast<void*>(&alternateConfigToken);
    *found = 1;
    return 1;
}
void* eglCreateContext(void*, void* config, void*, const int* attributes) {
    contextConfig = config;
    state.createContextCalls++;
    for (int i = 0; attributes && attributes[i] != 0x3038; i += 2)
        if (attributes[i] == 0x3098) state.contextVersion = uint32_t(attributes[i + 1]);
    return ++contextToken ? &contextToken : nullptr;
}
void* eglGetCurrentContext() { return currentContext ? &contextToken : nullptr; }
uint32_t eglMakeCurrent(void*, void* draw, void*, void* context) {
    if (!draw && refuseSurfaceless) {
        state.surfacelessRefusals++;
        return 0;
    }
    if (draw) state.surfaceMadeCurrent++;
    currentContext = context ? 1 : 0;
    return 1;
}
void* eglCreatePbufferSurface(void*, void* config, const int*) {
    state.pbufferCreated++;
    // EGL_BAD_MATCH: a pbuffer created from an incompatible config cannot be attached to
    // the context selected earlier. This catches the exact fallback mistake on real drivers.
    if (config != contextConfig) return nullptr;
    return &surfaceToken;
}
uint32_t eglDestroySurface(void*, void*) { return 1; }
uint32_t eglDestroyContext(void*, void*) {
    state.destroyContextCalls++;
    currentContext = 0;
    contextConfig = nullptr;
    return 1;
}
void* eglGetProcAddress(const char* name) {
    // Simulate implementations that expose a core entry point only after a context is current.
    if (currentContext && name && std::strcmp(name, "glEnable") == 0)
        return reinterpret_cast<void*>(&enableViaProc);
    return nullptr;
}
uint32_t eglSwapBuffers(void*, void*) {
    state.swapCalls++;
    return 1;
}

// --- GL ---
void glDisable(uint32_t capability) {
    state.disableCalls++;
    state.disabledCapability = capability;
}
void glFrontFace(uint32_t mode) {
    state.frontFaceCalls++;
    state.frontFaceMode = mode;
}
void glActiveTexture(uint32_t) {}
void glClientActiveTexture(uint32_t) {}
void glBindTexture(uint32_t target, uint32_t name) {
    state.boundTextureTarget = target;
    state.boundTexture = name;
}
void glBlendFunc(uint32_t, uint32_t) {}
void glColor4f(float, float, float, float) {}
void glDepthFunc(uint32_t) {}
void glDepthMask(uint8_t) {}
void glLineWidth(float) {}
void glLoadMatrixf(const float*) {}
void glLightfv(uint32_t, uint32_t, const float*) {}
void glMaterialfv(uint32_t, uint32_t, const float*) {}
void glMatrixMode(uint32_t) {}
void glPixelStorei(uint32_t, int) {}
void glScissor(int, int, int, int) {}
void glTexEnvi(uint32_t, uint32_t, int) {}
void glTexParameteri(uint32_t, uint32_t, int) {}
void glTexSubImage2D(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void*) {}
void glClear(uint32_t) {}
void glClearColor(float r, float g, float b, float a) {
    state.clearColourCalls++;
    state.clearColour[0] = r;
    state.clearColour[1] = g;
    state.clearColour[2] = b;
    state.clearColour[3] = a;
}
void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    state.colorMaskCalls++;
    state.colorMask[0] = red;
    state.colorMask[1] = green;
    state.colorMask[2] = blue;
    state.colorMask[3] = alpha;
}
void glViewport(int x, int y, int w, int h) {
    state.viewportCalls++;
    state.viewport[0] = x;
    state.viewport[1] = y;
    state.viewport[2] = w;
    state.viewport[3] = h;
}
void glEnableClientState(uint32_t which) {
    if (which == 0x8074) state.clientsEnabled[0]++;
    if (which == 0x8075) state.clientsEnabled[1]++;
    if (which == 0x8076) state.clientsEnabled[2]++;
    if (which == 0x8078) state.clientsEnabled[3]++;
}
void glDisableClientState(uint32_t which) {
    if (which == 0x8074) state.clientsEnabled[0]--;
    if (which == 0x8075) state.clientsEnabled[1]--;
    if (which == 0x8076) state.clientsEnabled[2]--;
    if (which == 0x8078) state.clientsEnabled[3]--;
}
// The pointer is recorded, not dereferenced: a guest address is not host memory, and the
// bridge only hands down host memory when it re-sets the array at draw time.
static const void* vertexPointer = nullptr;
void glVertexPointer(int size, unsigned type, int stride, const void* pointer) {
    state.vertexComponents = size;
    state.vertexType = int(type);
    state.vertexStride = stride;
    vertexPointer = pointer;
}
void glColorPointer(int, unsigned, int, const void*) {}
void glNormalPointer(unsigned, int, const void*) {}
void glTexCoordPointer(int, unsigned, int, const void*) {}
void glDrawArrays(uint32_t mode, int first, int count) {
    state.drawCalls++;
    state.drawMode = int(mode);
    state.drawFirst = first;
    state.drawCount = count;
    // A host pointer (heap address) is the bridge's draw-time copy of the guest array; one
    // vertex is enough to prove the bytes crossed over, and it is always inside the copy.
    const uintptr_t address = reinterpret_cast<uintptr_t>(vertexPointer);
    if (address > 0x100000 && state.vertexComponents > 0)
        std::memcpy(state.vertices, vertexPointer,
                    std::min<size_t>(sizeof(state.vertices), size_t(state.vertexComponents) * 4));
}
void glDrawElements(uint32_t mode, int count, uint32_t, const void*) {
    state.drawCalls++;
    state.drawMode = int(mode);
    state.drawFirst = 0;
    state.drawCount = count;
}
void glTexImage2D(uint32_t, int, uint32_t internal, int w, int h, int, uint32_t format,
                  uint32_t type, const void* pixels) {
    state.texImageCalls++;
    state.texImageInternalFormat = internal;
    state.texImageWidth = w;
    state.texImageHeight = h;
    state.texImageFormat = format;
    state.texImageType = type;
    state.texImageBytes = pixels ? size_t(w) * size_t(h) * 4 : 0;
    if (pixels && state.texImageBytes <= sizeof(state.texels))
        std::memcpy(state.texels, pixels, state.texImageBytes);
}
void glCompressedTexImage2D(uint32_t, int, uint32_t format, int w, int h, int, int imageSize,
                            const void*) {
    state.compressedTexImageCalls++;
    state.compressedTexImageFormat = format;
    state.compressedTexImageWidth = w;
    state.compressedTexImageHeight = h;
    state.compressedTexImageBytes = size_t(imageSize);
}
void glGenTextures(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) {
        names[i] = nextTexture++;
        state.lastGeneratedTexture = names[i];
        state.generatedTextureCalls++;
    }
}
void glDeleteTextures(int, const uint32_t*) {}
void glBindBuffer(uint32_t, uint32_t name) { state.boundArrayBuffer = name; }
void glGenBuffers(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) {
        names[i] = nextBuffer++;
        state.lastGeneratedBuffer = names[i];
        state.generatedBufferCalls++;
    }
}
void glDeleteBuffers(int, const uint32_t*) {}
void glBufferData(uint32_t, std::ptrdiff_t, const void*, uint32_t) {}
void glGenRenderbuffers(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = nextRenderbuffer++;
}
void glBindRenderbuffer(uint32_t, uint32_t name) { state.boundRenderbuffer = name; }
void glRenderbufferStorage(uint32_t, uint32_t format, int w, int h) {
    state.renderbufferFormat = format;
    state.renderbufferWidth = w;
    state.renderbufferHeight = h;
}
void glGetRenderbufferParameteriv(uint32_t, uint32_t pname, int32_t* value) {
    if (!value) return;
    if (pname == 0x8d42)
        *value = state.renderbufferWidth;  // GL_RENDERBUFFER_WIDTH
    else if (pname == 0x8d43)
        *value = state.renderbufferHeight;  // GL_RENDERBUFFER_HEIGHT
    else if (pname == 0x8d44)
        *value = int32_t(state.renderbufferFormat);  // GL_RENDERBUFFER_INTERNAL_FORMAT
    else
        *value = 0;
}
void glDeleteRenderbuffers(int, const uint32_t*) {}
void glGenFramebuffers(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = nextFramebuffer++;
}
void glBindFramebuffer(uint32_t, uint32_t name) {
    state.boundFramebuffer = name;
    if (state.framebufferBindCalls <
        int(sizeof(state.framebufferBinds) / sizeof(state.framebufferBinds[0])))
        state.framebufferBinds[state.framebufferBindCalls] = name;
    state.framebufferBindCalls++;
}
void glFramebufferRenderbuffer(uint32_t, uint32_t, uint32_t, uint32_t name) {
    state.attachedRenderbuffer = name;
}
void glFramebufferTexture2D(uint32_t, uint32_t, uint32_t, uint32_t, int) {}
void glDeleteFramebuffers(int, const uint32_t*) {}
uint32_t glCheckFramebufferStatus(uint32_t) { return 0x8cd5; }
void glReadPixels(int /*x*/, int y, int w, int h, uint32_t, uint32_t, void* pixels) {
    state.readPixelsCalls++;
    state.readPixelsWidth = w;
    state.readPixelsHeight = h;
    state.framebufferAtRead = state.boundFramebuffer;
    // Encode the GL row index in the green channel so the host-side flip is checkable.
    auto* bytes = static_cast<uint8_t*>(pixels);
    for (int row = 0; row < h; row++) {
        for (int column = 0; column < w; column++) {
            uint8_t* texel = bytes + (size_t(row) * size_t(w) + size_t(column)) * 4;
            texel[0] = uint8_t(column);
            texel[1] = uint8_t(y + row);
            texel[2] = 0;
            texel[3] = 255;
        }
    }
}
void glFinish() { state.finishCalls++; }
void glGetIntegerv(uint32_t pname, int32_t* values) {
    if (!values) return;
    // The Android game queries this before uploading its splash texture. Returning zero here
    // makes a 512x1024 texture look unsupported, so report a conservative GLES-sized limit;
    // this remains a recording test double, not evidence of rendering.
    values[0] = int32_t(pname == 0x0d33 ? 4096 : state.getIntegerValue);  // GL_MAX_TEXTURE_SIZE
}
}
