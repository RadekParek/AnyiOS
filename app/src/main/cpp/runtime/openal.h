#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "memory.h"

namespace podnative::runtime {
// OpenAL over the null output device.
//
// The device, contexts, buffers and sources are real state: buffers keep the PCM the guest
// uploaded, sources queue buffers and advance through them against the host clock, and the
// queries a title polls (source state, buffers processed, playback offset) are computed from
// that timeline instead of being invented. No samples reach a speaker, because this runtime
// has no audio backend yet, so the device reports itself as OpenAL's null device and
// describe() says exactly that. Timing without rendering still matters: a title that streams
// music decides what to upload next from those queries.
class OpenAL final {
   public:
    explicit OpenAL(Memory& memory) : memory(memory) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    std::string describe() const;

   private:
    Memory& memory;
    using Clock = std::chrono::steady_clock;
    struct Buffer {
        uint32_t format = 0, frequency = 0;
        std::vector<uint8_t> pcm;
        bool queued = false;
        double seconds = 0;  // duration, from the frame size and the sample rate
    };
    struct Source {
        std::vector<uint32_t> queued;  // buffer names, front is playing
        std::vector<uint32_t> processed;
        // `paused` is its own state: OpenAL reports AL_PAUSED for a source that was
        // paused mid-buffer, and -alSourcePlay must resume from the offset it stopped at.
        bool playing = false, paused = false, looping = false, relative = false;
        // OpenAL distinguishes a source that has never run (AL_INITIAL) from one that has
        // stopped (AL_STOPPED). Queueing buffers does not count as running.
        bool hasPlayed = false;
        float gain = 1.0f, pitch = 1.0f, maxDistance = 1.0f, rolloff = 1.0f,
              referenceDistance = 1.0f;
        float coneInner = 360.0f, coneOuter = 360.0f, coneOuterGain = 0.0f;
        float position[3] = {0.0f, 0.0f, 0.0f}, velocity[3] = {0.0f, 0.0f, 0.0f},
              direction[3] = {0.0f, 0.0f, 0.0f};
        Clock::time_point started{};
        double playedBefore = 0.0;
    };
    uint32_t device = 0, context = 0;
    std::map<uint32_t, Buffer> buffers;
    std::map<uint32_t, Source> sources;
    uint32_t nextBuffer = 1, nextSource = 1, error = 0;
    float listenerGain = 1.0f;
    double played = 0.0;
    size_t audioUploaded = 0;
    void advance();
    Source& sourceAt(uint32_t name);
    Buffer& bufferAt(uint32_t name);
    void setError(uint32_t code) {
        if (!error) error = code;
    }
};
}  // namespace podnative::runtime
