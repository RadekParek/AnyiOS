#pragma once
#include <cstddef>
#include <cstdint>

// State recorded by gles_es2_stub_driver, the ES2-only test double.
//
// Where gles_stub_driver records a driver that already speaks OpenGL ES 1.1, this one
// deliberately does not export a single ES 1.1 entry point: no glVertexPointer, no
// glMatrixMode, no glColor4f. That absence is the condition under which the runtime has to
// emulate the fixed-function pipeline over ES 2.0, so this double is what makes that path
// testable on a machine with no GPU at all. Everything recorded here is what the bridge
// passed *down*, which is the only observable evidence that an ES 1.1 guest call was
// translated rather than dropped.
namespace podnative::testing {

struct Es2Draw {
    unsigned mode = 0;
    int first = 0;
    int count = 0;
    unsigned type = 0;     // 0 for glDrawArrays, else the index type
    unsigned indices = 0;  // index-buffer offset for glDrawElements
};

struct Es2AttribPointer {
    unsigned index = 0;
    int size = 0;
    unsigned type = 0;
    int normalized = 0;
    int stride = 0;
    unsigned offset = 0;  // the pointer argument, which is a VBO offset when one is bound
};

struct Es2StubState {
    // --- context: the version the bridge asked EGL for ---
    int contextVersion = 0;
    int createContextCalls = 0;
    int makeCurrentCalls = 0;

    // --- the generated shader pair ---
    int compileCalls = 0;
    int linkCalls = 0;
    int useProgramCalls = 0;
    int shaderSourceCalls = 0;
    int getShaderCalls = 0;
    int getProgramCalls = 0;
    int shaderInfoLogCalls = 0;
    int programInfoLogCalls = 0;
    char vertexSource[16384] = {};
    char fragmentSource[16384] = {};
    // Set by a test to make the next compile report failure, which must stop the emulation
    // rather than draw with a broken program.
    int forceCompileFailure = 0;

    char attribName[4][32] = {};
    int attribIndex[4] = {};
    int attribBindings = 0;
    int attribLocationCalls = 0;
    int lastAttribLocation = -1;

    // --- guest shader, uniform and generic-attribute imports ---
    int scalarUniformCalls = 0;
    int scalarUniformComponents = 0;
    int scalarUniformInteger = 0;
    float scalarUniformValues[4] = {};
    int vectorUniformCalls = 0;
    int vectorUniformComponents = 0;
    int vectorUniformInteger = 0;
    int vectorUniformCount = 0;
    float vectorUniformValues[16] = {};
    int32_t vectorUniformInts[16] = {};
    int matrixUniformCalls = 0;
    int matrixUniformDimension = 0;
    int matrixUniformCount = 0;
    int matrixUniformTranspose = 0;
    float matrixUniformValues[64] = {};
    int uniform4fvCalls = 0;
    int lightAmbientUploads = 0, lightDiffuseUploads = 0, lightPositionUploads = 0;
    float lightAmbientValues[4] = {}, lightDiffuseValues[4] = {}, lightPositionValues[4] = {};
    int clientAttribPointerCalls = 0;
    int attribVectorCalls = 0;
    unsigned lastAttribVectorIndex = 0;
    int lastAttribVectorComponents = 0;
    float lastAttribVector[4] = {};
    unsigned lastClientAttribIndex = 0;
    float clientAttribValues[4] = {};

    // --- uniforms, by name ---
    float mvp[16] = {};
    int mvpUploads = 0;
    float colour[4] = {};
    int colourUploads = 0;
    float textureMatrix[16] = {};
    int textureMatrixUploads = 0;
    int samplerUnit = -1;
    int samplerUploads = 0;

    // --- client arrays turned into vertex buffers ---
    int bufferUploads = 0;
    int bufferSubDataCalls = 0;
    unsigned lastSubDataTarget = 0;
    size_t lastSubDataOffset = 0, lastSubDataBytes = 0;
    unsigned lastUploadTarget = 0;
    unsigned lastUploadBytes = 0;
    // Every upload, so a test can tell the vertex copy from the index copy: an indexed draw
    // has to copy as far as its largest index, and only the sizes prove whether it did.
    unsigned uploadTargets[8] = {};
    unsigned uploadBytes[8] = {};
    float lastUploadVertex[4] = {};  // first vertex of the last ARRAY_BUFFER upload
    int attribPointerCalls = 0;
    Es2AttribPointer pointers[8] = {};
    int enabledAttribs = 0;
    int disabledAttribs = 0;

    // --- draws ---
    int drawArraysCalls = 0;
    int drawElementsCalls = 0;
    Es2Draw drawLog[16] = {};
    int draws = 0;

    // --- textures, so a texcoord array has something to sample ---
    int textureUploads = 0;
    int activeTextureCalls = 0;
    unsigned lastActiveTexture = 0;
    int enableCalls = 0, disableCalls = 0;
    unsigned lastCapability = 0;

    int errors = 0;
    unsigned lastError = 0;
    int frontFaceCalls = 0, cullFaceCalls = 0, blendEquationCalls = 0;
    unsigned lastFrontFace = 0, lastCullFace = 0, lastBlendEquation = 0;
    int readPixelsCalls = 0, readPixelsX = 0, readPixelsY = 0, readPixelsWidth = 0,
        readPixelsHeight = 0, readPixelsFormat = 0, readPixelsType = 0;
    size_t readPixelsBytes = 0;
    unsigned char readPixelsData[256] = {};
    int unpackAlignment = 4, packAlignment = 4;
    unsigned boundFramebuffer = 0, boundRenderbuffer = 0;
    int framebufferBindCalls = 0, framebufferStatusCalls = 0;
    int framebufferRenderbufferCalls = 0, framebufferTextureCalls = 0;
    int framebufferAttachmentQueries = 0, lastAttachmentLevel = 0;
    int activeAttribQueries = 0, activeUniformQueries = 0, attachedShaderQueries = 0;
    int precisionQueries = 0, uniformQueries = 0;
    unsigned lastFramebufferTarget = 0, lastAttachment = 0, lastAttachmentRenderbufferTarget = 0;
    int texParameterIntCalls = 0, texParameterFloatCalls = 0, texParameterVectorCalls = 0;
    unsigned lastTexParameterTarget = 0, lastTexParameterName = 0;
    int lastTexParameterInt = 0;
    float lastTexParameterFloat = 0;
    unsigned lastAttachmentObjectType = 0, lastAttachmentObjectName = 0;
    unsigned lastAttachmentTextureTarget = 0;
    int renderbufferStorageCalls = 0, renderbufferWidth = 0, renderbufferHeight = 0;
    unsigned renderbufferInternalFormat = 0;
    int bufferParameterQueries = 0;
    unsigned lastBufferParameterTarget = 0, lastBufferParameterPname = 0;
    int texImageCalls = 0;
    unsigned texImageFormat = 0, texImageType = 0;
    int texImageWidth = 0, texImageHeight = 0;
    size_t texImageBytes = 0;
    unsigned char texImageData[256] = {};
};

}  // namespace podnative::testing

extern "C" podnative::testing::Es2StubState* es2_stub_state();
extern "C" void es2_stub_reset();
extern "C" void es2_stub_set_error(unsigned error);
