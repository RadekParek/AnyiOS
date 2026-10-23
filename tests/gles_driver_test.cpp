// Drives the GL bridge against tests/gles_stub_driver.cpp, a recording driver.
//
// The unit proves the parts of the bridge that do not need a GPU: the AAPCS-VFP register
// split (floats in s0-s15, integers in r0-r3 then the stack), the draw-time copy of client
// arrays, the texel-size computation for pixel uploads, the EAGL drawable (renderbuffer +
// framebuffer over EGL), and the bottom-left → top-row-first flip in present(). The driver
// itself records rather than renders, so nothing here claims a frame was drawn.
#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gles_stub_driver.h"
#include "runtime/error.h"
#include "runtime/gles.h"
#include "runtime/memory.h"
using namespace podnative::runtime;

namespace {

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
// The recording driver is dlopen()ed by the bridge itself, exactly as a real driver is, so
// the test reaches its recorder through the same global symbol table.
StubState* stub() {
    using Getter = StubState* (*)();
    auto getter = reinterpret_cast<Getter>(dlsym(RTLD_DEFAULT, "stub_state"));
    check(getter != nullptr, "the recording driver exposes its state");
    return getter();
}
void resetStub() {
    using Reset = void (*)();
    auto reset = reinterpret_cast<Reset>(dlsym(RTLD_DEFAULT, "stub_reset"));
    check(reset != nullptr, "the recording driver exposes a reset");
    reset();
}
uint32_t bitsOf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
float floatAt(const float* values, int index) { return values[index]; }

// One guest call through the bridge. Integer/pointer arguments are placed in r0-r3 and
// then on the guest stack at sp; float arguments go to s0-s15, numbered independently —
// exactly the AAPCS-VFP layout the bridge has to read.
struct Call {
    GLES& gles;
    Memory& memory;
    CpuState state;
    uint32_t stackWords = 0;
    explicit Call(GLES& gles, Memory& memory) : gles(gles), memory(memory) {
        state = {};
        state.registers[13] = kStack;
    }
    void operator()(const std::string& symbol) {
        const bool handled = gles.invoke(symbol, state);
        check(handled, "bridge answers " + symbol);
    }
    void ints(std::initializer_list<uint32_t> values) {
        size_t index = 0;
        for (uint32_t value : values) state.registers[index++] = value;
    }
    void floats(std::initializer_list<float> values) {
        size_t index = 0;
        for (float value : values) state.extensionRegisters[index++] = bitsOf(value);
    }
    // Words the 5th and later integer arguments arrive in, written where sp points.
    void stack(std::initializer_list<uint32_t> values) {
        uint32_t offset = 0;
        for (uint32_t value : values) {
            memory.write32(kStack + offset, value);
            offset += 4;
        }
        stackWords = offset;
    }
    static constexpr uint32_t kStack = 0x10100;
};

constexpr uint32_t GL_FLOAT = 0x1406, GL_UNSIGNED_BYTE = 0x1401, GL_RGBA = 0x1908;
constexpr uint32_t GL_FRAMEBUFFER = 0x8d40, GL_RENDERBUFFER = 0x8d41;
constexpr uint32_t GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_FRAMEBUFFER_BINDING = 0x8ca6;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074, GL_TRIANGLES = 0x0004, GL_TRIANGLE_STRIP = 0x0005;

void marshalling(const std::string& driver) {
    Memory memory(1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({0x10000, 65536, uint8_t(Read | Write), "guest vertices"}, page.data(), page.size());
    GLES gles(memory);
    check(gles.load(), "the recording driver is resolved");
    check(gles.backend() == driver, "backend names the library that answered: " + gles.backend());
    resetStub();
    StubState* recorded = stub();

    // Floats come from s0-s3 even though the integers r0-r3 are also set: a bridge that
    // read colours out of r0-r3 would see the sentinel values instead of the colour.
    Call call(gles, memory);
    call.ints({0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef});
    call.floats({0.25f, 0.5f, 0.75f, 1.0f});
    call("_glClearColor");
    check(recorded->clearColourCalls == 1, "glClearColor reaches the driver once");
    check(std::fabs(recorded->clearColour[0] - 0.25f) < 1e-6f &&
              std::fabs(recorded->clearColour[1] - 0.5f) < 1e-6f &&
              std::fabs(recorded->clearColour[2] - 0.75f) < 1e-6f &&
              std::fabs(recorded->clearColour[3] - 1.0f) < 1e-6f,
          "glClearColor reads its four GLfloat arguments from the VFP registers");

    // Integers still come from r0-r3 in order.
    Call viewport(gles, memory);
    viewport.ints({1, 2, 3, 4});
    viewport("_glViewport");
    check(recorded->viewport[0] == 1 && recorded->viewport[1] == 2 && recorded->viewport[2] == 3 &&
              recorded->viewport[3] == 4,
          "glViewport reads four integers from r0-r3");

    // A client array is read when the draw happens, not when it is described.
    const float vertices[6] = {0, 0, 10, 0, 0, 10};
    memory.write(0x10000, vertices, sizeof(vertices));
    Call enable(gles, memory);
    enable.ints({GL_VERTEX_ARRAY});
    enable("_glEnableClientState");
    check(recorded->clientsEnabled[0] == 1, "glEnableClientState records the vertex array");

    Call pointer(gles, memory);
    pointer.ints({2, GL_FLOAT, 0, 0x10000});
    pointer("_glVertexPointer");
    check(recorded->vertexComponents == 2 && recorded->vertexType == int(GL_FLOAT),
          "glVertexPointer passes size and type through");

    const float moved[6] = {1, 1, 11, 1, 1, 11};
    memory.write(0x10000, moved, sizeof(moved));
    Call draw(gles, memory);
    draw.ints({GL_TRIANGLES, 0, 3});
    draw("_glDrawArrays");
    check(recorded->drawCalls == 1 && recorded->drawCount == 3, "glDrawArrays reaches the driver");
    // One vertex is copied by the recording driver; the guest data was (0,0,10,0,0,10)
    // when the array was described and (1,1,11,1,1,11) when the draw happened.
    check(std::fabs(floatAt(recorded->vertices, 0) - 1.0f) < 1e-6f &&
              std::fabs(floatAt(recorded->vertices, 1) - 1.0f) < 1e-6f,
          "the draw copies the array as it is at draw time");

    // Pixel uploads are sized from the format/type pair, not from the guest's word count.
    const uint8_t texels[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    constexpr uint32_t kTexels = 0x11000;
    memory.write(kTexels, texels, sizeof(texels));
    Call upload(gles, memory);
    upload.ints({0x0de1, 0, 4, 2});  // target, level, internal format, width
    // height, border, format, type, pixels spill onto the stack.
    upload.stack({2, 0, GL_RGBA, GL_UNSIGNED_BYTE, kTexels});
    upload("_glTexImage2D");
    check(recorded->texImageCalls == 1 && recorded->texImageWidth == 2 &&
              recorded->texImageHeight == 2,
          "glTexImage2D keeps target geometry");
    check(recorded->texImageBytes == 16 && std::memcmp(recorded->texels, texels, 16) == 0,
          "glTexImage2D copies w*h*4 bytes of RGBA8");
    check(upload.stackWords == 20, "five trailing arguments were read from the guest stack");
}

void drawable(const std::string& driver) {
    Memory memory(1024 * 1024);
    std::vector<uint8_t> guestPage(65536, 0);
    memory.map({0x10000, 65536, uint8_t(Read | Write), "guest GL names"}, guestPage.data(),
               guestPage.size());
    GLES gles(memory);
    check(gles.load(), "driver resolved for the drawable test");
    check(gles.backend() == driver, "same backend");
    resetStub();
    StubState* recorded = stub();

    check(gles.clientApiVersion() == 1, "an ES 1.1 title starts on an ES 1.1 context");
    check(gles.createDrawable(8, 4), "the drawable framebuffer is created: " + gles.describe());
    check(recorded->platformDisplayCalls == 1 && recorded->platformDisplay == 0x3202 &&
              recorded->angleDisplayType == 0x3450,
          "bundled ANGLE gets the same explicit Vulkan platform as the Android renderer");
    check(recorded->contextVersion == 1, "the EGL context is OpenGL ES 1.1");
    check(gles.drawableWidth() == 8 && gles.drawableHeight() == 4, "drawable size is kept");
    check(recorded->renderbufferWidth == 8 && recorded->renderbufferHeight == 4,
          "the renderbuffer is the drawable's size");
    check(recorded->renderbufferFormat == 0x8056,
          "a driver without the RGBA8 extension gets the ES-required RGBA4 format");
    check(recorded->boundFramebuffer == 3 && recorded->attachedRenderbuffer == 7,
          "the colour renderbuffer is attached to the framebuffer");

    check(gles.present(), "present reads the drawable back: " + gles.describe());
    check(recorded->finishCalls == 1, "present finishes the driver first");
    check(recorded->readPixelsCalls == 1 && recorded->readPixelsWidth == 8 &&
              recorded->readPixelsHeight == 4,
          "present reads the whole drawable");
    check(recorded->framebufferAtRead == 3 && gles.frameCurrent(),
          "the private default framebuffer is selected and the host frame is current");
    const std::vector<uint8_t>& frame = gles.lastFrame();
    check(frame.size() == 8 * 4 * 4, "the frame is tightly packed RGBA8");
    check(frame[0] == 0 && frame[1] == 3 && frame[3] == 255,
          "the first host row is the last GL row (top row first)");
    check(frame[size_t(3) * 8 * 4 + 1] == 0, "the last host row is GL row 0");

    // EAGL storage belongs to the renderbuffer the guest bound, and the drawable FBO remains
    // the source of readback even if the guest later binds framebuffer zero.
    Call genFramebuffer(gles, memory);
    genFramebuffer.ints({1, 0x10000});
    genFramebuffer("_glGenFramebuffersOES");
    const uint32_t guestFramebuffer = memory.read32(0x10000);
    check(guestFramebuffer != 3, "guest framebuffer names do not alias the private fallback");
    Call bindFramebuffer(gles, memory);
    bindFramebuffer.ints({GL_FRAMEBUFFER, guestFramebuffer});
    bindFramebuffer("_glBindFramebufferOES");
    Call genRenderbuffer(gles, memory);
    genRenderbuffer.ints({1, 0x10004});
    genRenderbuffer("_glGenRenderbuffersOES");
    const uint32_t guestRenderbuffer = memory.read32(0x10004);
    Call bindRenderbuffer(gles, memory);
    bindRenderbuffer.ints({GL_RENDERBUFFER, guestRenderbuffer});
    bindRenderbuffer("_glBindRenderbufferOES");
    check(gles.renderbufferStorageFromDrawable(8, 4),
          "EAGL allocates storage on the bound guest renderbuffer");
    check(recorded->boundRenderbuffer == guestRenderbuffer && recorded->renderbufferWidth == 8 &&
              recorded->renderbufferHeight == 4,
          "EAGL storage uses the guest's bound renderbuffer and its drawable geometry");
    Call attach(gles, memory);
    attach.ints({GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, guestRenderbuffer});
    attach("_glFramebufferRenderbufferOES");
    check(gles.present(), "the attached guest EAGL framebuffer can be read back");
    check(recorded->framebufferAtRead == guestFramebuffer,
          "readback selects the guest framebuffer attached to the EAGL drawable");
    Call bindDefault(gles, memory);
    bindDefault.ints({GL_FRAMEBUFFER, 0});
    bindDefault("_glBindFramebufferOES");
    check(recorded->boundFramebuffer == 3,
          "binding guest framebuffer zero maps back to the private default drawable");
    memory.write32(0x10008, 0xffffffff);
    Call queryBinding(gles, memory);
    queryBinding.ints({GL_FRAMEBUFFER_BINDING, 0x10008});
    queryBinding("_glGetIntegerv");
    check(memory.read32(0x10008) == 0,
          "GL_FRAMEBUFFER_BINDING reports the guest-visible default name, not the private FBO");
    check(gles.present() && recorded->framebufferAtRead == guestFramebuffer &&
              recorded->boundFramebuffer == 3,
          "present reads the attached EAGL FBO then restores the guest's default binding");

    // An ES 2.0 request replaces the context: the version is the guest's, not a guess.
    gles.setClientApi(2);
    check(gles.createDrawable(8, 4), "the ES 2.0 drawable is created");
    check(recorded->contextVersion == 2, "the EGL context is OpenGL ES 2.0 after -initWithAPI:2");
    check(recorded->destroyContextCalls == 1, "the ES 1.1 context is destroyed, not leaked");

    // A driver that refuses a surfaceless context still gets one, through a pbuffer; the
    // guest draws into a framebuffer object, so a one-pixel surface is all that is needed.
    gles.setClientApi(1);
    using Refuse = void (*)(int);
    auto refuse = reinterpret_cast<Refuse>(dlsym(RTLD_DEFAULT, "stub_refuse_surfaceless"));
    check(refuse != nullptr, "the recording driver can refuse a surfaceless context");
    refuse(1);
    check(gles.makeCurrent(), "makeCurrent falls back to a pbuffer surface: " + gles.describe());
    check(recorded->surfacelessRefusals >= 1, "the surfaceless attempt was made first");
    check(recorded->pbufferCreated == 1 && recorded->surfaceMadeCurrent >= 1,
          "a pbuffer surface was created and made current");
}

void pvrtcUploads(const std::string& driver) {
    Memory memory(1024 * 1024);
    std::vector<uint8_t> page(65536, 0);
    memory.map({0x10000, 65536, uint8_t(Read | Write), "guest compressed texture"}, page.data(),
               page.size());
    GLES gles(memory);
    check(gles.load(), "the recording driver is resolved for compressed uploads");
    check(gles.backend() == driver, "compressed uploads stay on the selected driver");
    resetStub();
    StubState* recorded = stub();

    const std::vector<uint8_t> white(32, 0xff);
    memory.write(0x10000, white.data(), white.size());
    Call upload(gles, memory);
    upload.ints({0x0de1, 0, 0x8c00, 8});
    upload.stack({8, 0, uint32_t(white.size()), 0x10000});
    upload("_glCompressedTexImage2D");
    check(recorded->compressedTexImageCalls == 0,
          "PVRTC1 is decoded instead of sent as an unsupported ANGLE Vulkan format");
    check(recorded->texImageCalls == 1 && recorded->texImageInternalFormat == GL_RGBA &&
              recorded->texImageFormat == GL_RGBA && recorded->texImageType == GL_UNSIGNED_BYTE &&
              recorded->texImageWidth == 8 && recorded->texImageHeight == 8 &&
              recorded->texImageBytes == 8 * 8 * 4,
          "decoded PVRTC1 is uploaded to the selected driver as a bounded RGBA8 image");
    for (size_t offset = 0; offset < recorded->texImageBytes; offset++)
        check(recorded->texels[offset] == 255, "the driver receives the decoded white texels");

    const std::vector<uint8_t> black(32, 0);
    memory.write(0x10000, black.data(), black.size());
    Call rgbUpload(gles, memory);
    rgbUpload.ints({0x0de1, 0, 0x8c00, 8});
    rgbUpload.stack({8, 0, uint32_t(black.size()), 0x10000});
    rgbUpload("_glCompressedTexImage2D");
    check(recorded->texImageCalls == 2 && recorded->texels[0] == 0 && recorded->texels[1] == 0 &&
              recorded->texels[2] == 0 && recorded->texels[3] == 255,
          "RGB PVRTC uploads keep opaque alpha when endpoint alpha bits are unused");
    check(
        gles.describe().find("PVRTC1 software-decoded uploads=2 (128 texels)") != std::string::npos,
        "the launch report names the PVRTC software compatibility path");

    const std::vector<uint8_t> etc(8, 0);
    memory.write(0x10000, etc.data(), etc.size());
    Call otherCompressed(gles, memory);
    otherCompressed.ints({0x0de1, 0, 0x8d64, 4});  // ETC1 RGB8 OES, 4x4
    otherCompressed.stack({4, 0, uint32_t(etc.size()), 0x10000});
    otherCompressed("_glCompressedTexImage2D");
    check(recorded->compressedTexImageCalls == 1 && recorded->compressedTexImageFormat == 0x8d64 &&
              recorded->compressedTexImageWidth == 4 && recorded->compressedTexImageHeight == 4 &&
              recorded->compressedTexImageBytes == etc.size(),
          "other compressed formats are forwarded unchanged");
}
}  // namespace

int main(int argc, char** argv) {
    check(argc == 2, "usage: gles-driver-test <recording-driver-name>");
    const std::string driver = argv[1];
    marshalling(driver);
    drawable(driver);
    pvrtcUploads(driver);
    std::cout << "gles driver: AAPCS-VFP marshalling, client arrays, texel copies, EAGL drawable, "
                 "frame flip and PVRTC1 software decoding verified through "
              << driver << '\n';
}
