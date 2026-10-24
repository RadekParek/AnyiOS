#include "objc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <thread>

#include "abi.h"
#include "error.h"
#include "format.h"

namespace podnative::runtime {
namespace {
constexpr int GuestPosixBusy = 16;
constexpr uint32_t MaxGuestInstanceBytes = 4 * 1024 * 1024;
// Runtime entry points, in the order their call gates are mapped. The list covers the
// whole objc_msgSend family a 32-bit iOS title links against plus the Foundation C
// functions it calls directly.
const std::vector<std::string>& entryNames() {
    static const std::vector<std::string> names{
        "_objc_msgSend",
        "_objc_msgSendSuper2",
        "_objc_msgSend_stret",
        "_objc_msgSendSuper2_stret",
        "_objc_getClass",
        "_objc_lookUpClass",
        "_sel_registerName",
        "_sel_getName",
        "_objc_retain",
        "_objc_release",
        "_objc_autoreleaseReturnValue",
        "_objc_retainAutoreleasedReturnValue",
        "_objc_alloc",
        "_objc_alloc_init",
        "_objc_setProperty",
        "_objc_getProperty",
        "_objc_copyStruct",
        "_objc_sync_enter",
        "_objc_sync_exit",
        "_objc_enumerationMutation",
        "_objc_autoreleasePoolPush",
        "_objc_autoreleasePoolPop",
        "_objc_retainBlock",
        "_NSClassFromString",
        "_NSStringFromClass",
        "_NSStringFromSelector",
        "_NSLog",
        "_NSSearchPathForDirectoriesInDomains",
        "_NSHomeDirectory",
        "_NSSelectorFromString",
    };
    return names;
}
// Which iOS library exports each entry, so two-level ordinals resolve correctly.
bool isObjcLibraryEntry(const std::string& name) {
    return name.rfind("_objc_", 0) == 0 || name.rfind("__objc_", 0) == 0;
}
}  // namespace
ObjC::ObjC(Memory& m) : memory(m), entries(entryNames()) {}
void ObjC::mapRange(uint32_t base, uint32_t size, const char* label) {
    memory.map({base, size, uint8_t(Read | Write), label});
}
void ObjC::installGates() {
    std::vector<uint8_t> code(Memory::PageSize, 0);
    for (size_t n = 0; n < entries.size(); n++)
        for (unsigned byte = 0; byte < 8; byte++) {
            const uint32_t word = byte < 4 ? 0xef000000u : 0xe12fff1eu;
            code[n * 8 + byte] = uint8_t(word >> ((byte % 4) * 8));
        }
    memory.map({GateBase, Memory::PageSize, uint8_t(Read | Execute), "Objective-C entry gates"},
               code.data(), code.size());
    mapRange(0x6f300000, Memory::PageSize * 4, "Objective-C class objects");
    mapRange(0x6f400000, Memory::PageSize * 4, "Objective-C instances");
    mapRange(0x6f480000, Memory::PageSize * 4, "Objective-C string payloads");
    mapRange(0x6f4c0000, Memory::PageSize * 4, "Objective-C data payloads");
    classObject("NSObject");
    classObject("NSString");
    classObject("NSAutoreleasePool");
    classObject("NSRecursiveLock");
    classObject("NSNumber");
    classObject("NSArray");
    classObject("NSMutableArray");
    classObject("NSDictionary");
    classObject("NSMutableDictionary");
    classObject("NSData");
    classObject("NSNull");
    classObject("NSUserDefaults");
    classObject("NSBundle");
    classObject("NSValue");
    classObject("NSException");
    classObject("NSNotificationCenter");
    classObject("NSRunLoop");
    classObject("NSThread");
    classObject("NSURL");
    classObject("NSConstantString");
    // UIKit classes the launcher models. A class is registered only when its messages have
    // real semantics behind them.
    classObject("UIApplication");
    classObject("UIScreen");
    classObject("UIWindow");
    classObject("UIView");
    classObject("UIViewController");
    classObject("UIColor");
    classObject("UIAccelerometer");
    // EAGL/CAEAGLLayer objects and their drawable calls are modeled by the runtime; GLES owns
    // the real EGL context, renderbuffers, framebuffer and RGBA readback behind them.
    classObject("EAGLContext");
    classObject("CAEAGLLayer");
    classObject("CALayer");
    classObject("AVAudioSession");
}
uint32_t ObjC::gateAddress(const std::string& entry) const {
    for (size_t n = 0; n < entries.size(); n++)
        if (entries[n] == entry) return GateBase + uint32_t(n) * 8;
    return 0;
}
void ObjC::defineSymbols(ExportGraph& graph, size_t foundation, size_t objcLibrary,
                         size_t quartzCore) const {
    (void)quartzCore;
    for (size_t n = 0; n < entries.size(); n++)
        graph.define(isObjcLibraryEntry(entries[n]) ? objcLibrary : foundation, entries[n],
                     {GateBase + uint32_t(n) * 8});
    for (const auto& [name, address] : classes) {
        if (name == "AVAudioSession") continue;  // exported by AVFoundation, not Foundation
        graph.define(foundation, "_OBJC_CLASS_$_" + name, {address});
        graph.define(foundation, "_OBJC_METACLASS_$_" + name, {address});
    }
}
uint32_t ObjC::classObject(const std::string& name) {
    auto found = classes.find(name);
    if (found != classes.end()) return found->second;
    if (classes.size() >= 128) throw Error("OBJC_LIMIT", "too many Objective-C classes");
    const uint32_t address = 0x6f300000 + uint32_t(classes.size()) * 32;
    memory.write32(address, address);  // isa points at the class object itself.
    memory.write32(address + 4, 0);
    std::string canonical = name;
    classes.emplace(canonical, address);
    // The name is stored once so guest code reading the class can print it.
    const uint32_t text = internText(name);
    memory.write32(address + 8, text);
    return address;
}
uint32_t ObjC::classNamePointer(const std::string& name) {
    auto found = classes.find(name);
    if (found != classes.end()) return found->second;
    return classObject(name);
}
std::string ObjC::selectorText(uint32_t address) const {
    // A selector is a plain C string in guest memory.
    std::string name;
    for (size_t n = 0; n < 256; n++) {
        uint8_t byte = 0;
        if (!memory.permits(address + uint32_t(n), 1, Read)) break;
        memory.read(address + uint32_t(n), &byte, 1);
        if (!byte) break;
        name.push_back(char(byte));
    }
    return name;
}
std::string ObjC::readText(uint32_t address) const {
    std::string text;
    for (size_t n = 0; n < 64 * 1024; n++) {
        char c;
        memory.read(address + uint32_t(n), &c, 1);
        if (!c) return text;
        text.push_back(c);
    }
    throw Error("OBJC_STRING", "unterminated guest string", address);
}
uint32_t ObjC::internText(const std::string& text) {
    if (auto found = internedTexts.find(text); found != internedTexts.end()) return found->second;
    if (text.size() > 4096 || nextText + text.size() + 1 > 0x6f480000 + Memory::PageSize * 4)
        throw Error("OBJC_LIMIT", "guest string pool exhausted");
    const uint32_t address = nextText;
    memory.write(address, text.data(), text.size());
    const char terminator = 0;
    memory.write(address + uint32_t(text.size()), &terminator, 1);
    nextText += uint32_t(text.size()) + 1;
    internedTexts.emplace(text, address);
    return address;
}
uint32_t ObjC::internBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > Memory::PageSize * 4 ||
        nextBytes + bytes.size() > 0x6f4c0000 + Memory::PageSize * 4)
        throw Error("OBJC_LIMIT", "guest data pool exhausted");
    const uint32_t address = nextBytes;
    if (!bytes.empty()) memory.write(address, bytes.data(), bytes.size());
    nextBytes += uint32_t(bytes.size());
    // Keep the pool word aligned so a following buffer can be read as words.
    nextBytes = (nextBytes + 3) & ~3u;
    return address;
}
uint32_t ObjC::textOf(uint32_t object) const {
    auto found = objects.find(object);
    return found == objects.end() ? 0 : found->second.text;
}
uint32_t ObjC::makeObject(const std::string& className) {
    if (objects.size() >= 4096 || nextObject + 32 > 0x6f400000 + Memory::PageSize * 4)
        throw Error("OBJC_LIMIT", "guest object pool exhausted");
    const uint32_t address = nextObject;
    nextObject += 32;
    memory.write32(address, classNamePointer(className));
    memory.write32(address + 4, 0);
    Object object;
    object.className = className;
    objects.emplace(address, std::move(object));
    return address;
}
std::string ObjC::describe(uint32_t object) const {
    if (!object) return "(null)";
    auto found = objects.find(object);
    if (auto guest = guestInstances.find(object); guest != guestInstances.end()) {
        std::string out = "<";
        out += guestClassName(guest->second);
        out += '>';
        return out;
    }
    if (found == objects.end()) {
        for (const auto& [name, address] : classes)
            if (address == object) return name;
        return "(invalid object)";
    }
    const Object& value = found->second;
    if (value.className == "NSString" || value.className == "NSMutableString")
        return readText(value.text);
    if (value.className == "NSNumber") {
        if (value.real == double(value.integer)) return std::to_string(value.integer);
        return std::to_string(value.real);
    }
    if (value.className == "NSNull") return "<null>";
    if (value.className == "NSData") {
        std::string out = "<";
        for (size_t index = 0; index < value.bytes.size() && index < 32; index++) {
            static const char* digits = "0123456789abcdef";
            if (index) out += ' ';
            out += digits[value.bytes[index] >> 4];
            out += digits[value.bytes[index] & 15];
        }
        if (value.bytes.size() > 32) out += "...";
        return out + ">";
    }
    if (value.className == "NSArray" || value.className == "NSMutableArray") {
        std::string out = "(";
        for (size_t index = 0; index < value.elements.size(); index++) {
            if (index) out += ", ";
            out += describe(value.elements[index]);
        }
        return out + ")";
    }
    if (value.className == "NSDictionary" || value.className == "NSMutableDictionary") {
        std::string out = "{";
        for (size_t index = 0; index < value.pairs.size(); index++) {
            if (index) out += ", ";
            out += describe(value.pairs[index].first) + " = " + describe(value.pairs[index].second);
        }
        return out + "}";
    }
    return "<" + value.className + ">";
}
std::string ObjC::classNameOf(uint32_t object) const {
    auto found = objects.find(object);
    if (found != objects.end()) return found->second.className;
    if (auto guest = guestInstances.find(object); guest != guestInstances.end())
        return guestClassName(guest->second);
    if (const GuestClass* cls = guestClassAt(object)) return cls->name;
    for (const auto& [name, address] : classes)
        if (address == object) return "class " + name;
    throw Error("OBJC_OBJECT", "unknown or released object", object);
}
std::string ObjC::selector(uint32_t address) const { return readText(address); }
bool ObjC::dispatch(uint32_t pc, CpuState& state) {
    if (pc < GateBase || pc >= GateBase + uint32_t(entries.size()) * 8 || (pc - GateBase) % 8)
        return false;
    invoke(entries[(pc - GateBase) / 8], state);
    return true;
}
uint32_t ObjC::makeString(const std::string& text) {
    const uint32_t object = makeObject("NSString");
    objects[object].text = internText(text);
    return object;
}
uint32_t ObjC::makeHostBoolean(bool value) {
    const uint32_t object = makeObject("NSNumber");
    objects[object].integer = value ? 1 : 0;
    objects[object].real = value ? 1.0 : 0.0;
    return object;
}
void ObjC::ensureRecursiveLock(Object& object) {
    if (object.className != "NSRecursiveLock")
        throw Error("OBJC_LOCK", "recursive lock state requested for a different class");
    if (object.recursiveMutex) return;
    if (!createRecursiveMutex)
        throw Error("OBJC_LOCK", "NSRecursiveLock is not connected to the guest scheduler");
    object.recursiveMutex = createRecursiveMutex();
    if (!object.recursiveMutex)
        throw Error("OBJC_LOCK", "could not create NSRecursiveLock's recursive mutex");
}
uint32_t ObjC::makeArray(const std::vector<uint32_t>& elements) {
    const uint32_t object = makeObject("NSArray");
    objects[object].elements = elements;
    return object;
}
namespace {
// The two metadata layouts a 32-bit iOS image can carry. The modern (non-fragile) layout
// keeps a class_ro_t behind the class's data bits; the legacy layout puts the name, size
// and method list directly in the class. Both store methods as {SEL, types, IMP} words.
bool printableName(Memory& memory, uint32_t address, std::string& out) {
    if (!address || !memory.permits(address, 1, Read)) return false;
    out.clear();
    for (size_t n = 0; n < 256; n++) {
        if (!memory.permits(address + uint32_t(n), 1, Read)) return false;
        char c = 0;
        memory.read(address + uint32_t(n), &c, 1);
        if (!c) return !out.empty();
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) return false;
        out.push_back(c);
    }
    return false;
}
}  // namespace
uint32_t ObjC::resolveClass(const std::string& name, bool required) {
    // The image's own classes come first: `NSClassFromString` on a delegate class must return
    // the class the title defined. Then the runtime's own classes. A name that is neither is
    // refused by name instead of returning nil, because nil here silently turns into a null
    // dereference hundreds of instructions later, with no trace of the real cause.
    if (auto found = guestClasses.find(name); found != guestClasses.end())
        return found->second.address;
    if (auto found = classes.find(name); found != classes.end()) return found->second;
    if (!required && name.empty()) return 0;
    throw Error("OBJC_CLASS_MISSING",
                "class \"" + name +
                    "\" is not implemented by this runtime and is not defined "
                    "by the image");
}
namespace {
// A string literal in an ARM32 image is a four-word __CFConstantString: a class reference
// that dyld binds at load time, a flag word (0x7c8 for 8-bit, 0x7d0 for UTF-16), a pointer
// to the bytes and a length. Nothing here matches a host object, so the layout is checked
// explicitly instead of being guessed from the address.
constexpr uint32_t ConstantStringFlags = 0x7ff;
}  // namespace
std::optional<std::string> ObjC::literalString(uint32_t address) const {
    if (!address || !memory.permits(address, 16, Read)) return std::nullopt;
    const uint32_t flags = memory.read32(address + 4);
    if ((flags & ConstantStringFlags) != 0x7c8 && (flags & ConstantStringFlags) != 0x7d0)
        return std::nullopt;
    const uint32_t bytes = memory.read32(address + 8), length = memory.read32(address + 12);
    if (!bytes || length > 65535 || !memory.permits(bytes, size_t(length) + 1, Read))
        return std::nullopt;
    if ((flags & ConstantStringFlags) == 0x7d0) return std::nullopt;  // UTF-16 literals
    std::string text(length, '\0');
    if (length) memory.read(bytes, text.data(), length);
    char terminator = 0;
    memory.read(bytes + length, &terminator, 1);
    if (terminator) return std::nullopt;
    return text;
}
std::string ObjC::stringOf(uint32_t object) {
    if (auto found = objects.find(object); found != objects.end())
        return readText(found->second.text);
    if (auto literal = literalString(object)) return *literal;
    throw Error("OBJC_STRING", "expected a string object", object);
}
bool ObjC::isStringValue(uint32_t object) const {
    if (auto found = objects.find(object); found != objects.end())
        return found->second.className == "NSString" ||
               found->second.className == "NSMutableString" ||
               found->second.className == "NSConstantString" ||
               found->second.className == "NSCFString";
    return literalString(object).has_value();
}
bool ObjC::stringMessage(const std::string& text, uint32_t bytes, const std::string& method,
                         CpuState& state) {
    auto& r = state.registers;
    if (method == "length") {
        r[0] = uint32_t(text.size());
        return true;
    }
    if (method == "UTF8String" || method == "cString" || method == "fileSystemRepresentation") {
        r[0] = bytes;
        return true;
    }
    if (method == "hash") {
        uint32_t hash = 5381;
        for (unsigned char c : text) hash = hash * 33 + c;
        r[0] = hash;
        return true;
    }
    if (method == "isEqualToString:" || method == "compare:" || method == "isEqual:") {
        const std::string rhs = stringOf(r[2]);
        r[0] = method == "isEqualToString:" || method == "isEqual:" ? uint32_t(text == rhs)
                                                                    : uint32_t(text.compare(rhs));
        return true;
    }
    if (method == "hasPrefix:" || method == "hasSuffix:") {
        const std::string rhs = stringOf(r[2]);
        const bool match = method == "hasPrefix:"
                               ? text.rfind(rhs, 0) == 0
                               : (rhs.size() <= text.size() &&
                                  text.compare(text.size() - rhs.size(), rhs.size(), rhs) == 0);
        r[0] = match ? 1 : 0;
        return true;
    }
    if (method == "stringByAppendingString:") {
        r[0] = makeString(text + stringOf(r[2]));
        return true;
    }
    if (method == "stringByAppendingPathComponent:") {
        const std::string component = stringOf(r[2]);
        std::string path = text;
        if (path.empty()) {
            path = component;
        } else {
            if (path.back() != '/') path.push_back('/');
            size_t first = 0;
            while (first < component.size() && component[first] == '/') ++first;
            path.append(component, first, std::string::npos);
        }
        r[0] = makeString(path);
        return true;
    }
    if (method == "substringToIndex:" || method == "substringFromIndex:") {
        const size_t index = r[2] > text.size() ? text.size() : size_t(r[2]);
        r[0] =
            makeString(method == "substringToIndex:" ? text.substr(0, index) : text.substr(index));
        return true;
    }
    if (method == "intValue" || method == "integerValue" || method == "longLongValue") {
        r[0] = uint32_t(std::strtol(text.c_str(), nullptr, 10));
        return true;
    }
    if (method == "doubleValue" || method == "floatValue") {
        resultF64(state, std::strtod(text.c_str(), nullptr));
        return true;
    }
    if (method == "boolValue") {
        r[0] = text == "YES" || text == "true" || std::strtol(text.c_str(), nullptr, 10) != 0;
        return true;
    }
    return false;
}
const ObjC::GuestClass* ObjC::guestClassAt(uint32_t address) const {
    const auto found = guestClassesByAddress.find(address);
    return found == guestClassesByAddress.end() ? nullptr : found->second;
}
std::string ObjC::guestClassName(uint32_t classAddress) const {
    if (auto info = guestClassAt(classAddress)) return info->name;
    return {};
}
void ObjC::registerGuestClasses(const std::vector<uint32_t>& classList) {
    guestMethodCache.clear();
    for (uint32_t address : classList) {
        if (guestClasses.size() >= 4096)
            throw Error("OBJC_LIMIT", "the image defines too many classes");
        GuestClass info;
        info.address = address;
        info.meta = memory.read32(address);
        info.superclass = memory.read32(address + 4);
        std::string name;
        // Modern layout: class_data_bits_t at +16 hides a class_ro_t pointer whose name is
        // at +16. Legacy layout: the class carries name/version/info/instance_size directly.
        // Registration runs only after dyld has rebased/bound the class data pointer.
        const uint32_t bits = memory.read32(address + 16);
        const uint32_t ro = bits & ~3u;
        if (memory.permits(ro, 40, Read) && printableName(memory, memory.read32(ro + 16), name)) {
            info.modern = true;
            info.instanceSize = memory.read32(ro + 8);
        } else if (printableName(memory, memory.read32(address + 8), name)) {
            info.modern = false;
            info.instanceSize = memory.read32(address + 20);
        } else {
            throw Error("OBJC_CLASS_FORMAT", "class metadata is neither layout", address);
        }
        if (name.rfind("_Tt", 0) == 0)
            throw Error("OBJC_CLASS_FORMAT", "Swift class metadata is not supported", address);
        info.name = name;
        // Objective-C records the instance size in bytes; it need not be a multiple of the
        // ARM32 pointer width (AppController can legitimately be 210 bytes). Allocation below
        // rounds the reserved extent up to the host/guest object's 8-byte alignment.
        if (info.instanceSize < sizeof(uint32_t) || info.instanceSize > MaxGuestInstanceBytes)
            throw Error("OBJC_CLASS_FORMAT",
                        "implausible instance size " + std::to_string(info.instanceSize) +
                            " for class " + name + " (" +
                            (info.modern ? "modern class_ro_t" : "legacy class") + ")",
                        address);
        auto existing = guestClasses.find(name);
        if (existing != guestClasses.end()) {
            if (existing->second.address == info.address) continue;
            throw Error("OBJC_CLASS_FORMAT", "duplicate guest class name " + name, address);
        }
        auto [stored, inserted] = guestClasses.emplace(name, std::move(info));
        if (!inserted) throw Error("OBJC_CLASS_FORMAT", "duplicate guest class name " + name);
        auto addAddress = [&](uint32_t classAddress) {
            if (!classAddress) return;
            auto [known, unique] = guestClassesByAddress.emplace(classAddress, &stored->second);
            if (!unique && known->second != &stored->second)
                throw Error("OBJC_CLASS_FORMAT", "two guest classes share an Objective-C address",
                            classAddress);
        };
        addAddress(stored->second.address);
        addAddress(stored->second.meta);
    }
}
void ObjC::mapGuestArena() {
    if (guestArena) return;
    constexpr uint32_t Size = 8 * 1024 * 1024;
    memory.map({nextGuestObject, Size, uint8_t(Read | Write), "Guest Objective-C instances"});
    guestArena = true;
}
std::string ObjC::guestClassSummary() const {
    if (guestClasses.empty()) return "GUEST_OBJECTIVE_C: the image defines no classes.";
    std::string out = "GUEST_OBJECTIVE_C: " + std::to_string(guestClasses.size()) +
                      " classes defined by the image:";
    size_t shown = 0;
    for (const auto& [name, info] : guestClasses) {
        if (shown++ >= 8) {
            out += ", …";
            break;
        }
        out += (shown == 1 ? " " : ", ") + name;
    }
    return out;
}
std::pair<std::string, std::string> ObjC::inferMainNibDelegate() const {
    std::pair<std::string, std::string> candidate;
    for (const auto& [name, info] : guestClasses) {
        constexpr size_t SuffixLength = 11;
        if (name.size() < SuffixLength ||
            name.compare(name.size() - SuffixLength, SuffixLength, "AppDelegate") != 0)
            continue;
        std::string selectorName;
        if (guestMethod(info.address, false, "application:didFinishLaunchingWithOptions:"))
            selectorName = "application:didFinishLaunchingWithOptions:";
        else if (guestMethod(info.address, false, "applicationDidFinishLaunching:"))
            selectorName = "applicationDidFinishLaunching:";
        if (selectorName.empty()) continue;
        // A guess is only safe when a single class satisfies both the historic naming
        // convention and the actual UIKit callback contract. Ambiguous candidates leave the
        // existing explicit NIB-not-implemented outcome untouched.
        if (!candidate.first.empty()) return {};
        candidate = {name, selectorName};
    }
    return candidate;
}
uint32_t ObjC::allocateGuestInstance(uint32_t classAddress) {
    const GuestClass* info = guestClassAt(classAddress);
    if (!info) throw Error("OBJC_CLASS_MISSING", "alloc of an unknown class", classAddress);
    mapGuestArena();
    constexpr uint64_t Limit = 0x6f500000ULL + 8 * 1024 * 1024;
    const uint64_t alignedSize = (uint64_t(info->instanceSize) + 7) & ~uint64_t(7);
    if (guestInstances.size() >= 16384 || alignedSize > MaxGuestInstanceBytes ||
        uint64_t(nextGuestObject) + alignedSize > Limit)
        throw Error("OBJC_LIMIT", "guest object arena exhausted");
    const uint32_t size = uint32_t(alignedSize);
    const uint32_t object = nextGuestObject;
    nextGuestObject += size;
    memory.write32(object, info->address);  // isa
    guestInstances.emplace(object, info->address);
    guestRefs.emplace(object, 1);
    return object;
}
uint32_t ObjC::superclassOf(const GuestClass& info) const {
    if (!info.address) return info.superclass;
    uint32_t current = 0;
    try {
        current = memory.read32(info.address + 4);
    } catch (const Error&) {
        return info.superclass;
    }
    return current;
}
std::string ObjC::hostAncestorOf(uint32_t classAddress) const {
    uint32_t current = classAddress;
    for (size_t depth = 0; depth < 64 && current; depth++) {
        const GuestClass* info = guestClassAt(current);
        if (!info) return {};
        const uint32_t parent = superclassOf(*info);
        if (!guestClassAt(parent)) return hostClassName(parent);
        current = parent;
    }
    return {};
}
uint32_t ObjC::guestMethod(uint32_t classAddress, bool classMethod, const std::string& name) const {
    const GuestClass* start = guestClassAt(classAddress);
    if (!start) return 0;
    // A metaclass is also in the address index, and its receiver implies +method lookup.
    if (start->meta == classAddress) classMethod = true;
    const uint64_t cacheKey = (uint64_t(classAddress) << 1) | uint64_t(classMethod);
    if (auto byClass = guestMethodCache.find(cacheKey); byClass != guestMethodCache.end()) {
        if (auto method = byClass->second.find(name); method != byClass->second.end())
            return method->second;
    }
    auto remember = [&](uint32_t implementation) {
        guestMethodCache[cacheKey].emplace(name, implementation);
        return implementation;
    };
    // Scans one method_list_t: {entsizeAndFlags, count, methods[]} in the modern layout,
    // {obsolete, count, methods[]} in the legacy one. Both store {SEL, types, IMP} triples.
    auto scan = [&](uint32_t list) -> uint32_t {
        if (!list || !memory.permits(list, 8, Read)) return 0;
        const uint32_t first = memory.read32(list), count = memory.read32(list + 4);
        const uint32_t stride = first ? (first & 0xfffffffc) : 12;
        if (!stride || stride % 4 || stride > 64 || count > 4096) return 0;
        for (uint32_t entry = 0; entry < count; entry++) {
            const uint32_t at = list + 8 + entry * stride;
            if (!memory.permits(at, 12, Read)) return 0;
            std::string candidate;
            if (!printableName(memory, memory.read32(at), candidate)) continue;
            if (candidate != name) continue;
            const uint32_t imp = memory.read32(at + 8);
            if (!imp || !memory.permits(imp & ~1u, 4, Execute)) return 0;
            return imp;
        }
        return 0;
    };
    uint32_t current = classMethod ? (start->meta ? start->meta : start->address) : start->address;
    for (size_t depth = 0; depth < 64 && current; depth++) {
        const GuestClass* info = guestClassAt(current);
        if (!info) break;  // a host class ends the guest chain: the runtime handles the rest
        if (info->modern) {
            // The modern layout stores one method list pointer, not an array of them.
            const uint32_t ro = memory.read32(info->address + 16) & ~3u;
            if (memory.permits(ro + 20, 4, Read))
                if (uint32_t imp = scan(memory.read32(ro + 20))) return remember(imp);
        } else if (memory.permits(info->address + 28, 4, Read)) {
            // The legacy layout stores a nil-terminated array of method list pointers.
            const uint32_t lists = memory.read32(info->address + 28);
            for (size_t index = 0; lists && index < 256; index++) {
                const uint32_t list = memory.read32(lists + uint32_t(index * 4));
                if (!list) break;
                if (uint32_t imp = scan(list)) return remember(imp);
            }
        }
        current = superclassOf(*info);
    }
    return remember(0);
}
bool ObjC::guestChainContains(uint32_t classAddress, uint32_t candidate) const {
    uint32_t current = classAddress;
    for (size_t depth = 0; depth < 64 && current; depth++) {
        if (current == candidate) return true;
        const GuestClass* info = guestClassAt(current);
        if (!info) return false;
        current = superclassOf(*info);
    }
    return false;
}
uint32_t ObjC::selectorString(const std::string& name) { return internText(name); }
bool ObjC::guestFoundation(uint32_t receiver, const std::string& className,
                           const std::string& method, CpuState& state) {
    auto& r = state.registers;
    if (method == "retain") {
        retain(receiver);
        r[0] = receiver;
        return true;
    }
    if (method == "release" || method == "dealloc") {
        release(receiver);
        r[0] = receiver;
        return true;
    }
    if (method == "autorelease") {
        if (pools.empty()) throw Error("OBJC_POOL", "autorelease without a pool");
        pools.back().push_back(receiver);
        r[0] = receiver;
        return true;
    }
    if (method == "class" || method == "self") {
        auto found = guestInstances.find(receiver);
        r[0] = found == guestInstances.end() ? 0 : found->second;
        return true;
    }
    if (method == "isKindOfClass:" || method == "isMemberOfClass:") {
        auto found = guestInstances.find(receiver);
        if (found == guestInstances.end()) {
            r[0] = 0;
            return true;
        }
        r[0] = method == "isMemberOfClass:"
                   ? uint32_t(found->second == r[2])
                   : uint32_t(guestChainContains(found->second, r[2]) || r[2] == found->second);
        return true;
    }
    if (method == "respondsToSelector:") {
        auto found = guestInstances.find(receiver);
        std::string name;
        if (found != guestInstances.end() && printableName(memory, r[2], name))
            r[0] = guestMethod(found->second, false, name) ? 1 : 0;
        else
            r[0] = 0;
        return true;
    }
    if (method == "performSelector:withObject:waitUntilDone:" ||
        method == "performSelectorOnMainThread:withObject:waitUntilDone:") {
        // Foundation's main-thread hop. The selector is queued for UIKit's run loop, and a
        // caller that asked to wait stays blocked until the main thread has run it. The
        // waitUntilDone: flag is the fifth argument, so it arrives on the stack, not in r4.
        const std::string name = selectorText(r[2]);
        if (name.empty()) throw Error("OBJC_SELECTOR", "performSelector with a null selector");
        Arguments args(state, memory, 4);
        const bool wait = args.word() != 0;
        performOnMainThread(receiver, name, r[3], wait);
        r[0] = receiver;
        return true;
    }
    if (method == "performSelector:" || method == "performSelector:withObject:" ||
        method == "performSelector:withObject:withObject:") {
        // A direct perform is a synchronous call: there is no queue in between, so the
        // selector is entered with the same self and the objects the caller passed.
        const std::string name = selectorText(r[2]);
        auto found = guestInstances.find(receiver);
        if (found == guestInstances.end())
            throw Error("OBJC_RECEIVER", "performSelector on an object the image did not allocate",
                        receiver);
        const uint32_t imp = guestMethod(found->second, false, name);
        if (!imp)
            throw Error("OBJC_UNIMPLEMENTED",
                        "-" + className + " does not implement " + name + " for performSelector");
        state.registers[2] = r[3];
        state.registers[3] = r[4];
        return enterGuest(imp, state, receiver, r[2]);
    }
    if (method == "description") {
        r[0] = makeString("<" + className + ">");
        return true;
    }
    if (method == "isEqual:") {
        r[0] = uint32_t(receiver == r[2]);
        return true;
    }
    if (method == "hash") {
        r[0] = receiver;
        return true;
    }
    return false;
}
bool ObjC::guestMessage(uint32_t receiver, uint32_t selectorAddress, CpuState& state) {
    auto& r = state.registers;
    const std::string method = selector(selectorAddress);
    const GuestClass* cls = guestClassAt(receiver);
    const bool instance = guestInstances.count(receiver) != 0;
    if (!cls && !instance) return false;
    if (cls) {
        if (method == "alloc" || method == "new" || method == "allocWithZone:") {
            const uint32_t object = allocateGuestInstance(receiver);
            r[0] = object;
            if (method == "new") {
                // +new is alloc followed by init, so the guest's own -init runs when it
                // defines one; otherwise the allocated object is the result.
                if (uint32_t init = guestMethod(receiver, false, "init"))
                    return enterGuest(init, state, object, selectorString("init"));
            }
            return true;
        }
        if (method == "class") return true;
        if (method == "superclass") {
            r[0] = superclassOf(*cls);
            return true;
        }
        if (method == "description") {
            r[0] = makeString("<" + cls->name + ">");
            return true;
        }
        if (method == "isSubclassOfClass:") {
            r[0] = uint32_t(guestChainContains(receiver, r[2]));
            return true;
        }
    }
    const uint32_t target = instance ? guestInstances.at(receiver) : receiver;
    const std::string className = cls ? cls->name : guestClassName(target);
    if (uint32_t imp = guestMethod(target, cls != nullptr, method)) {
        traceSend((cls ? "+" : "-") + className, method);
        return enterGuest(imp, state, receiver, selectorAddress);
    }
    if (instance && guestFoundation(receiver, className, method, state)) return true;
    // The selector may be inherited from the framework class the image's class derives from
    // (UIView's -initWithFrame:, -setMultipleTouchEnabled:, -layer). Those are modelled on
    // the host side, keyed by the receiving instance.
    if (instance) {
        const std::string host = hostAncestorOf(target);
        if (!host.empty() && uiKitMessage(host, receiver, method, state)) return true;
    }
    throw Error("OBJC_UNIMPLEMENTED",
                "[" + className + (cls ? " +" : " -") + method +
                    "] is defined by the image, but this implementation of it is not "
                    "modelled by the runtime");
}
bool ObjC::enterGuest(uint32_t imp, CpuState& state, uint32_t self, uint32_t cmd) {
    state.registers[0] = self;
    state.registers[1] = cmd;
    state.registers[15] = imp & ~1u;
    state.cpsr = (state.cpsr & ~0x20u) | (imp & 1u ? 0x20u : 0u);
    return true;
}
bool ObjC::sendGuestMessage(uint32_t receiver, const std::string& selectorName, CpuState& state,
                            const std::vector<uint32_t>& arguments, uint32_t returnAddress) {
    const GuestClass* cls = guestClassAt(receiver);
    const bool instance = guestInstances.count(receiver) != 0;
    if (!cls && !instance) return false;
    const uint32_t target = instance ? guestInstances.at(receiver) : receiver;
    const std::string className = cls ? cls->name : guestClassName(target);
    traceSend("->" + className, selectorName);
    const uint32_t sel = selectorString(selectorName);
    const uint32_t imp = guestMethod(target, cls != nullptr, selectorName);
    if (!imp)
        throw Error("OBJC_UNIMPLEMENTED",
                    "the image's " + className + " does not implement " + selectorName);
    state.registers[0] = receiver;
    state.registers[1] = sel;
    for (size_t index = 0; index < arguments.size() && index < 2; index++)
        state.registers[2 + index] = arguments[index];
    state.registers[14] = returnAddress;
    state.registers[15] = imp & ~1u;
    state.cpsr = (state.cpsr & ~0x20u) | (imp & 1u ? 0x20u : 0u);
    return true;
}
namespace {
// A title that queues faster than the run loop drains would grow the queue without bound;
// a run loop that stops draining is a bug worth naming, not a memory leak to absorb.
constexpr size_t MainThreadQueueLimit = 8192;
}  // namespace
void ObjC::performOnMainThread(uint32_t target, const std::string& selector, uint32_t argument,
                               bool wait) {
    if (mainThreadTasks.size() >= MainThreadQueueLimit)
        throw Error("OBJC_LIMIT",
                    "the main-thread queue holds " + std::to_string(mainThreadTasks.size()) +
                        " pending selectors; the run loop is not draining it",
                    target);
    MainThreadTask task;
    task.target = target;
    task.selector = selector;
    task.argument = argument;
    task.eventId = nextEventId++;
    // Queue first and wake the run loop before blocking: a waitUntilDone:YES caller must never
    // block the only thread that could run its own selector.
    mainThreadTasks.push_back(task);
    if (logSink) {
        char line[128];
        std::snprintf(line, sizeof(line), "run loop: queued %s on 0x%08x (wait %s)",
                      selector.c_str(), target, wait ? "yes" : "no");
        logSink(line);
    }
    if (signalRunLoop) signalRunLoop();
    if (wait && blockOnEvent) blockOnEvent(task.eventId);
}
bool ObjC::nextMainThreadTask(MainThreadTask& task) {
    if (mainThreadTasks.empty()) return false;
    task = mainThreadTasks.front();
    mainThreadTasks.pop_front();
    return true;
}
void ObjC::finishMainThreadTask(uint32_t eventId) {
    if (wakeEvent) wakeEvent(eventId);
}
void ObjC::setGuestPaths(std::string bundle, std::string documents) {
    bundlePath = std::move(bundle);
    documentsPath = std::move(documents);
}
void ObjC::setRecursiveLockHooks(std::function<uint32_t()> create,
                                 std::function<int(uint32_t)> lock,
                                 std::function<int(uint32_t)> tryLock,
                                 std::function<int(uint32_t)> unlock,
                                 std::function<void(uint32_t)> destroy) {
    createRecursiveMutex = std::move(create);
    lockRecursiveMutex = std::move(lock);
    tryLockRecursiveMutex = std::move(tryLock);
    unlockRecursiveMutex = std::move(unlock);
    destroyRecursiveMutex = std::move(destroy);
}
void ObjC::setScreenSize(int width, int height) {
    if (width < 1 || height < 1 || width > 8192 || height > 8192)
        throw Error("UIKIT_SCREEN", "guest screen size is out of range");
    screenWidth = width;
    screenHeight = height;
}
bool ObjC::activateEaglContext() {
    // EAGLContext::setCurrentContext only activates the EGL context. The guest creates and
    // binds its own framebuffer/renderbuffer, then asks renderbufferStorage:fromDrawable: to
    // allocate layer-backed storage. Creating a fallback drawable here can make a valid
    // context switch fail before the view has even finished initializing.
    return gles && gles->makeCurrent();
}
void ObjC::drainPools() {
    while (!pools.empty()) {
        std::vector<uint32_t> owned = pools.back();
        pools.pop_back();
        for (uint32_t released : owned)
            if (objects.count(released) || guestInstances.count(released)) release(released);
    }
}
void ObjC::emitLog(const std::string& text) const {
    // The owning Services instance always supplies a sink. A standalone ObjC unit without
    // one drops diagnostics rather than leaking guest bytes to the host's stdout protocol.
    if (logSink) logSink(text);
}
bool ObjC::guestSuperMessage(uint32_t receiver, uint32_t classOfMethod, uint32_t selectorAddress,
                             CpuState& state) {
    const std::string method = selector(selectorAddress);
    const GuestClass* declaring = guestClassAt(classOfMethod);
    if (!declaring) return false;  // a runtime class did the super call: ordinary dispatch
    const bool instance = guestInstances.count(receiver) != 0;
    uint32_t start = superclassOf(*declaring);
    if (!start) return false;
    if (const GuestClass* parent = guestClassAt(start)) {
        if (uint32_t imp = guestMethod(parent->address, false, method))
            return enterGuest(imp, state, receiver, selectorAddress);
        // Continue through the runtime's own classes when the guest chain ends at one.
        const uint32_t parentSuper = superclassOf(*parent);
        if (!guestClassAt(parentSuper)) {
            const std::string hostName = hostClassName(parentSuper);
            if (!hostName.empty() && uiKitMessage(hostName, receiver, method, state)) return true;
        }
        throw Error("OBJC_UNIMPLEMENTED", "[super " + method + "] in " + declaring->name +
                                              " has no implementation in the guest chain");
    }
    const std::string hostName = hostClassName(start);
    (void)instance;
    if (!hostName.empty() && uiKitMessage(hostName, receiver, method, state)) return true;
    throw Error("OBJC_UNIMPLEMENTED", "[super " + method + "] in " + declaring->name + " reaches " +
                                          (hostName.empty() ? "an unknown class" : hostName) +
                                          ", which does not model that method");
}
std::string ObjC::hostClassName(uint32_t address) const {
    for (const auto& [name, object] : classes)
        if (object == address) return name;
    return {};
}
bool ObjC::uiKitMessage(const std::string& className, uint32_t object, const std::string& method,
                        CpuState& state) {
    auto& r = state.registers;
    // A view implemented by the image (its own UIView/UIWindow subclass) has guest-owned
    // metadata, so its view state lives in a second store keyed by the same address instead
    // of being invented in the host object map.
    Object* foundValue = nullptr;
    if (auto it = objects.find(object); it != objects.end()) {
        foundValue = &it->second;
    } else if (auto it = guestViews.find(object); it != guestViews.end()) {
        foundValue = &it->second;
    } else if (guestInstances.count(object)) {
        Object& created = guestViews[object];
        created.className = className;
        foundValue = &created;
    } else {
        return false;
    }
    Object& value = *foundValue;
    // NSThread's instance API. -start is where a title's own game loop begins: it must create
    // a real guest thread, not return success for a thread that never runs.
    if (className == "NSThread") {
        if (method == "initWithTarget:selector:object:") {
            Arguments args(state, memory, 4);
            value.threadTarget = r[2];
            value.threadArgument = args.word();
            const uint32_t selector = r[3];
            if (!selector || !memory.permits(selector, 1, Read))
                throw Error("OBJC_SELECTOR", "NSThread needs a readable selector", selector);
            value.threadSelector = selectorText(selector);
            value.threadStarted = false;
            r[0] = object;
            return true;
        }
        if (method == "start" || method == "main") {
            if (!value.threadTarget || value.threadSelector.empty())
                throw Error("OBJC_THREAD",
                            "NSThread -start without a target and selector; nothing to run");
            if (!spawnThread)
                throw Error("OBJC_THREAD", "no guest scheduler is wired for NSThread -start");
            if (value.threadStarted) {
                r[0] = 0;  // Darwin also refuses to start the same thread twice
                return true;
            }
            if (!spawnThread(value.threadTarget, value.threadSelector, value.threadArgument))
                throw Error("OBJC_THREAD", "the guest thread could not be created");
            value.threadStarted = true;
            r[0] = 0;
            return true;
        }
        if (method == "isExecuting") {
            r[0] = value.threadStarted && !value.threadFinished ? 1 : 0;
            return true;
        }
        if (method == "isFinished") {
            r[0] = value.threadFinished ? 1 : 0;
            return true;
        }
        if (method == "cancel") {
            // Cooperative cancellation, exactly like Foundation: the flag is set and a loop
            // that polls -isCancelled stops on its next pass.
            value.threadCancelled = true;
            r[0] = 0;
            return true;
        }
        if (method == "isCancelled") {
            r[0] = value.threadCancelled ? 1 : 0;
            return true;
        }
    }
    // CGRect and CGPoint results travel in r0-r3 (base AAPCS: no VFP argument registers).
    auto rect = [&](const float* values) {
        for (size_t n = 0; n < 4; n++) r[n] = floatBits(values[n]);
    };
    // A CGRect argument reaches an ARM callee either as a pointer to the caller's copy or
    // with its four words already in r2-r5, depending on how the framework header was
    // compiled. Both are accepted: the pointer form is used when the register really is
    // readable, otherwise the words are taken as they were passed.
    auto readRect = [&](uint32_t address, float* out) {
        if (memory.permits(address, 16, Read)) {
            for (size_t n = 0; n < 4; n++)
                out[n] = bitsFloat(memory.read32(address + uint32_t(n) * 4));
            return;
        }
        if (address == r[2]) {
            for (size_t n = 0; n < 4; n++) out[n] = bitsFloat(r[2 + n]);
            return;
        }
        throw Error("UIKIT_ARGUMENT", "geometry argument is not readable", address);
    };
    auto objectState = [&](uint32_t address) -> Object* {
        if (!address) return nullptr;
        if (auto it = objects.find(address); it != objects.end()) return &it->second;
        if (auto it = guestViews.find(address); it != guestViews.end()) return &it->second;
        if (auto it = guestInstances.find(address); it != guestInstances.end()) {
            Object& state = guestViews[address];
            if (state.className.empty()) state.className = guestClassName(it->second);
            return &state;
        }
        return nullptr;
    };
    auto detachView = [&](uint32_t viewAddress) {
        Object* viewState = objectState(viewAddress);
        if (!viewState) return;
        if (viewState->superview) {
            if (Object* parent = objectState(viewState->superview)) {
                auto& subviews = parent->elements;
                subviews.erase(std::remove(subviews.begin(), subviews.end(), viewAddress),
                               subviews.end());
            }
        }
        viewState->superview = 0;
        viewState->window = 0;
    };
    auto attachControllerView = [&](uint32_t windowAddress, uint32_t controllerAddress) {
        Object* windowState = objectState(windowAddress);
        Object* controllerState = objectState(controllerAddress);
        if (!windowState || !controllerState || !controllerState->view) return;
        const uint32_t viewAddress = controllerState->view;
        Object* viewState = objectState(viewAddress);
        if (!viewState)
            throw Error("UIKIT_VIEW", "UIViewController setView: received an unknown view",
                        viewAddress);
        if (viewState->superview && viewState->superview != windowAddress) detachView(viewAddress);
        viewState->superview = windowAddress;
        viewState->window = windowAddress;
        auto& subviews = windowState->elements;
        if (std::find(subviews.begin(), subviews.end(), viewAddress) == subviews.end())
            subviews.push_back(viewAddress);
    };
    auto detachControllerView = [&](uint32_t windowAddress, uint32_t controllerAddress) {
        Object* controllerState = objectState(controllerAddress);
        if (!controllerState || !controllerState->view) return;
        Object* viewState = objectState(controllerState->view);
        if (viewState && viewState->superview == windowAddress) detachView(controllerState->view);
    };
    if (className == "UIViewController") {
        if (method == "view") {
            r[0] = value.view;
            return true;
        }
        if (method == "setView:") {
            const uint32_t newView = r[2];
            if (newView && !objectState(newView))
                throw Error("UIKIT_VIEW", "UIViewController setView: received an unknown view",
                            newView);
            if (value.view != newView) {
                const uint32_t previous = value.view;
                if (previous) {
                    detachView(previous);
                    release(previous);
                }
                if (newView) retain(newView);
                value.view = newView;
            }
            // setView: can run before or after UIWindow's rootViewController is assigned.
            for (const auto& [windowAddress, window] : objects)
                if (window.className == "UIWindow" && window.rootViewController == object)
                    attachControllerView(windowAddress, object);
            for (const auto& [windowAddress, window] : guestViews)
                if (window.className == "UIWindow" && window.rootViewController == object)
                    attachControllerView(windowAddress, object);
            return true;
        }
        if (method == "isViewLoaded") {
            r[0] = value.view ? 1 : 0;
            return true;
        }
    }
    if (className == "UIApplication") {
        if (method == "delegate") {
            r[0] = value.delegate;
            return true;
        }
        if (method == "setDelegate:") {
            value.delegate = r[2];
            return true;
        }
        if (method == "keyWindow") {
            r[0] = value.keyWindow;
            return true;
        }
        // Status bar and idle-timer requests are accepted as configuration: there is no
        // window server to apply them to, and the boot view owns the real full-screen state.
        if (method == "applicationIconBadgeNumber") {
            r[0] = uint32_t(value.integer);
            return true;
        }
        if (method == "setApplicationIconBadgeNumber:") {
            value.integer = int32_t(r[2]);
            r[0] = 0;
            return true;
        }
        if (method == "setStatusBarOrientation:" || method == "setStatusBarOrientation:animated:") {
            value.integer = int32_t(r[2]);
            return true;
        }
        if (method == "setStatusBarHidden:" || method == "setStatusBarHidden:animated:" ||
            method == "setStatusBarStyle:" || method == "setStatusBarStyle:animated:" ||
            method == "setIdleTimerDisabled:" || method == "beginIgnoringInteractionEvents" ||
            method == "endIgnoringInteractionEvents")
            return true;
        if (method == "statusBarOrientation") {
            r[0] = value.integer ? uint32_t(value.integer)
                                 : uint32_t(screenWidth > screenHeight ? 3 : 1);
            return true;
        }
        if (method == "statusBarHidden" || method == "isStatusBarHidden") {
            r[0] = 0;
            return true;
        }
        if (method == "applicationFrame") {
            const float frame[4] = {0, 0, float(screenWidth), float(screenHeight)};
            rect(frame);
            return true;
        }
        if (method == "windows" || method == "connectedScenes") {
            r[0] = makeArray({});
            return true;
        }
        if (method == "openURL:") {
            // Opening another application is not something this runtime can do, and claiming
            // success would be wrong. The call is refused by name.
            throw Error("UIKIT_UNSUPPORTED", "-[UIApplication openURL:] needs the system launcher");
        }
        return false;
    }
    if (className == "UIScreen") {
        if (method == "bounds") {
            const float frame[4] = {0, 0, float(screenWidth), float(screenHeight)};
            rect(frame);
            return true;
        }
        if (method == "scale") {
            resultF64(state, 1.0);
            return true;
        }
        if (method == "applicationFrame" || method == "nativeBounds") {
            const float frame[4] = {0, 0, float(screenWidth), float(screenHeight)};
            rect(frame);
            return true;
        }
        return false;
    }
    if (className == "UIAccelerometer") {
        if (method == "setUpdateInterval:") {
            value.real = double(bitsFloat(r[2]));
            return true;
        }
        if (method == "updateInterval") {
            resultF64(state, value.real);
            return true;
        }
        if (method == "delegate") {
            r[0] = value.delegate;
            return true;
        }
        if (method == "setDelegate:") {
            value.delegate = r[2];
            return true;
        }
        return false;
    }
    if (className == "EAGLContext") {
        if (method == "initWithAPI:") {
            // kEAGLRenderingAPIOpenGLES1 (1) or OpenGLES2 (2): the guest is telling the
            // driver which dialect it will speak, and an ES 2.0 context refuses the
            // fixed-function calls an ES 1.1 title makes. The choice is carried through.
            value.integer = r[2];
            if (gles) gles->setClientApi(int(r[2]));
            r[0] = object;  // an init method returns the receiver
            return true;
        }
        if (method == "API") {
            r[0] = uint32_t(value.integer);
            return true;
        }
        if (method == "setCurrentContext") {
            currentEaglContext = object;
            r[0] = activateEaglContext() ? 1 : 0;
            return true;
        }
        if (method == "sharegroup") {
            r[0] = makeObject("EAGLSharegroup");
            return true;
        }
        if (method == "renderbufferStorage:fromDrawable:") {
            if (!gles || !gles->available())
                throw Error("EAGL_NO_DRIVER",
                            "-[EAGLContext renderbufferStorage:fromDrawable:] needs a real "
                            "OpenGL ES driver, and none is loaded in this build");
            // The renderbuffer's size comes from the layer this context draws into: the
            // guest has already set the view's frame, which is the drawable's geometry.
            int width = screenWidth, height = screenHeight;
            // -renderbufferStorage:fromDrawable: passes the CAEAGLLayer in r3; its geometry
            // is the drawable's, and the guest has already set the view frame it mirrors.
            const uint32_t layer = r[3];
            if (auto found = objects.find(layer); found != objects.end()) {
                const float* frame =
                    found->second.hasFrame ? found->second.frame : found->second.bounds;
                if (frame[2] >= 1 && frame[3] >= 1) {
                    width = int(frame[2] + 0.5f);
                    height = int(frame[3] + 0.5f);
                }
            }
            if (!gles->renderbufferStorageFromDrawable(width, height))
                throw Error("EAGL_DRAWABLE",
                            "-[EAGLContext renderbufferStorage:fromDrawable:] could not "
                            "allocate the bound renderbuffer: " +
                                gles->describe());
            value.integer = (int64_t(height) << 32) | uint32_t(width);
            r[0] = 1;
            value.hasFrame = true;
            value.frame[0] = value.frame[1] = 0;
            value.frame[2] = float(width);
            value.frame[3] = float(height);
            return true;
        }
        if (method == "presentRenderbuffer:") {
            if (!gles || !gles->available())
                throw Error("EAGL_NO_DRIVER",
                            "-[EAGLContext presentRenderbuffer:] needs a real OpenGL ES "
                            "driver, and none is loaded in this build");
            if (!gles->present())
                throw Error("EAGL_PRESENT",
                            "-[EAGLContext presentRenderbuffer:] failed: " + gles->describe());
            r[0] = 1;
            return true;
        }
        return false;
    }
    if (className == "CAEAGLLayer" || className == "CALayer") {
        if (method == "setDrawableProperties:") {
            value.delegate = r[2];
            return true;
        }
        if (method == "drawableProperties") {
            r[0] = value.delegate;
            return true;
        }
        if (method == "setOpaque:") {
            value.hidden = r[2] == 0;
            return true;
        }
        if (method == "isOpaque") {
            r[0] = value.hidden ? 0 : 1;
            return true;
        }
        if (method == "setFrame:" || method == "setBounds:" || method == "frame" ||
            method == "bounds" || method == "setContentsScale:" || method == "contentsScale")
            return false;  // falls through to the view/layer geometry below
        return false;
    }
    if (className == "UIWindow" || className == "UIView" || className == "UIControl" ||
        className == "UIButton" || className == "UILabel" || className == "UIAlertView" ||
        className == "UIScrollView" || className == "UITextField" ||
        className == "UIViewController") {
        if (method == "frame") {
            rect(value.hasFrame ? value.frame : value.bounds);
            return true;
        }
        if (method == "setFrame:") {
            readRect(r[2], value.hasFrame ? value.frame : value.frame);
            value.hasFrame = true;
            if (!value.bounds[2] && !value.bounds[3]) {
                value.bounds[0] = value.bounds[1] = 0;
                value.bounds[2] = value.frame[2];
                value.bounds[3] = value.frame[3];
            }
            return true;
        }
        if (method == "layer") {
            if (!value.layer) {
                // The layer kind is decided by the image's own +layerClass override when it
                // declares one; that override returns CAEAGLLayer for an OpenGL ES view.
                std::string kind = "CALayer";
                auto instance = guestInstances.find(object);
                if (instance != guestInstances.end())
                    if (const GuestClass* info = guestClassAt(instance->second))
                        if (guestMethod(info->meta ? info->meta : info->address, true,
                                        "layerClass"))
                            kind = "CAEAGLLayer";
                value.layer = makeObject(kind);
            }
            r[0] = value.layer;
            return true;
        }
        if (method == "initWithFrame:") {
            // -[UIView initWithFrame:] is where a subclass's super call lands. The frame is
            // real view state, so it is stored and the receiver comes back as the result.
            readRect(r[2], value.frame);
            value.hasFrame = true;
            value.bounds[0] = value.bounds[1] = 0;
            value.bounds[2] = value.frame[2];
            value.bounds[3] = value.frame[3];
            r[0] = object;
            return true;
        }
        if (method == "bounds") {
            rect(value.bounds);
            return true;
        }
        if (method == "setBounds:") {
            readRect(r[2], value.bounds);
            return true;
        }
        if (method == "center" || method == "setCenter:" || method == "setTransform:" ||
            method == "setAutoresizingMask:" || method == "setContentMode:" ||
            method == "setNeedsDisplay" || method == "layoutIfNeeded" ||
            method == "setNeedsLayout" || method == "sizeToFit" || method == "setExclusiveTouch:" ||
            method == "setClipsToBounds:" || method == "setBackgroundColor:" ||
            method == "setTintColor:" || method == "setText:" || method == "setFont:" ||
            method == "setTitle:" || method == "setTitleColor:forState:" ||
            method == "setImage:forState:" || method == "addTarget:action:forControlEvents:" ||
            method == "dismissWithClickedButtonIndex:animated:")
            return true;
        if (method == "addSubview:" || method == "insertSubview:atIndex:" ||
            method == "bringSubviewToFront:") {
            if (r[2] && objects.count(r[2])) {
                value.elements.push_back(r[2]);
                objects[r[2]].superview = object;
            }
            return true;
        }
        if (method == "removeFromSuperview") {
            if (value.superview) {
                auto parent = objects.find(value.superview);
                if (parent != objects.end())
                    parent->second.elements.erase(
                        std::remove(parent->second.elements.begin(), parent->second.elements.end(),
                                    object),
                        parent->second.elements.end());
            }
            value.superview = 0;
            return true;
        }
        if (method == "subviews") {
            r[0] = makeArray(value.elements);
            return true;
        }
        if (method == "superview") {
            r[0] = value.superview;
            return true;
        }
        if (method == "window") {
            r[0] = value.window ? value.window : (className == "UIWindow" ? object : 0);
            return true;
        }
        if (method == "isHidden") {
            r[0] = value.hidden ? 1 : 0;
            return true;
        }
        if (method == "setHidden:") {
            value.hidden = r[2] != 0;
            return true;
        }
        if (method == "alpha") {
            resultF64(state, value.alpha);
            return true;
        }
        if (method == "setAlpha:") {
            value.alpha = 0;
            Arguments args(state, memory, 2);
            value.alpha = args.f32();
            return true;
        }
        if (method == "isUserInteractionEnabled") {
            r[0] = value.userInteraction ? 1 : 0;
            return true;
        }
        if (method == "setUserInteractionEnabled:") {
            value.userInteraction = r[2] != 0;
            return true;
        }
        if (method == "setMultipleTouchEnabled:") {
            value.multipleTouch = r[2] != 0;
            return true;
        }
        if (method == "contentScaleFactor") {
            resultF64(state, value.contentScale);
            return true;
        }
        if (method == "setContentScaleFactor:") {
            Arguments args(state, memory, 2);
            value.contentScale = args.f32();
            return true;
        }
        if (method == "makeKeyAndVisible") {
            value.hidden = false;
            const uint32_t app = classObject("UIApplication");
            (void)app;
            if (!applicationObject) applicationObject = makeObject("UIApplication");
            objects[applicationObject].keyWindow = object;
            for (auto& [address, other] : objects)
                if (other.className == "UIView" || other.className == "UIWindow")
                    other.window = object;
            return true;
        }
        if (method == "rootViewController") {
            r[0] = value.rootViewController;
            return true;
        }
        if (method == "setRootViewController:") {
            const uint32_t next = r[2];
            const uint32_t previous = value.rootViewController;
            if (next && !objectState(next))
                throw Error("UIKIT_CONTROLLER", "UIWindow root controller is not an object", next);
            if (previous != next) {
                if (next) retain(next);
                if (previous) detachControllerView(object, previous);
                value.rootViewController = next;
                if (previous) release(previous);
            }
            if (next) attachControllerView(object, next);
            return true;
        }
        if (method == "backgroundColor") {
            r[0] = value.backgroundColor;
            return true;
        }
        if (method == "initWithFrame:") {
            readRect(r[2], value.frame);
            value.hasFrame = true;
            value.bounds[0] = value.bounds[1] = 0;
            value.bounds[2] = value.frame[2];
            value.bounds[3] = value.frame[3];
            r[0] = object;
            return true;
        }
        if (method == "initWithCoder:" || method == "awakeFromNib" || method == "init") {
            r[0] = object;
            return true;
        }
        if (method == "layer") {
            // A view's CALayer is the same object identity for the guest; the layer's own
            // rendering semantics are QuartzCore's, not invented here.
            r[0] = value.elements.empty() ? 0 : value.elements.front();
            return false;
        }
        return false;
    }
    if (className == "UIColor") {
        if (method == "set" || method == "setFill" || method == "setStroke") return true;
        return false;
    }
    return false;
}
bool ObjC::bundleMessage(const std::string& method, CpuState& state) {
    auto& r = state.registers;
    if (method == "bundlePath" || method == "resourcePath" || method == "builtInPlugInsPath" ||
        method == "privateFrameworksPath") {
        r[0] = makeString(bundlePath);
        return true;
    }
    if (method == "bundleIdentifier" || method == "executablePath" || method == "infoDictionary") {
        // The bundle's own Info.plist is not parsed yet, so these stay honestly empty
        // instead of inventing an identifier.
        r[0] = method == "infoDictionary" ? 0 : makeString("");
        return true;
    }
    if (method == "pathForResource:ofType:" || method == "pathForResource:ofType:inDirectory:") {
        const uint32_t name = r[2], type = r[3];
        if (!name) {
            r[0] = 0;
            return true;
        }
        std::string path = readText(name);
        if (type) {
            path += '.';
            path += readText(type);
        }
        const std::string candidate = bundlePath + "/" + path;
        if (fileExists && !fileExists(candidate)) {
            r[0] = 0;
            return true;
        }
        r[0] = makeString(candidate);
        return true;
    }
    if (method == "pathForResource:ofType:inDirectory:forLocalization:") {
        r[0] = 0;
        return true;
    }
    return false;
}
void ObjC::retain(uint32_t object) {
    if (!object) return;
    if (auto found = objects.find(object); found != objects.end()) {
        if (found->second.references < UINT32_MAX) found->second.references++;
        return;
    }
    if (guestInstances.count(object)) {
        uint32_t& references = guestRefs[object];
        if (references < UINT32_MAX) references++;
        return;
    }
    throw Error("OBJC_RETAIN", "retain of a non-object", object);
}
void ObjC::release(uint32_t object) {
    if (auto found = objects.find(object); found != objects.end()) {
        if (found->second.references > 1) {
            found->second.references--;
            return;
        }
        uint32_t ownedView = found->second.className == "UIViewController" ? found->second.view : 0;
        uint32_t rootController =
            found->second.className == "UIWindow" ? found->second.rootViewController : 0;
        if (found->second.recursiveMutex) {
            if (!destroyRecursiveMutex)
                throw Error("OBJC_LOCK", "NSRecursiveLock has no scheduler destroy hook", object);
            destroyRecursiveMutex(found->second.recursiveMutex);
        }
        if (found->second.className == "UIWindow")
            for (uint32_t child : found->second.elements) {
                Object* childState = nullptr;
                if (auto view = objects.find(child); view != objects.end())
                    childState = &view->second;
                if (auto view = guestViews.find(child); view != guestViews.end())
                    childState = &view->second;
                if (childState && childState->superview == object)
                    childState->superview = childState->window = 0;
            }
        objects.erase(found);
        if (ownedView && (objects.count(ownedView) || guestInstances.count(ownedView)))
            release(ownedView);
        if (rootController &&
            (objects.count(rootController) || guestInstances.count(rootController)))
            release(rootController);
        return;
    }
    if (guestInstances.count(object)) {
        auto found = guestRefs.find(object);
        if (found != guestRefs.end() && found->second > 1) {
            found->second--;
            return;
        }
        uint32_t ownedView = 0;
        if (auto state = guestViews.find(object); state != guestViews.end())
            ownedView = state->second.view;
        guestRefs.erase(object);
        guestViews.erase(object);
        guestInstances.erase(object);
        if (ownedView && (objects.count(ownedView) || guestInstances.count(ownedView)))
            release(ownedView);
        return;
    }
    throw Error("OBJC_RELEASE", "release of a non-object", object);
}
bool ObjC::ownsObject(uint32_t object) const {
    if (!object || objects.count(object) || guestInstances.count(object) || guestClassAt(object))
        return true;
    for (const auto& [name, address] : classes) {
        (void)name;
        if (address == object) return true;
    }
    return literalString(object).has_value();
}
void ObjC::retainObject(uint32_t object) {
    if (!object || objects.count(object) || guestInstances.count(object)) {
        retain(object);
        return;
    }
    if (ownsObject(object)) return;  // class objects and constant strings are immortal
    retain(object);                  // preserve the runtime's named invalid-object error
}
void ObjC::releaseObject(uint32_t object) {
    if (!object || objects.count(object) || guestInstances.count(object)) {
        release(object);
        return;
    }
    if (ownsObject(object)) return;
    release(object);  // preserve the runtime's named invalid-object error
}
bool ObjC::equalObjects(uint32_t a, uint32_t b) const {
    auto compare = [&](auto&& self, uint32_t left, uint32_t right, size_t depth) -> bool {
        if (left == right) return true;
        if (depth > 32) return false;
        auto stringValue = [&](uint32_t value) -> std::optional<std::string> {
            if (auto found = objects.find(value);
                found != objects.end() && (found->second.className == "NSString" ||
                                           found->second.className == "NSMutableString"))
                return readText(found->second.text);
            return literalString(value);
        };
        const auto leftString = stringValue(left), rightString = stringValue(right);
        if (leftString || rightString)
            return leftString && rightString && *leftString == *rightString;
        const auto leftObject = objects.find(left), rightObject = objects.find(right);
        if (leftObject == objects.end() || rightObject == objects.end() ||
            leftObject->second.className != rightObject->second.className)
            return false;
        const Object& x = leftObject->second;
        const Object& y = rightObject->second;
        if (x.className == "NSNumber") return x.integer == y.integer && x.real == y.real;
        if (x.className == "NSNull") return true;
        if (x.className == "NSData") return x.bytes == y.bytes;
        if (x.className == "NSArray" || x.className == "NSMutableArray") {
            if (x.elements.size() != y.elements.size()) return false;
            for (size_t i = 0; i < x.elements.size(); ++i)
                if (!self(self, x.elements[i], y.elements[i], depth + 1)) return false;
            return true;
        }
        return false;
    };
    return compare(compare, a, b, 0);
}
bool ObjC::collection(const std::string& className, const std::string& method, Object& object,
                      CpuState& state) {
    const bool array = className == "NSArray" || className == "NSMutableArray";
    const bool dictionary = className == "NSDictionary" || className == "NSMutableDictionary";
    const bool mutableCollection = className.rfind("NSMutable", 0) == 0;
    if (array) {
        if (method == "count") {
            state.registers[0] = uint32_t(object.elements.size());
            return true;
        }
        if (method == "objectAtIndex:" || method == "objectAtIndexedSubscript:") {
            const uint32_t index = state.registers[2];
            if (index >= object.elements.size())
                throw Error("OBJC_RANGE", "array index out of bounds", index);
            state.registers[0] = object.elements[index];
            return true;
        }
        if (method == "firstObject") {
            state.registers[0] = object.elements.empty() ? 0 : object.elements.front();
            return true;
        }
        if (method == "lastObject") {
            state.registers[0] = object.elements.empty() ? 0 : object.elements.back();
            return true;
        }
        if (method == "containsObject:") {
            const uint32_t value = state.registers[2];
            state.registers[0] = std::find(object.elements.begin(), object.elements.end(), value) !=
                                 object.elements.end();
            return true;
        }
        if (method == "indexOfObject:") {
            const uint32_t value = state.registers[2];
            auto found = std::find(object.elements.begin(), object.elements.end(), value);
            state.registers[0] = found == object.elements.end()
                                     ? 0x7fffffffu
                                     : uint32_t(found - object.elements.begin());
            return true;
        }
        if (mutableCollection && method == "addObject:") {
            if (object.elements.size() >= 65536) throw Error("OBJC_LIMIT", "array is full");
            object.elements.push_back(state.registers[2]);
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "insertObject:atIndex:") {
            const uint32_t index = state.registers[3];
            if (index > object.elements.size())
                throw Error("OBJC_RANGE", "array insert index out of bounds", index);
            object.elements.insert(object.elements.begin() + index, state.registers[2]);
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "removeObjectAtIndex:") {
            const uint32_t index = state.registers[2];
            if (index >= object.elements.size())
                throw Error("OBJC_RANGE", "array remove index out of bounds", index);
            object.elements.erase(object.elements.begin() + index);
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "removeLastObject") {
            if (object.elements.empty())
                throw Error("OBJC_RANGE", "removeLastObject on an empty array");
            object.elements.pop_back();
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "removeAllObjects") {
            object.elements.clear();
            state.registers[0] = 0;
            return true;
        }
        if (method == "componentsJoinedByString:") {
            const uint32_t separatorAddress = state.registers[2];
            const std::string separator = readText(separatorAddress);
            std::string joined;
            for (size_t index = 0; index < object.elements.size(); index++) {
                if (index) joined += separator;
                joined += readText(textOf(object.elements[index]));
            }
            const uint32_t text = makeObject("NSString");
            objects[text].text = internText(joined);
            state.registers[0] = text;
            return true;
        }
    }
    if (dictionary) {
        if (method == "count") {
            state.registers[0] = uint32_t(object.pairs.size());
            return true;
        }
        if (method == "objectForKey:" || method == "valueForKey:") {
            const uint32_t key = state.registers[2];
            for (const auto& [stored, value] : object.pairs)
                if (stored == key) {
                    state.registers[0] = value;
                    return true;
                }
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "setObject:forKey:") {
            const uint32_t value = state.registers[2], key = state.registers[3];
            for (auto& [stored, existing] : object.pairs)
                if (stored == key) {
                    existing = value;
                    state.registers[0] = 0;
                    return true;
                }
            if (object.pairs.size() >= 65536) throw Error("OBJC_LIMIT", "dictionary is full");
            object.pairs.emplace_back(key, value);
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "removeObjectForKey:") {
            const uint32_t key = state.registers[2];
            object.pairs.erase(
                std::remove_if(object.pairs.begin(), object.pairs.end(),
                               [key](const auto& entry) { return entry.first == key; }),
                object.pairs.end());
            state.registers[0] = 0;
            return true;
        }
        if (mutableCollection && method == "removeAllObjects") {
            object.pairs.clear();
            state.registers[0] = 0;
            return true;
        }
        if (method == "allKeys" || method == "allValues") {
            const uint32_t result = makeObject("NSArray");
            auto& elements = objects[result].elements;
            for (const auto& [key, value] : object.pairs)
                elements.push_back(method == "allKeys" ? key : value);
            state.registers[0] = result;
            return true;
        }
    }
    if (className == "NSData") {
        if (method == "length") {
            state.registers[0] = uint32_t(object.bytes.size());
            return true;
        }
        if (method == "bytes") {
            state.registers[0] = object.text;
            return true;
        }
        if (method == "getBytes:length:") {
            const uint32_t destination = state.registers[2];
            const uint32_t length =
                std::min<uint32_t>(state.registers[3], uint32_t(object.bytes.size()));
            if (length) {
                if (!destination) throw Error("OBJC_ARGUMENT", "getBytes:length: needs a buffer");
                memory.write(destination, object.bytes.data(), length);
            }
            state.registers[0] = 0;
            return true;
        }
    }
    if (className == "NSUserDefaults" || className == "NSUserDefaults") {
        if (method == "objectForKey:" || method == "stringForKey:" || method == "valueForKey:") {
            const uint32_t key = state.registers[2];
            for (const auto& [stored, value] : defaults)
                if (stored == key) {
                    state.registers[0] = value;
                    return true;
                }
            state.registers[0] = 0;
            return true;
        }
        if (method == "setObject:forKey:") {
            const uint32_t value = state.registers[2], key = state.registers[3];
            for (auto& [stored, existing] : defaults)
                if (stored == key) {
                    existing = value;
                    state.registers[0] = 0;
                    return true;
                }
            if (defaults.size() >= 4096) throw Error("OBJC_LIMIT", "defaults are full");
            defaults.emplace_back(key, value);
            state.registers[0] = 0;
            return true;
        }
        if (method == "removeObjectForKey:") {
            const uint32_t key = state.registers[2];
            defaults.erase(std::remove_if(defaults.begin(), defaults.end(),
                                          [key](const auto& entry) { return entry.first == key; }),
                           defaults.end());
            state.registers[0] = 0;
            return true;
        }
        if (method == "integerForKey:" || method == "boolForKey:") {
            const uint32_t key = state.registers[2];
            for (const auto& [stored, value] : defaults)
                if (stored == key) {
                    auto found = objects.find(value);
                    if (found == objects.end())
                        throw Error("OBJC_RECEIVER", "default value is not an object", value);
                    state.registers[0] =
                        uint32_t(method == "boolForKey:" ? (found->second.integer ? 1 : 0)
                                                         : found->second.integer);
                    return true;
                }
            state.registers[0] = 0;
            return true;
        }
        if (method == "setInteger:forKey:" || method == "setBool:forKey:") {
            const uint32_t object = makeObject("NSNumber");
            objects[object].integer = int32_t(state.registers[2]);
            objects[object].real = double(objects[object].integer);
            const uint32_t key = state.registers[3];
            for (auto& [stored, existing] : defaults)
                if (stored == key) {
                    existing = object;
                    state.registers[0] = 0;
                    return true;
                }
            defaults.emplace_back(key, object);
            state.registers[0] = 0;
            return true;
        }
    }
    return false;
}
void ObjC::traceSend(const std::string& receiver, const std::string& method) {
    if (!logSink) return;
    // Once per receiver/selector pair: a repeated send adds nothing a reader of the trace
    // needs, and a per-call line would bury the call graph under the audio thread.
    if (!tracedSends.insert(receiver + " " + method).second) return;
    logSink("msg " + receiver + " " + method);
}
void ObjC::message(CpuState& state) {
    const uint32_t self = state.registers[0], name = state.registers[1];
    // Messaging nil is well defined in Objective-C and returns zero — that is not an
    // unimplemented API and must not stop the guest.
    if (!self) {
        state.registers[0] = 0;
        state.registers[1] = 0;
        return;
    }
    if (!name) throw Error("OBJC_SELECTOR", "message sent with a null selector", self);
    // A string literal is a __CFConstantString in the image, not one of our objects: its
    // class reference is bound by dyld and its bytes live in __cstring. Titles message
    // literals constantly, so they are recognised here rather than refused.
    if (auto literal = literalString(self)) {
        const std::string& method = selector(name);
        const uint32_t bytes = memory.read32(self + 8);
        if (stringMessage(*literal, bytes, method, state)) return;
        if (method == "description" || method == "self") {
            state.registers[0] = self;
            return;
        }
        throw Error("OBJC_UNIMPLEMENTED", "NSString literal does not implement " + method);
    }
    // A receiver the image itself defines (one of its classes or an instance of one) is
    // dispatched into guest code, not into the host object store.
    if ((guestClassAt(self) || guestInstances.count(self)) && guestMessage(self, name, state))
        return;
    const std::string method = selector(name);
    bool classMethod = false;
    std::string className;
    for (const auto& [declared, address] : classes)
        if (address == self) {
            classMethod = true;
            className = declared;
        }
    if (!classMethod) {
        auto object = objects.find(self);
        if (object == objects.end()) {
            std::ostringstream out;
            out << "message \"" << method << "\" sent to 0x" << std::hex << self
                << ", which is not an object this runtime created";
            if (trapName)
                if (const std::string import = trapName(self); !import.empty())
                    out << ": the receiver is the unimplemented-import trap for " << import
                        << ", because that class or symbol has no implementation yet";
            throw Error("OBJC_RECEIVER", out.str(), self);
        }
        className = object->second.className;
    }
    traceSend((classMethod ? "+" : "-") + className, method);
    auto unsupported = [&] {
        throw Error("OBJC_UNIMPLEMENTED",
                    std::string(classMethod ? "+[" : "-[") + className + " " + method + "]");
    };
    if (classMethod) {
        if (method == "alloc") {
            state.registers[0] = makeObject(className);
            return;
        }
        if (method == "new") {
            const uint32_t object = makeObject(className);
            if (className == "NSRecursiveLock") ensureRecursiveLock(objects.at(object));
            state.registers[0] = object;
            return;
        }
        if (className == "AVAudioSession" && method == "sharedInstance") {
            if (!avAudioSession) avAudioSession = makeObject("AVAudioSession");
            state.registers[0] = avAudioSession;
            return;
        }
        if (method == "class") {
            return;
        }
        if (method == "description") {
            state.registers[0] = internText(className);
            return;
        }
        if (method == "stringWithFormat:") {
            Arguments args(state, memory, 3);
            const uint32_t object = makeObject("NSString");
            objects[object].text =
                internText(formatGuest(readText(state.registers[2]), args,
                                       [this](uint32_t value) { return describe(value); }));
            state.registers[0] = object;
            return;
        }
        if (className == "NSArray" && method == "array") {
            state.registers[0] = makeObject("NSArray");
            return;
        }
        if (className == "NSMutableArray" &&
            (method == "array" || method == "arrayWithCapacity:")) {
            state.registers[0] = makeObject("NSMutableArray");
            return;
        }
        if (className == "NSDictionary" && method == "dictionary") {
            state.registers[0] = makeObject("NSDictionary");
            return;
        }
        // +dictionaryWithObjectsAndKeys: is (object, key, ...) terminated by nil, so the
        // variadic words are read in pairs until the terminator.
        if ((className == "NSDictionary" || className == "NSMutableDictionary") &&
            (method == "dictionaryWithObjectsAndKeys:" ||
             method == "dictionaryWithObjects:forKeys:")) {
            const uint32_t object = makeObject(
                className == "NSMutableDictionary" ? "NSMutableDictionary" : "NSDictionary");
            Object& created = objects[object];
            if (method == "dictionaryWithObjectsAndKeys:") {
                Arguments args(state, memory, 2);
                for (size_t pair = 0; pair < 4096; pair++) {
                    const uint32_t value = args.pointer();
                    if (!value) break;
                    const uint32_t key = args.pointer();
                    if (!key) throw Error("OBJC_ARGUMENT", "dictionary key is nil");
                    created.pairs.emplace_back(key, value);
                }
            } else {
                const uint32_t values = state.registers[2], keys = state.registers[3];
                if (!values || !keys) throw Error("OBJC_ARGUMENT", "dictionary arrays are nil");
                const Object& valueList = objects.at(values);
                const Object& keyList = objects.at(keys);
                if (valueList.elements.size() != keyList.elements.size())
                    throw Error("OBJC_ARGUMENT", "dictionary arrays differ in length");
                for (size_t index = 0; index < valueList.elements.size(); index++)
                    created.pairs.emplace_back(keyList.elements[index], valueList.elements[index]);
            }
            state.registers[0] = object;
            return;
        }
        if (className == "NSMutableDictionary" &&
            (method == "dictionary" || method == "dictionaryWithCapacity:")) {
            state.registers[0] = makeObject("NSMutableDictionary");
            return;
        }
        if (className == "NSNull" && method == "null") {
            state.registers[0] = makeObject("NSNull");
            return;
        }
        if (className == "NSData" && method == "data") {
            const uint32_t object = makeObject("NSData");
            objects[object].text = internBytes({});
            state.registers[0] = object;
            return;
        }
        if (className == "NSData" && method == "dataWithBytes:length:") {
            const uint32_t address = state.registers[2], length = state.registers[3];
            if (length > 16 * 1024 * 1024) throw Error("OBJC_LIMIT", "NSData length exceeds limit");
            std::vector<uint8_t> copy(length);
            if (length) {
                if (!address) throw Error("OBJC_ARGUMENT", "dataWithBytes:length: needs a buffer");
                memory.read(address, copy.data(), length);
            }
            const uint32_t object = makeObject("NSData");
            objects[object].bytes = copy;
            objects[object].text = internBytes(copy);
            state.registers[0] = object;
            return;
        }
        if (className == "NSArray" && method == "arrayWithObjects:count:") {
            const uint32_t list = state.registers[2], count = state.registers[3];
            if (count > 65536) throw Error("OBJC_LIMIT", "array element count exceeds limit");
            const uint32_t object = makeObject("NSArray");
            auto& elements = objects[object].elements;
            for (uint32_t index = 0; index < count; index++)
                elements.push_back(memory.read32(list + index * 4));
            state.registers[0] = object;
            return;
        }
        if (className == "NSDictionary" && method == "dictionaryWithObjects:forKeys:count:") {
            Arguments args(state, memory, 4);
            const uint32_t objectsAt = state.registers[2], keysAt = state.registers[3];
            const uint32_t count = args.word();
            if (count > 65536) throw Error("OBJC_LIMIT", "dictionary entry count exceeds limit");
            const uint32_t object = makeObject("NSDictionary");
            auto& pairs = objects[object].pairs;
            for (uint32_t index = 0; index < count; index++)
                pairs.emplace_back(memory.read32(keysAt + index * 4),
                                   memory.read32(objectsAt + index * 4));
            state.registers[0] = object;
            return;
        }
        if (className == "NSNumber" &&
            (method == "numberWithInt:" || method == "numberWithInteger:" ||
             method == "numberWithUnsignedInt:" || method == "numberWithLong:")) {
            const uint32_t object = makeObject("NSNumber");
            objects[object].integer = int32_t(state.registers[2]);
            objects[object].real = double(int32_t(state.registers[2]));
            state.registers[0] = object;
            return;
        }
        if (className == "NSNumber" &&
            (method == "numberWithFloat:" || method == "numberWithDouble:")) {
            const uint32_t object = makeObject("NSNumber");
            Arguments args(state, memory, 2);
            const double value = method == "numberWithFloat:" ? double(args.f32()) : args.f64();
            objects[object].real = value;
            objects[object].integer = int64_t(value);
            state.registers[0] = object;
            return;
        }
        if (className == "NSNumber" && method == "numberWithBool:") {
            const uint32_t object = makeObject("NSNumber");
            objects[object].integer = state.registers[2] ? 1 : 0;
            objects[object].real = objects[object].integer;
            state.registers[0] = object;
            return;
        }
        if (className == "NSThread" && method == "isMultiThreaded") {
            state.registers[0] = liveThreads && liveThreads() > 1 ? 1 : 0;
            return;
        }
        if (className == "NSThread" && (method == "currentThread" || method == "mainThread")) {
            if (!currentThreadObject) currentThreadObject = makeObject("NSThread");
            state.registers[0] = currentThreadObject;
            return;
        }
        if (className == "NSThread" && method == "sleepForTimeInterval:") {
            // A real sleep, in short slices so the run's own wall-clock accounting still sees
            // progress. Long sleeps are capped and reported rather than silently shortened.
            Arguments args(state, memory, 2);
            double seconds = args.f64();
            if (!(seconds > 0)) seconds = 0;
            constexpr double Cap = 5.0;
            if (seconds > Cap) {
                emitLog("NSThread: -sleepForTimeInterval: asked for " + std::to_string(seconds) +
                        " s; waiting the " + std::to_string(Cap) + " s cap");
                seconds = Cap;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(int64_t(seconds * 1000));
            while (std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            state.registers[0] = 0;
            return;
        }
        if (className == "NSThread" &&
            (method == "detachNewThreadSelector:toTarget:withObject:" ||
             method == "detachNewThreadSelector:toTarget:withObject:pool:")) {
            // +detachNewThreadSelector: is -initWithTarget:selector:object: followed by -start,
            // so it creates the same real guest thread through the same scheduler hook.
            if (!spawnThread)
                throw Error("OBJC_THREAD", "no guest scheduler is wired for NSThread detach");
            const uint32_t target = state.registers[2];
            const std::string selector = selectorText(state.registers[3]);
            Arguments args(state, memory, 4);
            const uint32_t argument = args.word();
            if (target && !selector.empty()) spawnThread(target, selector, argument);
            state.registers[0] = 0;
            return;
        }
        if (className == "UIApplication" && (method == "sharedApplication" || method == "app")) {
            if (!applicationObject) applicationObject = makeObject("UIApplication");
            state.registers[0] = applicationObject;
            return;
        }
        if (className == "UIAccelerometer" && method == "sharedAccelerometer") {
            if (!accelerometer) accelerometer = makeObject("UIAccelerometer");
            state.registers[0] = accelerometer;
            return;
        }
        if (className == "EAGLContext" && method == "setCurrentContext:") {
            currentEaglContext = state.registers[2];
            state.registers[0] = activateEaglContext() ? 1 : 0;
            return;
        }
        if (className == "EAGLContext" && method == "currentContext") {
            state.registers[0] = currentEaglContext;
            return;
        }
        if (className == "CAEAGLLayer" || className == "CALayer") {
            if (method == "layer") {
                state.registers[0] = makeObject("CAEAGLLayer");
                return;
            }
        }
        if (className == "UIScreen" && method == "mainScreen") {
            if (!mainScreen) mainScreen = makeObject("UIScreen");
            state.registers[0] = mainScreen;
            return;
        }
        if (className == "UIColor") {
            static const std::map<std::string, std::array<float, 4>> colours{
                {"blackColor", {0, 0, 0, 1}},
                {"whiteColor", {1, 1, 1, 1}},
                {"redColor", {1, 0, 0, 1}},
                {"greenColor", {0, 1, 0, 1}},
                {"blueColor", {0, 0, 1, 1}},
                {"yellowColor", {1, 1, 0, 1}},
                {"orangeColor", {1, 0.5f, 0, 1}},
                {"purpleColor", {0.5f, 0, 0.5f, 1}},
                {"grayColor", {0.5f, 0.5f, 0.5f, 1}},
                {"darkGrayColor", {1.f / 3, 1.f / 3, 1.f / 3, 1}},
                {"lightGrayColor", {2.f / 3, 2.f / 3, 2.f / 3, 1}},
                {"clearColor", {0, 0, 0, 0}}};
            if (auto colour = colours.find(method); colour != colours.end()) {
                const uint32_t object = makeObject("UIColor");
                objects[object].real = colour->second[3];
                objects[object].integer = int64_t(floatBits(colour->second[0]) |
                                                  (uint64_t(floatBits(colour->second[1])) << 32));
                state.registers[0] = object;
                return;
            }
            if (method == "colorWithRed:green:blue:alpha:") {
                Arguments args(state, memory, 2);
                const float red = args.f32(), green = args.f32(), blue = args.f32();
                const float alpha = args.f32();
                const uint32_t object = makeObject("UIColor");
                objects[object].real = alpha;
                objects[object].integer =
                    int64_t(floatBits(red) | (uint64_t(floatBits(green)) << 32));
                (void)blue;
                state.registers[0] = object;
                return;
            }
        }
        if (className == "NSBundle" && method == "mainBundle") {
            if (!mainBundle) mainBundle = makeObject("NSBundle");
            state.registers[0] = mainBundle;
            return;
        }
        if (className == "NSUserDefaults" && method == "standardUserDefaults") {
            if (!userDefaults) userDefaults = makeObject("NSUserDefaults");
            state.registers[0] = userDefaults;
            return;
        }
        if (className == "NSString" && method == "stringWithUTF8String:") {
            const uint32_t object = makeObject("NSString");
            objects[object].text = internText(readText(state.registers[2]));
            state.registers[0] = object;
            return;
        }
        if (className == "NSString" && method == "stringWithCString:") {
            const uint32_t object = makeObject("NSString");
            objects[object].text = internText(readText(state.registers[2]));
            state.registers[0] = object;
            return;
        }
        unsupported();
    }
    auto& object = objects[self];
    if (className == "AVAudioSession") {
        auto writeNoError = [&](uint32_t address) {
            if (!address) return;
            if (!memory.permits(address, 4, Write))
                throw Error("OBJC_AUDIO_SESSION", "AVAudioSession error output is not writable",
                            address);
            memory.write32(address, 0);
        };
        auto validCategory = [](const std::string& category) {
            return category == "AVAudioSessionCategoryAmbient" ||
                   category == "AVAudioSessionCategorySoloAmbient" ||
                   category == "AVAudioSessionCategoryPlayback" ||
                   category == "AVAudioSessionCategoryRecord" ||
                   category == "AVAudioSessionCategoryPlayAndRecord" ||
                   category == "AVAudioSessionCategoryMultiRoute";
        };
        auto setCategory = [&](uint32_t categoryObject, uint32_t options, uint32_t errorOut) {
            if (!categoryObject)
                throw Error("OBJC_AUDIO_SESSION", "AVAudioSession category must not be nil");
            const std::string category = stringOf(categoryObject);
            if (!validCategory(category))
                throw Error("OBJC_AUDIO_SESSION",
                            "unsupported AVAudioSession category: " + category);
            constexpr uint32_t KnownOptions = 0x00000001u | 0x00000002u | 0x00000004u |
                                              0x00000008u | 0x00000011u | 0x00000020u | 0x00000040u;
            if (options & ~KnownOptions)
                throw Error("OBJC_AUDIO_SESSION", "unsupported AVAudioSession category options",
                            options);
            object.audioCategory = category;
            object.audioCategoryOptions = options;
            writeNoError(errorOut);
            state.registers[0] = 1;
        };
        if (method == "init") {
            state.registers[0] = self;
            return;
        }
        if (method == "delegate") {
            state.registers[0] = object.audioDelegate;
            return;
        }
        if (method == "setDelegate:") {
            object.audioDelegate = state.registers[2];
            return;
        }
        if (method == "setCategory:") {
            setCategory(state.registers[2], 0, 0);
            return;
        }
        if (method == "setCategory:error:") {
            setCategory(state.registers[2], 0, state.registers[3]);
            return;
        }
        if (method == "setCategory:withOptions:") {
            setCategory(state.registers[2], state.registers[3], 0);
            return;
        }
        if (method == "setCategory:withOptions:error:") {
            Arguments args(state, memory, 4);
            setCategory(state.registers[2], state.registers[3], args.word());
            return;
        }
        if (method == "setActive:error:") {
            object.audioSessionActive = state.registers[2] != 0;
            writeNoError(state.registers[3]);
            state.registers[0] = 1;
            return;
        }
        if (method == "setActive:withOptions:error:") {
            Arguments args(state, memory, 4);
            const uint32_t options = state.registers[3];
            if (options & ~uint32_t(3))
                throw Error("OBJC_AUDIO_SESSION", "unsupported AVAudioSession activation options",
                            options);
            object.audioSessionActive = state.registers[2] != 0;
            writeNoError(args.word());
            state.registers[0] = 1;
            return;
        }
        if (method == "isOtherAudioPlaying" || method == "inputAvailable") {
            state.registers[0] = 0;  // The emulator has no external audio session or input device.
            return;
        }
        if (method == "category") {
            state.registers[0] =
                object.audioCategory.empty() ? 0 : makeString(object.audioCategory);
            return;
        }
        if (method == "mode") {
            state.registers[0] = object.audioMode.empty() ? 0 : makeString(object.audioMode);
            return;
        }
        if (method == "setMode:error:") {
            const uint32_t modeObject = state.registers[2];
            if (!modeObject)
                throw Error("OBJC_AUDIO_SESSION", "AVAudioSession mode must not be nil");
            const std::string mode = stringOf(modeObject);
            if (mode != "AVAudioSessionModeDefault" && mode != "AVAudioSessionModeVoiceChat" &&
                mode != "AVAudioSessionModeVideoChat" && mode != "AVAudioSessionModeGameChat" &&
                mode != "AVAudioSessionModeMeasurement" &&
                mode != "AVAudioSessionModeMoviePlayback" &&
                mode != "AVAudioSessionModeSpokenAudio")
                throw Error("OBJC_AUDIO_SESSION", "unsupported AVAudioSession mode: " + mode);
            object.audioMode = mode;
            writeNoError(state.registers[3]);
            state.registers[0] = 1;
            return;
        }
        if (method == "recordPermission") {
            state.registers[0] = 0;  // AVAudioSessionRecordPermissionUndetermined
            return;
        }
        if (method == "preferredSampleRate") {
            resultF64(state, object.preferredSampleRate);
            return;
        }
        if (method == "setPreferredSampleRate:error:") {
            Arguments args(state, memory, 2);
            const double rate = args.f64();
            const uint32_t errorOut = args.word();
            if (!std::isfinite(rate) || rate <= 0 || rate > 384000)
                throw Error("OBJC_AUDIO_SESSION", "preferred sample rate is out of range");
            object.preferredSampleRate = rate;
            writeNoError(errorOut);
            state.registers[0] = 1;
            return;
        }
        if (method == "preferredIOBufferDuration") {
            resultF64(state, object.preferredIOBufferDuration);
            return;
        }
        if (method == "setPreferredIOBufferDuration:error:") {
            Arguments args(state, memory, 2);
            const double duration = args.f64();
            const uint32_t errorOut = args.word();
            if (!std::isfinite(duration) || duration <= 0 || duration > 10)
                throw Error("OBJC_AUDIO_SESSION", "preferred IO buffer duration is out of range");
            object.preferredIOBufferDuration = duration;
            writeNoError(errorOut);
            state.registers[0] = 1;
            return;
        }
        if (method == "description") {
            state.registers[0] =
                makeString("<AVAudioSession category=" + object.audioCategory +
                           (object.audioSessionActive ? " active>" : " inactive>"));
            return;
        }
        unsupported();
    }
    if (className == "NSRecursiveLock") {
        if (method == "init") {
            ensureRecursiveLock(object);
            state.registers[0] = self;
            return;
        }
        if (method == "lock" || method == "tryLock" || method == "unlock") {
            ensureRecursiveLock(object);
            if (method == "lock") {
                if (!lockRecursiveMutex || lockRecursiveMutex(object.recursiveMutex) != 0)
                    throw Error("OBJC_LOCK", "NSRecursiveLock lock failed", self);
                state.registers[0] = 0;
                return;
            }
            if (method == "tryLock") {
                if (!tryLockRecursiveMutex)
                    throw Error("OBJC_LOCK", "NSRecursiveLock tryLock is not connected");
                const int result = tryLockRecursiveMutex(object.recursiveMutex);
                if (result != 0 && result != GuestPosixBusy)
                    throw Error("OBJC_LOCK", "NSRecursiveLock tryLock failed", uint32_t(result));
                state.registers[0] = result == 0 ? 1 : 0;
                return;
            }
            if (!unlockRecursiveMutex || unlockRecursiveMutex(object.recursiveMutex) != 0)
                throw Error("OBJC_LOCK", "NSRecursiveLock unlock by a non-owner", self);
            state.registers[0] = 0;
            return;
        }
    }
    if (className == "NSAutoreleasePool" && method == "init") {
        pools.emplace_back();
        state.registers[0] = self;
        return;
    }
    if (method == "init" || method == "autorelease" || method == "retain") {
        if (method == "autorelease") {
            if (pools.empty()) throw Error("OBJC_POOL", "autorelease without a pool");
            pools.back().push_back(self);
        }
        if (method == "retain") retain(self);
        state.registers[0] = self;
        return;
    }
    if (method == "release" || method == "dealloc" || method == "drain") {
        if (className == "NSAutoreleasePool" && !pools.empty()) {
            std::vector<uint32_t> owned = pools.back();
            pools.pop_back();
            for (uint32_t released : owned)
                if (objects.count(released) || guestInstances.count(released)) release(released);
        }
        release(self);
        state.registers[0] = self;
        return;
    }
    if (method == "retainCount") {
        state.registers[0] = object.references;
        return;
    }
    if (method == "class" || method == "self") {
        state.registers[0] = classNamePointer(object.className);
        return;
    }
    if (method == "isKindOfClass:") {
        state.registers[0] = classNamePointer(object.className) == state.registers[2] ||
                             object.className == "NSString";
        return;
    }
    if (method == "respondsToSelector:") {
        state.registers[0] = 0;  // Conservatively false until a method table exists.
        return;
    }
    if (uiKitMessage(className, self, method, state)) return;
    if (className == "NSBundle" && bundleMessage(method, state)) return;
    if (collection(className, method, object, state)) return;
    if (className == "NSString" || className == "NSMutableString" ||
        className == "NSConstantString" || className == "NSCFString") {
        if (stringMessage(readText(object.text), object.text, method, state)) return;
    }
    if (className == "NSNumber") {
        if (method == "intValue" || method == "integerValue" || method == "unsignedIntValue" ||
            method == "longValue") {
            state.registers[0] = uint32_t(object.integer);
            return;
        }
        if (method == "boolValue") {
            state.registers[0] = object.integer ? 1 : 0;
            return;
        }
        if (method == "floatValue") {
            resultF32(state, float(object.real));
            return;
        }
        if (method == "doubleValue") {
            resultF64(state, object.real);
            return;
        }
    }
    unsupported();
}
void ObjC::invoke(const std::string& entry, CpuState& state) {
    auto& r = state.registers;
    if (entry == "_objc_msgSend" || entry == "_objc_alloc" || entry == "_objc_alloc_init") {
        if (entry == "_objc_alloc" || entry == "_objc_alloc_init") {
            if (guestClassAt(r[0])) {
                r[0] = allocateGuestInstance(r[0]);
                return;
            }
            const std::string name = classNameOf(r[0]);
            const std::string className = name.rfind("class ", 0) == 0 ? name.substr(6) : name;
            r[0] = makeObject(className);
            if (entry == "_objc_alloc_init" && className == "NSRecursiveLock")
                ensureRecursiveLock(objects.at(r[0]));
            return;
        }
        message(state);
        return;
    }
    if (entry == "_objc_msgSend_stret" || entry == "_objc_msgSendSuper2_stret") {
        // A large struct result is returned through a hidden pointer in r0, so self and
        // _cmd move to r1/r2. Only the scalar-returning classes are modelled, so a struct
        // request from anywhere else is refused by name rather than half-written.
        const uint32_t receiver = entry == "_objc_msgSend_stret" ? r[1] : memory.read32(r[0]);
        const uint32_t name = entry == "_objc_msgSend_stret" ? r[2] : r[1];
        if (!receiver) {
            r[0] = 0;
            return;
        }
        const std::string target = classNameOf(receiver);
        const std::string selectorName = selector(name);
        const bool scalarClass = target.rfind("class ", 0) == 0 || target == "NSString" ||
                                 target == "NSNumber" || target == "NSArray" ||
                                 target == "NSMutableArray" || target == "NSDictionary" ||
                                 target == "NSMutableDictionary" || target == "NSData";
        if (!scalarClass) {
            // A host-modelled object (UIView's -frame, UIScreen's -bounds) returns its
            // CGRect through r0's hidden pointer, so the argument registers move down one
            // place. The handler answers in r0-r3; store those four words and hand the
            // buffer back as the result.
            const uint32_t buffer = r[0];
            if (!buffer || !memory.permits(buffer, 16, Write))
                throw Error("OBJC_STRET", "struct return buffer is not writable", buffer);
            CpuState shifted = state;
            for (size_t n = 0; n + 3 < 16; n++) shifted.registers[2 + n] = r[3 + n];
            shifted.registers[0] = receiver;
            shifted.registers[1] = name;
            message(shifted);
            for (size_t n = 0; n < 4; n++)
                memory.write32(buffer + uint32_t(n * 4), shifted.registers[n]);
            r[0] = buffer;
            return;
        }
        CpuState shifted = state;
        shifted.registers[0] = receiver;
        shifted.registers[1] = name;
        shifted.registers[2] = r[3];
        shifted.registers[3] = 0;
        message(shifted);
        r[0] = shifted.registers[0];
        r[1] = shifted.registers[1];
        return;
    }
    if (entry == "_objc_msgSendSuper2") {
        // struct objc_super { id receiver; Class classOfMethod; } arrives in r0; the selector
        // follows in r1. Lookup must begin at that class's *superclass*, which is what makes
        // `[super initWithFrame:]` in an image-defined subclass reach UIView's implementation
        // instead of re-entering the subclass.
        const uint32_t super2 = r[0];
        if (!super2) throw Error("OBJC_SUPER", "msgSendSuper2 called with a null objc_super");
        const uint32_t receiver = memory.read32(super2);
        const uint32_t classOfMethod = memory.read32(super2 + 4);
        if ((guestInstances.count(receiver) || guestClassAt(receiver)) &&
            guestSuperMessage(receiver, classOfMethod, r[1], state))
            return;
        CpuState shifted = state;
        shifted.registers[0] = receiver;
        shifted.registers[1] = r[1];
        message(shifted);
        r[0] = shifted.registers[0];
        r[1] = shifted.registers[1];
        return;
    }
    if (entry == "_objc_getClass" || entry == "_objc_lookUpClass") {
        r[0] = resolveClass(readText(r[0]), entry == "_objc_getClass");
        return;
    }
    if (entry == "_sel_registerName") return;  // A selector is its guest string address.
    if (entry == "_sel_getName" || entry == "_NSSelectorFromString") return;
    if (entry == "_objc_setProperty" || entry == "_objc_setProperty_atomic") {
        // setProperty(self, _cmd, offset, newValue, atomic, propertyInfo) stores the value
        // into the ivar at self + offset, exactly like the runtime does.
        const uint32_t object = r[0], offset = r[2], value = r[3];
        if (!object) return;
        if (!memory.permits(object + offset, 4, Write))
            throw Error("OBJC_PROPERTY", "objc_setProperty ivar is not writable", object + offset);
        memory.write32(object + offset, value);
        return;
    }
    if (entry == "_objc_getProperty") {
        const uint32_t object = r[0], offset = r[2];
        r[0] = object ? memory.read32(object + offset) : 0;
        return;
    }
    if (entry == "_objc_copyStruct") {
        const uint32_t destination = r[0], source = r[1], size = r[2];
        if (size > 16 * 1024 * 1024) throw Error("OBJC_LIMIT", "objc_copyStruct exceeds budget");
        std::vector<uint8_t> bytes(size);
        if (size) {
            memory.read(source, bytes.data(), size);
            memory.write(destination, bytes.data(), size);
        }
        return;
    }
    if (entry == "_objc_sync_enter" || entry == "_objc_sync_exit") {
        // Single guest thread, so mutual exclusion is trivially satisfied; the balanced
        // enter/exit pair is tracked so a mismatch is still reported.
        const uint32_t object = r[0];
        auto found = syncDepth.find(object);
        if (entry == "_objc_sync_enter") {
            syncDepth[object] = found == syncDepth.end() ? 1 : found->second + 1;
            r[0] = 0;
            return;
        }
        if (found == syncDepth.end() || !found->second)
            throw Error("OBJC_SYNC", "@synchronized exit without a matching enter");
        if (--found->second == 0) syncDepth.erase(found);
        r[0] = 0;
        return;
    }
    if (entry == "_objc_enumerationMutation") {
        // The runtime calls this only when a collection is mutated during fast enumeration.
        throw Error("OBJC_ENUMERATION", "a collection was mutated while being enumerated", r[0]);
    }
    if (entry == "_objc_autoreleasePoolPush") {
        pools.emplace_back();
        r[0] = uint32_t(pools.size());
        return;
    }
    if (entry == "_objc_autoreleasePoolPop") {
        if (!pools.empty()) {
            std::vector<uint32_t> owned = pools.back();
            pools.pop_back();
            for (uint32_t released : owned)
                if (objects.count(released) || guestInstances.count(released)) release(released);
        }
        return;
    }
    if (entry == "_objc_retainBlock") return;  // Blocks are not modelled; the id is unchanged.
    if (entry == "_NSClassFromString") {
        const uint32_t name = r[0];
        if (!name) {
            r[0] = 0;
            return;
        }
        r[0] = resolveClass(readText(name), false);
        return;
    }
    if (entry == "_NSStringFromClass") {
        if (!r[0]) {
            r[0] = 0;
            return;
        }
        std::string name = classNameOf(r[0]);
        if (name.rfind("class ", 0) == 0) name.erase(0, 6);
        r[0] = makeString(name);
        return;
    }
    if (entry == "_NSStringFromSelector") {
        r[0] = r[0] ? makeString(selectorText(r[0])) : 0;
        return;
    }
    if (entry == "_NSLog") {
        Arguments args(state, memory, 1);
        emitLog(
            formatGuest(readText(r[0]), args, [this](uint32_t value) { return describe(value); }));
        return;
    }
    if (entry == "_NSHomeDirectory") {
        r[0] = makeString(bundlePath);
        return;
    }
    if (entry == "_NSSearchPathForDirectoriesInDomains") {
        // (directory, domain, expandTilde). Everything this sandbox offers lives under the
        // container, so Documents/Library/Caches all resolve there rather than pretending
        // to have a full iOS home directory layout.
        r[0] = makeArray({makeString(documentsPath)});
        return;
    }
    if (entry == "_objc_retain" || entry == "_objc_retainAutoreleasedReturnValue") {
        if (r[0] && (objects.count(r[0]) || guestInstances.count(r[0]))) retain(r[0]);
        return;
    }
    if (entry == "_objc_release") {
        if (r[0] && (objects.count(r[0]) || guestInstances.count(r[0]))) release(r[0]);
        return;
    }
    if (entry == "_objc_autoreleaseReturnValue") {
        if (r[0] && (objects.count(r[0]) || guestInstances.count(r[0]))) {
            if (pools.empty()) throw Error("OBJC_POOL", "autorelease without a pool");
            pools.back().push_back(r[0]);
        }
        return;
    }
    throw Error("OBJC_UNIMPLEMENTED", "runtime entry " + entry);
}
}  // namespace podnative::runtime
