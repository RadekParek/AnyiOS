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
#include "runtime/heap.h"
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
void setStubError(uint32_t error) {
    using SetError = void (*)(uint32_t);
    auto set = reinterpret_cast<SetError>(dlsym(RTLD_DEFAULT, "es2_stub_set_error"));
    check(set != nullptr, "the ES 2.0 double exposes error injection");
    set(error);
}
uint32_t bitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
std::string guestString(const Memory& memory, uint32_t address) {
    std::string text;
    for (size_t index = 0; index < 65536; ++index) {
        char value = 0;
        memory.read(address + uint32_t(index), &value, sizeof(value));
        if (!value) break;
        text.push_back(value);
    }
    return text;
}

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
    void stack(std::initializer_list<uint32_t> values) {
        size_t index = 0;
        for (uint32_t value : values) memory.write32(kStack + uint32_t(index++ * 4), value);
    }
    static constexpr uint32_t kStack = 0x10100;
};

constexpr uint32_t GL_TRIANGLES = 0x0004;
constexpr uint32_t GL_TEXTURE_2D = 0x0de1;
constexpr uint32_t GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr uint32_t GL_LINEAR = 0x2601;
constexpr uint32_t GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_FLOAT = 0x1406;
constexpr uint32_t GL_FIXED = 0x140c;
constexpr uint32_t GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701;
constexpr uint32_t GL_TEXTURE_ENV = 0x1703, GL_TEXTURE_ENV_MODE = 0x2200;
constexpr uint32_t GL_REPLACE = 0x1e01, GL_MODULATE = 0x2100;
constexpr uint32_t GL_TEXTURE_MATRIX = 0x1702;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076;
constexpr uint32_t GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_TEXTURE0 = 0x84c0, GL_TEXTURE1 = 0x84c1;
constexpr uint32_t GL_MAX_TEXTURE_UNITS = 0x8482;
constexpr uint32_t GL_MODELVIEW_MATRIX = 0x0ba6, GL_PROJECTION_MATRIX = 0x0ba7;
constexpr uint32_t GL_MODELVIEW_STACK_DEPTH = 0x0ba3, GL_TEXTURE_STACK_DEPTH = 0x0ba5;
constexpr uint32_t GL_MAX_VIEWPORT_DIMS = 0x0d3a, GL_VIEWPORT = 0x0ba2;
constexpr uint32_t GL_PACK_ALIGNMENT = 0x0d05, GL_UNPACK_ALIGNMENT = 0x0cf5;
constexpr uint32_t GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_EXTENSIONS = 0x1f03;
constexpr uint32_t GL_CW = 0x0900, GL_BACK = 0x0405, GL_CULL_FACE = 0x0b44;
constexpr uint32_t GL_LIGHTING = 0x0b50, GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0b51;
constexpr uint32_t GL_LIGHT_MODEL_TWO_SIDE = 0x0b52, GL_LIGHT0 = 0x4000;
constexpr uint32_t GL_NORMALIZE = 0x0ba1, GL_COLOR_MATERIAL = 0x0b57;
constexpr uint32_t GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_POSITION = 0x1203;
constexpr uint32_t GL_SPOT_EXPONENT = 0x1205, GL_FRONT_AND_BACK = 0x0408;
constexpr uint32_t GL_AMBIENT_AND_DIFFUSE = 0x1602, GL_SHININESS = 0x1601;
constexpr uint32_t GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501,
                   GL_INVALID_OPERATION = 0x0502;
constexpr uint32_t GL_FRAMEBUFFER = 0x8d40, GL_FRAMEBUFFER_BINDING = 0x8ca6;
constexpr uint32_t GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_RENDERBUFFER = 0x8d41;
constexpr uint32_t GL_TEXTURE = 0x1702, GL_RGBA4 = 0x8056, GL_RENDERBUFFER_WIDTH = 0x8d42;
constexpr uint32_t GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE = 0x8cd0;
constexpr uint32_t GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME = 0x8cd1;
constexpr uint32_t GL_BUFFER_SIZE = 0x8764;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623, GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645;
constexpr uint32_t GL_VERTEX_SHADER = 0x8b31, GL_COMPILE_STATUS = 0x8b81, GL_LINK_STATUS = 0x8b82;
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

    Call enableTexture(gles, memory);
    enableTexture.ints({GL_TEXTURE_2D});
    enableTexture.must("_glEnable");
    Call disableTexture(gles, memory);
    disableTexture.ints({GL_TEXTURE_2D});
    disableTexture.must("_glDisable");
    check(recorded->enableCalls == 0 && recorded->disableCalls == 0 && recorded->errors == 0,
          "ES 1.1 GL_TEXTURE_2D state is emulated without sending invalid enums to strict ES 2.0");

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

// Matrix operations, per-unit stacks and their guest-visible error/query semantics.
void fixedFunctionMatrixStacks(GLES& gles, Memory& memory) {
    Call mode(gles, memory);
    mode.ints({GL_MODELVIEW});
    mode.must("_glMatrixMode");
    Call identity(gles, memory);
    identity.must("_glLoadIdentity");
    Call translate(gles, memory);
    translate.floats({1, 2, 3});
    translate.must("_glTranslatef");
    Call push(gles, memory);
    push.must("_glPushMatrix");
    Call scale(gles, memory);
    scale.floats({2, 3, 4});
    scale.must("_glScalef");
    Call depth(gles, memory);
    depth.ints({GL_MODELVIEW_STACK_DEPTH, 0x12200});
    depth.must("_glGetIntegerv");
    check(memory.read32(0x12200) == 2,
          "modelview push advances the guest-visible matrix stack depth");
    Call scaledMatrix(gles, memory);
    scaledMatrix.ints({GL_MODELVIEW_MATRIX, 0x12210});
    scaledMatrix.must("_glGetFloatv");
    float matrix[16] = {};
    memory.read(0x12210, matrix, sizeof(matrix));
    check(near(matrix[0], 2) && near(matrix[5], 3) && near(matrix[10], 4) &&
              near(matrix[12], 1) && near(matrix[13], 2) && near(matrix[14], 3),
          "scale multiplies the current column-major matrix without changing translation");
    Call pop(gles, memory);
    pop.must("_glPopMatrix");
    Call restoredMatrix(gles, memory);
    restoredMatrix.ints({GL_MODELVIEW_MATRIX, 0x12210});
    restoredMatrix.must("_glGetFloatv");
    memory.read(0x12210, matrix, sizeof(matrix));
    check(near(matrix[0], 1) && near(matrix[5], 1) && near(matrix[10], 1) &&
              near(matrix[12], 1) && near(matrix[13], 2) && near(matrix[14], 3),
          "pop restores the saved matrix exactly");
    Call underflow(gles, memory);
    underflow.must("_glPopMatrix");
    Call getError(gles, memory);
    getError.must("_glGetError");
    check(getError.state.registers[0] == GL_INVALID_OPERATION,
          "matrix stack underflow reports GL_INVALID_OPERATION to the guest");

    Call projection(gles, memory);
    projection.ints({GL_PROJECTION});
    projection.must("_glMatrixMode");
    Call projectionIdentity(gles, memory);
    projectionIdentity.must("_glLoadIdentity");
    Call orthographic(gles, memory);
    orthographic.floats({-2, 2, -1, 3, 1, 11});
    orthographic.must("_glOrthof");
    Call projectionMatrix(gles, memory);
    projectionMatrix.ints({GL_PROJECTION_MATRIX, 0x12210});
    projectionMatrix.must("_glGetFloatv");
    memory.read(0x12210, matrix, sizeof(matrix));
    check(near(matrix[0], 0.5f) && near(matrix[5], 0.5f) && near(matrix[10], -0.2f) &&
              near(matrix[13], -0.5f) && near(matrix[14], -1.2f) && near(matrix[15], 1),
          "orthographic projection builds the specified OpenGL transform");

    Call activeTexture(gles, memory);
    activeTexture.ints({GL_TEXTURE1});
    activeTexture.must("_glActiveTexture");
    Call textureMode(gles, memory);
    textureMode.ints({GL_TEXTURE_MATRIX});
    textureMode.must("_glMatrixMode");
    Call texturePush(gles, memory);
    texturePush.must("_glPushMatrix");
    Call textureDepth(gles, memory);
    textureDepth.ints({GL_TEXTURE_STACK_DEPTH, 0x12200});
    textureDepth.must("_glGetIntegerv");
    check(memory.read32(0x12200) == 2,
          "texture matrix stack depth is tracked for the selected texture unit");
    Call activeTexture0(gles, memory);
    activeTexture0.ints({GL_TEXTURE0});
    activeTexture0.must("_glActiveTexture");
    Call textureDepth0(gles, memory);
    textureDepth0.ints({GL_TEXTURE_STACK_DEPTH, 0x12200});
    textureDepth0.must("_glGetIntegerv");
    check(memory.read32(0x12200) == 1,
          "each active texture unit owns its own texture matrix stack");
    Call modelview(gles, memory);
    modelview.ints({GL_MODELVIEW});
    modelview.must("_glMatrixMode");
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

// Texture enable, environment mode, texture matrix, and coordinates belong to the active unit.
// These draws exercise unit one on its own; they do not claim simultaneous multitexture support.
void textureUnitState(GLES& gles, Memory& memory, Es2StubState* recorded) {
    resetStub();
    float textureMatrix0[16] = {}, textureMatrix1[16] = {};
    textureMatrix0[0] = textureMatrix0[5] = textureMatrix0[10] = textureMatrix0[15] = 1;
    textureMatrix0[12] = 0.25f;
    textureMatrix1[0] = textureMatrix1[5] = textureMatrix1[10] = textureMatrix1[15] = 1;
    textureMatrix1[12] = 0.75f;

    Call active0(gles, memory);
    active0.ints({GL_TEXTURE0});
    active0.must("_glActiveTexture");
    Call textureMatrixMode(gles, memory);
    textureMatrixMode.ints({GL_TEXTURE_MATRIX});
    textureMatrixMode.must("_glMatrixMode");
    memory.write(kGuestPage + 0x400, textureMatrix0, sizeof(textureMatrix0));
    Call loadTextureMatrix0(gles, memory);
    loadTextureMatrix0.ints({kGuestPage + 0x400});
    loadTextureMatrix0.must("_glLoadMatrixf");
    Call enableTexture0(gles, memory);
    enableTexture0.ints({GL_TEXTURE_2D});
    enableTexture0.must("_glEnable");
    Call env0(gles, memory);
    env0.ints({GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE});
    env0.must("_glTexEnvi");
    Call client0(gles, memory);
    client0.ints({GL_TEXTURE0});
    client0.must("_glClientActiveTexture");
    Call enableCoords0(gles, memory);
    enableCoords0.ints({GL_TEXTURE_COORD_ARRAY});
    enableCoords0.must("_glEnableClientState");
    const float coords0[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    memory.write(kGuestPage + 0x500, coords0, sizeof(coords0));
    Call pointerCoords0(gles, memory);
    pointerCoords0.ints({2, GL_FLOAT, 0, kGuestPage + 0x500});
    pointerCoords0.must("_glTexCoordPointer");

    Call active1(gles, memory);
    active1.ints({GL_TEXTURE1});
    active1.must("_glActiveTexture");
    memory.write(kGuestPage + 0x400, textureMatrix1, sizeof(textureMatrix1));
    Call loadTextureMatrix1(gles, memory);
    loadTextureMatrix1.ints({kGuestPage + 0x400});
    loadTextureMatrix1.must("_glLoadMatrixf");
    Call disableTexture1(gles, memory);
    disableTexture1.ints({GL_TEXTURE_2D});
    disableTexture1.must("_glDisable");
    Call env1(gles, memory);
    env1.ints({GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE});
    env1.must("_glTexEnvi");
    Call client1(gles, memory);
    client1.ints({GL_TEXTURE1});
    client1.must("_glClientActiveTexture");
    Call enableCoords1(gles, memory);
    enableCoords1.ints({GL_TEXTURE_COORD_ARRAY});
    enableCoords1.must("_glEnableClientState");
    const float coords1[6] = {0.8f, 0.9f, 0.7f, 0.6f, 0.5f, 0.4f};
    memory.write(kGuestPage + 0x600, coords1, sizeof(coords1));
    Call pointerCoords1(gles, memory);
    pointerCoords1.ints({2, GL_FLOAT, 0, kGuestPage + 0x600});
    pointerCoords1.must("_glTexCoordPointer");

    const float vertices[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    memory.write(kGuestPage, vertices, sizeof(vertices));
    Call enableVertices(gles, memory);
    enableVertices.ints({GL_VERTEX_ARRAY});
    enableVertices.must("_glEnableClientState");
    Call vertexPointer(gles, memory);
    vertexPointer.ints({3, GL_FLOAT, 0, kGuestPage});
    vertexPointer.must("_glVertexPointer");

    // Unit one is disabled while unit zero is enabled. Its draw must remain untextured.
    Call drawUnit1Disabled(gles, memory);
    drawUnit1Disabled.ints({GL_TRIANGLES, 0, 3});
    drawUnit1Disabled.must("_glDrawArrays");
    check(recorded->drawArraysCalls == 1 && recorded->textureMatrixUploads == 0 &&
              recorded->samplerUploads == 0,
          "unit one's disabled texture state is independent of enabled unit zero");

    Call selectActive0(gles, memory);
    selectActive0.ints({GL_TEXTURE0});
    selectActive0.must("_glActiveTexture");
    Call drawUnit0(gles, memory);
    drawUnit0.ints({GL_TRIANGLES, 0, 3});
    drawUnit0.must("_glDrawArrays");
    check(recorded->textureMatrixUploads == 1 && near(recorded->textureMatrix[12], 0.25f) &&
              recorded->samplerUploads == 1 && recorded->samplerUnit == 0,
          "unit zero uses its own texture matrix and sampler (matrix uploads=" +
              std::to_string(recorded->textureMatrixUploads) +
              ", matrix translation=" + std::to_string(recorded->textureMatrix[12]) +
              ", sampler uploads=" + std::to_string(recorded->samplerUploads) +
              ", sampler unit=" + std::to_string(recorded->samplerUnit) + ")");
    check(std::string(recorded->fragmentSource).find(" * vColor") == std::string::npos,
          "unit zero's GL_REPLACE environment does not inherit unit one's GL_MODULATE mode");
    check(near(recorded->lastUploadVertex[0], coords0[0]) &&
              near(recorded->lastUploadVertex[1], coords0[1]),
          "unit zero's client texture-coordinate array is selected for its draw");

    // Enabling unit one now selects its own matrix, environment mode, coordinate array and
    // sampler. The bridge still has one shader stage and makes that limitation explicit.
    Call selectActive1(gles, memory);
    selectActive1.ints({GL_TEXTURE1});
    selectActive1.must("_glActiveTexture");
    Call enableTexture1(gles, memory);
    enableTexture1.ints({GL_TEXTURE_2D});
    enableTexture1.must("_glEnable");
    Call drawUnit1Enabled(gles, memory);
    drawUnit1Enabled.ints({GL_TRIANGLES, 0, 3});
    drawUnit1Enabled.must("_glDrawArrays");
    check(recorded->textureMatrixUploads == 2 && near(recorded->textureMatrix[12], 0.75f) &&
              recorded->samplerUploads == 2 && recorded->samplerUnit == 1,
          "unit one uses its own texture matrix and binds its own sampler unit");
    check(std::string(recorded->fragmentSource).find(" * vColor") != std::string::npos,
          "unit one's GL_MODULATE environment is independent of unit zero's GL_REPLACE");
    check(near(recorded->lastUploadVertex[0], coords1[0]) &&
              near(recorded->lastUploadVertex[1], coords1[1]),
          "unit one's client texture-coordinate array is selected for its draw");
    const std::string report = gles.describe();
    check(report.find("simultaneous texture stages are not emulated") != std::string::npos,
          "the runtime reports that separate unit state does not implement simultaneous stages");
    check(
        report.find("1 draw(s) had both fixed-function texture units enabled") != std::string::npos,
        "the runtime counts overlap rather than implying it emulated two active stages");

    Call restoreActive0(gles, memory);
    restoreActive0.ints({GL_TEXTURE0});
    restoreActive0.must("_glActiveTexture");
    Call disableTexture0(gles, memory);
    disableTexture0.ints({GL_TEXTURE_2D});
    disableTexture0.must("_glDisable");
    Call restoreClient0(gles, memory);
    restoreClient0.ints({GL_TEXTURE0});
    restoreClient0.must("_glClientActiveTexture");
    Call restoreMatrixMode(gles, memory);
    restoreMatrixMode.ints({GL_MODELVIEW});
    restoreMatrixMode.must("_glMatrixMode");
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


void shaderUniformAndGenericAttributeImports() {
    Memory memory(1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({kGuestPage, 65536, uint8_t(Read | Write), "guest shader data"}, page.data(),
               page.size());
    GLES gles(memory);
    check(gles.load(), "the ES 2.0 driver resolves shader and generic-attribute imports");
    resetStub();
    Es2StubState* recorded = stub();
    gles.setClientApi(2);
    check(gles.makeCurrent(), "guest shader calls run with a real ES 2.0 context");

    const std::string shaderText = "void main() { }";
    memory.write(0x11200, shaderText.data(), shaderText.size());
    memory.write32(0x11100, 0x11200);
    memory.write32(0x11300, uint32_t(shaderText.size()));
    Call createShader(gles, memory);
    createShader.ints({GL_VERTEX_SHADER});
    createShader.must("_glCreateShader");
    const uint32_t shader = createShader.state.registers[0];
    check(shader != 0, "glCreateShader returns the driver's shader name");
    Call source(gles, memory);
    source.ints({shader, 1, 0x11100, 0x11300});
    source.must("_glShaderSource");
    check(recorded->shaderSourceCalls == 1 && std::string(recorded->vertexSource) == shaderText,
          "glShaderSource copies the guest pointer array and honors explicit source lengths");
    Call compile(gles, memory);
    compile.ints({shader});
    compile.must("_glCompileShader");
    memory.write32(0x11400, 0xdeadbeef);
    Call shaderStatus(gles, memory);
    shaderStatus.ints({shader, GL_COMPILE_STATUS, 0x11400});
    shaderStatus.must("_glGetShaderiv");
    check(memory.read32(0x11400) == 1 && recorded->getShaderCalls == 1,
          "glGetShaderiv writes the driver status into guest memory");
    memory.write32(0x11404, 0xdeadbeef);
    memory.write(0x11500, "XXXXXXXX", 8);
    Call shaderLog(gles, memory);
    shaderLog.ints({shader, 8, 0x11404, 0x11500});
    shaderLog.must("_glGetShaderInfoLog");
    uint8_t shaderLogByte = 1;
    memory.read(0x11500, &shaderLogByte, sizeof(shaderLogByte));
    check(memory.read32(0x11404) == 0 && shaderLogByte == 0 && recorded->shaderInfoLogCalls == 1,
          "glGetShaderInfoLog safely copies both its length and output buffer");

    Call createProgram(gles, memory);
    createProgram.must("_glCreateProgram");
    const uint32_t program = createProgram.state.registers[0];
    check(program != 0, "glCreateProgram returns a driver object name");
    Call attach(gles, memory);
    attach.ints({program, shader});
    attach.must("_glAttachShader");
    const char attributeName[] = "aPosition";
    memory.write(0x11600, attributeName, sizeof(attributeName));
    Call bindAttribute(gles, memory);
    bindAttribute.ints({program, 5, 0x11600});
    bindAttribute.must("_glBindAttribLocation");
    Call link(gles, memory);
    link.ints({program});
    link.must("_glLinkProgram");
    memory.write32(0x11408, 0xdeadbeef);
    Call programStatus(gles, memory);
    programStatus.ints({program, GL_LINK_STATUS, 0x11408});
    programStatus.must("_glGetProgramiv");
    check(memory.read32(0x11408) == 1 && recorded->getProgramCalls == 1,
          "glGetProgramiv writes the driver's link status into guest memory");
    memory.write32(0x1140c, 0xdeadbeef);
    memory.write(0x11510, "XXXXXXXX", 8);
    Call programLog(gles, memory);
    programLog.ints({program, 8, 0x1140c, 0x11510});
    programLog.must("_glGetProgramInfoLog");
    uint8_t programLogByte = 1;
    memory.read(0x11510, &programLogByte, sizeof(programLogByte));
    check(memory.read32(0x1140c) == 0 && programLogByte == 0 &&
              recorded->programInfoLogCalls == 1,
          "glGetProgramInfoLog copies a safe, terminated driver log to guest memory");
    Call attributeLocation(gles, memory);
    attributeLocation.ints({program, 0x11600});
    attributeLocation.must("_glGetAttribLocation");
    check(int(attributeLocation.state.registers[0]) == 5 && recorded->attribLocationCalls == 1,
          "glGetAttribLocation marshals a guest C string and returns the driver's location");
    Call uniformLocation(gles, memory);
    uniformLocation.ints({program, 0x11620});
    memory.write(0x11620, "uTest", 6);
    uniformLocation.must("_glGetUniformLocation");
    check(int(uniformLocation.state.registers[0]) >= 0,
          "glGetUniformLocation returns the driver's result for a guest string");
    Call useProgram(gles, memory);
    useProgram.ints({program});
    useProgram.must("_glUseProgram");
    Call activeAttrib(gles, memory);
    activeAttrib.ints({program, 0, 16, 0x11720});
    activeAttrib.stack({0x11724, 0x11728, 0x1172c});
    activeAttrib.must("_glGetActiveAttrib");
    Call activeUniform(gles, memory);
    activeUniform.ints({program, 0, 16, 0x11740});
    activeUniform.stack({0x11744, 0x11748, 0x1174c});
    activeUniform.must("_glGetActiveUniform");
    char activeUniformName[8] = {};
    memory.read(0x1174c, activeUniformName, sizeof(activeUniformName));
    check(memory.read32(0x11740) == 7 && memory.read32(0x11744) == 2 &&
              memory.read32(0x11748) == 0x8b50 && std::strcmp(activeUniformName, "uVec[0]") == 0,
          "active shader queries safely copy type, size, length and bounded names to guest memory");
    const char vectorUniformName[] = "uVec[0]";
    memory.write(0x11630, vectorUniformName, sizeof(vectorUniformName));
    Call vectorUniformLocation(gles, memory);
    vectorUniformLocation.ints({program, 0x11630});
    vectorUniformLocation.must("_glGetUniformLocation");
    const int queriedLocation = int(vectorUniformLocation.state.registers[0]);
    Call getUniformFloat(gles, memory);
    getUniformFloat.ints({program, uint32_t(queriedLocation), 0x11760});
    getUniformFloat.must("_glGetUniformfv");
    Call getUniformInt(gles, memory);
    getUniformInt.ints({program, uint32_t(queriedLocation), 0x11768});
    getUniformInt.must("_glGetUniformiv");
    check(memory.read32(0x11760) == bitsOf(1.5f) && memory.read32(0x11764) == bitsOf(2.5f) &&
              memory.read32(0x11768) == 5 && memory.read32(0x1176c) == 6 &&
              recorded->uniformQueries == 2,
          "uniform queries derive the exact vector extent from active-uniform metadata");
    Call attachedShaders(gles, memory);
    attachedShaders.ints({program, 2, 0x11780, 0x11784});
    attachedShaders.must("_glGetAttachedShaders");
    Call precision(gles, memory);
    precision.ints({GL_VERTEX_SHADER, 0x8df0, 0x11790, 0x11798});
    precision.must("_glGetShaderPrecisionFormat");
    Call shaderSourceQuery(gles, memory);
    shaderSourceQuery.ints({shader, 16, 0x117a0, 0x117a4});
    shaderSourceQuery.must("_glGetShaderSource");
    check(memory.read32(0x11780) == 2 && memory.read32(0x11784) == 100 &&
              memory.read32(0x11788) == 101 && memory.read32(0x11790) == uint32_t(-126) &&
              memory.read32(0x11794) == 127 && memory.read32(0x11798) == 23 &&
              memory.read32(0x117a0) > 0,
          "shader metadata, precision and attached-object queries preserve output extents");

    Call uniform3f(gles, memory);
    uniform3f.ints({4});
    uniform3f.floats({0.25f, 0.5f, 0.75f});
    uniform3f.must("_glUniform3f");
    check(recorded->scalarUniformCalls == 1 && recorded->scalarUniformComponents == 3 &&
              recorded->scalarUniformInteger == 0 && near(recorded->scalarUniformValues[2], 0.75f),
          "glUniform3f reads independent AAPCS-VFP float registers in order");
    Call uniform2i(gles, memory);
    uniform2i.ints({4, 12, 34});
    uniform2i.must("_glUniform2i");
    check(recorded->scalarUniformCalls == 2 && recorded->scalarUniformInteger == 1 &&
              recorded->scalarUniformValues[0] == 12 && recorded->scalarUniformValues[1] == 34,
          "integer scalar uniforms preserve the location and value registers");

    const float vectorValues[] = {1, 2, 3, 4, 5, 6};
    memory.write(0x12000, vectorValues, sizeof(vectorValues));
    Call vectorUniform(gles, memory);
    vectorUniform.ints({4, 2, 0x12000});
    vectorUniform.must("_glUniform3fv");
    check(recorded->vectorUniformCalls == 1 && recorded->vectorUniformComponents == 3 &&
              recorded->vectorUniformCount == 2 && near(recorded->vectorUniformValues[5], 6.0f),
          "vector uniform arrays are copied at their full component count from guest memory");
    const int32_t integerValues[] = {10, 20, 30, 40};
    memory.write(0x12100, integerValues, sizeof(integerValues));
    Call integerVector(gles, memory);
    integerVector.ints({4, 2, 0x12100});
    integerVector.must("_glUniform2iv");
    check(recorded->vectorUniformCalls == 2 && recorded->vectorUniformInteger == 1 &&
              recorded->vectorUniformInts[3] == 40,
          "integer uniform arrays use signed 32-bit guest values and the exact vector size");
    const float matrixValues[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    memory.write(0x12200, matrixValues, sizeof(matrixValues));
    Call matrixUniform(gles, memory);
    matrixUniform.ints({4, 1, 0, 0x12200});
    matrixUniform.must("_glUniformMatrix3fv");
    check(recorded->matrixUniformCalls == 1 && recorded->matrixUniformDimension == 3 &&
              recorded->matrixUniformTranspose == 0 && near(recorded->matrixUniformValues[8], 9),
          "matrix uniforms copy count, transpose and all matrix values from guest memory");

    const float attributes[] = {0.125f, 0.25f, 0.5f, 0.75f};
    memory.write(0x12300, attributes, sizeof(attributes));
    Call pointer(gles, memory);
    pointer.ints({5, 2, GL_FLOAT, 0});
    pointer.stack({0, 0x12300});
    pointer.must("_glVertexAttribPointer");
    Call enable(gles, memory);
    enable.ints({5});
    enable.must("_glEnableVertexAttribArray");
    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 2});
    draw.must("_glDrawArrays");
    check(recorded->clientAttribPointerCalls == 1 && recorded->lastClientAttribIndex == 5 &&
              near(recorded->clientAttribValues[0], 0.125f) &&
              near(recorded->clientAttribValues[1], 0.25f) && recorded->drawArraysCalls == 1,
          "client generic attributes are captured and copied before the driver draw");
    Call queryAttributeSize(gles, memory);
    queryAttributeSize.ints({5, GL_VERTEX_ATTRIB_ARRAY_SIZE, 0x12600});
    queryAttributeSize.must("_glGetVertexAttribiv");
    Call queryAttributePointer(gles, memory);
    queryAttributePointer.ints({5, GL_VERTEX_ATTRIB_ARRAY_POINTER, 0x12604});
    queryAttributePointer.must("_glGetVertexAttribPointerv");
    check(memory.read32(0x12600) == 2 && memory.read32(0x12604) == 0x12300,
          "generic attribute queries return the guest's recorded size and pointer, not host memory");
    Call queryCurrentAttribute(gles, memory);
    queryCurrentAttribute.ints({7, 0x8626, 0x12640});
    queryCurrentAttribute.must("_glGetVertexAttribfv");
    check(memory.read32(0x1264c) == bitsOf(1.0f),
          "current generic attribute query copies all four driver values to guest memory");
    const float currentAttribute[] = {0.25f, 0.5f, 0.75f, 1.0f};
    memory.write(0x12620, currentAttribute, sizeof(currentAttribute));
    Call constantAttribute(gles, memory);
    constantAttribute.ints({7, 0x12620});
    constantAttribute.must("_glVertexAttrib4fv");
    check(recorded->attribVectorCalls == 1 && recorded->lastAttribVectorIndex == 7 &&
              recorded->lastAttribVectorComponents == 4 &&
              near(recorded->lastAttribVector[3], 1.0f),
          "vector-form vertex attributes copy guest constants before forwarding");

    uint16_t indices[] = {0, 1, 2};
    uint16_t replacementIndex = 3;
    memory.write(0x12400, indices, sizeof(indices));
    memory.write(0x12420, &replacementIndex, sizeof(replacementIndex));
    Call generateBuffer(gles, memory);
    generateBuffer.ints({1, 0x12500});
    generateBuffer.must("_glGenBuffers");
    const uint32_t elementBuffer = memory.read32(0x12500);
    Call bindElement(gles, memory);
    bindElement.ints({GL_ELEMENT_ARRAY_BUFFER, elementBuffer});
    bindElement.must("_glBindBuffer");
    Call data(gles, memory);
    data.ints({GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), 0x12400, 0x88e4});
    data.must("_glBufferData");
    Call subData(gles, memory);
    subData.ints({GL_ELEMENT_ARRAY_BUFFER, 4, 2, 0x12420});
    subData.must("_glBufferSubData");
    check(recorded->bufferSubDataCalls == 1 && recorded->lastSubDataTarget == GL_ELEMENT_ARRAY_BUFFER &&
              recorded->lastSubDataOffset == 4 && recorded->lastSubDataBytes == 2,
          "glBufferSubData preserves signed offsets and copies the guest payload");
    Call bufferSize(gles, memory);
    bufferSize.ints({GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, 0x12520});
    bufferSize.must("_glGetBufferParameteriv");
    check(memory.read32(0x12520) == sizeof(indices) && recorded->bufferParameterQueries == 1 &&
              recorded->lastBufferParameterTarget == GL_ELEMENT_ARRAY_BUFFER &&
              recorded->lastBufferParameterPname == GL_BUFFER_SIZE,
          "glGetBufferParameteriv copies the driver buffer-size result into guest memory");

    // Indices now include vertex 3, so the client attribute copy must cover four vertices.
    // Three vertices fit before the end of the guest page; the fourth does not. This tests the
    // bridge's shadow of element-buffer contents rather than letting the driver overread a host
    // pointer that happens to be adjacent to mapped memory.
    const float edgeAttributes[6] = {1, 2, 3, 4, 5, 6};
    memory.write(0x1ffe8, edgeAttributes, sizeof(edgeAttributes));
    Call edgePointer(gles, memory);
    edgePointer.ints({5, 2, GL_FLOAT, 0});
    edgePointer.stack({0, 0x1ffe8});
    edgePointer.must("_glVertexAttribPointer");
    Call edgeDraw(gles, memory);
    edgeDraw.ints({GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0});
    bool fourthVertexRejected = false;
    try {
        (void)edgeDraw("_glDrawElements");
    } catch (const Error& error) {
        fourthVertexRejected = error.code == "MEMORY_UNMAPPED" && error.address == 0x20000;
    }
    check(fourthVertexRejected && recorded->drawElementsCalls == 0,
          "captured element indices make a client-array boundary fault visible before the driver draw");
    const float expandedAttributes[] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f};
    memory.write(0x12300, expandedAttributes, sizeof(expandedAttributes));
    Call safePointer(gles, memory);
    safePointer.ints({5, 2, GL_FLOAT, 0});
    safePointer.stack({0, 0x12300});
    safePointer.must("_glVertexAttribPointer");
    Call indexed(gles, memory);
    indexed.ints({GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0});
    indexed.must("_glDrawElements");
    check(recorded->drawElementsCalls == 1 && recorded->drawLog[1].type == GL_UNSIGNED_SHORT,
          "the captured element-buffer data supports safe indexed client-array draws");

    Call invalidSource(gles, memory);
    invalidSource.ints({shader, 1, 0x1fffe, 0});
    bool badPointerRejected = false;
    try {
        (void)invalidSource("_glShaderSource");
    } catch (const Error& error) {
        badPointerRejected = error.code == "GLES_ARGUMENT" && error.address == 0x1fffe;
    }
    check(badPointerRejected, "shader source rejects an unreadable guest pointer instead of forwarding it");
    Call invalidCount(gles, memory);
    invalidCount.ints({4, 0xffffffffu, 0x12000});
    bool badCountRejected = false;
    try {
        (void)invalidCount("_glUniform3fv");
    } catch (const Error& error) {
        badCountRejected = error.code == "GLES_ARGUMENT";
    }
    check(badCountRejected, "uniform vectors reject negative counts before computing a byte size");

    GuestHeap exhaustedHeap(memory, kGuestPage, 65536);
    GLES noStringHeap(memory, &exhaustedHeap);
    check(noStringHeap.load(), "glGetString failure handling keeps the real GLES driver");
    Call unavailableString(noStringHeap, memory);
    unavailableString.ints({0x1f02});  // GL_VERSION
    unavailableString.must("_glGetString");
    check(unavailableString.state.registers[0] == 0 &&
              noStringHeap.describe().find("guest heap could not hold") != std::string::npos,
          "glGetString reports a guest allocation failure instead of leaking a host pointer");
}

void fixedFunctionLighting() {
    Memory memory(1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({kGuestPage, 65536, uint8_t(Read | Write), "guest lighting data"}, page.data(),
               page.size());
    GLES gles(memory);
    check(gles.load(), "fixed-function lighting resolves over an ES 2.0-only driver");
    resetStub();
    Es2StubState* recorded = stub();
    gles.setClientApi(1);
    check(gles.makeCurrent(), "the ES 1.1 lighting guest receives the ES 2.0 emulation context");

    Call enableLighting(gles, memory);
    enableLighting.ints({GL_LIGHTING});
    enableLighting.must("_glEnable");
    Call enableLight(gles, memory);
    enableLight.ints({GL_LIGHT0});
    enableLight.must("_glEnable");
    Call enableTwoSided(gles, memory);
    enableTwoSided.ints({GL_LIGHT_MODEL_TWO_SIDE, 1});
    enableTwoSided.must("_glLightModeli");
    Call enableLocalViewer(gles, memory);
    enableLocalViewer.ints({GL_LIGHT_MODEL_LOCAL_VIEWER, 1});
    enableLocalViewer.must("_glLightModeli");

    const float lightPosition[] = {0, 0, 2, 1};
    const float lightAmbient[] = {0.1f, 0.2f, 0.3f, 1};
    const float lightDiffuse[] = {0.25f, 0.5f, 0.75f, 1};
    const float material[] = {0.8f, 0.7f, 0.6f, 1};
    memory.write(0x11000, lightPosition, sizeof(lightPosition));
    memory.write(0x11020, lightAmbient, sizeof(lightAmbient));
    memory.write(0x11040, lightDiffuse, sizeof(lightDiffuse));
    memory.write(0x11060, material, sizeof(material));
    Call position(gles, memory);
    position.ints({GL_LIGHT0, GL_POSITION, 0x11000});
    position.must("_glLightfv");
    Call ambient(gles, memory);
    ambient.ints({GL_LIGHT0, GL_AMBIENT, 0x11020});
    ambient.must("_glLightfv");
    Call diffuse(gles, memory);
    diffuse.ints({GL_LIGHT0, GL_DIFFUSE, 0x11040});
    diffuse.must("_glLightfv");
    Call materialValues(gles, memory);
    materialValues.ints({GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, 0x11060});
    materialValues.must("_glMaterialfv");
    Call shininess(gles, memory);
    shininess.ints({GL_FRONT_AND_BACK, GL_SHININESS});
    shininess.floats({32});
    shininess.must("_glMaterialf");
    const float scaledModelview[] = {2, 0, 0, 0, 0, 3, 0, 0,
                                     0, 0, 4, 0, 0, 0, 0, 1};
    memory.write(0x11080, scaledModelview, sizeof(scaledModelview));
    Call modelviewMode(gles, memory);
    modelviewMode.ints({GL_MODELVIEW});
    modelviewMode.must("_glMatrixMode");
    Call loadModelview(gles, memory);
    loadModelview.ints({0x11080});
    loadModelview.must("_glLoadMatrixf");

    const float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    memory.write(0x11100, vertices, sizeof(vertices));
    memory.write(0x11200, normals, sizeof(normals));
    Call enableVertex(gles, memory);
    enableVertex.ints({GL_VERTEX_ARRAY});
    enableVertex.must("_glEnableClientState");
    Call vertexPointer(gles, memory);
    vertexPointer.ints({3, GL_FLOAT, 0, 0x11100});
    vertexPointer.must("_glVertexPointer");
    Call enableNormal(gles, memory);
    enableNormal.ints({GL_NORMAL_ARRAY});
    enableNormal.must("_glEnableClientState");
    Call normalPointer(gles, memory);
    normalPointer.ints({GL_FLOAT, 0, 0x11200});
    normalPointer.must("_glNormalPointer");

    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 3});
    draw.must("_glDrawArrays");
    check(recorded->drawArraysCalls == 1 && recorded->compileCalls == 2 &&
              recorded->linkCalls == 1 && recorded->matrixUniformDimension == 3 &&
              near(recorded->matrixUniformValues[0], 0.5f) &&
              near(recorded->matrixUniformValues[4], 1.0f / 3.0f) &&
              near(recorded->matrixUniformValues[8], 0.25f),
          "lighting draws use the modelview inverse-transpose normal matrix");
    check(std::string(recorded->vertexSource).find("uLightPosition[0]") != std::string::npos &&
              std::string(recorded->vertexSource).find("uLightDiffuse[7]") != std::string::npos &&
              std::string(recorded->vertexSource).find("vBackColor") != std::string::npos,
          "the generated ES 2.0 shader includes light, normal and two-sided material semantics");
    check(recorded->lightAmbientUploads == 1 && recorded->lightDiffuseUploads == 1 &&
              recorded->lightPositionUploads == 1 &&
              near(recorded->lightDiffuseValues[0], 0.25f) &&
              near(recorded->lightDiffuseValues[1], 0.5f) &&
              near(recorded->lightPositionValues[2], 2.0f),
          "GL_LIGHT0 parameters are transformed and uploaded to the generated shader");
    check(gles.describe().find("fixed-function lighting enabled with 1 light") != std::string::npos,
          "the launch report says how many ES 1.1 lights the emulation actually enabled");

    Call isEnabled(gles, memory);
    isEnabled.ints({GL_LIGHT0});
    isEnabled.must("_glIsEnabled");
    check(isEnabled.state.registers[0] == 1,
          "glIsEnabled reports the guest's emulated GL_LIGHT0 state");
    Call invalidExponent(gles, memory);
    invalidExponent.ints({GL_LIGHT0, GL_SPOT_EXPONENT});
    invalidExponent.floats({129});
    invalidExponent.must("_glLightf");
    Call getError(gles, memory);
    getError.must("_glGetError");
    check(getError.state.registers[0] == GL_INVALID_VALUE,
          "invalid emulated light parameters reach the guest through glGetError");
}

void coreAliasesQueriesReadbackAndTracing(const std::string& driver) {
    Memory memory(16 * 1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({kGuestPage, 65536, uint8_t(Read | Write), "guest GL data"}, page.data(), page.size());
    GuestHeap heap(memory);
    GLES gles(memory, &heap);
    check(gles.load(), "the ES 2.0 driver resolves the expanded GLES entry-point set");
    check(gles.backend() == driver, "the expanded path keeps the selected GLES backend");
    resetStub();
    Es2StubState* recorded = stub();
    gles.setClientApi(2);
    check(gles.createDrawable(8, 4), "the ES 2.0 context has a real FBO-backed drawable");

    // ES 2.0 games import core framebuffer names; ES 1.1 games import the OES suffixes.
    // Both must update the same guest-visible binding and preserve private framebuffer zero.
    Call generate(gles, memory);
    generate.ints({1, 0x10200});
    generate.must("_glGenFramebuffers");
    const uint32_t guestFramebuffer = memory.read32(0x10200);
    check(guestFramebuffer != 0, "the core glGenFramebuffers alias writes a driver name to guest memory");
    Call bindGuest(gles, memory);
    bindGuest.ints({GL_FRAMEBUFFER, guestFramebuffer});
    bindGuest.must("_glBindFramebuffer");
    check(recorded->boundFramebuffer == guestFramebuffer,
          "the core framebuffer bind reaches the ES 2.0 driver");
    memory.write32(0x10204, 0xdeadbeef);
    Call queryBinding(gles, memory);
    queryBinding.ints({GL_FRAMEBUFFER_BINDING, 0x10204});
    queryBinding.must("_glGetIntegerv");
    check(memory.read32(0x10204) == guestFramebuffer,
          "GL_FRAMEBUFFER_BINDING reports the guest name, not the private drawable FBO");
    Call bindDefault(gles, memory);
    bindDefault.ints({GL_FRAMEBUFFER, 0});
    bindDefault.must("_glBindFramebuffer");
    check(recorded->boundFramebuffer != 0 && recorded->boundFramebuffer != guestFramebuffer,
          "binding guest framebuffer zero restores the private drawable object");
    Call status(gles, memory);
    status.ints({GL_FRAMEBUFFER});
    status.must("_glCheckFramebufferStatus");
    check(status.state.registers[0] == 0x8cd5,
          "the core framebuffer status query returns the driver's complete status");
    Call generateRenderbuffer(gles, memory);
    generateRenderbuffer.ints({1, 0x10230});
    generateRenderbuffer.must("_glGenRenderbuffers");
    const uint32_t guestRenderbuffer = memory.read32(0x10230);
    Call bindRenderbuffer(gles, memory);
    bindRenderbuffer.ints({GL_RENDERBUFFER, guestRenderbuffer});
    bindRenderbuffer.must("_glBindRenderbuffer");
    const int renderbufferStorageBefore = recorded->renderbufferStorageCalls;
    Call storage(gles, memory);
    storage.ints({GL_RENDERBUFFER, GL_RGBA4, 8, 4});
    storage.must("_glRenderbufferStorage");
    Call renderbufferWidth(gles, memory);
    renderbufferWidth.ints({GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, 0x10234});
    renderbufferWidth.must("_glGetRenderbufferParameteriv");
    check(recorded->renderbufferStorageCalls == renderbufferStorageBefore + 1 &&
              recorded->renderbufferWidth == 8 &&
              recorded->renderbufferHeight == 4 && memory.read32(0x10234) == 8,
          "renderbuffer storage and size queries preserve dimensions across the core/OES bridge");
    const int framebufferRenderbufferBefore = recorded->framebufferRenderbufferCalls;
    const int framebufferTextureBefore = recorded->framebufferTextureCalls;
    Call attachRenderbuffer(gles, memory);
    attachRenderbuffer.ints({GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, guestRenderbuffer});
    attachRenderbuffer.must("_glFramebufferRenderbuffer");
    Call attachTexture(gles, memory);
    attachTexture.ints({GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 77});
    attachTexture.stack({0});
    attachTexture.must("_glFramebufferTexture2D");
    Call attachmentType(gles, memory);
    attachmentType.ints({GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                         0x10238});
    attachmentType.must("_glGetFramebufferAttachmentParameteriv");
    Call attachmentName(gles, memory);
    attachmentName.ints({GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                         0x1023c});
    attachmentName.must("_glGetFramebufferAttachmentParameteriv");
    check(recorded->framebufferRenderbufferCalls == framebufferRenderbufferBefore + 1 &&
              recorded->framebufferTextureCalls == framebufferTextureBefore + 1 &&
              recorded->lastFramebufferTarget == GL_FRAMEBUFFER &&
              recorded->lastAttachment == GL_COLOR_ATTACHMENT0 &&
              memory.read32(0x10238) == GL_TEXTURE && memory.read32(0x1023c) == 77 &&
              recorded->framebufferAttachmentQueries == 2,
          "framebuffer attachments and parameter queries reach the driver with guest values intact");

    // Multi-value queries copy the full GL result, not only the first word. This is a common
    // startup query for viewport sizing and used to leave the guest's second value stale.
    memory.write32(0x10210, 0xdeadbeef);
    memory.write32(0x10214, 0xdeadbeef);
    Call maxViewport(gles, memory);
    maxViewport.ints({GL_MAX_VIEWPORT_DIMS, 0x10210});
    maxViewport.must("_glGetIntegerv");
    check(memory.read32(0x10210) == 4096 && memory.read32(0x10214) == 4096,
          "glGetIntegerv copies the complete two-element GL_MAX_VIEWPORT_DIMS result");
    Call viewport(gles, memory);
    viewport.ints({GL_VIEWPORT, 0x10220});
    viewport.must("_glGetFloatv");
    float viewportValues[4] = {};
    memory.read(0x10220, viewportValues, sizeof(viewportValues));
    check(viewportValues[0] == 0.0f && viewportValues[1] == 0.0f &&
              viewportValues[2] == 320.0f && viewportValues[3] == 480.0f,
          "glGetFloatv safely copies all four viewport values to guest memory");

    // Driver strings are host pointers and must be copied into the emulated address space.
    Call extensions(gles, memory);
    extensions.ints({GL_EXTENSIONS});
    extensions.must("_glGetString");
    const uint32_t extensionPointer = extensions.state.registers[0];
    check(extensionPointer && memory.permits(extensionPointer, 1, Read),
          "glGetString returns a readable guest pointer rather than a host address");
    const std::string extensionText = guestString(memory, extensionPointer);
    check(extensionText.find("GL_TEST_recording") != std::string::npos &&
              extensionText.find("GL_OES_framebuffer_object") != std::string::npos &&
              extensionText.find("GL_IMG_texture_compression_pvrtc") != std::string::npos,
          "the virtual extension list preserves driver features and advertises implemented bridges");
    Call extensionsAgain(gles, memory);
    extensionsAgain.ints({GL_EXTENSIONS});
    extensionsAgain.must("_glGetString");
    check(extensionsAgain.state.registers[0] == extensionPointer,
          "repeated glGetString calls return the same stable guest string");

    // Pixel rows obey the guest's GL_PACK_ALIGNMENT / GL_UNPACK_ALIGNMENT state. A 1-pixel
    // RGB row is three bytes, padded to four by default; neither readback nor upload may
    // compact the rows and shift every pixel after the first.
    Call read(gles, memory);
    read.ints({1, 2, 1, 2});
    read.stack({GL_RGB, GL_UNSIGNED_BYTE, 0x10300});
    read.must("_glReadPixels");
    check(recorded->readPixelsCalls == 1 && recorded->readPixelsWidth == 1 &&
              recorded->readPixelsHeight == 2 && recorded->readPixelsBytes == 7 &&
              memory.read32(0x10300) == 0x00030201 && memory.read32(0x10304) == 0x00060504,
          "glReadPixels advances by aligned row stride without requiring final-row padding");
    Call readAtBoundary(gles, memory);
    readAtBoundary.ints({0, 0, 1, 2});
    readAtBoundary.stack({GL_RGB, GL_UNSIGNED_BYTE, 0x1fff9});
    readAtBoundary.must("_glReadPixels");
    check(recorded->readPixelsBytes == 7,
          "a seven-byte readback ending at the guest-page boundary does not overread padding");

    Call packOne(gles, memory);
    packOne.ints({GL_PACK_ALIGNMENT, 1});
    packOne.must("_glPixelStorei");
    Call readPacked(gles, memory);
    readPacked.ints({1, 2, 1, 2});
    readPacked.stack({GL_RGB, GL_UNSIGNED_BYTE, 0x10320});
    readPacked.must("_glReadPixels");
    check(recorded->readPixelsBytes == 6 && memory.read32(0x10320) == 0x04030201 &&
              (memory.read32(0x10324) & 0xffff) == 0x0605,
          "GL_PACK_ALIGNMENT=1 removes row padding in driver and guest memory");

    const uint8_t paddedRgb[8] = {1, 2, 3, 0xee, 4, 5, 6, 0xdd};
    memory.write(0x10400, paddedRgb, sizeof(paddedRgb));
    Call upload(gles, memory);
    upload.ints({0x0de1, 0, GL_RGB, 1});
    upload.stack({2, 0, GL_RGB, GL_UNSIGNED_BYTE, 0x10400});
    upload.must("_glTexImage2D");
    check(recorded->texImageBytes == 7 &&
              std::memcmp(recorded->texImageData, paddedRgb, 7) == 0,
          "glTexImage2D preserves aligned row stride but does not read final-row padding");
    const uint8_t edgeRgb[7] = {21, 22, 23, 0xaa, 24, 25, 26};
    memory.write(0x1fff9, edgeRgb, sizeof(edgeRgb));
    Call uploadAtBoundary(gles, memory);
    uploadAtBoundary.ints({0x0de1, 0, GL_RGB, 1});
    uploadAtBoundary.stack({2, 0, GL_RGB, GL_UNSIGNED_BYTE, 0x1fff9});
    uploadAtBoundary.must("_glTexImage2D");
    check(recorded->texImageBytes == 7 &&
              std::memcmp(recorded->texImageData, edgeRgb, sizeof(edgeRgb)) == 0,
          "texture uploads ending at the guest-page boundary do not overread final-row padding");
    Call unpackOne(gles, memory);
    unpackOne.ints({GL_UNPACK_ALIGNMENT, 1});
    unpackOne.must("_glPixelStorei");
    const uint8_t tightRgb[6] = {7, 8, 9, 10, 11, 12};
    memory.write(0x10420, tightRgb, sizeof(tightRgb));
    Call uploadTight(gles, memory);
    uploadTight.ints({0x0de1, 0, GL_RGB, 1});
    uploadTight.stack({2, 0, GL_RGB, GL_UNSIGNED_BYTE, 0x10420});
    uploadTight.must("_glTexImage2D");
    check(recorded->texImageBytes == 6 &&
              std::memcmp(recorded->texImageData, tightRgb, sizeof(tightRgb)) == 0,
          "GL_UNPACK_ALIGNMENT=1 copies tightly packed texture rows");

    // Core OpenGL ES state functions reach the driver with their true enum arguments.
    std::vector<std::string> trace;
    gles.setLogSink([&trace](const std::string& line) { trace.push_back(line); });
    gles.setVerbose(true);
    Call activeTexture(gles, memory);
    activeTexture.ints({0x84c0});
    activeTexture.must("_glActiveTexture");
    Call blendFunc(gles, memory);
    blendFunc.ints({0x0302, 0x0303});
    blendFunc.must("_glBlendFunc");
    Call texParameter(gles, memory);
    texParameter.ints({GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR});
    texParameter.must("_glTexParameteri");
    Call texParameterFloat(gles, memory);
    texParameterFloat.ints({GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER});
    texParameterFloat.floats({0.5f});
    texParameterFloat.must("_glTexParameterf");
    const float parameterValues[] = {0.75f};
    memory.write(0x10250, parameterValues, sizeof(parameterValues));
    Call texParameterVector(gles, memory);
    texParameterVector.ints({GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x10250});
    texParameterVector.must("_glTexParameterfv");
    Call queryTexParameter(gles, memory);
    queryTexParameter.ints({GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x10254});
    queryTexParameter.must("_glGetTexParameteriv");
    check(recorded->texParameterIntCalls == 1 && recorded->texParameterFloatCalls == 1 &&
              recorded->texParameterVectorCalls == 1 &&
              recorded->lastTexParameterTarget == GL_TEXTURE_2D &&
              recorded->lastTexParameterName == GL_TEXTURE_MIN_FILTER &&
              recorded->lastTexParameterInt == GL_LINEAR &&
              memory.read32(0x10254) == GL_LINEAR,
          "texture parameter setters preserve all arguments and query results safely");
    Call frontFace(gles, memory);
    frontFace.ints({GL_CW});
    frontFace.must("_glFrontFace");
    Call cullFace(gles, memory);
    cullFace.ints({GL_BACK});
    cullFace.must("_glCullFace");
    Call blendEquation(gles, memory);
    blendEquation.ints({0x8006});
    blendEquation.must("_glBlendEquation");
    check(recorded->frontFaceCalls == 1 && recorded->lastFrontFace == GL_CW &&
              recorded->cullFaceCalls == 1 && recorded->lastCullFace == GL_BACK &&
              recorded->blendEquationCalls == 1 && recorded->lastBlendEquation == 0x8006,
          "front-face, cull-face and blend-equation state is forwarded rather than stubbed");

    setStubError(GL_INVALID_ENUM);
    Call getError(gles, memory);
    getError.must("_glGetError");
    check(getError.state.registers[0] == GL_INVALID_ENUM,
          "verbose tracing does not drain the driver's guest-visible GL error");
    check(std::any_of(trace.begin(), trace.end(), [](const std::string& line) {
              return line.find("gl: glFrontFace(GL_CW)") != std::string::npos;
          }) &&
              std::any_of(trace.begin(), trace.end(), [](const std::string& line) {
                  return line.find("gl: glActiveTexture(GL_TEXTURE0)") != std::string::npos;
              }) &&
              std::any_of(trace.begin(), trace.end(), [](const std::string& line) {
                  return line.find("gl: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)") !=
                         std::string::npos;
              }) &&
              std::any_of(trace.begin(), trace.end(), [](const std::string& line) {
                  return line.find("glGetError() -> GL_INVALID_ENUM") != std::string::npos;
              }),
          "verbose GL traces decode state, texture units, blend factors and guest-polled errors");

    for (int index = 0; index < 8; ++index) {
        Call clear(gles, memory);
        clear.ints({0x4000});
        clear.must("_glClear");
    }
    size_t clearLines = 0;
    for (const std::string& line : trace)
        if (line.find("gl: glClear(") != std::string::npos) ++clearLines;
    check(clearLines == 4 &&
              gles.describe().find("glClear: 8 call(s), 4 shown, 4 summarized") != std::string::npos,
          "verbose tracing samples repeated hot GL calls and preserves exact totals");
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
    fixedFunctionMatrixStacks(gles, memory);
    oneTranslatedDraw(gles, memory, recorded);
    textureUnitState(gles, memory, recorded);
    colourArrayAndFixedPoint(gles, memory, recorded);
    indexedDraw(gles, memory, recorded);
    refusesToDrawWithABrokenProgram(gles, memory, recorded);
    coreAliasesQueriesReadbackAndTracing(driver);
    shaderUniformAndGenericAttributeImports();
    fixedFunctionLighting();

    std::cout << "gles emulation: an ES 1.1 guest drew through generated ES 2.0 programs over "
              << driver << ", with fixed-point widening, cached variants and a named refusal\n";
}
