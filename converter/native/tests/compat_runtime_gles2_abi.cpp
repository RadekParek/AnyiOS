#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/objc_shims.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)
using namespace radek::compat_runtime;

namespace {
constexpr GuestAddress kData = 0x10000;
constexpr GuestAddress kStack = 0x20000;

std::uint32_t bits(float value) { std::uint32_t result; std::memcpy(&result, &value, 4); return result; }

template <typename T> T symbol(void *lib, const char *name) {
    auto fn = reinterpret_cast<T>(dlsym(lib, name));
    CHECK(fn != nullptr);
    return fn;
}

struct Fixture {
    GuestAddressSpace memory;
    ShimRegistry registry;
    gles::Forwarder gl;
    objc::ShimAdapter objc;

    Fixture() {
        memory.mapAt(kData, 0x10000, MemoryPermission::Read | MemoryPermission::Write, "data");
        memory.mapAt(kStack, 0x10000, MemoryPermission::Read | MemoryPermission::Write, "stack");
        memory.mapAt(0x30000, 4096, MemoryPermission::Read, "read-only");
        objc.setGlesForwarder(&gl);
        objc.registerBindings(registry);
        gl.registerBindings(registry);
    }
    GuestCalloutResult call(const char *name, CpuRegisterState &r) {
        const auto binding = registry.resolve(name);
        CHECK(binding.has_value());
        if (!r.r[13]) r.r[13] = kStack;
        std::string reason;
        return registry.invokeCallout(binding->guestAddress, r, memory, reason);
    }
    GuestAddress selector(const char *name) {
        CHECK(memory.write(kData, name, std::strlen(name) + 1));
        CpuRegisterState r{}; r.r[0] = kData;
        CHECK(call("_sel_registerName", r) == GuestCalloutResult::Returned);
        return r.r[0];
    }
    GuestAddress contextClass() {
        const auto binding = registry.resolve("_OBJC_CLASS_$_EAGLContext");
        CHECK(binding && binding->resolveGuestAddress);
        GuestAddress klass = 0; std::string reason;
        CHECK(binding->resolveGuestAddress(memory, klass, reason));
        return klass;
    }
    void send(GuestAddress receiver, const char *name, CpuRegisterState &r) {
        r.r[0] = receiver; r.r[1] = selector(name);
        CHECK(call("_objc_msgSend", r) == GuestCalloutResult::Returned);
    }
};
}

int main() {
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void *gles2 = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    CHECK(egl && gles2);
    auto version = symbol<int (*)()>(egl, "recordedContextVersion");
    auto renderable = symbol<int (*)()>(egl, "recordedRenderable");
    auto contexts = symbol<int (*)()>(egl, "recordedContextsCreated");
    auto swaps = symbol<int (*)()>(egl, "recordedSwapCount");
    auto windowSurfaces = symbol<int (*)()>(egl, "recordedWindowSurfaceCount");
    auto lastWindow = symbol<std::uintptr_t (*)()>(egl, "recordedWindow");
    auto failWindow = symbol<void (*)(int)>(egl, "setWindowSurfaceFailure");
    auto framebuffer = symbol<unsigned (*)()>(gles2, "recordedBoundFramebuffer");
    auto framebufferCalls = symbol<unsigned (*)()>(gles2, "recordedFramebufferCalls");
    auto draws = symbol<unsigned (*)()>(gles2, "recordedDrawCalls");
    auto generatedBuffers = symbol<unsigned (*)()>(gles2, "recordedGeneratedBuffers");
    auto textureUploads = symbol<unsigned (*)()>(gles2, "recordedTextureUploads");
    auto pixelReads = symbol<unsigned (*)()>(gles2, "recordedPixelReads");
    auto integerQueries = symbol<unsigned (*)()>(gles2, "recordedIntegerQueries");
    auto unpackAlignment = symbol<int (*)()>(gles2, "recordedUnpackAlignment");
    auto attributeBuffer = symbol<unsigned (*)()>(gles2, "recordedAttribBuffer");
    auto attributePointer = symbol<std::uintptr_t (*)()>(gles2, "recordedAttribPointer");
    auto sourceCount = symbol<int (*)()>(gles2, "recordedSourceCount");
    auto sourceLength = symbol<int (*)(unsigned)>(gles2, "recordedSourceLength");
    auto sourceByte = symbol<int (*)(unsigned, unsigned)>(gles2, "recordedSourceByte");
    auto uniform = symbol<float (*)(unsigned)>(gles2, "recordedUniform");

    // ObjC lifecycle and imported GL calls intentionally share one Forwarder.
    Fixture f;
    CHECK(f.gl.supportsApi(1));
    CHECK(!f.gl.driver().gles2Loaded); // ES2 is only probed when requested
    const auto klass = f.contextClass();
    CpuRegisterState alloc{}; f.send(klass, "alloc", alloc); const auto context = alloc.r[0];
    CpuRegisterState init{}; init.r[2] = 2; f.send(context, "initWithAPI:", init); CHECK(init.r[0] == context);
    CHECK(f.gl.driver().gles2Loaded);
    CpuRegisterState api{}; f.send(context, "API", api); CHECK(api.r[0] == 2);
    CpuRegisterState current{}; current.r[2] = context; f.send(klass, "setCurrentContext:", current);
    CHECK(current.r[0] == 1 && version() == 2 && renderable() == 4 && contexts() == 1);
    CHECK(!f.gl.drawableReady());
    CHECK(!f.gl.attachDrawable(f.memory, 320, 200)); // renderbuffer zero is not drawable storage
    CHECK(!f.gl.presentDrawable() && !f.gl.drawableReady());

    // The drawable's renderbuffer attachment must remap the same GL state to
    // framebuffer zero, rather than an unrelated second Forwarder's state.
    CpuRegisterState genRender{}; genRender.r[0] = 1; genRender.r[1] = kData + 0x100;
    CHECK(f.call("_glGenRenderbuffers", genRender) == GuestCalloutResult::Returned);
    unsigned renderbuffer = 0; CHECK(f.memory.read(kData + 0x100, &renderbuffer, 4)); CHECK(renderbuffer == 40);
    CpuRegisterState bindRender{}; bindRender.r[0] = 0x8D41; bindRender.r[1] = renderbuffer;
    CHECK(f.call("_glBindRenderbuffer", bindRender) == GuestCalloutResult::Returned);
    CpuRegisterState storage{}; storage.r[2] = 0xDEAD; storage.r[3] = 0;
    f.send(context, "renderbufferStorage:fromDrawable:", storage); CHECK(storage.r[0] == 0 && !f.gl.drawableReady());
    storage.r[2] = 0x8D41;
    f.send(context, "renderbufferStorage:fromDrawable:", storage); CHECK(storage.r[0] == 1 && f.gl.drawableReady());
    CpuRegisterState genFrame{}; genFrame.r[0] = 1; genFrame.r[1] = kData + 0x104;
    CHECK(f.call("_glGenFramebuffers", genFrame) == GuestCalloutResult::Returned);
    unsigned fb = 0; CHECK(f.memory.read(kData + 0x104, &fb, 4)); CHECK(fb == 50);
    CpuRegisterState bindFrame{}; bindFrame.r[0] = 0x8D40; bindFrame.r[1] = fb;
    CHECK(f.call("_glBindFramebuffer", bindFrame) == GuestCalloutResult::Returned);
    CHECK(framebuffer() == fb);
    CpuRegisterState attach{}; attach.r[0] = 0x8D40; attach.r[1] = 0x8CE0;
    attach.r[2] = 0x8D41; attach.r[3] = renderbuffer;
    CHECK(f.call("_glFramebufferRenderbuffer", attach) == GuestCalloutResult::Returned);
    CHECK(framebuffer() == 0 && framebufferCalls() >= 2);
    CpuRegisterState status{}; status.r[0] = 0x8D40;
    CHECK(f.call("_glCheckFramebufferStatus", status) == GuestCalloutResult::Returned && status.r[0] == 0x8CD5);
    status.r[0] = 0xDEAD;
    CHECK(f.call("_glCheckFramebufferStatus", status) == GuestCalloutResult::Returned && status.r[0] == 0);
    const std::uint32_t querySentinel = 0x12345678;
    CHECK(f.memory.write(kData + 0x109, &querySentinel, sizeof(querySentinel)));
    CpuRegisterState rbQuery{}; rbQuery.r[0] = 0xDEAD; rbQuery.r[1] = 0x8D42; rbQuery.r[2] = kData + 0x109;
    CHECK(f.call("_glGetRenderbufferParameteriv", rbQuery) == GuestCalloutResult::Returned);
    std::uint32_t rbValue = 0; CHECK(f.memory.read(rbQuery.r[2], &rbValue, 4) && rbValue == querySentinel);
    rbQuery.r[0] = 0x8D41; rbQuery.r[1] = 0x8D50;
    CHECK(f.call("_glGetRenderbufferParameteriv", rbQuery) == GuestCalloutResult::Returned);
    CHECK(f.memory.read(rbQuery.r[2], &rbValue, 4) && rbValue == 8);
    CpuRegisterState present{}; present.r[2] = 0xDEAD; f.send(context, "presentRenderbuffer:", present);
    CHECK(present.r[0] == 0 && swaps() == 0);
    present.r[2] = 0x8D41; f.send(context, "presentRenderbuffer:", present);
    CHECK(present.r[0] == 1 && swaps() == 1);

    // Surface replacement happens on a different thread in Android. The EGL
    // context and resources survive while detached/failed windows are never
    // passed to eglSwapBuffers as if they were still valid.
    auto *windowA = reinterpret_cast<void *>(std::uintptr_t{0x1000});
    auto *windowB = reinterpret_cast<void *>(std::uintptr_t{0x2000});
    auto *windowC = reinterpret_cast<void *>(std::uintptr_t{0x3000});
    f.gl.setNativeWindow(windowA);
    CHECK(f.gl.presentDrawable() && lastWindow() == 0x1000 && windowSurfaces() == 1 && swaps() == 2);
    CHECK(contexts() == 1);
    f.gl.setNativeWindow(nullptr);
    CHECK(!f.gl.presentDrawable() && swaps() == 2);
    f.gl.setNativeWindow(windowA); // even the same pointer needs a new EGL surface after detach
    CHECK(f.gl.presentDrawable() && windowSurfaces() == 2 && swaps() == 3 && contexts() == 1);
    f.gl.setNativeWindow(windowB);
    failWindow(1);
    CHECK(!f.gl.presentDrawable() && lastWindow() == 0x2000 && swaps() == 3);
    failWindow(0);
    f.gl.setNativeWindow(windowC);
    CHECK(f.gl.presentDrawable() && lastWindow() == 0x3000 && swaps() == 4 && contexts() == 1);

    // A drawable-backed framebuffer reports its guest object name, and core
    // glGet outputs need only their actual number of writable integers.
    CpuRegisterState query{}; query.r[0] = 0x8CA6; query.r[1] = kData + 0xfffc;
    CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    int result = 0; CHECK(f.memory.read(query.r[1], &result, 4) && result == 50);
    query.r[0] = 0x0D33; // MAX_TEXTURE_SIZE: one integer at the end of the mapping
    CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    CHECK(f.memory.read(query.r[1], &result, 4) && result == 4096);
    const auto queriesBefore = integerQueries();
    query.r[0] = 0x0BA2; // VIEWPORT: four integers
    CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    CHECK(integerQueries() == queriesBefore);
    query.r[1] = kData + 0xfff0;
    CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    std::array<int, 4> viewport{}; CHECK(f.memory.read(query.r[1], viewport.data(), sizeof(viewport)));
    CHECK(viewport[0] == 10 && viewport[3] == 13);
    query.r[0] = 0x86A3; query.r[1] = kData + 0xfff8; // two compressed formats
    CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    std::array<int, 2> formats{}; CHECK(f.memory.read(query.r[1], formats.data(), sizeof(formats)));
    CHECK(formats[0] == 0x8D64 && formats[1] == 0x8D65);
    const auto afterFormats = integerQueries();
    query.r[0] = 0xdead; CHECK(f.call("_glGetIntegerv", query) == GuestCalloutResult::Returned);
    CHECK(integerQueries() == afterFormats); // unknown extension arity is refused

    // Drawable attachment replacement and deletion must keep the guest FBO
    // and the host default-FBO binding in sync. Invalid target/level calls must
    // not silently detach an otherwise valid drawable.
    CpuRegisterState invalidBind{}; invalidBind.r[0] = 0xDEAD; invalidBind.r[1] = 51;
    CHECK(f.call("_glBindFramebuffer", invalidBind) == GuestCalloutResult::Returned && framebuffer() == 0);
    CpuRegisterState fboTexture{}; fboTexture.r[0] = 0xDEAD; fboTexture.r[1] = 0x8CE0;
    fboTexture.r[2] = 0x0DE1; fboTexture.r[3] = 123; fboTexture.r[13] = kStack;
    std::uint32_t level = 0; CHECK(f.memory.write(kStack, &level, sizeof(level)));
    CHECK(f.call("_glFramebufferTexture2D", fboTexture) == GuestCalloutResult::Returned && framebuffer() == 0);
    fboTexture.r[0] = 0x8D40; level = 1; CHECK(f.memory.write(kStack, &level, sizeof(level)));
    CHECK(f.call("_glFramebufferTexture2D", fboTexture) == GuestCalloutResult::Returned && framebuffer() == 0);
    CpuRegisterState invalidAttachment = attach; invalidAttachment.r[0] = 0xDEAD; invalidAttachment.r[3] = 0;
    CHECK(f.call("_glFramebufferRenderbuffer", invalidAttachment) == GuestCalloutResult::Returned && framebuffer() == 0);

    CpuRegisterState detach = attach; detach.r[2] = 0; detach.r[3] = 0; // target ignored when renderbuffer is 0
    CHECK(f.call("_glFramebufferRenderbuffer", detach) == GuestCalloutResult::Returned && framebuffer() == fb);
    CHECK(f.call("_glFramebufferRenderbuffer", attach) == GuestCalloutResult::Returned && framebuffer() == 0);
    fboTexture.r[2] = 0; fboTexture.r[3] = 0; // target/level ignored when texture is 0
    CHECK(f.call("_glFramebufferTexture2D", fboTexture) == GuestCalloutResult::Returned && framebuffer() == fb);
    CHECK(f.call("_glFramebufferRenderbuffer", attach) == GuestCalloutResult::Returned && framebuffer() == 0);

    // Deleting a renderbuffer detaches it only from the *bound* FBO. A
    // non-bound framebuffer keeps its drawable attachment until it is deleted.
    CpuRegisterState otherFb = bindFrame; otherFb.r[1] = 51;
    CHECK(f.call("_glBindFramebuffer", otherFb) == GuestCalloutResult::Returned && framebuffer() == 51);
    CHECK(f.memory.write(kData + 0x159, &renderbuffer, sizeof(renderbuffer)));
    CpuRegisterState deleteRb{}; deleteRb.r[0] = 1; deleteRb.r[1] = kData + 0x159; // unaligned list
    CHECK(f.call("_glDeleteRenderbuffers", deleteRb) == GuestCalloutResult::Returned);
    // Reusing the same integer name creates a distinct drawable object. Its
    // deletion must not detach the old object still held by framebuffer 50.
    CHECK(f.call("_glBindRenderbuffer", bindRender) == GuestCalloutResult::Returned);
    f.send(context, "renderbufferStorage:fromDrawable:", storage); CHECK(storage.r[0] == 1);
    CHECK(f.call("_glBindFramebuffer", bindFrame) == GuestCalloutResult::Returned && framebuffer() == 0);
    CHECK(f.call("_glDeleteRenderbuffers", deleteRb) == GuestCalloutResult::Returned && framebuffer() == 0);
    CHECK(f.memory.write(kData + 0x159, &fb, sizeof(fb)));
    CpuRegisterState deleteFb = deleteRb;
    CHECK(f.call("_glDeleteFramebuffers", deleteFb) == GuestCalloutResult::Returned && framebuffer() == 0);
    CpuRegisterState fbQuery{}; fbQuery.r[0] = 0x8CA6; fbQuery.r[1] = kData + 0x158;
    CHECK(f.call("_glGetIntegerv", fbQuery) == GuestCalloutResult::Returned);
    int deletedBinding = -1; CHECK(f.memory.read(fbQuery.r[1], &deletedBinding, 4) && deletedBinding == 0);

    // Deletion while the drawable FBO is bound must rebind the now-detached
    // guest framebuffer, rather than leaving GL's window framebuffer active.
    const unsigned nextRenderbuffer = 41, nextFramebuffer = 52;
    bindRender.r[1] = nextRenderbuffer; bindFrame.r[1] = nextFramebuffer;
    CHECK(f.call("_glBindRenderbuffer", bindRender) == GuestCalloutResult::Returned);
    f.send(context, "renderbufferStorage:fromDrawable:", storage); CHECK(storage.r[0] == 1);
    CHECK(f.call("_glBindFramebuffer", bindFrame) == GuestCalloutResult::Returned && framebuffer() == 52);
    attach.r[3] = nextRenderbuffer;
    CHECK(f.call("_glFramebufferRenderbuffer", attach) == GuestCalloutResult::Returned && framebuffer() == 0);
    CHECK(f.memory.write(kData + 0x159, &nextRenderbuffer, sizeof(nextRenderbuffer)));
    CHECK(f.call("_glDeleteRenderbuffers", deleteRb) == GuestCalloutResult::Returned && framebuffer() == 52);
    fbQuery.r[0] = 0x8CA7; CHECK(f.call("_glGetIntegerv", fbQuery) == GuestCalloutResult::Returned);
    CHECK(f.memory.read(fbQuery.r[1], &deletedBinding, 4) && deletedBinding == 0);

    // Shader pointer arrays are 32-bit in the guest even on a 64-bit host.
    // Explicit lengths preserve embedded NULs; a negative length is NUL-terminated.
    const std::array<GuestAddress, 2> strings{kData + 0x200, kData + 0x210};
    const std::array<std::int32_t, 2> lengths{4, -1};
    const std::array<char, 4> first{'a', 0, 'b', 'c'};
    CHECK(f.memory.write(kData + 0x180, strings.data(), sizeof(strings)));
    CHECK(f.memory.write(kData + 0x190, lengths.data(), sizeof(lengths)));
    CHECK(f.memory.write(strings[0], first.data(), first.size()));
    CHECK(f.memory.write(strings[1], "other", 6));
    CpuRegisterState shader{}; shader.r[0] = 101; shader.r[1] = 2;
    shader.r[2] = kData + 0x180; shader.r[3] = kData + 0x190;
    CHECK(f.call("_glShaderSource", shader) == GuestCalloutResult::Returned);
    CHECK(sourceCount() == 2 && sourceLength(0) == 4 && sourceByte(0, 1) == 0 && sourceByte(0, 3) == 'c');
    CHECK(sourceLength(1) == 5 && sourceByte(1, 4) == 'r');
    shader.r[2] = 0xfffffffcu; shader.r[1] = 2;
    CHECK(f.call("_glShaderSource", shader) == GuestCalloutResult::Returned);
    CHECK(sourceCount() == 2); // invalid guest pointer never reaches the driver

    // Object queries and info logs preserve output the driver does not write.
    const std::uint32_t sentinel = 0xa5a5a5a5; CHECK(f.memory.write(kData + 0x350, &sentinel, sizeof(sentinel)));
    CpuRegisterState shaderQuery{}; shaderQuery.r[0] = 101; shaderQuery.r[1] = 0xDEAD; shaderQuery.r[2] = kData + 0x350;
    CHECK(f.call("_glGetShaderiv", shaderQuery) == GuestCalloutResult::Returned);
    std::uint32_t unchanged = 0; CHECK(f.memory.read(kData + 0x350, &unchanged, sizeof(unchanged)) && unchanged == sentinel);
    const std::array<char, 8> filled{'x','x','x','x','x','x','x','x'};
    CHECK(f.memory.write(kData + 0x360, filled.data(), filled.size()));
    CpuRegisterState info{}; info.r[0] = 101; info.r[1] = 8; info.r[2] = 0; info.r[3] = kData + 0x360;
    CHECK(f.call("_glGetShaderInfoLog", info) == GuestCalloutResult::Returned);
    std::array<char, 8> logged{}; CHECK(f.memory.read(kData + 0x360, logged.data(), logged.size()));
    CHECK(logged[0] == 'a' && logged[2] == 'c' && logged[3] == 0 && logged[4] == 'x');
    info.r[0] = 0; CHECK(f.call("_glGetShaderInfoLog", info) == GuestCalloutResult::Returned);
    std::array<char, 8> afterInvalid{}; CHECK(f.memory.read(kData + 0x360, afterInvalid.data(), afterInvalid.size()));
    CHECK(afterInvalid == logged);

    CpuRegisterState uniformCall{}; uniformCall.r[0] = 7; uniformCall.r[1] = bits(0.25f);
    uniformCall.r[2] = bits(0.5f); uniformCall.r[3] = bits(0.75f); uniformCall.r[13] = kStack;
    const auto fourth = bits(1.0f); CHECK(f.memory.write(kStack, &fourth, sizeof(fourth)));
    CHECK(f.call("_glUniform4f", uniformCall) == GuestCalloutResult::Returned);
    CHECK(uniform(0) == 0.25f && uniform(1) == 0.5f && uniform(2) == 0.75f && uniform(3) == 1.0f);

    // Writable output and CPU dirty tracking, rather than treating read-only
    // guest memory as a writable host pointer.
    CpuRegisterState buffers{}; buffers.r[0] = 1; buffers.r[1] = 0x30000;
    const auto beforeGenerated = generatedBuffers();
    CHECK(f.call("_glGenBuffers", buffers) == GuestCalloutResult::Returned);
    CHECK(generatedBuffers() == beforeGenerated);
    buffers.r[1] = kData + 0x300;
    CHECK(f.call("_glGenBuffers", buffers) == GuestCalloutResult::Returned);
    CHECK(generatedBuffers() == beforeGenerated + 1);
    buffers.r[1] = kData + 0x311; // aligned host staging for an unaligned guest pointer
    CHECK(f.call("_glGenBuffers", buffers) == GuestCalloutResult::Returned);
    std::uint32_t unalignedName = 0;
    CHECK(f.memory.read(buffers.r[1], &unalignedName, sizeof(unalignedName)) && unalignedName == 30);
    buffers.r[0] = 0; buffers.r[1] = 0; // zero names need no output pointer
    CHECK(f.call("_glGenBuffers", buffers) == GuestCalloutResult::Returned);
    CHECK(generatedBuffers() == beforeGenerated + 3);

    // Padded pixel rows must be fully inside guest memory; width*height*4 is
    // not sufficient when UNPACK_ALIGNMENT is eight.
    CpuRegisterState alignment{}; alignment.r[0] = 0x0CF5; alignment.r[1] = 8;
    CHECK(f.call("_glPixelStorei", alignment) == GuestCalloutResult::Returned && unpackAlignment() == 8);
    CpuRegisterState texture{}; texture.r[0] = 0x0DE1; texture.r[1] = 0; texture.r[2] = 0x1907; texture.r[3] = 1;
    texture.r[13] = kStack;
    std::array<std::uint32_t, 5> textureStack{2, 0, 0x1907, 0x1401, kData + 0xFFFB};
    CHECK(f.memory.write(kStack, textureStack.data(), sizeof(textureStack)));
    CHECK(f.call("_glTexImage2D", texture) == GuestCalloutResult::Returned && textureUploads() == 0);
    textureStack[4] = kData + 0x500;
    CHECK(f.memory.write(kStack, textureStack.data(), sizeof(textureStack)));
    CHECK(f.call("_glTexImage2D", texture) == GuestCalloutResult::Returned && textureUploads() == 1);

    // PACK_ALIGNMENT also includes padding between readback rows. The last
    // row needs no trailing padding; neither a short nor read-only output can
    // reach the driver.
    alignment.r[0] = 0x0D05; alignment.r[1] = 8;
    CHECK(f.call("_glPixelStorei", alignment) == GuestCalloutResult::Returned);
    CpuRegisterState read{}; read.r[2] = 1; read.r[3] = 2; read.r[13] = kStack;
    std::array<std::uint32_t, 3> readStack{0x1907, 0x1401, kData + 0xfffb};
    CHECK(f.memory.write(kStack, readStack.data(), sizeof(readStack)));
    CHECK(f.call("_glReadPixels", read) == GuestCalloutResult::Returned && pixelReads() == 0);
    readStack[2] = 0x30000; CHECK(f.memory.write(kStack, readStack.data(), sizeof(readStack)));
    CHECK(f.call("_glReadPixels", read) == GuestCalloutResult::Returned && pixelReads() == 0);
    readStack[2] = kData + 0xfff5; CHECK(f.memory.write(kStack, readStack.data(), sizeof(readStack)));
    CHECK(f.call("_glReadPixels", read) == GuestCalloutResult::Returned && pixelReads() == 1);
    unsigned char last = 0; CHECK(f.memory.read(kData + 0xffff, &last, 1) && last == 0x5a);
    // Four unsigned-int RGBA components need 16 bytes per pixel, not four.
    readStack = {0x1908, 0x1405, kData + 0xfff8};
    CHECK(f.memory.write(kStack, readStack.data(), sizeof(readStack)));
    CHECK(f.call("_glReadPixels", read) == GuestCalloutResult::Returned && pixelReads() == 1);

    // Client arrays are bounded at draw time and never passed as guest offsets.
    CpuRegisterState bindBuffer{}; bindBuffer.r[0] = 0x8892; bindBuffer.r[1] = 0;
    CHECK(f.call("_glBindBuffer", bindBuffer) == GuestCalloutResult::Returned);
    CpuRegisterState pointer{}; pointer.r[0] = 0; pointer.r[1] = 2; pointer.r[2] = 0x1406; pointer.r[3] = 0;
    pointer.r[13] = kStack; const std::array<std::uint32_t, 2> extra{0, kData + 0x400};
    CHECK(f.memory.write(kStack, extra.data(), sizeof(extra)));
    CHECK(f.call("_glVertexAttribPointer", pointer) == GuestCalloutResult::Returned);
    CpuRegisterState enable{}; enable.r[0] = 0;
    CHECK(f.call("_glEnableVertexAttribArray", enable) == GuestCalloutResult::Returned);
    CpuRegisterState draw{}; draw.r[0] = 4; draw.r[1] = 0; draw.r[2] = 3;
    CHECK(f.call("_glDrawArrays", draw) == GuestCalloutResult::Returned);
    CHECK(draws() == 1 && attributeBuffer() == 0 && attributePointer() != kData + 0x400);
    draw.r[1] = 0x7fffffff; draw.r[2] = 3;
    CHECK(f.call("_glDrawArrays", draw) == GuestCalloutResult::Returned && draws() == 1);

    // A deleted VBO no longer backs the captured attribute, so an old offset
    // cannot bypass client-memory bounds checks after the deletion.
    bindBuffer.r[1] = 30; CHECK(f.call("_glBindBuffer", bindBuffer) == GuestCalloutResult::Returned);
    const std::array<std::uint32_t, 2> offset{0, 8}; CHECK(f.memory.write(kStack, offset.data(), sizeof(offset)));
    CHECK(f.call("_glVertexAttribPointer", pointer) == GuestCalloutResult::Returned);
    const std::uint32_t deleted = 30; CHECK(f.memory.write(kData + 0x321, &deleted, sizeof(deleted)));
    CpuRegisterState erase{}; erase.r[0] = 1; erase.r[1] = kData + 0x321;
    CHECK(f.call("_glDeleteBuffers", erase) == GuestCalloutResult::Returned);
    draw.r[1] = 0; draw.r[2] = 3;
    CHECK(f.call("_glDrawArrays", draw) == GuestCalloutResult::Returned && draws() == 1);

    // Null current context detaches but keeps the real context for reselection;
    // an additional EAGLContext is refused rather than aliasing GL state.
    CpuRegisterState clear{}; clear.r[2] = 0; f.send(klass, "setCurrentContext:", clear); CHECK(clear.r[0] == 1);
    CpuRegisterState again{}; again.r[2] = context; f.send(klass, "setCurrentContext:", again);
    CHECK(again.r[0] == 1 && contexts() == 1);
    CpuRegisterState anotherAlloc{}; f.send(klass, "alloc", anotherAlloc);
    CpuRegisterState anotherInit{}; anotherInit.r[2] = 2; f.send(anotherAlloc.r[0], "initWithAPI:", anotherInit);
    CpuRegisterState anotherSelect{}; anotherSelect.r[2] = anotherAlloc.r[0]; f.send(klass, "setCurrentContext:", anotherSelect);
    CHECK(anotherSelect.r[0] == 0 && contexts() == 1);
    CpuRegisterState es3{}; es3.r[2] = 3; f.send(anotherAlloc.r[0], "initWithAPI:", es3); CHECK(es3.r[0] == 0);
    clear.r[2] = 0; f.send(klass, "setCurrentContext:", clear); CHECK(clear.r[0] == 1);
    CpuRegisterState release{}; f.send(context, "release", release);
    f.send(anotherAlloc.r[0], "release", release);
    dlclose(gles2); dlclose(egl);
}
