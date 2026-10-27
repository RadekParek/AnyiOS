/* Compiled by test_sdl_drawable.py with the actual patched SDL function. */
#include <assert.h>
#include <stddef.h>

#define EGL_NO_DISPLAY NULL
#define EGL_NO_SURFACE NULL
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

typedef int EGLint;
typedef int EGLBoolean;
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef struct {
    EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLint (*eglGetError)(void);
    EGLDisplay egl_display;
} EGLData;
typedef struct { EGLData *egl_data; } SDL_VideoDevice;
typedef struct { EGLSurface egl_surface; } SDL_WindowData;
typedef struct {
    void *driverdata;
    int w, h;
} SDL_Window;
#define _THIS SDL_VideoDevice *_this

static int mutex, *Android_ActivityMutex = &mutex;
static int locks, unlocks, query_count, errors;
static int reported_width, reported_height, fail_width, fail_height;

static void SDL_LockMutex(int *m) { assert(m == &mutex && mutex == 0); mutex = 1; ++locks; }
static void SDL_UnlockMutex(int *m) { assert(m == &mutex && mutex == 1); mutex = 0; ++unlocks; }
static EGLBoolean query(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value)
{
    assert(mutex == 1 && display != NULL && surface != NULL);
    ++query_count;
    if (attribute == EGL_WIDTH) {
        if (fail_width) return 0;
        *value = reported_width;
    } else {
        assert(attribute == EGL_HEIGHT);
        if (fail_height) return 0;
        *value = reported_height;
    }
    return 1;
}
static EGLint get_error(void) { ++errors; return 0; }

#include "sdl_drawable_fragment.h"

static void size_is(SDL_VideoDevice *device, SDL_Window *window, int width, int height)
{
    int w = -1, h = -1;
    Android_GLES_GetDrawableSize(device, window, &w, &h);
    assert(w == width && h == height && locks == unlocks && mutex == 0);
}

int main(void)
{
    int display, surface;
    EGLData egl = {query, get_error, &display};
    SDL_WindowData data = {&surface};
    SDL_Window window = {&data, 720, 1280};
    SDL_VideoDevice device = {&egl};

    /* EGL may ignore a smaller SurfaceHolder buffer: fill the real drawable. */
    reported_width = 1080; reported_height = 1920;
    size_is(&device, &window, 1080, 1920);
    assert(query_count == 2);
    /* If EGL honors scaling, use the scaled drawable instead. */
    reported_width = 720; reported_height = 1280;
    size_is(&device, &window, 720, 1280);
    /* Rotation/resume must query again rather than retaining the old size. */
    window.w = 1280; window.h = 720;
    reported_width = 1920; reported_height = 1080;
    size_is(&device, &window, 1920, 1080);
    /* A missing, destroyed, failed, or zero-sized EGL surface uses SDL's size. */
    data.egl_surface = EGL_NO_SURFACE;
    int before = query_count;
    size_is(&device, &window, 1280, 720);
    assert(query_count == before);
    data.egl_surface = &surface;
    egl.egl_display = EGL_NO_DISPLAY;
    size_is(&device, &window, 1280, 720);
    egl.egl_display = &display;
    fail_width = 1;
    size_is(&device, &window, 1280, 720);
    assert(errors == 1);
    fail_width = 0; fail_height = 1;
    size_is(&device, &window, 1280, 720);
    assert(errors == 2);
    fail_height = 0; reported_width = 0;
    size_is(&device, &window, 1280, 720);
    assert(errors == 2); /* a successful zero-size query must not clear an old error */
    egl.eglQuerySurface = NULL;
    size_is(&device, &window, 1280, 720);
    device.egl_data = NULL;
    size_is(&device, &window, 1280, 720);
    window.driverdata = NULL;
    size_is(&device, &window, 1280, 720);
    Android_GLES_GetDrawableSize(&device, &window, NULL, NULL);
    assert(locks == unlocks && mutex == 0);
    return 0;
}
