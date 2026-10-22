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
    // "Verbose logging": every file the guest opens, closes or fails to open, with the path
    // it asked for and what it was mapped to. Off by default.
    void setVerbose(bool on) { verbose = on; }
    // Guest address of the standard FILE object itself (what a caller passes to fprintf).
    uint32_t standardCell(int index) const;
    // Guest address of the `FILE *` variable behind __stdinp/__stdoutp/__stderrp: the
    // guest loads this cell to obtain the FILE*, so it holds the object's address.
    uint32_t standardPointerCell(int index);
    std::string describe() const;
    size_t openFiles() const { return entries.size(); }
    // Flushes and closes every guest-owned file. Called once a guest run stops.
    void closeAll();

   private:
    Memory& memory;
    GuestHeap& heap;
    FileSystem& files;
    std::function<void(const std::string&)> logSink;
    bool verbose = false;
    struct Entry {
        std::FILE* file;
        bool owned, standard;
        std::string guestPath;
        // Open flags as the guest asked for them, in Darwin's O_* numbering, so fcntl(F_GETFL)
        // reports the mode this stream really has.
        int flags = 0;
    };
    std::map<uint32_t, Entry> entries;
    std::map<uint32_t, uint32_t> objects;  // guest FILE object -> token
    // Guest descriptors. A host descriptor is never handed to the guest: the numbers overlap
    // with files this process has open that the guest never asked for.
    std::map<int, uint32_t> descriptors;  // guest fd -> guest FILE object
    int nextDescriptor = 3;
    int descriptorFor(uint32_t object);
    uint32_t objectForDescriptor(int descriptor) const;
    uint32_t nextToken = 1;
    uint32_t stdinCell = 0, stdoutCell = 0, stderrCell = 0;
    uint32_t stdinPointerCell = 0, stdoutPointerCell = 0, stderrPointerCell = 0;
    // Where the next ungetc() byte goes and which stream it belongs to.
    std::map<uint32_t, int> pushedBack;
    uint32_t makeObject(uint32_t token, const char* what);
    uint32_t makePointerCell(uint32_t object, const char* what);
    uint32_t tokenFor(uint32_t object) const;
    Entry* lookup(uint32_t object);
    std::string readString(uint32_t address) const;
    void emit(const std::string& text) const;
    uint32_t openStream(uint32_t pathAddress, uint32_t modeAddress);
    // `vaList` is a guest va_list when non-zero (vfprintf), otherwise the arguments are read
    // from the call's own registers and stack (fprintf).
    void fprintfGuest(CpuState& cpu, size_t firstWord, uint32_t vaList = 0);
    // Reads one byte from a stream, honouring bytes pushed back by ungetc.
    int readByte(uint32_t object);
    void unreadByte(uint32_t object, int value);
};
}  // namespace podnative::runtime
