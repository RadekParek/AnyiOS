// Drives the GL bridge against tests/gles_es2_stub_driver.cpp, an OpenGL ES 2.0-only driver.
//
// gles_driver_test proves the other half of the bridge: that an ES 1.1 call reaches a driver
// that already speaks ES 1.1 with its arguments intact. This one proves the translation. ANGLE
// has no ES 1.1 frontend and neither do most current mobile drivers, so a guest written for
// fixed-function GL has nothing to call. The bridge answers those entry points itself and draws
// through a generated ES 2.0 program; what is asserted here is that translation, end to end:
// the context version asked for, the shader pair, the attribute bindings, the matrices, the
// vertex bytes lifted out of guest memory, GL_FIXED widening, the indexed-draw copy, the
// program cache, and the honest failure when a generated shader does not compile.
//
// The driver records rather than renders, so nothing here claims a pixel was drawn.
#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gles_es2_stub_driver.h"
#include "runtime/error.h"
#include "runtime/gles.h"
#include "runtime/memory.h"
using namespace podnative::runtime;
using podnative::testing::Es2StubState;

namespace {

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
Es2StubState* stub() {
    using Getter = Es2StubState* (*)();
    auto getter = reinterpret_cast<Getter>(dlsym(RTLD_DEFAULT, "es2_stub_state"));
    check(getter != nullptr, "the ES 2.0 double exposes its state");
    return getter();
}
void resetStub() {
    using Reset = void (*)();
    auto reset = reinterpret_cast<Reset>(dlsym(RTLD_DEFAULT, "es2_stub_reset"));
    check(reset != nullptr, "the ES 2.0 double exposes a reset");
    reset();
}
uint32_t bitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// One guest call through the bridge, in the AAPCS-VFP layout: integers in r0-r3 then on the
// stack at sp, floats in s0-s15 numbered independently of the integers.
struct Call {
    GLES& gles;
    Memory& memory;
    CpuState state;
    explicit Call(GLES& gles, Memory& memory) : gles(gles), memory(memory) {
        state = {};
        state.registers[13] = kStack;
    }
    bool operator()(const std::string& symbol) { return gles.invoke(symbol, state); }
    void must(const std::string& symbol) {
        check((*this)(symbol), "the bridge answers " + symbol + " over an ES 2.0-only driver");
    }
    void ints(std::initializer_list<uint32_t> values) {
        size_t index = 0;
        for (uint32_t value : values) state.registers[index++] = value;
    }
    void floats(std::initializer_list<float> values) {
        size_t index = 0;
        for (float value : values) state.extensionRegisters[index++] = bitsOf(value);
    }
    static constexpr uint32_t kStack = 0x10100;
};

constexpr uint32_t GL_TRIANGLES = 0x0004;
constexpr uint32_t GL_TEXTURE_2D = 0x0de1;
constexpr uint32_t GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_FLOAT = 0x1406;
constexpr uint32_t GL_FIXED = 0x140c;
constexpr uint32_t GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701;
constexpr uint32_t GL_TEXTURE_ENV = 0x1703, GL_TEXTURE_ENV_MODE = 0x2200, GL_REPLACE = 0x1e01;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076;
constexpr uint32_t GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_TEXTURE0 = 0x84c0;
constexpr uint32_t GL_MAX_TEXTURE_UNITS = 0x8482;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t kGuestPage = 0x10000;

// The ES 1.1 entry points a driver without an ES 1.1 frontend simply does not have.
void answeredWithoutAFixedFunctionDriver(GLES& gles, Memory& memory, Es2StubState* recorded) {
    check(gles.emulatesFixedFunction(),
          "a driver with glDrawArrays and no glVertexPointer is served by the emulation");
    check(gles.describe().find("fixed function emulated over ES 2.0") != std::string::npos,
          "the report says the fixed function pipeline is emulated: " + gles.describe());

    // The guest asked for ES 1.1; the context has to be ES 2.0, because that is all this
    // driver has. Asking for 1 here is what makes eglCreateContext fail on ANGLE.
    gles.setClientApi(1);
    check(gles.makeCurrent(), "makeCurrent succeeds over the ES 2.0 double: " + gles.describe());
    check(recorded->contextVersion == 2,
          "an ES 1.1 client gets an ES 2.0 context when the driver has no ES 1.1 frontend");

    Call call(gles, memory);
    call.ints({GL_PROJECTION});
    call.must("_glMatrixMode");
    call.ints({GL_MODELVIEW});
    call.must("_glMatrixMode");
    call.ints({GL_VERTEX_ARRAY});
    call.must("_glEnableClientState");
    call.ints({2, GL_FLOAT, 0, kGuestPage});
    call.must("_glVertexPointer");
    call.ints({GL_TEXTURE0});
    call.must("_glClientActiveTexture");
    call.ints({GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE});
    call.must("_glTexEnvi");
    call.floats({0.25f, 0.5f, 0.75f, 1.0f});
    call.must("_glColor4f");

    // An ES 1.1 limit an ES 2.0 driver cannot answer is answered by the bridge rather than
    // left as whatever the guest's buffer happened to hold.
    memory.write32(0x10200, 0xdeadbeef);
    Call limit(gles, memory);
    limit.ints({GL_MAX_TEXTURE_UNITS, 0x10200});
    limit.must("_glGetIntegerv");
    check(memory.read32(0x10200) == 2,
          "GL_MAX_TEXTURE_UNITS is answered for an ES 1.1 guest over an ES 2.0 driver");
}

// A complete fixed-function frame: matrices, a colour, a vertex array, one draw.
void oneTranslatedDraw(GLES& gles, Memory& memory, Es2StubState* recorded) {
    resetStub();
    // Projection scales by (2, 3, 4); modelview translates by (5, 7, 9). Column-major, as GL
    // stores them, so the product puts the scaled translation in the fourth column.
    float projection[16] = {}, modelview[16] = {};
    projection[0] = 2;
    projection[5] = 3;
    projection[10] = 4;
    projection[15] = 1;
    modelview[0] = modelview[5] = modelview[10] = modelview[15] = 1;
    modelview[12] = 5;
    modelview[13] = 7;
    modelview[14] = 9;
    memory.write(kGuestPage, projection, sizeof(projection));
    Call projectionMode(gles, memory);
    projectionMode.ints({GL_PROJECTION});
    projectionMode.must("_glMatrixMode");
    Call loadProjection(gles, memory);
    loadProjection.ints({kGuestPage});
    loadProjection.must("_glLoadMatrixf");
    memory.write(kGuestPage, modelview, sizeof(modelview));
    Call modelviewMode(gles, memory);
    modelviewMode.ints({GL_MODELVIEW});
    modelviewMode.must("_glMatrixMode");
    Call loadModelview(gles, memory);
    loadModelview.ints({kGuestPage});
    loadModelview.must("_glLoadMatrixf");

    // Texturing off, no colour array: the plainest variant there is.
    Call disableTexture(gles, memory);
    disableTexture.ints({GL_TEXTURE_2D});
    disableTexture.must("_glDisable");
    Call colour(gles, memory);
    colour.floats({0.25f, 0.5f, 0.75f, 1.0f});
    colour.must("_glColor4f");

    const float vertices[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    memory.write(kGuestPage, vertices, sizeof(vertices));
    Call pointer(gles, memory);
    pointer.ints({3, GL_FLOAT, 0, kGuestPage});
    pointer.must("_glVertexPointer");

    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 3});
    draw.must("_glDrawArrays");

    check(recorded->compileCalls == 2 && recorded->linkCalls == 1,
          "one draw of an ES 1.1 guest produced one linked ES 2.0 program");
    check(recorded->useProgramCalls == 1, "the generated program is in use for the draw");
    check(std::string(recorded->vertexSource).find("uMvp") != std::string::npos &&
              std::string(recorded->vertexSource).find("aPosition") != std::string::npos,
          std::string("the generated vertex shader transforms a position by the matrix: ") +
              recorded->vertexSource);
    check(std::string(recorded->vertexSource).find("uColor") != std::string::npos &&
              std::string(recorded->fragmentSource).find("vColor") != std::string::npos,
          std::string("the current colour is carried from the uniform to the fragment: ") +
              recorded->fragmentSource);
    bool positionBound = false;
    for (int index = 0; index < recorded->attribBindings && index < 4; ++index)
        if (std::string(recorded->attribName[index]) == "aPosition")
            positionBound = recorded->attribIndex[index] == 0;
    check(positionBound, "aPosition is bound to attribute 0 before linking");

    check(recorded->mvpUploads == 1, "the model-view-projection matrix is uploaded once");
    check(near(recorded->mvp[0], 2) && near(recorded->mvp[5], 3) && near(recorded->mvp[10], 4) &&
              near(recorded->mvp[12], 10) && near(recorded->mvp[13], 21) &&
              near(recorded->mvp[14], 36) && near(recorded->mvp[15], 1),
          "the uploaded matrix is the projection multiplied by the modelview");
    check(recorded->colourUploads == 1 && near(recorded->colour[0], 0.25f) &&
              near(recorded->colour[3], 1.0f),
          "glColor4f reaches the shader as a uniform instead of being dropped");

    check(recorded->bufferUploads == 1 && recorded->lastUploadTarget == GL_ARRAY_BUFFER,
          "the guest's client array is copied into one vertex buffer");
    check(recorded->lastUploadBytes == 9 * sizeof(float),
          "the copy is exactly the three vertices the draw reaches");
    check(near(recorded->lastUploadVertex[0], 1) && near(recorded->lastUploadVertex[1], 2) &&
              near(recorded->lastUploadVertex[2], 3),
          "the bytes the driver received are the guest's own vertex data");
    check(recorded->attribPointerCalls == 1 && recorded->pointers[0].index == 0 &&
              recorded->pointers[0].size == 3 && recorded->pointers[0].type == GL_FLOAT &&
              recorded->pointers[0].normalized == 0,
          "the position array is described to the driver as three unnormalised floats");
    check(recorded->enabledAttribs == 1, "only the attribute this program reads is enabled");
    check(recorded->disabledAttribs == 3,
          "the colour, texture-coordinate and normal attributes are left disabled");
    check(recorded->drawArraysCalls == 1 && recorded->draws == 1 &&
              recorded->drawLog[0].mode == GL_TRIANGLES && recorded->drawLog[0].count == 3,
          "the guest's glDrawArrays came out the other side as one ES 2.0 draw");

    // The same variant must not build a second program: a draw per frame that relinks is a
    // stutter, and the cache is what keeps the emulation cheap.
    Call again(gles, memory);
    again.ints({GL_TRIANGLES, 0, 3});
    again.must("_glDrawArrays");
    check(recorded->linkCalls == 1 && recorded->drawArraysCalls == 2,
          "a second draw of the same variant reuses the cached program");
}

// A colour array and GL_FIXED positions: the variant changes, and so does the marshalling.
void colourArrayAndFixedPoint(GLES& gles, Memory& memory, Es2StubState* recorded) {
    resetStub();
    Call enableColour(gles, memory);
    enableColour.ints({GL_COLOR_ARRAY});
    enableColour.must("_glEnableClientState");

    // Four bytes per colour, which is what an ES 1.1 title built for armv6 actually uses.
    const uint8_t colours[12] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64};
    memory.write(kGuestPage, colours, sizeof(colours));
    Call colourPointer(gles, memory);
    colourPointer.ints({4, GL_UNSIGNED_BYTE, 0, kGuestPage});
    colourPointer.must("_glColorPointer");

    // 16.16 fixed point, the other ES 1.1 type an ES 2.0 driver has no equivalent of.
    const int32_t fixed[9] = {65536, -65536, 0, 32768, 0, 0, 0, 0, 131072};
    memory.write(kGuestPage + 0x100, fixed, sizeof(fixed));
    Call vertexPointer(gles, memory);
    vertexPointer.ints({3, GL_FIXED, 0, kGuestPage + 0x100});
    vertexPointer.must("_glVertexPointer");

    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 3});
    draw.must("_glDrawArrays");

    check(recorded->linkCalls == 1 && recorded->compileCalls == 2,
          "a colour array is a different variant and gets its own program");
    check(recorded->bufferUploads == 2, "both the vertex and the colour array are uploaded");
    check(recorded->attribPointerCalls == 2, "both arrays are described to the driver");
    bool positionWide = false, colourNormalised = false;
    for (int index = 0; index < recorded->attribPointerCalls && index < 8; ++index) {
        const auto& pointer = recorded->pointers[index];
        if (pointer.index == 0) positionWide = pointer.type == GL_FLOAT;
        if (pointer.index == 1)
            colourNormalised = pointer.type == GL_UNSIGNED_BYTE && pointer.normalized == 1;
    }
    check(positionWide, "GL_FIXED positions are widened to floats, which ES 2.0 has no name for");
    check(colourNormalised, "a byte colour array is passed as normalised, so 255 means 1.0");
    check(recorded->enabledAttribs == 2 && recorded->disabledAttribs == 2,
          "the colour attribute is enabled and the unused ones are disabled");
    check(recorded->drawArraysCalls == 1, "the two-array draw still comes out as one draw");
}

// An indexed draw: the copy has to reach as far as the largest index, not as far as the count.
void indexedDraw(GLES& gles, Memory& memory, Es2StubState* recorded) {
    resetStub();
    const float vertices[18] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5};
    memory.write(kGuestPage, vertices, sizeof(vertices));
    Call vertexPointer(gles, memory);
    vertexPointer.ints({3, GL_FLOAT, 0, kGuestPage});
    vertexPointer.must("_glVertexPointer");
    const uint16_t indices[4] = {0, 1, 2, 5};
    memory.write(kGuestPage + 0x200, indices, sizeof(indices));

    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 4, GL_UNSIGNED_SHORT, kGuestPage + 0x200});
    draw.must("_glDrawElements");

    check(recorded->drawElementsCalls == 1 && recorded->drawLog[0].count == 4 &&
              recorded->drawLog[0].type == GL_UNSIGNED_SHORT,
          "the indexed draw reaches the driver with its own count and index type");
    // Six vertices, because index 5 is the furthest the driver will read: copying only the
    // four the count names would let the driver read past the end of the copy.
    bool vertexCopyWideEnough = false, elementCopyExact = false;
    for (int index = 0; index < recorded->bufferUploads && index < 8; ++index) {
        if (recorded->uploadTargets[index] == GL_ARRAY_BUFFER &&
            recorded->uploadBytes[index] == 6 * 3 * sizeof(float))
            vertexCopyWideEnough = true;
        if (recorded->uploadTargets[index] == GL_ELEMENT_ARRAY_BUFFER &&
            recorded->uploadBytes[index] == sizeof(indices))
            elementCopyExact = true;
    }
    check(vertexCopyWideEnough,
          "the vertex copy reaches the largest index, not just the four the count names");
    check(elementCopyExact, "the index copy holds exactly the guest's indices");
}

// A generated shader that does not compile must not be drawn with: the draw is refused and the
// driver's own info log ends up in the report, because on a device that is the only evidence.
void refusesToDrawWithABrokenProgram(GLES& gles, Memory& memory, Es2StubState* recorded) {
    resetStub();
    recorded->forceCompileFailure = 1;
    // A new variant, so the cache cannot hide the failure behind an earlier good program.
    Call enableTexture(gles, memory);
    enableTexture.ints({GL_TEXTURE_2D});
    enableTexture.must("_glEnable");
    Call texcoord(gles, memory);
    texcoord.ints({GL_TEXTURE_COORD_ARRAY});
    texcoord.must("_glEnableClientState");
    const float coords[6] = {0, 0, 1, 0, 0, 1};
    memory.write(kGuestPage + 0x300, coords, sizeof(coords));
    Call texPointer(gles, memory);
    texPointer.ints({2, GL_FLOAT, 0, kGuestPage + 0x300});
    texPointer.must("_glTexCoordPointer");

    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 3});
    check(!draw("_glDrawArrays"), "a draw whose program failed to compile is refused");
    check(recorded->drawArraysCalls == 0,
          "nothing is drawn through a program that did not compile");
    check(gles.describe().find("did not compile") != std::string::npos,
          "the report names the shader failure and the driver's own log: " + gles.describe());
    recorded->forceCompileFailure = 0;
    Call retry(gles, memory);
    retry.ints({GL_TRIANGLES, 0, 3});
    retry.must("_glDrawArrays");
    check(recorded->drawArraysCalls == 1, "the same draw succeeds once the driver compiles again");
}

}  // namespace

int main(int argc, char** argv) {
    check(argc == 2, "usage: gles-emulation-test <es2-driver-name>");
    const std::string driver = argv[1];
    Memory memory(1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({kGuestPage, 65536, uint8_t(Read | Write), "guest arrays"}, page.data(),
               page.size());
    GLES gles(memory);
    check(gles.load(), "the ES 2.0-only driver is resolved");
    check(gles.backend() == driver, "backend names the library that answered: " + gles.backend());
    Es2StubState* recorded = stub();

    answeredWithoutAFixedFunctionDriver(gles, memory, recorded);
    oneTranslatedDraw(gles, memory, recorded);
    colourArrayAndFixedPoint(gles, memory, recorded);
    indexedDraw(gles, memory, recorded);
    refusesToDrawWithABrokenProgram(gles, memory, recorded);

    std::cout << "gles emulation: an ES 1.1 guest drew through generated ES 2.0 programs over "
              << driver << ", with fixed-point widening, cached variants and a named refusal\n";
}
