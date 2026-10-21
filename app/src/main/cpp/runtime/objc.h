#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cpu.h"
#include "exports.h"
#include "memory.h"

namespace podnative::runtime {
// Session-owned Objective-C runtime core. Classes, instances and selectors are guest
// addresses; the object store lives on the host and is never exposed as host pointers.
// Only implemented classes/selectors resolve: anything else is a hard failure.
class ObjC final {
   public:
    explicit ObjC(Memory& memory);
    void installGates();
    void defineSymbols(ExportGraph& graph, size_t foundation) const;
    bool dispatch(uint32_t pc, CpuState& state);
    size_t gateCount() const { return entries.size(); }
    static constexpr uint32_t GateBase = 0x6f200000;

   private:
    struct Object {
        std::string className;
        uint32_t references = 1;
        uint32_t text = 0;  // NSString payload: pointer to a terminated guest string.
    };
    Memory& memory;
    std::vector<std::string> entries;
    std::map<std::string, uint32_t> classes;
    std::map<uint32_t, Object> objects;
    std::vector<std::vector<uint32_t>> pools;
    uint32_t nextObject = 0x6f400000;
    uint32_t nextText = 0x6f480000;

    void mapRange(uint32_t base, uint32_t size, const char* label);
    uint32_t classNamePointer(const std::string& name);
    uint32_t makeObject(const std::string& className);
    uint32_t internText(const std::string& text);
    std::string classNameOf(uint32_t object) const;
    std::string readText(uint32_t address) const;
    std::string selector(uint32_t address) const;
    uint32_t classObject(const std::string& name);
    void invoke(const std::string& entry, CpuState& state);
    void message(CpuState& state);
    void release(uint32_t object);
};
}  // namespace podnative::runtime
