#include "openal.h"

#include <algorithm>
#include <cstring>

#include "abi.h"
#include "error.h"

namespace podnative::runtime {
namespace {
// OpenAL constants, from <al/al.h> and <al/alc.h>.
constexpr uint32_t AL_NO_ERROR = 0;
constexpr uint32_t AL_INVALID_NAME = 0xA001;
constexpr uint32_t AL_INVALID_ENUM = 0xA002;
constexpr uint32_t AL_INVALID_VALUE = 0xA003;
constexpr uint32_t AL_INVALID_OPERATION = 0xA004;
constexpr uint32_t AL_SOURCE_STATE = 0x1010;
constexpr uint32_t AL_INITIAL = 0x1011;
constexpr uint32_t AL_PLAYING = 0x1012;
constexpr uint32_t AL_PAUSED = 0x1013;
constexpr uint32_t AL_STOPPED = 0x1014;
constexpr uint32_t AL_BUFFERS_QUEUED = 0x1015;
constexpr uint32_t AL_BUFFERS_PROCESSED = 0x1016;
constexpr uint32_t AL_LOOPING = 0x1007;
constexpr uint32_t AL_BUFFER = 0x1009;
constexpr uint32_t AL_GAIN = 0x100A;
constexpr uint32_t AL_PITCH = 0x1003;
constexpr uint32_t AL_POSITION = 0x1004;
constexpr uint32_t AL_DIRECTION = 0x1005;
constexpr uint32_t AL_VELOCITY = 0x1006;
constexpr uint32_t AL_MAX_DISTANCE = 0x1023;
constexpr uint32_t AL_ROLLOFF_FACTOR = 0x1021;
constexpr uint32_t AL_REFERENCE_DISTANCE = 0x1020;
constexpr uint32_t AL_CONE_INNER_ANGLE = 0x1001;
constexpr uint32_t AL_CONE_OUTER_ANGLE = 0x1002;
constexpr uint32_t AL_CONE_OUTER_GAIN = 0x1022;
constexpr uint32_t AL_SEC_OFFSET = 0x1024;
constexpr uint32_t AL_SOURCE_RELATIVE = 0x202;
constexpr uint32_t AL_FORMAT_MONO8 = 0x1100;
constexpr uint32_t AL_FORMAT_MONO16 = 0x1101;
constexpr uint32_t AL_FORMAT_STEREO8 = 0x1102;
constexpr uint32_t AL_FORMAT_STEREO16 = 0x1103;
constexpr uint32_t ALC_TRUE = 1;
// ALC integer queries the null device answers.
constexpr uint32_t ALC_FREQUENCY = 0x1007;
constexpr uint32_t ALC_REFRESH = 0x1008;
constexpr uint32_t ALC_SYNC = 0x1009;
constexpr uint32_t ALC_MAJOR_VERSION = 0x100B;
constexpr uint32_t ALC_MINOR_VERSION = 0x100C;
constexpr uint32_t DeviceHandle = 1;
}  // namespace
void OpenAL::advance() {
    const Clock::time_point now = Clock::now();
    for (auto& entry : sources) {
        Source& source = entry.second;
        if (!source.playing) continue;
        const double elapsed =
            source.playedBefore +
            std::chrono::duration_cast<std::chrono::duration<double>>(now - source.started).count();
        while (!source.queued.empty()) {
            auto buffer = buffers.find(source.queued.front());
            if (buffer == buffers.end()) {
                source.queued.erase(source.queued.begin());
                continue;
            }
            if (elapsed < buffer->second.seconds) break;
            const uint32_t finished = source.queued.front();
            source.queued.erase(source.queued.begin());
            if (source.looping) {
                // A looping buffer goes straight back to the end of the queue and the
                // timeline restarts, which is what the guest observes as continuous play.
                source.queued.push_back(finished);
                source.playedBefore = 0.0;
                source.started = now;
                break;
            }
            source.processed.push_back(finished);
            source.playedBefore += buffer->second.seconds;
        }
        if (source.queued.empty() && !source.looping) {
            source.playing = false;
            played += elapsed;
            source.playedBefore = 0.0;
        }
    }
}
OpenAL::Source& OpenAL::sourceAt(uint32_t name) {
    auto found = sources.find(name);
    if (found == sources.end())
        throw Error("AL_BAD_SOURCE", "source name was never generated", name);
    return found->second;
}
OpenAL::Buffer& OpenAL::bufferAt(uint32_t name) {
    auto found = buffers.find(name);
    if (found == buffers.end())
        throw Error("AL_BAD_BUFFER", "buffer name was never generated", name);
    return found->second;
}
bool OpenAL::invoke(const std::string& symbol, CpuState& cpu) {
    auto& r = cpu.registers;
    advance();
    if (symbol == "_alcOpenDevice") {
        device = DeviceHandle;
        r[0] = device;
        return true;
    }
    if (symbol == "_alcCloseDevice") {
        if (r[0] != device || !device) {
            r[0] = 0;
            return true;
        }
        device = 0;
        context = 0;
        r[0] = ALC_TRUE;
        return true;
    }
    if (symbol == "_alcCreateContext") {
        if (!device || r[0] != device) {
            r[0] = 0;
            return true;
        }
        context = 2;
        r[0] = context;
        return true;
    }
    if (symbol == "_alcDestroyContext") {
        if (r[0] != context || !context) {
            r[0] = 0;
            return true;
        }
        context = 0;
        r[0] = ALC_TRUE;
        return true;
    }
    if (symbol == "_alcMakeContextCurrent") {
        if (r[0] && r[0] != context) {
            r[0] = 0;
            return true;
        }
        r[0] = ALC_TRUE;
        return true;
    }
    if (symbol == "_alcProcessContext" || symbol == "_alcSuspendContext" ||
        symbol == "_alcGetError") {
        r[0] = symbol == "_alcGetError" ? AL_NO_ERROR : 0;
        return true;
    }
    if (symbol == "_alcGetContextsDevice") {
        r[0] = context ? device : 0;
        return true;
    }
    if (symbol == "_alcGetIntegerv") {
        uint32_t value = 0;
        switch (r[1]) {
            case ALC_FREQUENCY:
                value = 44100;
                break;
            case ALC_REFRESH:
                value = 1;
                break;
            case ALC_SYNC:
                value = ALC_TRUE;
                break;
            case ALC_MAJOR_VERSION:
                value = 1;
                break;
            case ALC_MINOR_VERSION:
                value = 1;
                break;
            default:
                setError(AL_INVALID_ENUM);
                r[0] = 0;
                return true;
        }
        if (r[3] && memory.permits(r[3], 4, Write)) memory.write32(r[3], value);
        if (r[2] && memory.permits(r[2], 4, Write)) memory.write32(r[2], 1);
        r[0] = 0;
        return true;
    }
    if (symbol == "_alGetError") {
        r[0] = error;
        error = AL_NO_ERROR;
        return true;
    }
    if (symbol == "_alGenBuffers" || symbol == "_alGenSources") {
        const uint32_t count = r[0], at = r[1];
        if (count && (!at || !memory.permits(at, size_t(count) * 4, Write)))
            throw Error("AL_BUFFER", "generated names need a writable guest array", at);
        for (uint32_t n = 0; n < count; n++) {
            if (symbol == "_alGenBuffers") {
                const uint32_t name = nextBuffer++;
                buffers[name] = Buffer{};
                memory.write32(at + n * 4, name);
            } else {
                const uint32_t name = nextSource++;
                sources[name] = Source{};
                memory.write32(at + n * 4, name);
            }
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alDeleteBuffers" || symbol == "_alDeleteSources") {
        const uint32_t count = r[0], at = r[1];
        if (count && (!at || !memory.permits(at, size_t(count) * 4, Read)))
            throw Error("AL_BUFFER", "deleted names need a readable guest array", at);
        for (uint32_t n = 0; n < count; n++) {
            const uint32_t name = memory.read32(at + n * 4);
            if (symbol == "_alDeleteBuffers") {
                auto found = buffers.find(name);
                if (found == buffers.end()) {
                    setError(AL_INVALID_NAME);
                    continue;
                }
                if (found->second.queued) {
                    setError(AL_INVALID_OPERATION);
                    continue;
                }
                buffers.erase(found);
            } else {
                auto found = sources.find(name);
                if (found == sources.end()) {
                    setError(AL_INVALID_NAME);
                    continue;
                }
                // Deleting a source releases the buffers it held, as OpenAL specifies.
                for (uint32_t queued : found->second.queued) {
                    auto buffer = buffers.find(queued);
                    if (buffer != buffers.end()) buffer->second.queued = false;
                }
                sources.erase(found);
            }
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alBufferData") {
        Buffer& buffer = bufferAt(r[0]);
        const uint32_t format = r[1], bytes = r[3], frequency = r[4];
        int channels = 0, bits = 0;
        switch (format) {
            case AL_FORMAT_MONO8:
                channels = 1, bits = 8;
                break;
            case AL_FORMAT_MONO16:
                channels = 1, bits = 16;
                break;
            case AL_FORMAT_STEREO8:
                channels = 2, bits = 8;
                break;
            case AL_FORMAT_STEREO16:
                channels = 2, bits = 16;
                break;
            default:
                setError(AL_INVALID_ENUM);
                r[0] = 0;
                return true;
        }
        if (!frequency) {
            setError(AL_INVALID_VALUE);
            r[0] = 0;
            return true;
        }
        if (bytes && !memory.permits(r[2], bytes, Read))
            throw Error("AL_BUFFER", "buffer data is outside guest memory", r[2]);
        buffer.format = format;
        buffer.frequency = frequency;
        buffer.pcm.assign(bytes, 0);
        if (bytes) memory.read(r[2], buffer.pcm.data(), bytes);
        const double frames = double(bytes) / double(channels * (bits / 8));
        buffer.seconds = frames / double(frequency);
        audioUploaded += bytes;
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSourceQueueBuffers") {
        Source& source = sourceAt(r[0]);
        const uint32_t count = r[1], at = r[2];
        if (count && (!at || !memory.permits(at, size_t(count) * 4, Read)))
            throw Error("AL_BUFFER", "queued names need a readable guest array", at);
        for (uint32_t n = 0; n < count; n++) {
            const uint32_t name = memory.read32(at + n * 4);
            Buffer& buffer = bufferAt(name);
            if (buffer.queued) {
                setError(AL_INVALID_OPERATION);
                continue;
            }
            buffer.queued = true;
            source.queued.push_back(name);
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSourceUnqueueBuffers") {
        Source& source = sourceAt(r[0]);
        const uint32_t count = r[1], at = r[2];
        if (count > uint32_t(source.processed.size())) {
            setError(AL_INVALID_VALUE);
            r[0] = 0;
            return true;
        }
        if (count && (!at || !memory.permits(at, size_t(count) * 4, Write)))
            throw Error("AL_BUFFER", "unqueue destination is not writable", at);
        for (uint32_t n = 0; n < count; n++) {
            const uint32_t name = source.processed.front();
            source.processed.erase(source.processed.begin());
            auto found = buffers.find(name);
            if (found != buffers.end()) found->second.queued = false;
            memory.write32(at + n * 4, name);
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSourcePlay" || symbol == "_alSourcePause" || symbol == "_alSourceStop" ||
        symbol == "_alSourceRewind" || symbol == "_alSourcePlayv") {
        if (symbol == "_alSourcePlayv") {
            const uint32_t count = r[0], at = r[1];
            for (uint32_t n = 0; n < count; n++) {
                Source& source = sourceAt(memory.read32(at + n * 4));
                if (source.queued.empty() && !source.looping) continue;
                source.playing = true;
                source.paused = false;
                source.hasPlayed = true;
                source.started = Clock::now();
            }
            r[0] = 0;
            return true;
        }
        Source& source = sourceAt(r[0]);
        if (symbol == "_alSourcePlay") {
            // Play resumes a paused source where it stopped, so the time it already played
            // is kept and only the running clock is restarted.
            if (!source.queued.empty() || source.looping) {
                source.playing = true;
                source.paused = false;
                source.hasPlayed = true;
                source.started = Clock::now();
            }
        } else if (symbol == "_alSourcePause") {
            // Pause keeps the offset it reached; the state query then reports AL_PAUSED
            // instead of pretending the source had stopped.
            if (source.playing) {
                source.playedBefore += std::chrono::duration_cast<std::chrono::duration<double>>(
                                           Clock::now() - source.started)
                                           .count();
                source.playing = false;
                source.paused = true;
            }
        } else if (symbol == "_alSourceStop") {
            source.playing = false;
            source.paused = false;
            source.playedBefore = 0.0;
            for (uint32_t name : source.queued) {
                auto buffer = buffers.find(name);
                if (buffer != buffers.end()) buffer->second.queued = false;
            }
            source.processed.insert(source.processed.end(), source.queued.begin(),
                                    source.queued.end());
            source.queued.clear();
        } else {
            source.playing = false;
            source.paused = false;
            source.playedBefore = 0.0;
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alGetBufferi") {
        Buffer& buffer = bufferAt(r[0]);
        uint32_t value = 0;
        switch (r[1]) {
            case 0x2001:
                value = buffer.frequency;
                break;  // AL_FREQUENCY
            case 0x2002:
                value = uint32_t(buffer.pcm.size());
                break;  // AL_SIZE
            case 0x2000:
                value = buffer.format;
                break;    // AL_FORMAT
            case 0x2004:  // AL_CHANNELS
                value = (buffer.format == AL_FORMAT_STEREO8 || buffer.format == AL_FORMAT_STEREO16)
                            ? 2
                            : 1;
                break;
            case 0x2003:  // AL_BITS
                value = (buffer.format == AL_FORMAT_MONO16 || buffer.format == AL_FORMAT_STEREO16)
                            ? 16
                            : 8;
                break;
            default:
                setError(AL_INVALID_ENUM);
                r[0] = 0;
                return true;
        }
        if (r[2] && memory.permits(r[2], 4, Write)) memory.write32(r[2], value);
        r[0] = 0;
        return true;
    }
    if (symbol == "_alGetSourcei") {
        Source& source = sourceAt(r[0]);
        uint32_t value = 0;
        switch (r[1]) {
            case AL_SOURCE_STATE:
                // A source that has never run is AL_INITIAL even with buffers queued; only a
                // source that played and then stopped reports AL_STOPPED.
                value = source.playing      ? AL_PLAYING
                        : source.paused     ? AL_PAUSED
                        : !source.hasPlayed ? AL_INITIAL
                                            : AL_STOPPED;
                break;
            case AL_BUFFERS_QUEUED:
                value = uint32_t(source.queued.size());
                break;
            case AL_BUFFERS_PROCESSED:
                value = uint32_t(source.processed.size());
                break;
            case AL_LOOPING:
                value = source.looping ? 1 : 0;
                break;
            case AL_BUFFER:
                value = source.queued.empty() ? 0 : source.queued.front();
                break;
            case AL_SOURCE_RELATIVE:
                value = source.relative ? 1 : 0;
                break;
            default:
                setError(AL_INVALID_ENUM);
                r[0] = 0;
                return true;
        }
        if (r[2] && memory.permits(r[2], 4, Write)) memory.write32(r[2], value);
        r[0] = 0;
        return true;
    }
    if (symbol == "_alGetSourcef") {
        Source& source = sourceAt(r[0]);
        float value = 0;
        switch (r[1]) {
            case AL_GAIN:
                value = source.gain;
                break;
            case AL_PITCH:
                value = source.pitch;
                break;
            case AL_MAX_DISTANCE:
                value = source.maxDistance;
                break;
            case AL_ROLLOFF_FACTOR:
                value = source.rolloff;
                break;
            case AL_REFERENCE_DISTANCE:
                value = source.referenceDistance;
                break;
            case AL_CONE_INNER_ANGLE:
                value = source.coneInner;
                break;
            case AL_CONE_OUTER_ANGLE:
                value = source.coneOuter;
                break;
            case AL_CONE_OUTER_GAIN:
                value = source.coneOuterGain;
                break;
            case AL_SEC_OFFSET: {
                double offset = source.playedBefore;
                if (source.playing)
                    offset = std::chrono::duration_cast<std::chrono::duration<double>>(
                                 Clock::now() - source.started)
                                 .count();
                value = float(offset);
                break;
            }
            default:
                setError(AL_INVALID_ENUM);
                r[0] = 0;
                return true;
        }
        if (r[2] && memory.permits(r[2], 4, Write)) memory.write32(r[2], floatBits(value));
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSourcei") {
        Source& source = sourceAt(r[0]);
        switch (r[1]) {
            case AL_LOOPING:
                source.looping = r[2] != 0;
                break;
            case AL_SOURCE_RELATIVE:
                source.relative = r[2] != 0;
                break;
            case AL_BUFFER: {
                for (uint32_t name : source.queued) {
                    auto buffer = buffers.find(name);
                    if (buffer != buffers.end()) buffer->second.queued = false;
                }
                source.queued.clear();
                source.processed.clear();
                if (r[2]) {
                    Buffer& buffer = bufferAt(r[2]);
                    buffer.queued = true;
                    source.queued.push_back(r[2]);
                }
                break;
            }
            default:
                setError(AL_INVALID_ENUM);
                break;
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSourcef") {
        Source& source = sourceAt(r[0]);
        const float value = bitsFloat(r[2]);
        switch (r[1]) {
            case AL_GAIN:
                source.gain = value;
                break;
            case AL_PITCH:
                source.pitch = value;
                break;
            case AL_MAX_DISTANCE:
                source.maxDistance = value;
                break;
            case AL_ROLLOFF_FACTOR:
                source.rolloff = value;
                break;
            case AL_REFERENCE_DISTANCE:
                source.referenceDistance = value;
                break;
            case AL_CONE_INNER_ANGLE:
                source.coneInner = value;
                break;
            case AL_CONE_OUTER_ANGLE:
                source.coneOuter = value;
                break;
            case AL_CONE_OUTER_GAIN:
                source.coneOuterGain = value;
                break;
            default:
                setError(AL_INVALID_ENUM);
                break;
        }
        r[0] = 0;
        return true;
    }
    if (symbol == "_alSource3f") {
        Source& source = sourceAt(r[0]);
        float* target = nullptr;
        if (r[1] == AL_POSITION) target = source.position;
        if (r[1] == AL_VELOCITY) target = source.velocity;
        if (r[1] == AL_DIRECTION) target = source.direction;
        if (!target) {
            setError(AL_INVALID_ENUM);
            r[0] = 0;
            return true;
        }
        target[0] = bitsFloat(r[2]);
        target[1] = bitsFloat(r[3]);
        // The third float is the fifth argument, which AAPCS places on the stack.
        target[2] = bitsFloat(memory.read32(r[13]));
        r[0] = 0;
        return true;
    }
    if (symbol == "_alListenerf" || symbol == "_alListener3f" || symbol == "_alListenerfv" ||
        symbol == "_alDistanceModel" || symbol == "_alDopplerFactor" ||
        symbol == "_alSpeedOfSound") {
        if (symbol == "_alListenerf" && r[0] == AL_GAIN) listenerGain = bitsFloat(r[1]);
        r[0] = 0;
        return true;
    }
    if (symbol == "_alGetListenerf") {
        if (r[0] == AL_GAIN) {
            if (r[1] && memory.permits(r[1], 4, Write))
                memory.write32(r[1], floatBits(listenerGain));
        } else {
            setError(AL_INVALID_ENUM);
        }
        r[0] = 0;
        return true;
    }
    return false;
}
std::string OpenAL::describe() const {
    if (!device) return "OpenAL: the guest did not open a device";
    return "OpenAL: null output device (no samples are rendered), " +
           std::to_string(buffers.size()) + " buffer(s), " + std::to_string(sources.size()) +
           " source(s), " + std::to_string(audioUploaded) + " PCM byte(s) uploaded, " +
           std::to_string(size_t(played)) + " s of playback timed";
}
}  // namespace podnative::runtime
