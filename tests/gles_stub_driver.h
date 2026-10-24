// Shared description of what the recording GLES driver in gles_stub_driver.cpp records.
// Only the host tests include this; it is not part of the app.
#pragma once
#include <cstddef>
#include <cstdint>

struct StubState {
    int clearColourCalls;
    float clearColour[4];
    int enableCalls, disableCalls, frontFaceCalls;
    uint32_t enabledCapability, disabledCapability, frontFaceMode;
    int generatedTextureCalls, generatedBufferCalls;
    uint32_t lastGeneratedTexture, lastGeneratedBuffer, boundTextureTarget, boundTexture,
        boundArrayBuffer;
    int viewportCalls;
    int viewport[4];
    int colorMaskCalls;
    uint8_t colorMask[4];
    int drawCalls;
    int drawMode, drawFirst, drawCount;
    int vertexComponents, vertexType, vertexStride;
    float vertices[16];
    int clientsEnabled[4];
    int texImageCalls;
    unsigned texImageInternalFormat, texImageFormat, texImageType;
    int texImageWidth, texImageHeight;
    int compressedTexImageCalls;
    unsigned compressedTexImageFormat;
    int compressedTexImageWidth, compressedTexImageHeight;
    size_t compressedTexImageBytes;
    size_t texImageBytes;
    unsigned char texels[1024];
    int renderbufferWidth, renderbufferHeight;
    uint32_t renderbufferFormat;
    unsigned boundRenderbuffer, boundFramebuffer, attachedRenderbuffer;
    int framebufferBindCalls;
    unsigned framebufferBinds[32], framebufferAtRead;
    int createContextCalls, destroyContextCalls;
    uint32_t contextVersion;
    int readPixelsCalls, readPixelsWidth, readPixelsHeight;
    int swapCalls, finishCalls;
    int pbufferCreated, surfaceMadeCurrent, surfacelessRefusals;
    int platformDisplayCalls;
    uint32_t platformDisplay, angleDisplayType;
    uint32_t getIntegerValue;
};
extern "C" StubState* stub_state();
extern "C" void stub_reset();
// Makes eglMakeCurrent refuse a surfaceless context, the way ANGLE's Android backend does.
extern "C" void stub_refuse_surfaceless(int refuse);
