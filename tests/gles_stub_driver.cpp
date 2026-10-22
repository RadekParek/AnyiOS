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
extern "C" void stub_refuse_surfaceless(int refuse) { refuseSurfaceless = refuse; }

extern "C" void stub_reset() {
    std::memset(&state, 0, sizeof(state));
    state.getIntegerValue = 0;
    refuseSurfaceless = 0;
}

extern "C" {
// --- EGL: surfaceless, as on Android when the guest draws into a renderbuffer ---
static uintptr_t displayToken = 1;
static uintptr_t contextToken = 0;
static uintptr_t currentContext = 0;
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
void* eglCreatePbufferSurface(void*, void*, const int*) {
    static int surfaceToken = 1;
    state.pbufferCreated++;
    return &surfaceToken;
}
uint32_t eglDestroyContext(void*, void*) {
    state.destroyContextCalls++;
    currentContext = 0;
    return 1;
}
void* eglGetProcAddress(const char*) { return nullptr; }
uint32_t eglSwapBuffers(void*, void*) {
    state.swapCalls++;
    return 1;
}

// --- GL ---
void glClear(uint32_t) {}
void glClearColor(float r, float g, float b, float a) {
    state.clearColourCalls++;
    state.clearColour[0] = r;
    state.clearColour[1] = g;
    state.clearColour[2] = b;
    state.clearColour[3] = a;
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
// The pointer is recorded, not dereferenced: a guest address is not host memory, and the
// bridge only hands down host memory when it re-sets the array at draw time.
static const void* vertexPointer = nullptr;
void glVertexPointer(int size, unsigned type, int stride, const void* pointer) {
    state.vertexComponents = size;
    state.vertexType = int(type);
    state.vertexStride = stride;
    vertexPointer = pointer;
}
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
void glTexImage2D(uint32_t, int, uint32_t, int w, int h, int, uint32_t format, uint32_t type,
                  const void* pixels) {
    state.texImageCalls++;
    state.texImageWidth = w;
    state.texImageHeight = h;
    state.texImageFormat = format;
    state.texImageType = type;
    state.texImageBytes = pixels ? size_t(w) * size_t(h) * 4 : 0;
    if (pixels && state.texImageBytes <= sizeof(state.texels))
        std::memcpy(state.texels, pixels, state.texImageBytes);
}
void glGenRenderbuffers(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = 7;
}
void glBindRenderbuffer(uint32_t, uint32_t name) { state.boundRenderbuffer = name; }
void glRenderbufferStorage(uint32_t, uint32_t, int w, int h) {
    state.renderbufferWidth = w;
    state.renderbufferHeight = h;
}
void glGenFramebuffers(int count, uint32_t* names) {
    for (int i = 0; i < count; i++) names[i] = 3;
}
void glBindFramebuffer(uint32_t, uint32_t name) { state.boundFramebuffer = name; }
void glFramebufferRenderbuffer(uint32_t, uint32_t, uint32_t, uint32_t name) {
    state.attachedRenderbuffer = name;
}
uint32_t glCheckFramebufferStatus(uint32_t) { return 0x8cd5; }
void glReadPixels(int /*x*/, int y, int w, int h, uint32_t, uint32_t, void* pixels) {
    state.readPixelsCalls++;
    state.readPixelsWidth = w;
    state.readPixelsHeight = h;
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
void glGetIntegerv(uint32_t, int32_t* values) { values[0] = int32_t(state.getIntegerValue); }
}
