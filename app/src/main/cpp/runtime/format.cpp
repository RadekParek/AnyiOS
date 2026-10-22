#include "format.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "error.h"

namespace podnative::runtime {
namespace {
struct Spec {
    bool left = false, plus = false, space = false, alternate = false, zero = false;
    int width = 0, precision = -1;
    enum Length { None, Long, LongLong, Size } length = None;
};
// Converts the digits after a '%' into a Spec. Returns the index of the conversion
// character, or throws when the directive is malformed.
size_t parseSpec(const std::string& format, size_t at, Spec& spec) {
    const size_t limit = at + 32;
    while (at < format.size()) {
        const char c = format[at];
        if (c == '-')
            spec.left = true;
        else if (c == '+')
            spec.plus = true;
        else if (c == ' ')
            spec.space = true;
        else if (c == '#')
            spec.alternate = true;
        else if (c == '0')
            spec.zero = true;
        else
            break;
        if (++at > limit) break;
    }
    while (at < format.size() && format[at] >= '0' && format[at] <= '9') {
        spec.width = spec.width * 10 + (format[at] - '0');
        if (spec.width > 4096) throw Error("FORMAT_WIDTH", "field width exceeds limit");
        at++;
    }
    if (at < format.size() && format[at] == '.') {
        at++;
        spec.precision = 0;
        while (at < format.size() && format[at] >= '0' && format[at] <= '9') {
            spec.precision = spec.precision * 10 + (format[at] - '0');
            if (spec.precision > 4096) throw Error("FORMAT_PRECISION", "precision exceeds limit");
            at++;
        }
    }
    if (format.compare(at, 2, "ll") == 0) {
        spec.length = Spec::LongLong;
        at += 2;
    } else if (at < format.size() && format[at] == 'l') {
        spec.length = Spec::Long;
        at += 1;
    } else if (at < format.size() && format[at] == 'z') {
        spec.length = Spec::Size;
        at += 1;
    }
    if (at >= format.size())
        throw Error("FORMAT_TRUNCATED", "format string ends inside a directive");
    return at;
}
std::string pad(const std::string& text, const Spec& spec, size_t length) {
    if (spec.width <= int(length)) return text;
    const size_t fill = size_t(spec.width) - length;
    if (spec.left) return text + std::string(fill, ' ');
    if (spec.zero && text.size() && (text[0] == '-' || text[0] == '+' || text[0] == ' '))
        return text.substr(0, 1) + std::string(fill, '0') + text.substr(1);
    return std::string(fill, spec.zero ? '0' : ' ') + text;
}
std::string unsignedText(uint64_t value, unsigned base, bool upper, bool alternate) {
    if (!value) return std::string(alternate && base == 16 ? "0" : "0");
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    while (value) {
        out.insert(out.begin(), digits[value % base]);
        value /= base;
    }
    if (alternate && base == 16)
        out.insert(0, upper ? "0X" : "0x");
    else if (alternate && base == 8 && out[0] != '0')
        out.insert(out.begin(), '0');
    return out;
}
std::string signedText(int64_t value, const Spec& spec) {
    const bool negative = value < 0;
    // INT64_MIN has no positive counterpart; unsigned negate keeps it exact.
    const uint64_t magnitude = negative ? uint64_t(0) - uint64_t(value) : uint64_t(value);
    std::string body = unsignedText(magnitude, 10, false, false);
    if (negative) return "-" + body;
    if (spec.plus) return "+" + body;
    if (spec.space) return " " + body;
    return body;
}
std::string floatText(double value, const Spec& spec, char conversion) {
    const int precision = spec.precision < 0 ? 6 : spec.precision;
    char buffer[512];
    std::string directive = "%";
    if (spec.plus) directive += "+";
    if (spec.space) directive += " ";
    if (spec.alternate) directive += "#";
    if (spec.precision >= 0) {
        directive += '.';
        directive += std::to_string(precision);
    }
    directive += conversion;
    // std::snprintf on the host performs IEEE-754 rounding, which is what the guest libc
    // does; the buffer is fixed and the directive is built from a validated grammar.
    const int written = std::snprintf(buffer, sizeof(buffer), directive.c_str(), value);
    if (written < 0) throw Error("FORMAT_FLOAT", "float formatting failed");
    if (size_t(written) >= sizeof(buffer)) throw Error("FORMAT_FLOAT", "float field exceeds limit");
    return std::string(buffer, size_t(written));
}
}  // namespace
std::string readGuestString(Memory& memory, uint32_t address, size_t limit) {
    if (!address) return std::string();
    std::string text;
    char chunk[256];
    while (text.size() < limit) {
        size_t want = std::min(sizeof(chunk), limit - text.size());
        try {
            memory.read(address + uint32_t(text.size()), chunk, want);
        } catch (const Error&) {
            // A string that ends exactly at the end of a mapping is legal; read what is
            // available one byte at a time before deciding the pointer is invalid.
            if (!text.empty()) break;
            throw;
        }
        const void* terminator = std::memchr(chunk, 0, want);
        if (terminator) {
            text.append(chunk, static_cast<const char*>(terminator) - chunk);
            return text;
        }
        text.append(chunk, want);
    }
    if (text.size() >= limit) throw Error("FORMAT_STRING", "unterminated guest string", address);
    return text;
}
namespace {
// Cursor over a guest va_list: the saved argument area is a run of words, and an 8-byte
// value starts on an 8-byte boundary there.
class VaCursor {
   public:
    VaCursor(Memory& memory, uint32_t address) : memory(memory), address(address) {}
    uint32_t word() {
        const uint32_t value = memory.read32(address);
        address += 4;
        return value;
    }
    double f64() {
        if (address % 8) address += 4;
        const uint64_t low = word();
        return bitsDouble(low | uint64_t(word()) << 32);
    }
    Memory& memoryRef() const { return memory; }

   private:
    Memory& memory;
    uint32_t address;
};
}  // namespace
template <typename Cursor>
std::string formatGuestFrom(const std::string& format, Cursor& args,
                            const ObjectDescriber& object) {
    std::string out;
    for (size_t at = 0; at < format.size();) {
        if (format[at] != '%') {
            out.push_back(format[at++]);
            if (out.size() > 1024 * 1024)
                throw Error("FORMAT_SIZE", "formatted text exceeds limit");
            continue;
        }
        if (at + 1 < format.size() && format[at + 1] == '%') {
            out.push_back('%');
            at += 2;
            continue;
        }
        Spec spec;
        const size_t conversionAt = parseSpec(format, at + 1, spec);
        const char conversion = format[conversionAt];
        at = conversionAt + 1;
        std::string piece;
        switch (conversion) {
            case 'd':
            case 'i': {
                const bool wide = spec.length != Spec::None;
                const uint64_t bits = wide ? (uint64_t(args.word()) | uint64_t(args.word()) << 32)
                                           : uint64_t(args.word());
                piece = signedText(wide ? int64_t(bits) : int64_t(int32_t(bits)), spec);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                const bool wide = spec.length != Spec::None;
                const uint64_t bits = wide ? (uint64_t(args.word()) | uint64_t(args.word()) << 32)
                                           : uint64_t(args.word());
                const unsigned base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);
                piece = unsignedText(wide ? bits : uint64_t(uint32_t(bits)), base,
                                     conversion == 'X', spec.alternate);
                break;
            }
            case 'c':
                if (spec.length != Spec::None)
                    throw Error("FORMAT_CONVERSION",
                                "wide %c needs UTF-16 semantics that are not implemented");
                piece = std::string(1, char(args.word() & 0xff));
                break;
            case 's': {
                if (spec.length != Spec::None)
                    throw Error("FORMAT_CONVERSION",
                                "wide %s needs UTF-16 semantics that are not implemented");
                const uint32_t address = args.word();
                piece = address ? readGuestString(args.memoryRef(), address) : "(null)";
                if (spec.precision >= 0 && piece.size() > size_t(spec.precision))
                    piece.resize(size_t(spec.precision));
                break;
            }
            case 'p': {
                const uint32_t value = args.word();
                piece = value ? "0x" + unsignedText(value, 16, false, false) : "0x0";
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                piece = floatText(args.f64(), spec, conversion);
                break;
            case '@': {
                if (!object) throw Error("FORMAT_CONVERSION", "%@ outside an Objective-C context");
                piece = object(args.word());
                break;
            }
            default:
                throw Error("FORMAT_CONVERSION",
                            std::string("unsupported format conversion %") + conversion);
        }
        const size_t length = piece.size();
        out += pad(piece, spec, length);
        if (out.size() > 1024 * 1024) throw Error("FORMAT_SIZE", "formatted text exceeds limit");
    }
    return out;
}
std::string formatGuest(const std::string& format, Arguments& args, const ObjectDescriber& object) {
    return formatGuestFrom(format, args, object);
}
std::string formatGuestVarargs(const std::string& format, Memory& memory, uint32_t vaList,
                               const ObjectDescriber& object) {
    VaCursor cursor(memory, vaList);
    return formatGuestFrom(format, cursor, object);
}
}  // namespace podnative::runtime
