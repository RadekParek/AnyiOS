#include "objc.h"

#include <algorithm>

#include "error.h"

namespace podnative::runtime {
namespace {
const std::vector<std::string>& entryNames() {
    static const std::vector<std::string> names{"_objc_msgSend",
                                                "_objc_getClass",
                                                "_sel_registerName",
                                                "_objc_retain",
                                                "_objc_release",
                                                "_objc_autoreleaseReturnValue",
                                                "_objc_retainAutoreleasedReturnValue",
                                                "_objc_alloc",
                                                "_objc_alloc_init"};
    return names;
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
    classObject("NSObject");
    classObject("NSString");
    classObject("NSAutoreleasePool");
    classObject("NSNumber");
}
void ObjC::defineSymbols(ExportGraph& graph, size_t foundation) const {
    for (size_t n = 0; n < entries.size(); n++)
        graph.define(foundation, entries[n], {GateBase + uint32_t(n) * 8});
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
uint32_t ObjC::makeObject(const std::string& className) {
    if (objects.size() >= 4096 || nextObject + 32 > 0x6f400000 + Memory::PageSize * 4)
        throw Error("OBJC_LIMIT", "guest object pool exhausted");
    const uint32_t address = nextObject;
    nextObject += 32;
    memory.write32(address, classNamePointer(className));
    memory.write32(address + 4, 0);
    objects.emplace(address, Object{className});
    return address;
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
void ObjC::release(uint32_t object) {
    auto found = objects.find(object);
    if (found == objects.end()) throw Error("OBJC_RELEASE", "release of a non-object", object);
    if (found->second.references > 1) {
        found->second.references--;
        return;
    }
    objects.erase(found);
}
void ObjC::message(CpuState& state) {
    const uint32_t self = state.registers[0], name = state.registers[1];
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
    if (className == "NSNumber" && method == "intValue") {
        state.registers[0] = object.text;
        return;
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
    if (entry == "_objc_getClass") {
        const std::string name = readText(r[0]);
        auto found = classes.find(name);
        r[0] = found == classes.end() ? 0 : found->second;
        return;
    }
    if (entry == "_sel_registerName") return;  // A selector is its guest string address.
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
