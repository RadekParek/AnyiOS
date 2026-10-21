#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "cpu.h"
#include "error.h"

namespace podnative::runtime {
// iOS ARMv7 uses the base AAPCS variant of the ARM procedure call standard:
// floating-point and aggregate arguments travel in the core registers r0-r3 and then
// on the stack, and results come back in r0-r3. The VFP register file is used for
// arithmetic, not for the call boundary. Apple's ARMv7 ABI reference only deviates
// from the ARMv6 rules for VFP register preservation and prologue shape, so the
// base-standard marshalling below is the documented convention, not a host guess.
//
// Every HLE entry point that takes or returns a float, double or by-value aggregate
// marshals through these helpers so guest code sees the Apple convention.
inline uint32_t floatBits(float value) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline float bitsFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline uint64_t doubleBits(double value) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline double bitsDouble(uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
// Argument cursor. Word n lives in r0-r3 while those run out and afterwards in the
// stack argument area at sp, which is where AAPCS places the fifth and later words.
// Only whole-word values are marshalled; Apple's ABI rounds each small argument up to
// a word in both register and stack slots, so a word cursor is exact for these APIs.
class Arguments final {
   public:
    Arguments(const CpuState& cpu, Memory& memory, size_t firstWord = 0)
        : cpu(cpu), memory(memory), next(firstWord), stackBase(cpu.registers[13]) {}
    size_t consumed() const { return next; }
    Memory& memoryRef() const { return memory; }
    uint32_t word() {
        const size_t index = next++;
        return index < 4 ? cpu.registers[index]
                         : memory.read32(stackBase + uint32_t(index - 4) * 4);
    }
    int32_t signedWord() { return int32_t(word()); }
    uint32_t pointer() { return word(); }
    float f32() { return bitsFloat(word()); }
    // AAPCS requires an even-numbered core register for an eight-byte argument, so a
    // misaligned cursor skips one word rather than splitting the value.
    double f64() {
        if (next % 2) next++;
        const uint64_t low = word();
        return bitsDouble(low | uint64_t(word()) << 32);
    }
    // By-value aggregate no larger than 16 bytes: reads it word by word.
    template <typename T>
    T aggregate() {
        static_assert(std::is_trivially_copyable<T>::value, "aggregate must be trivial");
        static_assert(sizeof(T) % 4 == 0, "aggregate must be word sized");
        T value{};
        auto* bytes = reinterpret_cast<uint8_t*>(&value);
        for (size_t offset = 0; offset < sizeof(T); offset += 4) {
            const uint32_t part = word();
            std::memcpy(bytes + offset, &part, 4);
        }
        return value;
    }

   private:
    const CpuState& cpu;
    Memory& memory;
    size_t next = 0;
    uint32_t stackBase = 0;
};
// Results. Word-sized and aggregate results of at most four words come back in
// r0-r3; larger aggregates are returned through the hidden pointer the caller passes
// in r0, which callers read with Arguments(cpu, memory, 1).
inline void resultWord(CpuState& cpu, size_t index, uint32_t value) {
    cpu.registers[index] = value;
}
inline void resultBool(CpuState& cpu, bool value) { cpu.registers[0] = value ? 1 : 0; }
inline void resultF32(CpuState& cpu, float value) { cpu.registers[0] = floatBits(value); }
inline void resultF64(CpuState& cpu, double value) {
    const uint64_t bits = doubleBits(value);
    cpu.registers[0] = uint32_t(bits);
    cpu.registers[1] = uint32_t(bits >> 32);
}
template <typename T>
void resultAggregate(CpuState& cpu, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "aggregate must be trivial");
    static_assert(sizeof(T) % 4 == 0 && sizeof(T) <= 16, "aggregate must fit four words");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t offset = 0, word = 0; offset < sizeof(T); offset += 4, word++) {
        uint32_t part;
        std::memcpy(&part, bytes + offset, 4);
        cpu.registers[word] = part;
    }
}
// Hidden-result pointer plus the store that completes it, for aggregates larger than
// four words (CGAffineTransform is six).
inline uint32_t resultPointer(const CpuState& cpu) { return cpu.registers[0]; }
template <typename T>
void storeThroughPointer(Memory& memory, uint32_t pointer, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "aggregate must be trivial");
    if (!pointer) throw Error("ABI_RESULT", "null hidden-result pointer");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t offset = 0; offset < sizeof(T); offset += 4) {
        uint32_t part;
        std::memcpy(&part, bytes + offset, 4);
        memory.write32(pointer + uint32_t(offset), part);
    }
}
}  // namespace podnative::runtime
