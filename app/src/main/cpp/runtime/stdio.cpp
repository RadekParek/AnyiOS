#include "stdio.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "abi.h"
#include "error.h"
#include "format.h"

namespace podnative::runtime {
namespace {
constexpr uint32_t FileMagic = 0x46494c45;  // 'FILE'
constexpr size_t CopyLimit = 16 * 1024 * 1024;
bool validMode(const std::string& mode, bool& write, bool& create, bool& append) {
    if (mode.empty() || mode.size() > 8) return false;
    write = create = append = false;
    size_t at = 0;
    if (mode[0] == 'r' || mode[0] == 'w' || mode[0] == 'a') {
        if (mode[0] == 'w') create = write = true;
        if (mode[0] == 'a') create = write = append = true;
        at = 1;
    } else if (mode[0] == '+') {
        write = true;
        at = 1;
    } else {
        return false;
    }
    for (; at < mode.size(); at++) {
        if (mode[at] == '+')
            write = true;
        else if (mode[at] != 'b' && mode[at] != 'x' && mode[at] != 'e')
            return false;
    }
    return true;
}
}  // namespace
Stdio::Stdio(Memory& memory, GuestHeap& heap, FileSystem& files)
    : memory(memory), heap(heap), files(files), logSink([](const std::string&) {}) {
    // The standard streams are real host streams so buffering and ferror behave.
    entries[1] = {stdin, false, true, "<stdin>"};
    entries[2] = {stdout, false, true, "<stdout>"};
    entries[3] = {stderr, false, true, "<stderr>"};
    stdinCell = makeObject(1, "stdin");
    stdoutCell = makeObject(2, "stdout");
    stderrCell = makeObject(3, "stderr");
    nextToken = 4;
}
void Stdio::setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
void Stdio::emit(const std::string& text) const {
    if (logSink) logSink(text);
}
uint32_t Stdio::makeObject(uint32_t token, const char* what) {
    const uint32_t object = heap.allocate(8, 8);
    if (!object)
        throw Error("STDIO_ALLOC", std::string("could not allocate a FILE object for ") + what);
    memory.write32(object, FileMagic);
    memory.write32(object + 4, token);
    objects[object] = token;
    return object;
}
uint32_t Stdio::standardCell(int index) const {
    if (index == 0) return stdinCell + 0;  // The object address doubles as the cell address;
    if (index == 1) return stdoutCell;     // the symbol points straight at the FILE pointer.
    return stderrCell;
}
uint32_t Stdio::tokenFor(uint32_t object) const {
    auto known = objects.find(object);
    if (known == objects.end() || memory.read32(object) != FileMagic)
        throw Error("STDIO_BAD_FILE", "guest passed an invalid or stale FILE pointer", object);
    return known->second;
}
Stdio::Entry* Stdio::lookup(uint32_t object) {
    const uint32_t token = tokenFor(object);
    auto entry = entries.find(token);
    if (entry == entries.end() || memory.read32(object + 4) != token)
        throw Error("STDIO_BAD_FILE", "guest used a closed FILE pointer", object);
    return &entry->second;
}
std::string Stdio::readString(uint32_t address) const { return readGuestString(memory, address); }
std::string Stdio::describe() const {
    size_t guestFiles = 0;
    for (const auto& [token, entry] : entries)
        if (entry.owned) guestFiles++;
    return std::to_string(guestFiles) + " guest file(s) open; " + files.describe();
}
void Stdio::closeAll() {
    for (auto& [token, entry] : entries) {
        if (!entry.owned || !entry.file) continue;
        std::fclose(entry.file);
        entry.file = nullptr;
        entry.owned = false;
    }
    for (const auto& [object, token] : objects) {
        if (object == stdinCell || object == stdoutCell || object == stderrCell) continue;
        heap.release(object);
    }
    objects.clear();
    entries.clear();
}
uint32_t Stdio::openStream(uint32_t pathAddress, uint32_t modeAddress) {
    const std::string path = readString(pathAddress), mode = readString(modeAddress);
    bool write = false, create = false, append = false;
    if (!validMode(mode, write, create, append)) {
        emit("fopen rejected mode \"" + mode + "\"");
        return 0;
    }
    std::string reason;
    const std::string host = files.resolve(path, write, reason);
    if (host.empty()) {
        emit("fopen(\"" + path + "\") refused: " + reason);
        return 0;
    }
    std::string hostMode;
    hostMode += (append ? 'a' : create ? 'w' : 'r');
    if (write) hostMode += '+';
    hostMode += 'b';
    std::FILE* file = std::fopen(host.c_str(), hostMode.c_str());
    if (!file) {
        emit("fopen(\"" + path + "\") failed: " + std::strerror(errno));
        return 0;
    }
    const uint32_t token = nextToken++;
    entries[token] = {file, true, false, path};
    return makeObject(token, path.c_str());
}
void Stdio::fprintfGuest(CpuState& cpu, size_t firstWord, uint32_t vaList) {
    auto& r = cpu.registers;
    Entry* entry = lookup(r[0]);
    std::string text;
    if (vaList) {
        text = formatGuestVarargs(readString(r[1]), memory, vaList);
    } else {
        Arguments args(cpu, memory, firstWord);
        text = formatGuest(readString(r[1]), args);
    }
    if (!entry->standard) {
        const size_t written =
            text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), entry->file);
        r[0] = uint32_t(written);
        return;
    }
    emit(text);
    r[0] = uint32_t(text.size());
}
bool Stdio::invoke(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    if (name == "_fopen") {
        r[0] = openStream(r[0], r[1]);
        return true;
    }
    if (name == "_fclose") {
        if (!r[0]) {
            r[0] = 0;
            return true;
        }
        const uint32_t token = tokenFor(r[0]);
        auto entry = entries.find(token);
        if (entry == entries.end()) throw Error("STDIO_BAD_FILE", "fclose on an unknown handle");
        if (entry->second.standard) {
            std::fflush(entry->second.file);
            r[0] = 0;
            return true;
        }
        const int result = std::fclose(entry->second.file);
        entries.erase(entry);
        objects.erase(r[0]);
        heap.release(r[0]);
        r[0] = result == 0 ? 0 : uint32_t(-1);
        return true;
    }
    if (name == "_fflush") {
        if (!r[0]) {
            r[0] = uint32_t(std::fflush(nullptr));
            return true;
        }
        r[0] = uint32_t(std::fflush(lookup(r[0])->file));
        return true;
    }
    if (name == "_fprintf" || name == "_vfprintf") {
        // vfprintf(file, format, ap): the varargs live in the guest's saved-argument area.
        if (name == "_vfprintf")
            fprintfGuest(cpu, 2, /*fromVaList=*/r[2]);
        else
            fprintfGuest(cpu, 2);
        return true;
    }
    if (name == "_fread" || name == "_fwrite") {
        const uint32_t buffer = r[0], size = r[1], count = r[2];
        if (size && count > CopyLimit / size)
            throw Error("STDIO_LIMIT", "stdio transfer exceeds the call budget");
        Entry* entry = lookup(r[3]);
        const size_t bytes = size_t(size) * count;
        std::vector<uint8_t> data(bytes);
        uint32_t items = 0;
        if (name == "_fread") {
            const size_t got = bytes ? std::fread(data.data(), 1, bytes, entry->file) : 0;
            if (got) memory.write(buffer, data.data(), got);
            items = size ? uint32_t(got / size) : 0;
        } else {
            if (bytes) memory.read(buffer, data.data(), bytes);
            const size_t put = bytes ? std::fwrite(data.data(), 1, bytes, entry->file) : 0;
            items = size ? uint32_t(put / size) : 0;
        }
        r[0] = items;
        return true;
    }
    if (name == "_fseek") {
        r[0] = uint32_t(std::fseek(lookup(r[0])->file, long(int32_t(r[1])), r[2]) == 0 ? 0 : -1);
        return true;
    }
    if (name == "_ftell") {
        const long position = std::ftell(lookup(r[0])->file);
        r[0] = uint32_t(position);
        return true;
    }
    if (name == "_rewind") {
        std::rewind(lookup(r[0])->file);
        return true;
    }
    if (name == "_feof") {
        r[0] = uint32_t(std::feof(lookup(r[0])->file));
        return true;
    }
    if (name == "_ferror") {
        r[0] = uint32_t(std::ferror(lookup(r[0])->file));
        return true;
    }
    if (name == "_clearerr") {
        std::clearerr(lookup(r[0])->file);
        return true;
    }
    if (name == "_fileno") {
        Entry* entry = lookup(r[0]);
        const int descriptor = entry->file == stdin    ? 0
                               : entry->file == stdout ? 1
                               : entry->file == stderr ? 2
                                                       : ::fileno(entry->file);
        r[0] = uint32_t(descriptor);
        return true;
    }
    if (name == "_fgetc" || name == "_getc") {
        r[0] = uint32_t(std::fgetc(lookup(r[0])->file));
        return true;
    }
    if (name == "_fputc" || name == "_putc") {
        r[0] = uint32_t(std::fputc(int(r[1]), lookup(r[0])->file));
        return true;
    }
    if (name == "_fgets") {
        const uint32_t buffer = r[0], size = r[1];
        Entry* entry = lookup(r[2]);
        if (size <= 1) {
            r[0] = 0;
            return true;
        }
        std::vector<char> text(std::min<uint32_t>(size, 1024 * 1024));
        if (!std::fgets(text.data(), int(text.size()), entry->file)) {
            r[0] = 0;
            return true;
        }
        const size_t length = std::strlen(text.data());
        memory.write(buffer, text.data(), length + 1);
        r[0] = buffer;
        return true;
    }
    if (name == "_fputs") {
        const std::string text = readString(r[0]);
        r[0] = uint32_t(std::fputs(text.c_str(), lookup(r[1])->file));
        return true;
    }
    if (name == "_setvbuf") {
        Entry* entry = lookup(r[0]);
        const int mode = int(r[2]);
        if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
            r[0] = uint32_t(-1);
            return true;
        }
        r[0] = uint32_t(std::setvbuf(entry->file, nullptr, mode, size_t(r[3])));
        return true;
    }
    if (name == "_remove") {
        std::string reason;
        const std::string guest = readString(r[0]);
        const std::string host = files.resolve(guest, true, reason);
        if (host.empty()) {
            emit("remove(\"" + guest + "\") refused: " + reason);
            r[0] = uint32_t(-1);
            return true;
        }
        r[0] = std::remove(host.c_str()) == 0 ? 0 : uint32_t(-1);
        return true;
    }
    if (name == "_rename") {
        std::string reason;
        const std::string from = readString(r[0]), to = readString(r[1]);
        const std::string fromHost = files.resolve(from, true, reason),
                          toHost = files.resolve(to, true, reason);
        if (fromHost.empty() || toHost.empty()) {
            emit("rename(\"" + from + "\", \"" + to + "\") refused: " + reason);
            r[0] = uint32_t(-1);
            return true;
        }
        r[0] = std::rename(fromHost.c_str(), toHost.c_str()) == 0 ? 0 : uint32_t(-1);
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
