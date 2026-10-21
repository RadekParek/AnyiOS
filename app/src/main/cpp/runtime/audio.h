#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "cpu.h"
#include "memory.h"

namespace podnative::runtime {
// AudioToolbox AudioSession state. iOS titles configure the session before any sound is
// produced, and a wrong answer here changes their behaviour (some refuse to start audio,
// others pause the game). The state machine below is real: it remembers the category,
// mode, sample rate and active flag that the guest set, returns exactly those values for
// matching property queries, and reports an error for unknown property IDs instead of
// pretending to have applied them. It does not play audio; playback APIs stay traps.
class AudioSession final {
   public:
    explicit AudioSession(Memory& memory) : memory(memory) {}
    bool invoke(const std::string& symbol, CpuState& cpu);
    bool active() const { return isActive; }
    std::string describe() const;

   private:
    Memory& memory;
    bool initialised = false, isActive = false;
    // Property IDs from AudioToolbox/AudioSession.h, keyed by their numeric value.
    std::map<uint32_t, uint32_t> properties;
    std::string readString(uint32_t address) const;
};
}  // namespace podnative::runtime
