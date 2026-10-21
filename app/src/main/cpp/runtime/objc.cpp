#include "objc.h"

#include <algorithm>

#include "abi.h"
#include "error.h"
#include "format.h"
#include "libc.h"

namespace podnative::runtime {
namespace {
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
    classObject("NSURL");
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
    if (text.size() > 4096 || nextText + text.size() + 1 > 0x6f480000 + Memory::PageSize * 4)
        throw Error("OBJC_LIMIT", "guest string pool exhausted");
    const uint32_t address = nextText;
    memory.write(address, text.data(), text.size());
    const char terminator = 0;
    memory.write(address + uint32_t(text.size()), &terminator, 1);
    nextText += uint32_t(text.size()) + 1;
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
uint32_t ObjC::makeArray(const std::vector<uint32_t>& elements) {
    const uint32_t object = makeObject("NSArray");
    objects[object].elements = elements;
    return object;
}
void ObjC::setGuestPaths(std::string bundle, std::string documents) {
    bundlePath = std::move(bundle);
    documentsPath = std::move(documents);
}
void ObjC::drainPools() {
    while (!pools.empty()) {
        std::vector<uint32_t> owned = pools.back();
        pools.pop_back();
        for (uint32_t released : owned)
            if (objects.count(released)) release(released);
    }
}
void ObjC::emitLog(const std::string& text) const {
    if (logSink) {
        logSink(text);
        return;
    }
    guestLog(text);
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
        if (type) path += "." + readText(type);
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
void ObjC::release(uint32_t object) {
    auto found = objects.find(object);
    if (found == objects.end()) throw Error("OBJC_RELEASE", "release of a non-object", object);
    if (found->second.references > 1) {
        found->second.references--;
        return;
    }
    objects.erase(found);
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
        if (object == objects.end()) throw Error("OBJC_RECEIVER", "message to a non-object", self);
        className = object->second.className;
    }
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
            state.registers[0] = object;
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
        if (method == "retain" && object.references - 1 < UINT32_MAX) object.references++;
        state.registers[0] = self;
        return;
    }
    if (method == "release" || method == "dealloc" || method == "drain") {
        if (className == "NSAutoreleasePool" && !pools.empty()) {
            std::vector<uint32_t> owned = pools.back();
            pools.pop_back();
            for (uint32_t released : owned)
                if (objects.count(released)) release(released);
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
    if (className == "NSBundle" && bundleMessage(method, state)) return;
    if (collection(className, method, object, state)) return;
    if (className == "NSString") {
        const std::string text = readText(object.text);
        if (method == "length") {
            state.registers[0] = uint32_t(text.size());
            return;
        }
        if (method == "UTF8String" || method == "cString") {
            state.registers[0] = object.text;
            return;
        }
        if (method == "hash") {
            uint32_t hash = 5381;
            for (unsigned char c : text) hash = hash * 33 + c;
            state.registers[0] = hash;
            return;
        }
        if (method == "isEqualToString:" || method == "compare:") {
            auto other = objects.find(state.registers[2]);
            if (other == objects.end()) unsupported();
            const std::string rhs = readText(other->second.text);
            state.registers[0] =
                method == "isEqualToString:" ? uint32_t(text == rhs) : uint32_t(text.compare(rhs));
            return;
        }
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
        if (entry == "_objc_alloc") {
            const std::string name = classNameOf(r[0]);
            r[0] = makeObject(name.rfind("class ", 0) == 0 ? name.substr(6) : name);
            return;
        }
        if (entry == "_objc_alloc_init") {
            const std::string name = classNameOf(r[0]);
            r[0] = makeObject(name.rfind("class ", 0) == 0 ? name.substr(6) : name);
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
        if (!scalarClass)
            throw Error("OBJC_STRET_UNIMPLEMENTED",
                        "[" + target + " " + selectorName +
                            "] returns a struct through a hidden pointer, which is not modelled");
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
        // struct objc_super { id receiver; Class superClass; } arrives in r0; the selector
        // follows in r1. Single-inheritance dispatch is the same method lookup for us.
        const uint32_t super2 = r[0];
        if (!super2) throw Error("OBJC_SUPER", "msgSendSuper2 called with a null objc_super");
        CpuState shifted = state;
        shifted.registers[0] = memory.read32(super2);
        shifted.registers[1] = r[1];
        message(shifted);
        r[0] = shifted.registers[0];
        r[1] = shifted.registers[1];
        return;
    }
    if (entry == "_objc_getClass" || entry == "_objc_lookUpClass") {
        const std::string name = readText(r[0]);
        auto found = classes.find(name);
        r[0] = found == classes.end() ? 0 : found->second;
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
                if (objects.count(released)) release(released);
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
        const std::string text = readText(name);
        auto found = classes.find(text);
        r[0] = found == classes.end() ? 0 : found->second;
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
        auto found = objects.find(r[0]);
        if (found != objects.end() && found->second.references < UINT32_MAX)
            found->second.references++;
        return;
    }
    if (entry == "_objc_release") {
        if (r[0] && objects.count(r[0])) release(r[0]);
        return;
    }
    if (entry == "_objc_autoreleaseReturnValue") {
        if (r[0] && objects.count(r[0])) {
            if (pools.empty()) throw Error("OBJC_POOL", "autorelease without a pool");
            pools.back().push_back(r[0]);
        }
        return;
    }
    throw Error("OBJC_UNIMPLEMENTED", "runtime entry " + entry);
}
}  // namespace podnative::runtime
