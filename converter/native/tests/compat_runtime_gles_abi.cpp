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
    CHECK(color && function && reference);
    GuestAddressSpace memory;
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
    dlclose(mock);
}
