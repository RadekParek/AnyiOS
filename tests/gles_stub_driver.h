// Shared description of what the recording GLES driver in gles_stub_driver.cpp records.
// Only the host tests include this; it is not part of the app.
#pragma once
#include <cstddef>
#include <cstdint>

struct StubState {
    int clearColourCalls;
    float clearColour[4];
    int viewportCalls;
    int viewport[4];
    int drawCalls;
    int drawMode, drawFirst, drawCount;
    int vertexComponents, vertexType, vertexStride;
    float vertices[16];
    int clientsEnabled[4];
    int texImageCalls;
    unsigned texImageFormat, texImageType;
    int texImageWidth, texImageHeight;
    size_t texImageBytes;
    unsigned char texels[64];
    int renderbufferWidth, renderbufferHeight;
    unsigned boundRenderbuffer, boundFramebuffer, attachedRenderbuffer;
    int createContextCalls, destroyContextCalls;
    uint32_t contextVersion;
    int readPixelsCalls, readPixelsWidth, readPixelsHeight;
    int swapCalls, finishCalls;
    int pbufferCreated, surfaceMadeCurrent, surfacelessRefusals;
    uint32_t getIntegerValue;
};
extern "C" StubState* stub_state();
extern "C" void stub_reset();
// Makes eglMakeCurrent refuse a surfaceless context, the way ANGLE's Android backend does.
extern "C" void stub_refuse_surfaceless(int refuse);
