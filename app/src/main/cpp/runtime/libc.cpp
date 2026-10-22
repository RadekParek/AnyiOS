#include "libc.h"

#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "abi.h"
#include "error.h"
#include "format.h"

namespace podnative::runtime {
namespace {
constexpr size_t CopyLimit = 16 * 1024 * 1024;
// Marks a guest jmp_buf this runtime filled in, so a stale or forged buffer is refused.
constexpr uint32_t JumpMagic = 0x4a4d5042;  // 'JMPB'
std::function<void(const std::string&)> defaultLogSink() {
    return [](const std::string& text) {
#if defined(__ANDROID__)
        __android_log_write(ANDROID_LOG_INFO, "AnyiOS-guest", text.c_str());
#else
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
#endif
    };
}
}  // namespace
void guestLog(const std::string& text) { defaultLogSink()(text); }
std::string Libc::readString(uint32_t address, size_t limit) const {
    std::string text;
    for (size_t offset = 0; offset < limit; offset++) {
        if (uint64_t(address) + offset > UINT32_MAX)
            throw Error("LIBC_RANGE", "string address overflow");
        char c;
        memory.read(address + uint32_t(offset), &c, 1);
        if (!c) return text;
        text.push_back(c);
    }
    throw Error("LIBC_LIMIT", "unterminated guest string exceeds scan budget", address);
}
void Libc::zero(uint32_t address, size_t bytes) {
    std::vector<uint8_t> zeros(std::min<size_t>(bytes, 64 * 1024));
    size_t done = 0;
    while (done < bytes) {
        const size_t chunk = std::min(zeros.size(), bytes - done);
        memory.write(address + uint32_t(done), zeros.data(), chunk);
        done += chunk;
    }
}
uint32_t Libc::reserveString(const std::string& text) {
    auto known = reservedStrings.find(text);
    if (known != reservedStrings.end()) return known->second;
    const uint32_t cell = heap.allocate(uint32_t(text.size() + 1), 4);
    if (!cell) throw Error("LIBC_ALLOC", "could not reserve a guest string");
    memory.write(cell, text.c_str(), text.size() + 1);
    reservedStrings.emplace(text, cell);
    return cell;
}
uint32_t Libc::tmBuffer() {
    if (!tmCell) {
        tmCell = heap.allocate(44, 4);
        if (!tmCell) throw Error("LIBC_ALLOC", "could not allocate the guest tm buffer");
    }
    return tmCell;
}
void Libc::writeTm(uint32_t address, const std::tm& parts) {
    // Darwin: nine ints, then long tm_gmtoff and char *tm_zone.
    const int fields[9] = {parts.tm_sec,  parts.tm_min,  parts.tm_hour, parts.tm_mday, parts.tm_mon,
                           parts.tm_year, parts.tm_wday, parts.tm_yday, parts.tm_isdst};
    for (size_t n = 0; n < 9; n++) memory.write32(address + uint32_t(n * 4), uint32_t(fields[n]));
    memory.write32(address + 36, uint32_t(int32_t(parts.tm_gmtoff)));
    memory.write32(address + 40, reserveString(parts.tm_zone ? parts.tm_zone : "UTC"));
}
void Libc::readTm(uint32_t address, std::tm& parts) {
    const int fields[9] = {int(memory.read32(address)),      int(memory.read32(address + 4)),
                           int(memory.read32(address + 8)),  int(memory.read32(address + 12)),
                           int(memory.read32(address + 16)), int(memory.read32(address + 20)),
                           int(memory.read32(address + 24)), int(memory.read32(address + 28)),
                           int(memory.read32(address + 32))};
    parts.tm_sec = fields[0];
    parts.tm_min = fields[1];
    parts.tm_hour = fields[2];
    parts.tm_mday = fields[3];
    parts.tm_mon = fields[4];
    parts.tm_year = fields[5];
    parts.tm_wday = fields[6];
    parts.tm_yday = fields[7];
    parts.tm_isdst = fields[8];
    parts.tm_gmtoff = int32_t(memory.read32(address + 36));
    const uint32_t zone = memory.read32(address + 40);
    parts.tm_zone = zone ? nullptr : "UTC";
    if (zone) {
        // The zone string is only read back for display; keep it in the reserved pool so
        // the pointer stays valid for the life of the process.
        zoneText[address] = readString(zone, 64);
        parts.tm_zone = zoneText[address].c_str();
    }
}
uint32_t Libc::localeconvCell() {
    if (lconvCell) return lconvCell;
    lconvCell = heap.allocate(96, 4);
    if (!lconvCell) throw Error("LIBC_ALLOC", "could not allocate the guest lconv");
    zero(lconvCell, 96);
    const uint32_t empty = reserveString(""), dot = reserveString(".");
    // decimal_point, thousands_sep, grouping, int_curr_symbol, currency_symbol,
    // mon_decimal_point, mon_thousands_sep, mon_grouping, positive_sign, negative_sign.
    const uint32_t pointers[10] = {dot,   empty, empty, empty, empty,
                                   empty, empty, empty, empty, empty};
    for (size_t n = 0; n < 10; n++) memory.write32(lconvCell + uint32_t(n * 4), pointers[n]);
    // The C locale reports CHAR_MAX for every remaining numeric field.
    for (uint32_t offset = 40; offset < 54; offset++) {
        const uint8_t value = 0x7f;
        memory.write(lconvCell + offset, &value, 1);
    }
    return lconvCell;
}
bool Libc::invoke(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    if (name == "_printf" || name == "_puts" || name == "_snprintf" || name == "_sprintf" ||
        name == "_vsnprintf" || name == "_vsprintf" || name == "_vprintf") {
        formatted(name, cpu);
        return true;
    }
    if (name == "_exit" || name == "_Exit" || name == "_exit_group" || name == "exit" ||
        name == "abort" || name == "_abort" || name == "__assert_rtn") {
        // Terminating the guest is real: the run stops with the guest's own exit status, and
        // the crash screen reports that status rather than pretending the app finished.
        const uint32_t status =
            name == "_exit" || name == "_Exit" || name == "_exit_group" || name == "exit"
                ? r[0]
                : 134;  // abort() raises SIGABRT
        r[0] = status;
        r[15] = 0;  // returning to address zero ends the run with the status above.
        emit("guest exited with status " + std::to_string(status));
        return true;
    }
    if (name == "_malloc") {
        r[0] = heap.allocate(r[0]);
        return true;
    }
    if (name == "_calloc") {
        const size_t count = r[0], size = r[1];
        if (size && count > CopyLimit / size) {
            r[0] = 0;
            return true;
        }
        r[0] = heap.allocate(uint32_t(count * size));
        if (r[0]) zero(r[0], count * size);
        return true;
    }
    if (name == "_free") {
        if (r[0]) heap.release(r[0]);
        return true;
    }
    if (name == "_realloc") {
        r[0] = r[0] ? heap.resize(r[0], r[1]) : heap.allocate(r[1]);
        return true;
    }
    if (name == "_memchr") {
        const size_t count = r[2];
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "memchr exceeds scan budget");
        std::vector<uint8_t> bytes(count);
        memory.read(r[0], bytes.data(), count);
        auto found = std::find(bytes.begin(), bytes.end(), uint8_t(r[1]));
        r[0] = found == bytes.end() ? 0 : r[0] + uint32_t(found - bytes.begin());
        return true;
    }
    if (name == "_memcmp") {
        const size_t count = r[2];
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "memcmp exceeds call budget");
        std::vector<uint8_t> a(count), b(count);
        memory.read(r[0], a.data(), count);
        memory.read(r[1], b.data(), count);
        int result = 0;
        for (size_t n = 0; n < count; n++)
            if (a[n] != b[n]) {
                result = int(a[n]) - int(b[n]);
                break;
            }
        r[0] = uint32_t(result);
        return true;
    }
    if (name == "_memcpy" || name == "_memmove") {
        const size_t count = r[2];
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "copy exceeds call budget");
        if (name == "_memcpy" && count && uint64_t(r[0]) < uint64_t(r[1]) + count &&
            uint64_t(r[1]) < uint64_t(r[0]) + count)
            throw Error("LIBC_OVERLAP", "overlapping memcpy is undefined; use memmove");
        std::vector<uint8_t> bytes(count);
        memory.read(r[1], bytes.data(), count);
        memory.write(r[0], bytes.data(), count);
        return true;
    }
    if (name == "_memset") {
        const size_t count = r[2];
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "memset exceeds call budget");
        std::vector<uint8_t> bytes(count, uint8_t(r[1]));
        memory.write(r[0], bytes.data(), count);
        return true;
    }
    if (name == "_bzero") {
        const size_t count = r[1];
        if (count > CopyLimit) throw Error("LIBC_LIMIT", "bzero exceeds call budget");
        zero(r[0], count);
        return true;
    }
    if (name == "_strlen") {
        r[0] = uint32_t(readString(r[0]).size());
        return true;
    }
    if (name == "_strcmp" || name == "_strcasecmp") {
        const bool folded = name == "_strcasecmp";
        const std::string a = readString(r[0]), b = readString(r[1]);
        int result = 0;
        for (size_t n = 0; n <= std::min(a.size(), b.size()); n++) {
            unsigned char x = n < a.size() ? uint8_t(a[n]) : 0;
            unsigned char y = n < b.size() ? uint8_t(b[n]) : 0;
            if (folded) {
                x = uint8_t(std::tolower(x));
                y = uint8_t(std::tolower(y));
            }
            if (x != y) {
                result = int(x) - int(y);
                break;
            }
        }
        r[0] = uint32_t(result);
        return true;
    }
    if (name == "_strncmp" || name == "_strncasecmp") {
        const bool folded = name == "_strncasecmp";
        const size_t count = r[2];
        const std::string a = readString(r[0]), b = readString(r[1]);
        int result = 0;
        for (size_t n = 0; n < count; n++) {
            unsigned char x = n < a.size() ? uint8_t(a[n]) : 0;
            unsigned char y = n < b.size() ? uint8_t(b[n]) : 0;
            if (folded) {
                x = uint8_t(std::tolower(x));
                y = uint8_t(std::tolower(y));
            }
            if (x != y) {
                result = int(x) - int(y);
                break;
            }
            if (!x) break;
        }
        r[0] = uint32_t(result);
        return true;
    }
    if (name == "_strcpy") {
        const std::string text = readString(r[1]);
        memory.write(r[0], text.data(), text.size());
        zero(r[0] + uint32_t(text.size()), 1);
        return true;
    }
    if (name == "_strncpy") {
        const std::string text = readString(r[1]);
        const size_t count = r[2];
        const size_t copied = std::min(text.size(), count);
        memory.write(r[0], text.data(), copied);
        if (copied < count) zero(r[0] + uint32_t(copied), count - copied);
        return true;
    }
    if (name == "_strcat" || name == "_strlcat") {
        const std::string base = readString(r[0]), tail = readString(r[1]);
        const size_t wanted = base.size() + tail.size();
        if (wanted > CopyLimit) throw Error("LIBC_LIMIT", "concatenation exceeds budget");
        const size_t limit = name == "_strlcat" ? r[2] : SIZE_MAX;
        if (base.size() >= limit) {
            r[0] = uint32_t(wanted);
            return true;
        }
        const size_t copied = std::min(tail.size(), limit - base.size() - 1);
        memory.write(r[0] + uint32_t(base.size()), tail.data(), copied);
        zero(r[0] + uint32_t(base.size() + copied), 1);
        r[0] = name == "_strlcat" ? uint32_t(wanted) : r[0];
        return true;
    }
    if (name == "_strchr" || name == "_strrchr") {
        const std::string text = readString(r[0]);
        const char needle = char(r[1] & 0xff);
        const size_t found = name == "_strchr" ? text.find(needle) : text.rfind(needle);
        r[0] = found == std::string::npos ? 0 : r[0] + uint32_t(found);
        return true;
    }
    if (name == "_strstr") {
        const std::string text = readString(r[0]), needle = readString(r[1]);
        const size_t found = text.find(needle);
        r[0] = found == std::string::npos ? 0 : r[0] + uint32_t(found);
        return true;
    }
    if (name == "_strdup") {
        const std::string text = readString(r[0]);
        const uint32_t copy = heap.allocate(uint32_t(text.size() + 1));
        if (!copy) {
            r[0] = 0;
            return true;
        }
        memory.write(copy, text.data(), text.size());
        zero(copy + uint32_t(text.size()), 1);
        r[0] = copy;
        return true;
    }
    if (name == "_strerror_r") {
        const std::string text = "guest runtime error";
        const size_t capacity = r[2];
        if (!capacity) return true;
        const size_t copied = std::min(text.size(), capacity - 1);
        memory.write(r[1], text.data(), copied);
        zero(r[1] + uint32_t(copied), 1);
        r[0] = 0;
        return true;
    }
    // ---- ctype -----------------------------------------------------------------------
    // Darwin spells its case-mapping entry points with a leading extra underscore, and its
    // <ctype.h> macros lower to ___maskrune. All three answer over the full unsigned-char
    // range, which is what the titles use them for; an out-of-range argument is returned
    // unchanged, exactly as Darwin's table-driven versions do.
    if (name == "___tolower" || name == "_tolower") {
        const int c = int(uint8_t(r[0] & 0xff));
        r[0] = uint32_t(r[0] & ~0xffu) | uint32_t(std::tolower(c));
        return true;
    }
    if (name == "___toupper" || name == "_toupper") {
        const int c = int(uint8_t(r[0] & 0xff));
        r[0] = uint32_t(r[0] & ~0xffu) | uint32_t(std::toupper(c));
        return true;
    }
    if (name == "___maskrune") {
        // (rune, mask) with Darwin's _CTYPE_* bits: A 0x100, C 0x200, D 0x400, G 0x800,
        // L 0x1000, P 0x2000, S 0x4000, U 0x8000, X 0x10000, B 0x20000, R 0x40000,
        // I 0x80000, T 0x100000, N 0x200000, S 0x400000 (_CTYPE_SW? no: _CTYPE_S is enough).
        const uint32_t rune = r[0], mask = r[1];
        uint32_t bits = 0;
        const int c = int(rune & 0xff);
        if (std::isalpha(c)) bits |= 0x100;
        if (std::iscntrl(c)) bits |= 0x200;
        if (std::isdigit(c)) bits |= 0x400;
        if (std::isgraph(c)) bits |= 0x800;
        if (std::islower(c)) bits |= 0x1000;
        if (std::ispunct(c)) bits |= 0x2000;
        if (std::isspace(c)) bits |= 0x4000;
        if (std::isupper(c)) bits |= 0x8000;
        if (std::isxdigit(c)) bits |= 0x10000;
        if (c == ' ') bits |= 0x20000;  // _CTYPE_B: blank
        if (std::isprint(c)) bits |= 0x40000;
        r[0] = (bits & mask) != 0 ? 1u : 0u;
        return true;
    }
    // ---- strings ---------------------------------------------------------------------
    if (name == "_strncat") {
        const std::string base = readString(r[0]), tail = readString(r[1]);
        const size_t count = r[2];
        const size_t copied = std::min(tail.size(), count);
        memory.write(r[0] + uint32_t(base.size()), tail.data(), copied);
        zero(r[0] + uint32_t(base.size() + copied), 1);
        return true;
    }
    if (name == "_strlcpy") {
        const std::string text = readString(r[1]);
        const size_t capacity = r[2];
        if (capacity) {
            const size_t copied = std::min(text.size(), capacity - 1);
            memory.write(r[0], text.data(), copied);
            zero(r[0] + uint32_t(copied), 1);
        }
        r[0] = uint32_t(text.size());
        return true;
    }
    if (name == "_strndup") {
        const std::string text = readString(r[0]).substr(0, r[1]);
        const uint32_t copy = heap.allocate(uint32_t(text.size() + 1));
        if (!copy) {
            r[0] = 0;
            return true;
        }
        memory.write(copy, text.data(), text.size());
        zero(copy + uint32_t(text.size()), 1);
        r[0] = copy;
        return true;
    }
    if (name == "_strspn" || name == "_strcspn") {
        const std::string text = readString(r[0]), set = readString(r[1]);
        size_t count = 0;
        while (count < text.size()) {
            const bool member = set.find(text[count]) != std::string::npos;
            if (member == (name == "_strcspn")) break;
            count++;
        }
        r[0] = uint32_t(count);
        return true;
    }
    if (name == "_strpbrk") {
        const std::string text = readString(r[0]), set = readString(r[1]);
        const size_t found = text.find_first_of(set);
        r[0] = found == std::string::npos ? 0 : r[0] + uint32_t(found);
        return true;
    }
    if (name == "_strcasestr") {
        const std::string text = readString(r[0]), needle = readString(r[1]);
        if (needle.empty()) {
            r[0] = r[0];
            return true;
        }
        std::string foldedText = text, foldedNeedle = needle;
        for (char& c : foldedText) c = char(std::tolower(uint8_t(c)));
        for (char& c : foldedNeedle) c = char(std::tolower(uint8_t(c)));
        const size_t found = foldedText.find(foldedNeedle);
        r[0] = found == std::string::npos ? 0 : r[0] + uint32_t(found);
        return true;
    }
    if (name == "_strcoll") {
        const std::string a = readString(r[0]), b = readString(r[1]);
        r[0] = uint32_t(a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0);
        return true;
    }
    if (name == "_strtok") {
        // The single-argument form keeps its cursor in process state, as libc does.
        const uint32_t source = r[0], separators = r[1];
        const std::string set = readString(separators);
        uint32_t start = source ? source : strtokCursor;
        if (!start) {
            r[0] = 0;
            return true;
        }
        for (;;) {
            char c;
            memory.read(start, &c, 1);
            if (!c) {
                strtokCursor = 0;
                r[0] = 0;
                return true;
            }
            if (set.find(c) == std::string::npos) break;
            start++;
        }
        uint32_t end = start;
        for (;;) {
            char c;
            memory.read(end, &c, 1);
            if (!c) {
                strtokCursor = 0;
                break;
            }
            if (set.find(c) != std::string::npos) {
                const char terminator = 0;
                memory.write(end, &terminator, 1);
                strtokCursor = end + 1;
                break;
            }
            end++;
        }
        r[0] = start;
        return true;
    }
    if (name == "_strsep") {
        const uint32_t cursorAt = r[0], separators = r[1];
        if (!memory.permits(cursorAt, 4, Write))
            throw Error("LIBC_BUFFER", "strsep cursor is not writable", cursorAt);
        uint32_t start = memory.read32(cursorAt);
        if (!start) {
            r[0] = 0;
            return true;
        }
        const std::string set = readString(separators);
        uint32_t at = start, end = 0;
        for (;;) {
            char c;
            memory.read(at, &c, 1);
            if (!c) break;
            if (set.find(c) != std::string::npos) {
                const char terminator = 0;
                memory.write(at, &terminator, 1);
                end = at + 1;
                break;
            }
            at++;
        }
        memory.write32(cursorAt, end);
        r[0] = start;
        return true;
    }
    if (name == "_memmem") {
        const uint32_t haystack = r[0], haystackLength = r[1];
        const uint32_t needle = r[2], needleLength = r[3];
        if (needleLength > haystackLength) {
            r[0] = 0;
            return true;
        }
        const std::string text = readString(haystack, haystackLength + 1);
        const std::string target = readString(needle, needleLength + 1);
        const size_t found = text.find(target);
        r[0] = found == std::string::npos ? 0 : haystack + uint32_t(found);
        return true;
    }
    if (name == "_strerror") {
        // A stable guest buffer, so a caller may print the result without copying it.
        const char* text = std::strerror(int(r[0]));
        const size_t length = std::strlen(text);
        if (errorBufferSize < length + 1) {
            errorBuffer = heap.allocate(uint32_t(length + 1));
            errorBufferSize = uint32_t(length + 1);
        }
        if (errorBuffer) memory.write(errorBuffer, text, length + 1);
        r[0] = errorBuffer;
        return true;
    }
    if (name == "___error") {
        // Darwin's errno accessor: a pointer to the calling thread's errno cell.
        r[0] = errnoCell ? errnoCell : (errnoCell = [&] {
            const uint32_t cell = heap.allocate(4, 4);
            if (!cell) throw Error("LIBC_ALLOC", "could not allocate the guest errno cell");
            zero(cell, 4);
            return cell;
        }());
        return true;
    }
    if (name == "_setlocale" || name == "_localeconv") {
        // Only the "C" locale is modelled. setlocale(C) succeeds and returns "C";
        // localeconv hands back a real lconv full of C-locale values.
        if (name == "_setlocale") {
            static const std::string clocale = "C";
            if (r[1] == 0) {
                r[0] = reserveString(clocale);
                return true;
            }
            const std::string requested = readString(r[1]);
            const bool c = requested.empty() || requested == "C" || requested == "POSIX";
            if (!c) {
                emit("setlocale(\"" + requested + "\") is not available; only the C locale is");
                r[0] = 0;
                return true;
            }
            r[0] = reserveString(clocale);
            return true;
        }
        r[0] = localeconvCell();
        return true;
    }
    // ---- setjmp / longjmp --------------------------------------------------------------
    if (name == "_setjmp" || name == "_sigsetjmp" || name == "setjmp") {
        // A jmp_buf is opaque to the caller, so it holds a token rather than a copy of the
        // host register file. That keeps this correct for any guest buffer size: exactly two
        // words are written. Like Darwin's _setjmp, no signal mask is saved.
        const uint32_t buffer = r[0];
        if (!memory.permits(buffer, 8, Write))
            throw Error("LIBC_BUFFER", "setjmp buffer is not a writable guest range", buffer);
        const uint32_t token = nextJumpToken++;
        JumpRecord& record = jumpRecords[token];
        for (size_t n = 0; n < 8; n++) record.registers[n] = cpu.registers[4 + n];
        record.stack = cpu.registers[13];
        record.resume = cpu.registers[14];  // where this call returns to
        record.cpsr = cpu.cpsr;
        memory.write32(buffer, JumpMagic);
        memory.write32(buffer + 4, token);
        r[0] = 0;
        return true;
    }
    if (name == "_longjmp" || name == "_siglongjmp" || name == "longjmp") {
        const uint32_t buffer = r[0], value = r[1] ? r[1] : 1;
        if (!memory.permits(buffer, 8, Read) || memory.read32(buffer) != JumpMagic)
            throw Error("LIBC_BUFFER", "longjmp on a buffer that was never set", buffer);
        const uint32_t token = memory.read32(buffer + 4);
        auto record = jumpRecords.find(token);
        if (record == jumpRecords.end())
            throw Error("LIBC_STATE", "longjmp on a stale jmp_buf", buffer);
        for (size_t n = 0; n < 8; n++) cpu.registers[4 + n] = record->second.registers[n];
        cpu.registers[13] = record->second.stack;
        cpu.registers[15] = record->second.resume & ~1u;
        cpu.cpsr = (cpu.cpsr & ~0x20u) | (record->second.resume & 1 ? 0x20 : 0);
        r[0] = value;  // setjmp appears to return `value` in the resumed frame
        // The record is single-shot until the buffer is set again, as with a real jmp_buf.
        jumpRecords.erase(record);
        return true;
    }
    // ---- process ---------------------------------------------------------------------
    if (name == "_getpid") {
        r[0] = 1000;
        return true;
    }
    if (name == "_getuid" || name == "_geteuid") {
        r[0] = 501;  // mobile
        return true;
    }
    if (name == "_system") {
        // iOS has no shell, and a device returns -1 with EPERM here. Reporting that is the
        // honest answer; pretending to run a command would let a title believe it worked.
        if (errnoCell) memory.write32(errnoCell, 1 /* EPERM */);
        emit("system(\"" + readString(r[0]) + "\") refused: this platform has no shell");
        r[0] = uint32_t(-1);
        return true;
    }
    if (name == "_clock") {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        // Darwin's CLOCKS_PER_SEC is 1,000,000, and clock() counts process CPU time.
        r[0] = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count() &
                        0xffffffff);
        return true;
    }
    // ---- time ------------------------------------------------------------------------
    if (name == "_difftime") {
        resultF64(cpu, double(int32_t(r[0])) - double(int32_t(r[1])));
        return true;
    }
    if (name == "_gmtime" || name == "_localtime" || name == "_gmtime_r" ||
        name == "_localtime_r") {
        const bool local = name == "_localtime" || name == "_localtime_r";
        time_t when = time_t(int32_t(r[0]));
        std::tm parts{};
        if (local)
            localtime_r(&when, &parts);
        else
            gmtime_r(&when, &parts);
        uint32_t target = 0;
        if (name == "_gmtime_r" || name == "_localtime_r") {
            if (!r[1]) throw Error("LIBC_ARGUMENT", "gmtime_r/localtime_r needs a result buffer");
            target = r[1];
        } else {
            target = tmBuffer();
        }
        writeTm(target, parts);
        r[0] = target;
        return true;
    }
    if (name == "_timegm" || name == "_mktime") {
        std::tm parts{};
        readTm(r[0], parts);
        const time_t value = name == "_timegm" ? timegm(&parts) : mktime(&parts);
        writeTm(r[0], parts);  // mktime normalises the caller's struct in place
        r[0] = uint32_t(int32_t(value));
        return true;
    }
    if (name == "_strftime") {
        const uint32_t buffer = r[0], capacity = r[1];
        const std::string format = readString(r[2]);
        std::tm parts{};
        readTm(r[3], parts);
        if (!capacity) {
            r[0] = 0;
            return true;
        }
        std::vector<char> text(capacity);
        const size_t written = std::strftime(text.data(), capacity, format.c_str(), &parts);
        if (written) memory.write(buffer, text.data(), written + 1);
        r[0] = uint32_t(written);
        return true;
    }
    if (name == "_tzset") {
        // No timezone database is mounted; UTC is the honest default and matches gmtime.
        return true;
    }
    if (name == "_atoi" || name == "_atol" || name == "_atof" || name == "_strtod" ||
        name == "_strtol" || name == "_strtoul" || name == "_strtoll" || name == "_strtoull")
        return numbers(name, cpu);
    if (name == "_mach_absolute_time") {
        // Monotonic nanoseconds since the runtime started, with a timebase of 1/1 so guest
        // scaling arithmetic is exact.
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        r[0] = uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        return true;
    }
    if (name == "_mach_timebase_info") {
        if (r[0]) {
            memory.write32(r[0], 1);      // numer
            memory.write32(r[0] + 4, 1);  // denom
        }
        r[0] = 0;
        return true;
    }
    if (name == "_sysctlbyname") {
        // A small, honest device description: only the keys a title realistically queries
        // are answered, everything else is an errno failure.
        const std::string key = readString(r[0]);
        static const std::map<std::string, std::string> values = {
            {"hw.machine", "iPod5,1"},      {"hw.model", "iPod touch"},
            {"hw.ncpu", std::to_string(2)}, {"hw.physicalcpu", "2"},
            {"hw.logicalcpu", "2"},         {"hw.cpusubtype", "9"},
            {"kern.osversion", "11A502"},   {"kern.ostype", "Darwin"},
            {"kern.osrelease", "11.0.0"},
        };
        auto found = values.find(key);
        if (found == values.end()) {
            r[0] = uint32_t(-1);
            return true;
        }
        const uint32_t sizeAt = r[1], buffer = r[2];
        const std::string& value = found->second;
        const size_t length = value.size() + 1;
        if (sizeAt) memory.write32(sizeAt, uint32_t(length));
        if (buffer) {
            if (!memory.permits(buffer, length, Write))
                throw Error("LIBC_BUFFER", "sysctlbyname destination is not writable", buffer);
            memory.write(buffer, value.c_str(), length);
        }
        r[0] = 0;
        return true;
    }
    if (name == "_strtok_r") {
        const uint32_t source = r[0], separators = r[1], saveAt = r[2];
        const std::string separatorsText = readString(separators);
        if (!memory.permits(saveAt, 4, Write))
            throw Error("LIBC_BUFFER", "strtok_r save pointer is not writable", saveAt);
        uint32_t start = source ? source : memory.read32(saveAt);
        if (!start) return true;  // No more tokens: the result stays null.
        // Skip leading separators.
        for (;;) {
            char c;
            memory.read(start, &c, 1);
            if (!c) {
                memory.write32(saveAt, 0);
                r[0] = 0;
                return true;
            }
            if (separatorsText.find(c) == std::string::npos) break;
            start++;
        }
        uint32_t end = start;
        for (;;) {
            char c;
            memory.read(end, &c, 1);
            if (!c) {
                memory.write32(saveAt, 0);
                break;
            }
            if (separatorsText.find(c) != std::string::npos) {
                const char terminator = 0;
                memory.write(end, &terminator, 1);
                memory.write32(saveAt, end + 1);
                break;
            }
            end++;
        }
        r[0] = start;
        return true;
    }
    if (name == "_getenv") {
        const std::string key = readString(r[0]);
        auto known = environmentCells.find(key);
        if (known != environmentCells.end()) {
            r[0] = known->second;
            return true;
        }
        auto value = environmentValues.find(key);
        if (value == environmentValues.end()) {
            r[0] = 0;
            return true;
        }
        // An empty variable is still present in the environment; report it as the empty
        // string rather than null, which is what a device does.
        const uint32_t bytes = uint32_t(value->second.size() + 1);
        r[0] = heap.allocate(bytes, 4);
        if (!r[0]) throw Error("LIBC_ALLOC", "could not copy a guest environment variable");
        memory.write(r[0], value->second.c_str(), bytes);
        environmentCells.emplace(key, r[0]);
        return true;
    }
    if (name == "_rand") {
        randomState = randomState * 1103515245u + 12345u;
        r[0] = (randomState >> 16) & 0x7fff;
        return true;
    }
    if (name == "_srand") {
        randomState = r[0];
        return true;
    }
    if (name == "_time") {
        const time_t now = std::time(nullptr);
        if (r[0]) memory.write32(r[0], uint32_t(now));
        r[0] = uint32_t(now);
        return true;
    }
    if (name == "_gettimeofday") {
        timeval tv{};
        gettimeofday(&tv, nullptr);
        if (r[0]) {
            memory.write32(r[0], uint32_t(tv.tv_sec));
            memory.write32(r[0] + 4, uint32_t(tv.tv_usec));
        }
        r[0] = 0;
        return true;
    }
    if (name == "_OSAtomicCompareAndSwap32Barrier") {
        const uint32_t old = memory.read32(r[2]);
        if (old == r[0]) memory.write32(r[2], r[1]);
        r[0] = old == r[0] ? 1 : 0;
        return true;
    }
    if (name == "_OSAtomicAnd32OrigBarrier" || name == "_OSAtomicOr32OrigBarrier") {
        const uint32_t old = memory.read32(r[1]);
        memory.write32(r[1], name == "_OSAtomicAnd32OrigBarrier" ? old & r[0] : old | r[0]);
        r[0] = old;
        return true;
    }
    return math(cpu, memory, name);
}
namespace {
// Shared bounded integer parser: returns the number of bytes consumed.
size_t parseInt(const std::string& text, int base, bool& negative, uint64_t& magnitude,
                bool& overflow) {
    size_t at = 0;
    negative = false;
    magnitude = 0;
    overflow = false;
    while (at < text.size() && std::isspace(uint8_t(text[at]))) at++;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        negative = text[at] == '-';
        at++;
    }
    if ((base == 0 || base == 16) && at + 1 < text.size() && text[at] == '0' &&
        (text[at + 1] == 'x' || text[at + 1] == 'X')) {
        base = 16;
        at += 2;
    } else if (base == 0) {
        base = at < text.size() && text[at] == '0' ? 8 : 10;
    }
    const size_t digits = at;
    for (; at < text.size(); at++) {
        const unsigned char c = uint8_t(text[at]);
        int value;
        if (std::isdigit(c))
            value = c - '0';
        else if (std::isalpha(c))
            value = std::tolower(c) - 'a' + 10;
        else
            break;
        if (value >= base) break;
        if (magnitude > (UINT64_MAX - uint64_t(value)) / uint64_t(base))
            overflow = true;
        else
            magnitude = magnitude * uint64_t(base) + uint64_t(value);
    }
    return at == digits ? digits : at;
}
}  // namespace
bool Libc::numbers(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    const std::string text = readString(r[0]);
    // strtod/atof use the host parser on a private copy, so no guest byte is over-read.
    if (name == "_strtod" || name == "_atof") {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (name == "_strtod" && r[1]) memory.write32(r[1], r[0] + uint32_t(end - text.c_str()));
        resultF64(cpu, value);
        return true;
    }
    const int base =
        name == "_strtol" || name == "_strtoul" || name == "_strtoll" || name == "_strtoull"
            ? int(r[2])
            : 10;
    if (base && (base < 2 || base > 36)) throw Error("LIBC_ARGUMENT", "invalid integer base");
    bool negative = false, overflow = false;
    uint64_t magnitude = 0;
    const size_t consumed = parseInt(text, base, negative, magnitude, overflow);
    const bool unsignedResult =
        name == "_strtoul" || name == "_strtoull" || name == "_atoi" || name == "_atol";
    if (r[1] &&
        (name == "_strtol" || name == "_strtoul" || name == "_strtoll" || name == "_strtoull"))
        memory.write32(r[1], r[0] + uint32_t(consumed));
    if (name == "_strtoll" || name == "_strtoull") {
        uint64_t value = negative ? 0 - magnitude : magnitude;
        if (!unsignedResult &&
            (negative ? magnitude > uint64_t(INT64_MAX) + 1 : magnitude > uint64_t(INT64_MAX)))
            value = negative ? uint64_t(INT64_MIN) : uint64_t(INT64_MAX);
        r[0] = uint32_t(value);
        r[1] = uint32_t(value >> 32);
        return true;
    }
    int64_t signedValue = negative ? -int64_t(magnitude) : int64_t(magnitude);
    if (!unsignedResult) {
        const int64_t limit = INT32_MAX;
        if (magnitude > uint64_t(limit) || overflow) signedValue = negative ? INT32_MIN : INT32_MAX;
    }
    r[0] = uint32_t(signedValue);
    return true;
}
void Libc::formatted(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    // Argument layout follows the iOS ARMv7 base ABI: every vararg is a word in
    // r0-r3 and then on the stack, so a single cursor walks the format arguments.
    if (name == "_puts") {
        const std::string text = readString(r[0]);
        emit(text + "\n");
        r[0] = uint32_t(text.size() + 1);
        return;
    }
    uint32_t formatAddress = r[0], buffer = 0, capacity = 0, vaList = r[3];
    size_t firstWord = 1;
    if (name == "_snprintf" || name == "_vsnprintf") {
        buffer = r[0];
        capacity = r[1];
        formatAddress = r[2];
        firstWord = 3;
    } else if (name == "_sprintf") {
        buffer = r[0];
        formatAddress = r[1];
        firstWord = 2;
    } else if (name == "_vsprintf") {
        buffer = r[0];
        formatAddress = r[1];
        vaList = r[2];
        firstWord = 2;
    } else if (name == "_vprintf") {
        formatAddress = r[0];
        vaList = r[1];
        firstWord = 1;
    }
    // vsnprintf(str, size, format, ap): the expected string is written through the same
    // path as snprintf, but arguments come from the guest's saved-argument area.
    std::string text;
    if (name == "_vsnprintf" || name == "_vsprintf" || name == "_vprintf") {
        // A guest va_list is the address of its saved-argument area, so the same cursor
        // serves vprintf/vsprintf/vsnprintf.
        if (!vaList && name != "_vsnprintf")
            throw Error("LIBC_ARGUMENT", "a v-format call needs a guest va_list");
        text = formatGuestVarargs(readString(formatAddress), memory, vaList);
    } else {
        Arguments args(cpu, memory, firstWord);
        text = formatGuest(readString(formatAddress), args);
    }
    if (buffer) {
        // sprintf/vsprintf have no capacity argument: they write the whole string plus its
        // terminator. Treating their missing capacity as "nothing fits" silently produced an
        // empty string, which is the kind of quiet wrong answer this runtime must not give.
        const bool bounded = capacity != 0;
        const size_t limit = bounded ? capacity - 1 : text.size();
        if (bounded && !memory.permits(buffer, capacity, Write))
            throw Error("LIBC_BUFFER", "format destination is not a writable guest range", buffer);
        if (!memory.permits(buffer, limit + 1, Write))
            throw Error("LIBC_BUFFER", "format destination is not a writable guest range", buffer);
        const size_t written = std::min<size_t>(text.size(), limit);
        if (written) memory.write(buffer, text.data(), written);
        const char terminator = 0;
        memory.write(buffer + uint32_t(written), &terminator, 1);
    } else {
        emit(text);
    }
    r[0] = uint32_t(text.size());
}
void Libc::setLogSink(std::function<void(const std::string&)> sink) { logSink = std::move(sink); }
void Libc::setEnvironment(const std::map<std::string, std::string>& values) {
    environmentValues = values;
    environmentCells.clear();
}
void Libc::emit(const std::string& text) const {
    if (logSink) {
        logSink(text);
        return;
    }
    defaultLogSink()(text);
}
bool Libc::math(CpuState& cpu, Memory& memory, const std::string& name) {
    // iOS ARMv7 marshals floating-point arguments in the core registers, so these
    // entry points read r0-r3 and return in r0/r0-r1 rather than the VFP file.
    using Unary64 = double (*)(double);
    using Unary32 = float (*)(float);
    using Binary64 = double (*)(double, double);
    using Binary32 = float (*)(float, float);
    static const std::map<std::string, Unary64> unary64 = {
        {"_sin", [](double x) { return std::sin(x); }},
        {"_cos", [](double x) { return std::cos(x); }},
        {"_tan", [](double x) { return std::tan(x); }},
        {"_asin", [](double x) { return std::asin(x); }},
        {"_acos", [](double x) { return std::acos(x); }},
        {"_atan", [](double x) { return std::atan(x); }},
        {"_sinh", [](double x) { return std::sinh(x); }},
        {"_cosh", [](double x) { return std::cosh(x); }},
        {"_tanh", [](double x) { return std::tanh(x); }},
        {"_exp", [](double x) { return std::exp(x); }},
        {"_exp2", [](double x) { return std::exp2(x); }},
        {"_log", [](double x) { return std::log(x); }},
        {"_log10", [](double x) { return std::log10(x); }},
        {"_log2", [](double x) { return std::log2(x); }},
        {"_sqrt", [](double x) { return std::sqrt(x); }},
        {"_cbrt", [](double x) { return std::cbrt(x); }},
        {"_ceil", [](double x) { return std::ceil(x); }},
        {"_floor", [](double x) { return std::floor(x); }},
        {"_round", [](double x) { return std::round(x); }},
        {"_trunc", [](double x) { return std::trunc(x); }},
        {"_fabs", [](double x) { return std::fabs(x); }},
    };
    static const std::map<std::string, Unary32> unary32 = {
        {"_sinf", [](float x) { return std::sin(x); }},
        {"_cosf", [](float x) { return std::cos(x); }},
        {"_tanf", [](float x) { return std::tan(x); }},
        {"_asinf", [](float x) { return std::asin(x); }},
        {"_acosf", [](float x) { return std::acos(x); }},
        {"_atanf", [](float x) { return std::atan(x); }},
        {"_sinhf", [](float x) { return std::sinh(x); }},
        {"_coshf", [](float x) { return std::cosh(x); }},
        {"_tanhf", [](float x) { return std::tanh(x); }},
        {"_expf", [](float x) { return std::exp(x); }},
        {"_exp2f", [](float x) { return std::exp2(x); }},
        {"_logf", [](float x) { return std::log(x); }},
        {"_log10f", [](float x) { return std::log10(x); }},
        {"_log2f", [](float x) { return std::log2(x); }},
        {"_sqrtf", [](float x) { return std::sqrt(x); }},
        {"_cbrtf", [](float x) { return std::cbrt(x); }},
        {"_ceilf", [](float x) { return std::ceil(x); }},
        {"_floorf", [](float x) { return std::floor(x); }},
        {"_roundf", [](float x) { return std::round(x); }},
        {"_truncf", [](float x) { return std::trunc(x); }},
        {"_fabsf", [](float x) { return std::fabs(x); }},
        {"_expm1f", [](float x) { return std::expm1(x); }},
        {"_log1pf", [](float x) { return std::log1p(x); }},
    };
    static const std::map<std::string, Binary64> binary64 = {
        {"_pow", [](double a, double b) { return std::pow(a, b); }},
        {"_fmod", [](double a, double b) { return std::fmod(a, b); }},
        {"_atan2", [](double a, double b) { return std::atan2(a, b); }},
        {"_hypot", [](double a, double b) { return std::hypot(a, b); }},
        {"_copysign", [](double a, double b) { return std::copysign(a, b); }},
        {"_remainder", [](double a, double b) { return std::remainder(a, b); }},
        {"_fmin", [](double a, double b) { return std::fmin(a, b); }},
        {"_fmax", [](double a, double b) { return std::fmax(a, b); }},
    };
    static const std::map<std::string, Binary32> binary32 = {
        {"_powf", [](float a, float b) { return std::pow(a, b); }},
        {"_fmodf", [](float a, float b) { return std::fmod(a, b); }},
        {"_atan2f", [](float a, float b) { return std::atan2(a, b); }},
        {"_hypotf", [](float a, float b) { return std::hypot(a, b); }},
        {"_copysignf", [](float a, float b) { return std::copysign(a, b); }},
        {"_remainderf", [](float a, float b) { return std::remainder(a, b); }},
        {"_fminf", [](float a, float b) { return std::fmin(a, b); }},
        {"_fmaxf", [](float a, float b) { return std::fmax(a, b); }},
    };
    if (auto found = unary64.find(name); found != unary64.end()) {
        Arguments args(cpu, memory);
        resultF64(cpu, found->second(args.f64()));
        return true;
    }
    if (auto found = unary32.find(name); found != unary32.end()) {
        Arguments args(cpu, memory);
        resultF32(cpu, found->second(args.f32()));
        return true;
    }
    if (auto found = binary64.find(name); found != binary64.end()) {
        Arguments args(cpu, memory);
        const double first = args.f64(), second = args.f64();
        resultF64(cpu, found->second(first, second));
        return true;
    }
    if (auto found = binary32.find(name); found != binary32.end()) {
        Arguments args(cpu, memory);
        const float first = args.f32(), second = args.f32();
        resultF32(cpu, found->second(first, second));
        return true;
    }
    // frexp/ldexp/modf split a value rather than mapping it, so they are written out.
    if (name == "_frexp" || name == "_frexpf") {
        Arguments args(cpu, memory);
        const bool single = name == "_frexpf";
        int exponent = 0;
        if (single) {
            const float mantissa = std::frexp(args.f32(), &exponent);
            const uint32_t exponentAt = args.pointer();
            if (exponentAt) memory.write32(exponentAt, uint32_t(exponent));
            resultF32(cpu, mantissa);
        } else {
            const double mantissa = std::frexp(args.f64(), &exponent);
            const uint32_t exponentAt = args.pointer();
            if (exponentAt) memory.write32(exponentAt, uint32_t(exponent));
            resultF64(cpu, mantissa);
        }
        return true;
    }
    if (name == "_ldexp" || name == "_ldexpf") {
        if (name == "_ldexpf") {
            resultF32(cpu, std::ldexp(bitsFloat(cpu.registers[0]), int32_t(cpu.registers[1])));
        } else {
            resultF64(cpu, std::ldexp(bitsDouble(uint64_t(cpu.registers[0]) |
                                                 uint64_t(cpu.registers[1]) << 32),
                                      int32_t(cpu.registers[2])));
        }
        return true;
    }
    if (name == "_modf" || name == "_modff") {
        if (name == "_modff") {
            const uint32_t integralAt = cpu.registers[1];
            float integral = 0;
            const float fraction = std::modf(bitsFloat(cpu.registers[0]), &integral);
            if (integralAt) memory.write32(integralAt, floatBits(integral));
            resultF32(cpu, fraction);
        } else {
            const uint32_t integralAt = cpu.registers[2];
            double integral = 0;
            const double value =
                bitsDouble(uint64_t(cpu.registers[0]) | uint64_t(cpu.registers[1]) << 32);
            const double fraction = std::modf(value, &integral);
            if (integralAt) storeThroughPointer(memory, integralAt, integral);
            resultF64(cpu, fraction);
        }
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
