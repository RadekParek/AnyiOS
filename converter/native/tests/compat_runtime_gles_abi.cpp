#include "compat_runtime/gles_shims.hpp"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)
using namespace radek::compat_runtime;

static std::uint32_t bits(float value) {
    std::uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

int main() {
    void *mock = dlopen("libGLESv1_CM.so.1", RTLD_NOW | RTLD_LOCAL);
    CHECK(mock != nullptr);
    auto color = reinterpret_cast<float (*)(unsigned int)>(dlsym(mock, "recordedColor"));
    auto function = reinterpret_cast<unsigned int (*)()>(dlsym(mock, "recordedAlphaFunction"));
    auto reference = reinterpret_cast<float (*)()>(dlsym(mock, "recordedAlphaReference"));
    auto recorded = [mock](const char *name) { auto value = dlsym(mock, name); CHECK(value); return value; };
    auto arrayBuffer = reinterpret_cast<unsigned (*)()>(recorded("recordedArrayBuffer"));
    auto vertexBuffer = reinterpret_cast<unsigned (*)()>(recorded("recordedVertexBuffer"));
    auto normalBuffer = reinterpret_cast<unsigned (*)()>(recorded("recordedNormalBuffer"));
    auto normalType = reinterpret_cast<unsigned (*)()>(recorded("recordedNormalType"));
    auto normalStride = reinterpret_cast<int (*)()>(recorded("recordedNormalStride"));
    auto vertexPointer = reinterpret_cast<const void *(*)()>(recorded("recordedVertexPointer"));
    auto normalPointer = reinterpret_cast<const void *(*)()>(recorded("recordedNormalPointer"));
    auto draws = reinterpret_cast<unsigned (*)()>(recorded("recordedDraws"));
    auto framebuffer = reinterpret_cast<unsigned (*)()>(recorded("recordedFramebuffer"));
    CHECK(color && function && reference);
    GuestAddressSpace memory;
    memory.mapAt(0x10000, 4096, MemoryPermission::Read | MemoryPermission::Write, "arrays");
    ShimRegistry registry;
    gles::Forwarder forwarder;
    forwarder.registerBindings(registry);
    std::string reason;
    CpuRegisterState r{};
    r.r[0] = bits(0.25f); r.r[1] = bits(0.5f); r.r[2] = bits(0.75f); r.r[3] = bits(1.0f);
    r.d[0] = 0; r.d[1] = 0; // VFP values must not be interpreted as parameters.
    auto clear = registry.resolve("_glClearColor");
    CHECK(clear.has_value());
    CHECK(registry.invokeCallout(clear->guestAddress, r, memory, reason) == GuestCalloutResult::Returned);
    CHECK(color(0) == 0.25f && color(1) == 0.5f && color(2) == 0.75f && color(3) == 1.0f);
    r.r[0] = 0x0204; r.r[1] = bits(0.125f); // mixed integer/float arguments
    auto alpha = registry.resolve("_glAlphaFunc");
    CHECK(alpha.has_value());
    CHECK(registry.invokeCallout(alpha->guestAddress, r, memory, reason) == GuestCalloutResult::Returned);
    CHECK(function() == 0x0204 && reference() == 0.125f);
    auto call = [&](const char *name, CpuRegisterState &args) {
        const auto binding = registry.resolve(name); CHECK(binding);
        CHECK(registry.invokeCallout(binding->guestAddress, args, memory, reason) == GuestCalloutResult::Returned);
    };
    // glNormalPointer has three arguments (no size) and always has 3 components.
    CpuRegisterState normal{}; normal.r[0] = 0x1406; normal.r[1] = 12; normal.r[2] = 0x10000;
    call("_glNormalPointer", normal);
    CpuRegisterState enable{}; enable.r[0] = 0x8075; call("_glEnableClientState", enable);
    CpuRegisterState draw{}; draw.r[0] = 4; draw.r[1] = 0; draw.r[2] = 2;
    call("_glDrawArrays", draw);
    CHECK(draws() == 1 && normalType() == 0x1406 && normalStride() == 12);
    CHECK(normalBuffer() == 0 && reinterpret_cast<std::uintptr_t>(normalPointer()) != 0x10000);

    // The vertex pointer captures buffer 7 even though buffer zero is rebound
    // before the draw. Other enabled arrays still use their own client memory.
    CpuRegisterState bind{}; bind.r[0] = 0x8892; bind.r[1] = 7; call("_glBindBuffer", bind);
    CpuRegisterState vertex{}; vertex.r[0] = 2; vertex.r[1] = 0x1406; vertex.r[2] = 0; vertex.r[3] = 8;
    call("_glVertexPointer", vertex);
    enable.r[0] = 0x8074; call("_glEnableClientState", enable);
    bind.r[1] = 0; call("_glBindBuffer", bind);
    call("_glDrawArrays", draw);
    CHECK(draws() == 2 && vertexBuffer() == 7 && reinterpret_cast<std::uintptr_t>(vertexPointer()) == 8);
    CHECK(normalBuffer() == 0 && arrayBuffer() == 0);

    // With GPU-only indices the maximum client index is unknowable here; do
    // not hand an unbounded normal array to the driver.
    bind.r[0] = 0x8893; bind.r[1] = 9; call("_glBindBuffer", bind);
    CpuRegisterState indexed{}; indexed.r[0] = 4; indexed.r[1] = 2; indexed.r[2] = 0x1403;
    call("_glDrawElements", indexed); CHECK(draws() == 2);
    draw.r[1] = 0x7fffffff; draw.r[2] = 2;
    call("_glDrawArrays", draw); CHECK(draws() == 2);

    // Deleting a VBO resets both its current binding and the captured pointer
    // binding; an old byte offset must now be checked as guest client memory.
    bind.r[0] = 0x8892; bind.r[1] = 7; call("_glBindBuffer", bind);
    const std::uint32_t deleted = 7; CHECK(memory.write(0x10011, &deleted, sizeof(deleted)));
    CpuRegisterState erase{}; erase.r[0] = 1; erase.r[1] = 0x10011; // intentionally unaligned
    call("_glDeleteBuffers", erase); CHECK(arrayBuffer() == 0);
    draw.r[1] = 0; draw.r[2] = 2; call("_glDrawArrays", draw); CHECK(draws() == 2);
    // The ES1 OES attachment path must also detach and clear deleted guest
    // state rather than continuing to draw into the EGL default framebuffer.
    CHECK(forwarder.makeCurrent(7, 1, 320, 200));
    CpuRegisterState rb{}; rb.r[0] = 0x8D41; rb.r[1] = 40; call("_glBindRenderbufferOES", rb);
    CHECK(forwarder.attachDrawable(memory, 320, 200));
    CpuRegisterState fb{}; fb.r[0] = 0x8D40; fb.r[1] = 50; call("_glBindFramebufferOES", fb);
    CHECK(framebuffer() == 50);
    CpuRegisterState attachment{}; attachment.r[0] = 0x8D40; attachment.r[1] = 0x8CE0;
    attachment.r[2] = 0x8D41; attachment.r[3] = 40;
    call("_glFramebufferRenderbufferOES", attachment); CHECK(framebuffer() == 0);
    CpuRegisterState detached = attachment; detached.r[2] = 0; detached.r[3] = 0;
    call("_glFramebufferRenderbufferOES", detached); CHECK(framebuffer() == 50);
    call("_glFramebufferRenderbufferOES", attachment); CHECK(framebuffer() == 0);
    const std::uint32_t deletedRenderbuffer = 40;
    CHECK(memory.write(0x10021, &deletedRenderbuffer, sizeof(deletedRenderbuffer)));
    CpuRegisterState deleteRb{}; deleteRb.r[0] = 1; deleteRb.r[1] = 0x10021;
    call("_glDeleteRenderbuffersOES", deleteRb); CHECK(framebuffer() == 50);
    CpuRegisterState query{}; query.r[0] = 0x8CA7; query.r[1] = 0x10020;
    call("_glGetIntegerv", query);
    unsigned current = 9; CHECK(memory.read(0x10020, &current, sizeof(current)) && current == 0);
    const std::uint32_t deletedFramebuffer = 50;
    CHECK(memory.write(0x10021, &deletedFramebuffer, sizeof(deletedFramebuffer)));
    call("_glDeleteFramebuffersOES", deleteRb);
    query.r[0] = 0x8CA6; call("_glGetIntegerv", query);
    CHECK(memory.read(0x10020, &current, sizeof(current)) && current == 0 && framebuffer() == 0);
    dlclose(mock);
}
