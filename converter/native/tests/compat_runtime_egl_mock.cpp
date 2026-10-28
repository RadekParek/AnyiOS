// Recording EGL double. It proves dispatch/lifecycle wiring, not GPU rendering.
#include <cstdint>

namespace {
int requestedRenderable = 0;
int requestedVersion = 0;
int creates = 0;
int makeCurrentCount = 0;
int swaps = 0;
int windowSurfaceCount = 0;
bool failWindow = false;
void *lastWindow = nullptr;
void *const display = reinterpret_cast<void *>(std::uintptr_t{1});
void *const config = reinterpret_cast<void *>(std::uintptr_t{2});
void *const context = reinterpret_cast<void *>(std::uintptr_t{3});
void *const surface = reinterpret_cast<void *>(std::uintptr_t{4});
}
extern "C" {
void *eglGetDisplay(void *) { return display; }
unsigned int eglInitialize(void *, int *major, int *minor) { *major = 1; *minor = 5; return 1; }
unsigned int eglChooseConfig(void *, const int *attrs, void **out, int, int *count) {
    for (int i = 0; attrs[i] != 0x3038; i += 2)
        if (attrs[i] == 0x3040) requestedRenderable = attrs[i + 1];
    *out = config; *count = 1; return 1;
}
void *eglCreateContext(void *, void *, void *, const int *attrs) {
    ++creates;
    for (int i = 0; attrs && attrs[i] != 0x3038; i += 2)
        if (attrs[i] == 0x3098) requestedVersion = attrs[i + 1];
    return context;
}
void *eglCreateWindowSurface(void *, void *, void *window, const int *) {
    lastWindow = window;
    ++windowSurfaceCount;
    if (failWindow) return nullptr;
    return reinterpret_cast<void *>(std::uintptr_t{4} + static_cast<std::uintptr_t>(windowSurfaceCount));
}
void *eglCreatePbufferSurface(void *, void *, const int *) { return surface; }
unsigned int eglMakeCurrent(void *, void *, void *, void *) { ++makeCurrentCount; return 1; }
unsigned int eglSwapBuffers(void *, void *) { ++swaps; return 1; }
unsigned int eglDestroySurface(void *, void *) { return 1; }
unsigned int eglDestroyContext(void *, void *) { return 1; }
unsigned int eglTerminate(void *) { return 1; }
int eglGetError() { return 0x3000; }
int recordedRenderable() { return requestedRenderable; }
int recordedContextVersion() { return requestedVersion; }
int recordedContextsCreated() { return creates; }
int recordedMakeCurrentCount() { return makeCurrentCount; }
int recordedSwapCount() { return swaps; }
int recordedWindowSurfaceCount() { return windowSurfaceCount; }
std::uintptr_t recordedWindow() { return reinterpret_cast<std::uintptr_t>(lastWindow); }
void setWindowSurfaceFailure(int fail) { failWindow = fail != 0; }
}
