#include "stdio.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
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

// One scanf conversion's worth of state. The input side is a pair of callbacks so the same
// engine serves sscanf (a guest string) and fscanf (a stream): scanf may consume one
// character of lookahead, and both cases can push it back.
class Scanner final {
   public:
    Scanner(Memory& memory, CpuState& cpu, size_t firstWord, std::function<int()> next,
            std::function<void(int)> back)
        : memory(memory), cpu(cpu), word(firstWord), next(std::move(next)), back(std::move(back)) {}
    int run(const std::string& format);

   private:
    Memory& memory;
    CpuState& cpu;
    size_t word;
    std::function<int()> next;
    std::function<void(int)> back;
    int conversions = 0;
    int pending = -1;  // lookahead byte, -1 when empty
    // Number of bytes consumed in total, which is what %n reports.
    size_t consumed = 0;

    uint32_t argument() {
        const size_t index = word++;
        if (index < 4) return cpu.registers[index];
        const uint32_t stack = cpu.registers[13] + uint32_t((index - 4) * 4);
        return memory.read32(stack);
    }
    int peek() {
        if (pending < 0) {
            const int value = next();
            pending = value;
        }
        return pending;
    }
    int take() {
        const int value = peek();
        if (value >= 0) {
            pending = -1;
            consumed++;
        }
        return value;
    }
    void push(int value) {
        if (value < 0) return;
        if (pending >= 0) {
            back(pending);
        } else {
            pending = value;
            consumed--;
        }
    }
    void skipSpace() {
        while (peek() >= 0 && std::isspace(uint8_t(peek()))) take();
    }
    // Collects the characters of a numeric or string token, honouring the width limit.
    std::string token(int width, bool (*accept)(int)) {
        std::string text;
        while (width <= 0 || int(text.size()) < width) {
            const int c = peek();
            if (c < 0 || !accept(c)) break;
            text.push_back(char(take()));
        }
        return text;
    }
    void store(int bytes, uint64_t value, uint32_t address) {
        if (!address) return;
        for (int n = 0; n < bytes; n++)
            memory.write(address + uint32_t(n), reinterpret_cast<const char*>(&value) + n, 1);
    }
};
int Scanner::run(const std::string& format) {
    size_t at = 0;
    while (at < format.size()) {
        const char spec = format[at];
        if (std::isspace(uint8_t(spec))) {
            skipSpace();
            at++;
            continue;
        }
        if (spec != '%') {
            if (take() != uint8_t(spec)) {
                at = format.size();
                break;
            }
            at++;
            continue;
        }
        at++;
        if (at >= format.size()) break;
        if (format[at] == '%') {
            if (take() != '%') break;
            at++;
            continue;
        }
        bool suppress = false;
        if (format[at] == '*') {
            suppress = true;
            at++;
        }
        int width = 0;
        while (at < format.size() && std::isdigit(uint8_t(format[at])))
            width = width * 10 + (format[at++] - '0');
        int bytes = 4;
        bool floating = false;
        if (at < format.size()) {
            if (format[at] == 'h') {
                bytes = 2;
                at++;
                if (at < format.size() && format[at] == 'h') {
                    bytes = 1;
                    at++;
                }
            } else if (format[at] == 'l') {
                bytes = 8;
                at++;
                if (at < format.size() && format[at] == 'l') at++;
            } else if (format[at] == 'L' || format[at] == 'q') {
                bytes = 8;
                floating = true;
                at++;
            }
        }
        if (at >= format.size()) break;
        const char conversion = format[at++];
        const uint32_t destination = suppress ? 0 : argument();
        // Every conversion except %c, %[ and %n skips leading whitespace first.
        if (conversion != 'c' && conversion != '[' && conversion != 'n') skipSpace();
        if (peek() < 0) break;
        switch (conversion) {
            case 'n':
                store(4, consumed, destination);
                continue;
            case 'd':
            case 'i':
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'p': {
                const int base = conversion == 'd' || conversion == 'u' ? 10
                                 : conversion == 'i'                    ? 0
                                 : conversion == 'o'                    ? 8
                                                                        : 16;
                std::string text =
                    token(width, [](int c) { return std::isalnum(c) || c == '-' || c == '+'; });
                if (text.empty()) return conversions;
                const bool negative = text[0] == '-';
                char* end = nullptr;
                const unsigned long long magnitude = std::strtoull(text.c_str(), &end, base);
                if (end == text.c_str()) {
                    // Digits were consumed but did not form a number: scanf fails here and
                    // the input stays where the failed conversion started.
                    return conversions;
                }
                const uint64_t value = negative ? 0ULL - magnitude : magnitude;
                if (!suppress) store(bytes == 8 ? 8 : 4, value, destination);
                if (!suppress) conversions++;
                // Anything the C parser rejected goes back to the input.
                for (size_t n = text.size(); n > size_t(end - text.c_str()); n--)
                    push(uint8_t(text[n - 1]));
                break;
            }
            case 'a':
            case 'A':
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G': {
                std::string text = token(width, [](int c) {
                    return std::isalnum(c) || c == '.' || c == '+' || c == '-' || c == 'e' ||
                           c == 'E' || c == 'p' || c == 'P' || c == 'x' || c == 'X';
                });
                if (text.empty()) return conversions;
                char* end = nullptr;
                const double value = std::strtod(text.c_str(), &end);
                if (end == text.c_str()) return conversions;
                if (!suppress) {
                    if (floating || bytes == 8) {
                        store(8, doubleBits(value), destination);
                    } else {
                        store(4, floatBits(float(value)), destination);
                    }
                    conversions++;
                }
                for (size_t n = text.size(); n > size_t(end - text.c_str()); n--)
                    push(uint8_t(text[n - 1]));
                break;
            }
            case 's': {
                std::string text = token(width, [](int c) { return !std::isspace(c); });
                if (text.empty()) return conversions;
                if (!suppress) {
                    memory.write(destination, text.c_str(), text.size());
                    const char terminator = 0;
                    memory.write(destination + uint32_t(text.size()), &terminator, 1);
                    conversions++;
                }
                break;
            }
            case 'c': {
                const int count = width > 0 ? width : 1;
                std::string text;
                while (int(text.size()) < count && peek() >= 0) text.push_back(char(take()));
                if (int(text.size()) < count) return conversions;
                if (!suppress) {
                    memory.write(destination, text.c_str(), text.size());
                    conversions++;
                }
                break;
            }
            case '[': {
                // A scanset: everything up to the closing ']' is either an explicit set or,
                // with a leading '^', its complement.
                const bool negate = at < format.size() && format[at] == '^';
                if (negate) at++;
                std::string set;
                while (at < format.size() && format[at] != ']') set.push_back(format[at++]);
                if (at < format.size()) at++;
                std::string text;
                while (width <= 0 || int(text.size()) < width) {
                    const int c = peek();
                    if (c < 0) break;
                    const bool member = set.find(char(c)) != std::string::npos;
                    if (member == negate) break;
                    text.push_back(char(take()));
                }
                if (text.empty()) return conversions;
                if (!suppress) {
                    memory.write(destination, text.c_str(), text.size());
                    const char terminator = 0;
                    memory.write(destination + uint32_t(text.size()), &terminator, 1);
                    conversions++;
                }
                break;
            }
            default:
                throw Error("STDIO_FORMAT", std::string("sscanf conversion '%") + conversion +
                                                "' is not supported");
        }
        if (peek() >= 0) {
            // Leave the unconsumed lookahead for the stream it came from.
            back(pending);
            pending = -1;
        }
    }
    if (pending >= 0) {
        back(pending);
        pending = -1;
    }
    return conversions;
}
}  // namespace
Stdio::Stdio(Memory& memory, GuestHeap& heap, FileSystem& files)
    : memory(memory), heap(heap), files(files), logSink([](const std::string&) {}) {
    // The standard streams are real host streams so buffering and ferror behave.
    entries[1] = {stdin, false, true, "<stdin>", 0};
    entries[2] = {stdout, false, true, "<stdout>", 1};
    entries[3] = {stderr, false, true, "<stderr>", 1};
    stdinCell = makeObject(1, "stdin");
    stdoutCell = makeObject(2, "stdout");
    stderrCell = makeObject(3, "stderr");
    // __stdinp/__stdoutp/__stderrp are `FILE *` variables: their symbol address is a cell
    // that holds the FILE object's address, which is what a guest loads before calling
    // fprintf.
    stdinPointerCell = makePointerCell(stdinCell, "__stdinp");
    stdoutPointerCell = makePointerCell(stdoutCell, "__stdoutp");
    stderrPointerCell = makePointerCell(stderrCell, "__stderrp");
    nextToken = 4;
}
uint32_t Stdio::makePointerCell(uint32_t object, const char* what) {
    const uint32_t cell = heap.allocate(4, 4);
    if (!cell) throw Error("STDIO_ALLOC", std::string("could not allocate ") + what);
    memory.write32(cell, object);
    return cell;
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
    if (index == 0) return stdinCell;
    if (index == 1) return stdoutCell;
    return stderrCell;
}
uint32_t Stdio::standardPointerCell(int index) {
    if (index == 0) return stdinPointerCell;
    if (index == 1) return stdoutPointerCell;
    return stderrPointerCell;
}
int Stdio::readByte(uint32_t object) {
    auto pushed = pushedBack.find(object);
    if (pushed != pushedBack.end()) {
        const int value = pushed->second;
        pushedBack.erase(pushed);
        return value;
    }
    return std::fgetc(lookup(object)->file);
}
void Stdio::unreadByte(uint32_t object, int value) {
    // One byte of pushback per stream, as C guarantees. A second consecutive ungetc is a
    // documented failure rather than a silently lost byte.
    if (pushedBack.count(object))
        throw Error("STDIO_UNGETC", "two consecutive ungetc calls on one stream", object);
    pushedBack[object] = value;
}
uint32_t Stdio::tokenFor(uint32_t object) const {
    auto known = objects.find(object);
    if (known == objects.end() || memory.read32(object) != FileMagic)
        throw Error("STDIO_BAD_FILE", "guest passed an invalid or stale FILE pointer", object);
    return known->second;
}
int Stdio::descriptorFor(uint32_t object) {
    // A descriptor is allocated on first use, so `fileno(stdout)` and an explicit open() both
    // hand the guest a number from this runtime's own space, never a host descriptor.
    lookup(object);
    if (object == standardCell(0)) return 0;
    if (object == standardCell(1)) return 1;
    if (object == standardCell(2)) return 2;
    for (const auto& [descriptor, known] : descriptors)
        if (known == object) return descriptor;
    const int descriptor = nextDescriptor++;
    descriptors[descriptor] = object;
    return descriptor;
}
uint32_t Stdio::objectForDescriptor(int descriptor) const {
    auto found = descriptors.find(descriptor);
    if (found != descriptors.end()) return found->second;
    if (descriptor == 0) return standardCell(0);
    if (descriptor == 1) return standardCell(1);
    if (descriptor == 2) return standardCell(2);
    return 0;
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
    if (verbose) emit("stdio: fopen(\"" + path + "\", \"" + mode + "\") -> " + host);
    const uint32_t token = nextToken++;
    // Darwin's O_* numbering: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 8, O_CREAT 0x200,
    // O_TRUNC 0x400.
    int flags = write ? (append || create ? 2 : 2) : 0;
    if (append) flags |= 8;
    if (create) flags |= 0x400;
    entries[token] = {file, true, false, path, flags};
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
        r[0] = uint32_t(descriptorFor(r[0]));
        return true;
    }
    if (name == "_fgetc" || name == "_getc") {
        lookup(r[0]);  // validates the handle before any host call
        r[0] = uint32_t(readByte(r[0]));
        return true;
    }
    if (name == "_fputc" || name == "_putc") {
        r[0] = uint32_t(std::fputc(int(r[1]), lookup(r[0])->file));
        return true;
    }
    if (name == "_fgets") {
        const uint32_t buffer = r[0], size = r[1];
        if (size <= 1) {
            r[0] = 0;
            return true;
        }
        // Read through readByte so a pushed-back byte is returned first, and stop after a
        // newline or size-1 bytes exactly as C specifies.
        std::string text;
        const size_t limit = std::min<size_t>(size - 1, 1024 * 1024);
        while (text.size() < limit) {
            const int c = readByte(r[2]);
            if (c < 0) break;
            text.push_back(char(c));
            if (c == '\n') break;
        }
        if (text.empty()) {
            r[0] = 0;
            return true;
        }
        memory.write(buffer, text.c_str(), text.size());
        const char terminator = 0;
        memory.write(buffer + uint32_t(text.size()), &terminator, 1);
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
    if (name == "_freopen") {
        // freopen(path, mode, stream): the stream object stays, the underlying file changes.
        const uint32_t path = r[0], mode = r[1], object = r[2];
        Entry* entry = lookup(object);
        if (entry->standard && entry->owned)
            throw Error("STDIO_BAD_FILE", "a standard stream cannot own a guest file", object);
        const uint32_t reopened = openStream(path, mode);
        if (!reopened) {
            r[0] = 0;
            return true;
        }
        const uint32_t token = tokenFor(reopened);
        // Move the newly opened file into this stream's slot and drop the new object: a
        // successful freopen returns the original stream, not a new one.
        Entry fresh = entries[token];
        entries.erase(token);
        objects.erase(reopened);
        heap.release(reopened);
        if (entry->owned && entry->file) std::fclose(entry->file);
        *entry = fresh;
        entry->standard = true;  // bound to the caller's stream object from now on
        r[0] = object;
        return true;
    }
    if (name == "_ungetc") {
        const int value = int(r[0]);
        if (value < 0 || value > 255) {
            r[0] = uint32_t(-1);
            return true;
        }
        if (pushedBack.count(r[1])) {
            r[0] = uint32_t(-1);
            return true;
        }
        pushedBack[r[1]] = value;
        r[0] = uint32_t(value);
        return true;
    }
    if (name == "_sscanf") {
        const std::string input = readString(r[0]);
        const std::string format = readString(r[1]);
        auto cursor = std::make_shared<size_t>(0);
        Scanner scanner(
            memory, cpu, 2,
            [&input, cursor]() {
                return *cursor < input.size() ? int(uint8_t(input[(*cursor)++])) : -1;
            },
            [&input, cursor](int value) {
                (void)value;
                if (*cursor) (*cursor)--;
            });
        r[0] = uint32_t(scanner.run(format));
        return true;
    }
    if (name == "_fscanf") {
        Entry* entry = lookup(r[0]);
        std::FILE* file = entry->file;
        const std::string format = readString(r[1]);
        Scanner scanner(
            memory, cpu, 2, [file]() { return std::fgetc(file); },
            [file](int value) { std::ungetc(value, file); });
        r[0] = uint32_t(scanner.run(format));
        return true;
    }
    if (name == "_perror") {
        const std::string prefix = r[0] ? readString(r[0]) : std::string();
        emit(prefix.empty() ? std::string(std::strerror(errno))
                            : prefix + ": " + std::strerror(errno));
        r[0] = 0;
        return true;
    }
    if (name == "_setbuf") {
        // The host stream's own buffering stays; a guest-supplied buffer cannot be handed to
        // the host libc, so a non-null buffer is refused instead of silently ignored.
        if (r[1]) {
            emit("setbuf with a guest buffer is not supported; the host buffer is kept");
            r[0] = uint32_t(-1);
            return true;
        }
        std::setbuf(lookup(r[0])->file, nullptr);
        r[0] = 0;
        return true;
    }
    if (name == "_tmpfile") {
        std::FILE* file = std::tmpfile();
        if (!file) {
            r[0] = 0;
            return true;
        }
        const uint32_t token = nextToken++;
        entries[token] = {file, true, false, "<tmpfile>"};
        r[0] = makeObject(token, "tmpfile");
        return true;
    }
    // POSIX file descriptors over the same sandboxed handles. These are the calls a title
    // uses when it bypasses stdio; a descriptor is validated against the table above, so a
    // number the guest invented is refused instead of reaching a host file.
    if (name == "_open" || name == "_creat") {
        const uint32_t pathAddress = r[0];
        const std::string path = readString(pathAddress);
        // O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 8, O_CREAT 0x200, O_TRUNC 0x400.
        const int flags = name == "_creat" ? (1 | 0x200 | 0x400) : int(r[1]);
        const char* mode = (flags & 3) == 0   ? "r"
                           : (flags & 3) == 1 ? ((flags & 8) ? "a" : "w")
                                              : "r+";
        if (verbose) emit("stdio: open(\"" + path + "\") flags=0x" + std::to_string(flags));
        const uint32_t modeAddress = heap.allocate(4, 1);
        if (!modeAddress) throw Error("STDIO_ALLOC", "could not hold an open mode");
        memory.write(modeAddress, mode, std::strlen(mode) + 1);
        const uint32_t object = openStream(pathAddress, modeAddress);
        heap.release(modeAddress);
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        Entry* entry = lookup(object);
        entry->flags = flags;
        const int descriptor = nextDescriptor++;
        descriptors[descriptor] = object;
        r[0] = uint32_t(descriptor);
        return true;
    }
    if (name == "_read") {
        const int descriptor = int(r[0]);
        const uint32_t buffer = r[1], count = r[2];
        const uint32_t object = objectForDescriptor(descriptor);
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        if (count > 64 * 1024 * 1024)
            throw Error("STDIO_LIMIT", "read() size exceeds the guest buffer budget", count);
        if (!memory.permits(buffer, count, Write))
            throw Error("STDIO_BUFFER", "read() destination is not writable", buffer);
        uint32_t done = 0;
        while (done < count) {
            const int value = readByte(object);
            if (value < 0) break;
            const uint8_t byte = uint8_t(value);
            memory.write(buffer + done, &byte, 1);
            done++;
        }
        r[0] = done;
        return true;
    }
    if (name == "_write") {
        const int descriptor = int(r[0]);
        const uint32_t buffer = r[1], count = r[2];
        const uint32_t object = objectForDescriptor(descriptor);
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        if (count > 64 * 1024 * 1024)
            throw Error("STDIO_LIMIT", "write() size exceeds the guest buffer budget", count);
        if (!memory.permits(buffer, count, Read))
            throw Error("STDIO_BUFFER", "write() source is not readable", buffer);
        std::vector<uint8_t> bytes(count);
        if (count) memory.read(buffer, bytes.data(), count);
        Entry* entry = lookup(object);
        if (entry->standard) {
            emit(std::string(bytes.begin(), bytes.end()));
            r[0] = count;
            return true;
        }
        const size_t written = count ? std::fwrite(bytes.data(), 1, count, entry->file) : 0;
        r[0] = uint32_t(written);
        return true;
    }
    if (name == "_close") {
        const int descriptor = int(r[0]);
        const uint32_t object = objectForDescriptor(descriptor);
        if (!object || descriptor < 3) {
            r[0] = uint32_t(-1);
            return true;
        }
        descriptors.erase(descriptor);
        CpuState nested = cpu;
        nested.registers[0] = object;
        invoke("_fclose", nested);
        r[0] = nested.registers[0];
        return true;
    }
    if (name == "_lseek") {
        const int descriptor = int(r[0]);
        const int32_t offset = int32_t(r[1]);
        const int whence = int(r[2]);
        const uint32_t object = objectForDescriptor(descriptor);
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        Entry* entry = lookup(object);
        const int origin = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
        if (std::fseek(entry->file, long(offset), origin) != 0) {
            r[0] = uint32_t(-1);
            return true;
        }
        const long position = std::ftell(entry->file);
        r[0] = position < 0 ? uint32_t(-1) : uint32_t(position);
        return true;
    }
    if (name == "_fcntl") {
        const int descriptor = int(r[0]);
        const int command = int(r[1]);
        const uint32_t object = objectForDescriptor(descriptor);
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        Entry* entry = lookup(object);
        // F_DUPFD 0, F_GETFD 1, F_SETFD 2, F_GETFL 3, F_SETFL 4.
        if (command == 0) {
            const int copy = nextDescriptor++;
            descriptors[copy] = object;
            r[0] = uint32_t(copy);
        } else if (command == 1) {
            r[0] = 0;  // no close-on-exec bit is modelled
        } else if (command == 2) {
            r[0] = 0;
        } else if (command == 3) {
            r[0] = uint32_t(entry->flags);
        } else if (command == 4) {
            // A stream's buffering is owned by this runtime; the guest cannot change the
            // host's mode behind its back, and saying so is better than pretending.
            if (verbose) emit("stdio: fcntl(F_SETFL) is accepted, host buffering is unchanged");
            r[0] = 0;
        } else {
            r[0] = uint32_t(-1);
        }
        return true;
    }
    if (name == "_select") {
        const int count = int(r[0]);
        const uint32_t readSet = r[1], writeSet = r[2], exceptSet = r[3];
        if (count < 0 || count > 1024)
            throw Error("STDIO_LIMIT", "select() descriptor count is implausible", uint32_t(count));
        int ready = 0;
        auto scan = [&](uint32_t set, bool writing) {
            if (!set) return;
            const uint32_t words = uint32_t((count + 31) / 32);
            if (!memory.permits(set, words * 4, Read | Write))
                throw Error("STDIO_BUFFER", "select() set is not writable", set);
            std::vector<uint32_t> bits(words);
            for (uint32_t word = 0; word < words; word++)
                bits[word] = memory.read32(set + word * 4);
            for (uint32_t word = 0; word < words; word++) {
                for (uint32_t bit = 0; bit < 32; bit++) {
                    const int descriptor = int(word * 32 + bit);
                    if (descriptor >= count) continue;
                    const uint32_t object = objectForDescriptor(descriptor);
                    if (!(bits[word] & (1u << bit))) continue;
                    if (!object) {
                        bits[word] &= ~(1u << bit);
                        continue;
                    }
                    // A regular file or a standard stream with no event source behind it is
                    // ready immediately, which is what a device reports for filedescriptors.
                    Entry* entry = lookup(object);
                    (void)writing;
                    (void)entry;
                    ready++;
                }
                memory.write32(set + word * 4, bits[word]);
            }
        };
        scan(readSet, false);
        scan(writeSet, true);
        (void)exceptSet;
        r[0] = uint32_t(ready);
        return true;
    }
    if (name == "_fsync") {
        const uint32_t object = objectForDescriptor(int(r[0]));
        if (!object) {
            r[0] = uint32_t(-1);
            return true;
        }
        std::fflush(lookup(object)->file);
        r[0] = 0;
        return true;
    }
    if (name == "_tmpnam") {
        // A name in the guest's Documents directory, unique per call.
        static uint32_t counter = 0;
        const std::string path = "tmp" + std::to_string(++counter);
        const uint32_t capacity = 1024;
        if (r[0]) {
            memory.write(r[0], path.c_str(), path.size());
            const char terminator = 0;
            memory.write(r[0] + uint32_t(path.size()), &terminator, 1);
            r[0] = r[0];
            return true;
        }
        const uint32_t buffer = heap.allocate(capacity, 4);
        if (!buffer) {
            r[0] = 0;
            return true;
        }
        memory.write(buffer, path.c_str(), path.size());
        const char terminator = 0;
        memory.write(buffer + uint32_t(path.size()), &terminator, 1);
        r[0] = buffer;
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
