#include "runtime/audio.h"

#include <cstring>
#include <iostream>
#include <vector>

#include "runtime/abi.h"
#include "runtime/error.h"
#include "runtime/libc.h"
#include "runtime/memory.h"
#include "runtime/openal.h"
using namespace podnative::runtime;
namespace {
void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        if (e.code == code) return;
        std::cerr << e.what() << " expected " << code << '\n';
        std::abort();
    }
    std::cerr << "Expected fault " << code << '\n';
    std::abort();
}
struct Call {
    Memory& memory;
    AudioSession& audio;
    CpuState state;
    uint32_t operator()(const std::string& name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
        state = {};
        state.registers[0] = a;
        state.registers[1] = b;
        state.registers[2] = c;
        if (!audio.invoke(name, state)) throw Error("TEST_SYMBOL", "unimplemented " + name);
        return state.registers[0];
    }
};
void put(Memory& memory, uint32_t address, const char* text) {
    memory.write(address, text, std::strlen(text) + 1);
}
}  // namespace
int main() {
    Memory memory;
    std::vector<uint8_t> page(65536, 0);
    memory.map({0x1000, 65536, uint8_t(Read | Write), "guest data"}, page.data(), page.size());
    AudioSession audio(memory);
    Call call{memory, audio, {}};

    // Nothing may be configured before the session exists.
    check(call("_AudioSessionSetActive", 1) == 0x21696E69, "setActive before init fails");
    check(call("_AudioSessionGetProperty", 0x61746367, 0x2000, 0x2100) == 0x21696E69,
          "getProperty before init fails");
    check(call("_AudioSessionInitialize", 0, 0, 0) == 0, "initialize succeeds");
    check(!audio.active(), "a fresh session is inactive");

    // Category round-trips, and a query before any set reports the default (no category).
    memory.write32(0x2200, 0);
    check(call("_AudioSessionGetProperty", 0x61746367, 0x2000, 0x2200) == 0, "category query ok");
    check(memory.read32(0x2200) == 0, "an unset category reads back as zero");
    memory.write32(0x2300, 0x616D6262);  // kAudioSessionCategory_MediaPlayback
    check(call("_AudioSessionSetProperty", 0x61746367, 4, 0x2300) == 0, "set category");
    memory.write32(0x2200, 0);
    check(call("_AudioSessionGetProperty", 0x61746367, 0x2000, 0x2200) == 0, "read category");
    check(memory.read32(0x2200) == 0x616D6262, "the category the guest set is returned");

    // A wrong property size is rejected instead of truncated.
    check(call("_AudioSessionSetProperty", 0x61746367, 8, 0x2300) == 0x21737A65,
          "bad property size is reported");

    // Unknown property IDs are refused, not silently accepted.
    check(call("_AudioSessionSetProperty", 0x12345678, 4, 0x2300) == 0x70747970,
          "unknown property id is refused");
    check(call("_AudioSessionGetProperty", 0x12345678, 0x2000, 0x2200) == 0x70747970,
          "unknown property cannot be read");

    // The route query writes a real C string the guest can use.
    memory.write(0x2400, "XXXXXXXXXXXXXXXXXXXXXXXX", 24);
    check(call("_AudioSessionGetProperty", 0x726F7574, 0x2000, 0x2400) == 0, "route query ok");
    std::vector<char> route(16, 0);
    memory.read(0x2400, route.data(), 8);
    check(std::string(route.data()) == "Speaker", "route is a terminated string");

    // Sample rate is stable across reads.
    check(call("_AudioSessionGetProperty", 0x68777274, 0x2000, 0x2500) == 0, "sample rate ok");
    const float rate = bitsFloat(memory.read32(0x2500));
    check(rate == 44100.0f, "reported sample rate");

    // Activation state is tracked.
    check(call("_AudioSessionSetActive", 1) == 0, "activate");
    check(audio.active(), "session is active after activation");
    check(audio.describe().find("active") != std::string::npos, "describe reports the state");
    check(call("_AudioSessionSetActive", 0) == 0, "deactivate");
    check(!audio.active(), "session is inactive again");

    // A missing value pointer is an error, not a silent success.
    check(call("_AudioSessionGetProperty", 0x61746367, 0x2000, 0) == 0x21737A65,
          "null destination is rejected");

    // Guest libc timing and identifier helpers stay real.
    Libc libc(memory);
    CpuState state{};
    check(libc.invoke("_mach_absolute_time", state), "mach_absolute_time is implemented");
    const uint32_t first = state.registers[0];
    CpuState later{};
    check(libc.invoke("_mach_absolute_time", later), "mach_absolute_time repeats");
    check(later.registers[0] >= first, "mach time is monotonic");
    CpuState base{};
    base.registers[0] = 0x2600;
    check(libc.invoke("_mach_timebase_info", base), "mach_timebase_info is implemented");
    check(memory.read32(0x2600) == 1 && memory.read32(0x2604) == 1, "timebase is 1/1");
    put(memory, 0x2700, "hw.machine");
    CpuState sysctl{};
    sysctl.registers[0] = 0x2700;
    sysctl.registers[1] = 0x2710;
    sysctl.registers[2] = 0x2720;
    check(libc.invoke("_sysctlbyname", sysctl), "sysctlbyname is implemented");
    check(sysctl.registers[0] == 0 && memory.read32(0x2710) == 8, "hw.machine length");
    std::vector<char> machine(16, 0);
    memory.read(0x2720, machine.data(), 8);
    check(std::string(machine.data()) == "iPod5,1", "device identifier");
    put(memory, 0x2730, "no.such.key");
    CpuState missing{};
    missing.registers[0] = 0x2730;
    missing.registers[1] = 0x2710;
    missing.registers[2] = 0x2720;
    check(libc.invoke("_sysctlbyname", missing) && missing.registers[0] == uint32_t(-1),
          "unknown sysctl key fails");
    put(memory, 0x2800, "a,b;;c");
    put(memory, 0x2810, ",;");
    CpuState token{};
    token.registers[0] = 0x2800;
    token.registers[1] = 0x2810;
    token.registers[2] = 0x2820;
    check(libc.invoke("_strtok_r", token), "strtok_r first token");
    std::vector<char> firstToken(8, 0);
    memory.read(token.registers[0], firstToken.data(), 2);
    check(std::string(firstToken.data()) == "a", "first token is \"a\"");
    CpuState second{};
    second.registers[1] = 0x2810;
    second.registers[2] = 0x2820;
    check(libc.invoke("_strtok_r", second), "strtok_r second token");
    std::vector<char> secondToken(8, 0);
    memory.read(second.registers[0], secondToken.data(), 2);
    check(std::string(secondToken.data()) == "b", "empty fields are skipped, as strtok does");
    fault("LIBC_BUFFER", [&] {
        CpuState invalid{};
        invalid.registers[0] = 0x2800;
        invalid.registers[1] = 0x2810;
        invalid.registers[2] = 0x90000;
        libc.invoke("_strtok_r", invalid);
    });

    // OpenAL source state. A paused source is not a stopped one: OpenAL reports AL_PAUSED
    // until it is played again, and playing resumes from the offset it was paused at.
    {
        OpenAL al(memory);
        struct AlCall {
            OpenAL& al;
            CpuState state;
            uint32_t operator()(const std::string& name, uint32_t a = 0, uint32_t b = 0,
                                uint32_t c = 0, uint32_t d = 0, uint32_t e = 0) {
                state = {};
                state.registers[0] = a;
                state.registers[1] = b;
                state.registers[2] = c;
                state.registers[3] = d;
                state.registers[4] = e;
                if (!al.invoke(name, state)) throw Error("TEST_SYMBOL", "unimplemented " + name);
                return state.registers[0];
            }
        } call{al, {}};
        const uint32_t names = 0x8000, pcm = 0x9000, cell = 0x8100;
        const uint32_t device = call("_alcOpenDevice");
        check(device != 0, "an OpenAL device opens");
        const uint32_t context = call("_alcCreateContext", device);
        check(context != 0, "an OpenAL context is created");
        check(call("_alcMakeContextCurrent", context) == 1, "the context becomes current");
        check(call("_alGenSources", 1, names) == 0, "a source name is generated");
        const uint32_t source = memory.read32(names);
        check(call("_alGenBuffers", 1, names) == 0, "a buffer name is generated");
        const uint32_t buffer = memory.read32(names);
        check(call("_alBufferData", buffer, 0x1100 /*MONO8*/, pcm, 4096, 8000) == 0,
              "buffer data is accepted");
        memory.write32(names, buffer);
        check(call("_alSourceQueueBuffers", source, 1, names) == 0, "the buffer is queued");
        auto stateOf = [&]() {
            call("_alGetSourcei", source, 0x1010 /*AL_SOURCE_STATE*/, cell);
            return memory.read32(cell);
        };
        check(stateOf() == 0x1011, "a queued but never played source is AL_INITIAL");
        check(call("_alSourcePlay", source) == 0, "the source plays");
        check(stateOf() == 0x1012, "a playing source is AL_PLAYING");
        check(call("_alSourcePause", source) == 0, "the source pauses");
        check(stateOf() == 0x1013, "a paused source is AL_PAUSED");
        check(call("_alSourcePlay", source) == 0, "a paused source resumes");
        check(stateOf() == 0x1012, "the resumed source is AL_PLAYING again");
        check(call("_alSourceStop", source) == 0, "the source stops");
        check(stateOf() == 0x1014, "a source that played and stopped is AL_STOPPED");
        check(call("_alGetError") == 0, "no OpenAL error was recorded");
    }
    std::cout << "audio_test: ok\n";
    return 0;
}
