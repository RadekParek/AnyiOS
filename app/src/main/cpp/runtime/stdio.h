#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

#include "cpu.h"
#include "fs.h"
#include "heap.h"
#include "memory.h"

namespace podnative::runtime {
// Guest stdio over the mounted sandbox. Guest FILE* values are handle objects allocated
// inside the guest address space: the object holds a magic word and a token, and the host
// FILE handle lives in a host-side table, so a forged or stale FILE* is refused instead of
// being dereferenced. stdin/stdout/stderr are real standard streams whose output goes to
// the same log sink as printf.
class Stdio final {
   public:
    Stdio(Memory& memory, GuestHeap& heap, FileSystem& files);
    bool invoke(const std::string& symbol, CpuState& cpu);
    void setLogSink(std::function<void(const std::string&)> sink);
    // Guest address of the FILE* cell behind __stdinp/__stdoutp/__stderrp.
    uint32_t standardCell(int index) const;
    std::string describe() const;
    size_t openFiles() const { return entries.size(); }
    // Flushes and closes every guest-owned file. Called once a guest run stops.
    void closeAll();

   private:
    Memory& memory;
    GuestHeap& heap;
    FileSystem& files;
    std::function<void(const std::string&)> logSink;
    struct Entry {
        std::FILE* file;
        bool owned, standard;
        std::string guestPath;
    };
    std::map<uint32_t, Entry> entries;
    std::map<uint32_t, uint32_t> objects;  // guest FILE object -> token
    uint32_t nextToken = 1;
    uint32_t stdinCell = 0, stdoutCell = 0, stderrCell = 0;
    uint32_t makeObject(uint32_t token, const char* what);
    uint32_t tokenFor(uint32_t object) const;
    Entry* lookup(uint32_t object);
    std::string readString(uint32_t address) const;
    void emit(const std::string& text) const;
    uint32_t openStream(uint32_t pathAddress, uint32_t modeAddress);
    void fprintfGuest(CpuState& cpu, size_t firstWord);
};
}  // namespace podnative::runtime
