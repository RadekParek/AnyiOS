#pragma once

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
    char vertexSource[1024] = {};
    char fragmentSource[1024] = {};
    // Set by a test to make the next compile report failure, which must stop the emulation
    // rather than draw with a broken program.
    int forceCompileFailure = 0;

    char attribName[4][32] = {};
    int attribIndex[4] = {};
    int attribBindings = 0;

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

    int errors = 0;
    unsigned lastError = 0;
};

}  // namespace podnative::testing

extern "C" podnative::testing::Es2StubState* es2_stub_state();
extern "C" void es2_stub_reset();
