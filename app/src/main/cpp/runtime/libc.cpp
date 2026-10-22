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

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "abi.h"
#include "error.h"
#include "format.h"

namespace podnative::runtime {
namespace {
constexpr size_t CopyLimit = 16 * 1024 * 1024;
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
bool Libc::invoke(const std::string& name, CpuState& cpu) {
    auto& r = cpu.registers;
    if (name == "_printf" || name == "_puts" || name == "_snprintf" || name == "_sprintf" ||
        name == "_vsnprintf" || name == "_vsprintf" || name == "_vprintf") {
        formatted(name, cpu);
        return true;
    }
    if (name == "_exit" || name == "_Exit" || name == "_exit_group" || name == "exit" ||
        name == "abort" || name == "__assert_rtn") {
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
    Arguments args(cpu, memory);
    if (name == "_sin" || name == "_cos") {
        const double value = args.f64();
        resultF64(cpu, name == "_sin" ? std::sin(value) : std::cos(value));
        return true;
    }
    if (name == "_pow" || name == "_fmod") {
        const double first = args.f64(), second = args.f64();
        resultF64(cpu, name == "_pow" ? std::pow(first, second) : std::fmod(first, second));
        return true;
    }
    if (name == "_powf") {
        const float first = args.f32(), second = args.f32();
        resultF32(cpu, std::pow(first, second));
        return true;
    }
    if (name == "_sinf" || name == "_cosf" || name == "_tanf" || name == "_acosf" ||
        name == "_atanf") {
        const float value = args.f32();
        float result = 0;
        if (name == "_sinf") result = std::sin(value);
        if (name == "_cosf") result = std::cos(value);
        if (name == "_tanf") result = std::tan(value);
        if (name == "_acosf") result = std::acos(value);
        if (name == "_atanf") result = std::atan(value);
        resultF32(cpu, result);
        return true;
    }
    if (name == "_atan2f") {
        const float first = args.f32(), second = args.f32();
        resultF32(cpu, std::atan2(first, second));
        return true;
    }
    return false;
}
}  // namespace podnative::runtime
