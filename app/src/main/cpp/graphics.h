#pragma once
#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <string>
namespace podnative {
// PodNative owns the backend boundary. Guest EAGL/state emulation is a separate future layer.
class GraphicsBackend {
   public:
    virtual ~GraphicsBackend() = default;
    virtual void attach(ANativeWindow*, int width, int height) = 0;
    virtual void draw(float seconds) = 0;
    // Owner-thread RGBA upload for actual imported launch artwork, not guest GLES calls.
    virtual void image(const uint8_t* rgba, int width, int height) = 0;
    virtual std::string diagnostics() const = 0;
};
std::unique_ptr<GraphicsBackend> makeAngleBackend();
}  // namespace podnative
