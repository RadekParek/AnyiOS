#include "audio.h"

#include <vector>

#include "abi.h"
#include "error.h"
#include "format.h"
#include "libc.h"

namespace podnative::runtime {
namespace {
// AudioSession property IDs, from AudioToolbox/AudioSession.h.
constexpr uint32_t kAudioSessionProperty_AudioCategory = 0x61746367;  // 'atcg'
constexpr uint32_t kAudioSessionProperty_AudioRoute = 0x726F7574;     // 'rout'
constexpr uint32_t kAudioSessionProperty_OtherAudioIsPlaying = 0x6F746872;
constexpr uint32_t kAudioSessionProperty_PreferredHardwareSampleRate = 0x68777274;
constexpr uint32_t kAudioSessionProperty_OverrideCategoryMixWithOthers = 0x6D69786F;
constexpr uint32_t kAudioSessionProperty_InputIsAvailable = 0x696E7074;
// OSStatus values. 0 is noErr; these are the documented failures we can honestly produce.
constexpr uint32_t kAudioSessionNotInitialized = 0x21696E69;  // '!ini'
constexpr uint32_t kAudioSessionUnsupportedPropertyError = 0x70747970;
constexpr uint32_t kAudioSessionBadPropertySizeError = 0x21737A65;
}  // namespace
std::string AudioSession::readString(uint32_t address) const {
    return readGuestString(memory, address);
}
bool AudioSession::invoke(const std::string& symbol, CpuState& cpu) {
    auto& r = cpu.registers;
    if (symbol == "_AudioSessionInitialize") {
        // (runLoop, runLoopMode, interruptionListener, clientData)
        if (initialised) {
            r[0] = 0;  // Re-initialising an initialised session is documented as noErr.
            return true;
        }
        initialised = true;
        isActive = false;
        r[0] = 0;
        return true;
    }
    if (symbol == "_AudioSessionSetActive" || symbol == "_AudioSessionSetActiveWithFlags") {
        if (!initialised) {
            r[0] = kAudioSessionNotInitialized;
            return true;
        }
        isActive = r[0] != 0;
        r[0] = 0;
        return true;
    }
    if (symbol == "_AudioSessionGetProperty" || symbol == "_AudioSessionGetPropertySize") {
        if (!initialised) {
            r[0] = kAudioSessionNotInitialized;
            return true;
        }
        const uint32_t property = r[0], sizeAt = r[1], valueAt = r[2];
        if (symbol == "_AudioSessionGetPropertySize") {
            if (!sizeAt) {
                r[0] = kAudioSessionBadPropertySizeError;
                return true;
            }
            const uint32_t size = property == kAudioSessionProperty_AudioCategory ? 4u
                                  : property == kAudioSessionProperty_AudioRoute  ? 32u
                                                                                  : 4u;
            memory.write32(sizeAt, size);
            r[0] = 0;
            return true;
        }
        if (!valueAt) {
            r[0] = kAudioSessionBadPropertySizeError;
            return true;
        }
        if (property == kAudioSessionProperty_AudioCategory) {
            memory.write32(valueAt, properties[kAudioSessionProperty_AudioCategory]);
            r[0] = 0;
            return true;
        }
        if (property == kAudioSessionProperty_AudioRoute) {
            // "Speaker" — the route is a real string the guest will read.
            const char route[] = "Speaker";
            memory.write(valueAt, route, sizeof(route));
            r[0] = 0;
            return true;
        }
        if (property == kAudioSessionProperty_OtherAudioIsPlaying ||
            property == kAudioSessionProperty_InputIsAvailable) {
            memory.write32(valueAt, 0);
            r[0] = 0;
            return true;
        }
        if (property == kAudioSessionProperty_PreferredHardwareSampleRate) {
            // The device output rate the renderer would run at; reported, not invented per
            // query, so repeated reads agree with each other.
            const float rate = 44100.0f;
            memory.write32(valueAt, floatBits(rate));
            r[0] = 0;
            return true;
        }
        auto stored = properties.find(property);
        if (stored == properties.end()) {
            r[0] = kAudioSessionUnsupportedPropertyError;
            return true;
        }
        memory.write32(valueAt, stored->second);
        r[0] = 0;
        return true;
    }
    if (symbol == "_AudioSessionSetProperty") {
        if (!initialised) {
            r[0] = kAudioSessionNotInitialized;
            return true;
        }
        const uint32_t property = r[0], size = r[1], valueAt = r[2];
        if (property == kAudioSessionProperty_AudioCategory) {
            if (size != 4) {
                r[0] = kAudioSessionBadPropertySizeError;
                return true;
            }
            properties[property] = memory.read32(valueAt);
            r[0] = 0;
            return true;
        }
        if (property == kAudioSessionProperty_OtherAudioIsPlaying ||
            property == kAudioSessionProperty_AudioRoute) {
            r[0] = kAudioSessionUnsupportedPropertyError;
            return true;
        }
        if (property == kAudioSessionProperty_OverrideCategoryMixWithOthers ||
            property == kAudioSessionProperty_PreferredHardwareSampleRate) {
            if (size != 4) {
                r[0] = kAudioSessionBadPropertySizeError;
                return true;
            }
            properties[property] = memory.read32(valueAt);
            r[0] = 0;
            return true;
        }
        // Unknown IDs are refused by name rather than silently accepted.
        guestLog("AudioSessionSetProperty(" + std::to_string(property) +
                 ") is not a property this runtime models");
        r[0] = kAudioSessionUnsupportedPropertyError;
        return true;
    }
    return false;
}
std::string AudioSession::describe() const {
    if (!initialised) return "audio session not initialised";
    return std::string("audio session ") + (isActive ? "active" : "inactive") + ", category=0x" +
           std::to_string(properties.count(kAudioSessionProperty_AudioCategory)
                              ? properties.at(kAudioSessionProperty_AudioCategory)
                              : 0) +
           " (state only; no audio is rendered yet)";
}
}  // namespace podnative::runtime
