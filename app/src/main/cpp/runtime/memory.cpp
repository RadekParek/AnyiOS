#include "memory.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

#include "error.h"
#if defined(__unix__) || defined(__APPLE__)
#define PODNATIVE_MEMORY_ARENA 1
#include <sys/mman.h>
#include <unistd.h>
#endif
namespace podnative::runtime {
namespace {
uint64_t endOf(uint32_t base, size_t size) {
    if (size > uint64_t(0x100000000ULL) - base)
        throw Error("MEMORY_OVERFLOW", "range exceeds ARM32 address space", base);
    return uint64_t(base) + size;
}
constexpr size_t ArenaBytes = size_t(0x100000000ULL);
#ifdef PODNATIVE_MEMORY_ARENA
size_t hostPageSize() {
    const long size = ::sysconf(_SC_PAGESIZE);
    // Hosts with pages larger than the guest's are supported, but then one host page can
    // hold guest pages with different permissions, so protections stop being exact.
    return size > 0 ? size_t(size) : 0;
}
int protectionOf(uint8_t permissions) {
    int protection = PROT_NONE;
    if (permissions & Read) protection |= PROT_READ;
    if (permissions & Write) protection |= PROT_WRITE;
    // Deliberately no PROT_EXEC: guest execute permission is metadata for the loader and
    // the fetch path. Translated guest code lives in the CPU engine's own W^X mapping.
    return protection;
}
// mprotect works in host pages, which are the guest's 4 KiB on most hosts and can be
// larger (16 KiB page Android devices). Round outward so a guest-sized request is always
// expressible; the caller restores the real union protection afterwards.
void protectHost(uint8_t* arena, uint32_t base, size_t size, int protection) {
    const uint64_t host = hostPageSize();
    const uint64_t start = base & ~(host - 1);
    const uint64_t end = (uint64_t(base) + size + host - 1) & ~(host - 1);
    ::mprotect(arena + start, size_t(end - start), protection);
}
#endif
}  // namespace
Memory::~Memory() {
#ifdef PODNATIVE_MEMORY_ARENA
    if (arena_) ::munmap(arena_, arenaSize_);
#endif
}
bool Memory::enableDirectAccess() {
    if (arena_) return true;
#ifdef PODNATIVE_MEMORY_ARENA
    const size_t host = hostPageSize();
    // A host page smaller than a guest page cannot express guest protections at all.
    if (!host || host < PageSize) return false;
    void* reservation = ::mmap(nullptr, ArenaBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
        // Kernels without anonymous overcommit refuse a plain 4 GiB reservation; ask for
        // the address space without reserving swap-backed pages for it.
#ifdef MAP_NORESERVE
        reservation = ::mmap(nullptr, ArenaBytes, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
    }
    if (reservation == MAP_FAILED) return false;
    arena_ = static_cast<uint8_t*>(reservation);
    arenaSize_ = ArenaBytes;
    exactProtection_ = host == PageSize;
    pageTable_ = std::make_unique<PageTable>();
    pageTable_->fill(nullptr);
    if (!storage_.empty()) {
        // Migrate the already-mapped image: writable host pages first, copy, then restore
        // each region's real protection.
        uint32_t lowest = storage_.begin()->first;
        uint32_t highest = 0;
        for (const auto& entry : storage_) {
            const uint32_t end = entry.second.region.base + entry.second.region.size;
            lowest = std::min(lowest, entry.second.region.base);
            highest = std::max(highest, end);
        }
        protectHost(arena_, lowest, size_t(highest) - lowest, PROT_READ | PROT_WRITE);
        for (auto& entry : storage_) {
            const uint32_t base = entry.second.region.base;
            const size_t size = entry.second.region.size;
            if (!entry.second.bytes.empty()) {
                std::memcpy(arena_ + base, entry.second.bytes.data(), size);
                // The arena is now the only copy: release the region vector's pages.
                std::vector<uint8_t>().swap(entry.second.bytes);
            }
            publishPage(entry.second);
        }
        for (const auto& entry : storage_)
            applyArenaProtection(entry.second.region.base, entry.second.region.size);
    }
    return true;
#else
    return false;
#endif
}
void Memory::applyArenaProtection(uint32_t base, uint32_t size) {
#ifdef PODNATIVE_MEMORY_ARENA
    if (!arena_) return;
    const uint64_t host = hostPageSize();
    const uint64_t start = base & ~(host - 1);
    const uint64_t end = (uint64_t(base) + size + host - 1) & ~(host - 1);
    // Collect the regions that can touch these host pages once, instead of rescanning the
    // whole map for every page of a large mapping.
    std::vector<const Storage*> overlapping;
    auto it = storage_.upper_bound(uint32_t(start));
    if (it != storage_.begin()) --it;
    for (; it != storage_.end() && it->second.region.base < end; ++it) {
        if (uint64_t(it->second.region.base) + it->second.region.size <= start) continue;
        overlapping.push_back(&it->second);
    }
    // Protect whole host pages with the union of the guest regions inside them. With
    // 4 KiB host pages that union is exactly one guest region, so protections are exact;
    // with larger host pages they are not, and the CPU engine is told so.
    for (uint64_t page = start; page < end; page += host) {
        int protection = PROT_NONE;
        for (const Storage* storage : overlapping) {
            const uint64_t regionBegin = storage->region.base;
            const uint64_t regionEnd = regionBegin + storage->region.size;
            if (regionEnd <= page || regionBegin >= page + host) continue;
            protection |= protectionOf(storage->region.permissions);
        }
        ::mprotect(arena_ + page, size_t(host), protection);
    }
#else
    (void)base;
    (void)size;
#endif
}
void Memory::publishPage(const Storage& storage) {
    if (!arena_ || !pageTable_) return;
    // Only writable guest pages get a fast-path entry. A store into a read-only page and
    // any access to an unmapped page must keep reaching the checked callback path, which
    // is what reports MEMORY_ACCESS and MEMORY_UNMAPPED instead of silently succeeding.
    if ((storage.region.permissions & Write) == 0) return;
    const uint32_t pages = storage.region.size / PageSize;
    for (uint32_t index = 0; index < pages; index++)
        (*pageTable_)[(storage.region.base / PageSize) + index] = arena_;
}
void Memory::map(Region region, const uint8_t* initial, size_t initialSize) {
    const uint64_t end = endOf(region.base, region.size);
    if (!region.size || region.base % PageSize || region.size % PageSize)
        throw Error("MEMORY_ALIGNMENT", "mapping must contain aligned, whole pages", region.base);
    if ((region.permissions & ~uint8_t(7)) || !region.permissions)
        throw Error("MEMORY_PERMISSION", "invalid mapping permissions", region.base);
    if (initialSize > region.size || (initialSize && !initial))
        throw Error("MEMORY_INITIAL_DATA", "invalid initialization range", region.base);
    auto next = storage_.lower_bound(region.base);
    if ((next != storage_.end() && next->first < end) ||
        (next != storage_.begin() &&
         uint64_t(std::prev(next)->first) + std::prev(next)->second.region.size > region.base))
        throw Error("MEMORY_OVERLAP", "mapping overlaps an existing region", region.base);
    if (region.size > budget_ - allocated_ || storage_.size() >= 1024)
        throw Error("MEMORY_BUDGET", "guest allocation/region budget exceeded", region.base);
    Storage storage;
    storage.region = region;
    const auto size = storage.region.size;
    if (arena_) {
#ifdef PODNATIVE_MEMORY_ARENA
        if (initialSize) {
            // Anonymous arena pages start unwritable; open them, fill them, then restore
            // the protection this region actually declares.
            protectHost(arena_, region.base, size, PROT_READ | PROT_WRITE);
            std::memcpy(arena_ + region.base, initial, initialSize);
        }
#endif
    } else {
        storage.bytes.assign(size, 0);
        if (initialSize) std::memcpy(storage.bytes.data(), initial, initialSize);
    }
    const auto inserted = storage_.emplace(storage.region.base, std::move(storage));
    allocated_ += size;
    ++revision_;
    if (arena_) {
        publishPage(inserted.first->second);
        applyArenaProtection(region.base, size);
    }
    if ((region.permissions & uint8_t(Write | Execute)) == uint8_t(Write | Execute))
        writableExecutable_ = true;
    if (region.permissions & Execute) noteCodeEdit(region.base, size);
}
const Memory::Storage& Memory::at(uint32_t address, uint8_t permission) const {
    // A null access is reported as such: the guest dereferenced a pointer that a missing
    // object or a nil message result left at zero. Naming it removes the guesswork a plain
    // "unmapped at 0x0" leaves behind.
    auto unmapped = [&](uint32_t fault) -> const Storage& {
        if (fault < PageSize)
            throw Error("MEMORY_NULL",
                        "null pointer dereference: the guest read or wrote address " +
                            std::to_string(fault) +
                            ", which no object, allocation or mapped image region owns",
                        fault);
        throw Error("MEMORY_UNMAPPED", "unmapped guest access", fault);
    };
    auto it = storage_.upper_bound(address);
    if (it == storage_.begin()) unmapped(address);
    --it;
    if (uint64_t(address) >= uint64_t(it->first) + it->second.region.size) unmapped(address);
    if ((it->second.region.permissions & permission) != permission)
        throw Error("MEMORY_ACCESS", "guest permission violation", address);
    return it->second;
}
uint8_t* Memory::hostAt(const Storage& storage, uint32_t address) const {
    if (arena_) return arena_ + address;
    return const_cast<uint8_t*>(storage.bytes.data()) + (address - storage.region.base);
}
void Memory::validate(uint32_t address, size_t size, uint8_t permission) const {
    const uint64_t end = endOf(address, size);
    uint64_t cursor = address;
    while (cursor < end) {
        const auto& s = at(uint32_t(cursor), permission);
        cursor = std::min(end, uint64_t(s.region.base) + s.region.size);
    }
}
bool Memory::permits(uint32_t address, size_t size, uint8_t permission) const {
    try {
        validate(address, size, permission);
        return true;
    } catch (const Error&) {
        return false;
    }
}
void Memory::noteCodeEdit(uint32_t address, size_t size) {
    // Translated code for these guest bytes is stale now. The range is remembered so the
    // CPU engine can invalidate exactly it instead of everything it has compiled.
    ++codeRevision_;
    const uint64_t end = endOf(address, size);
    if (!codeEdits_.empty()) {
        Range& last = codeEdits_.back();
        if (address <= last.end && last.begin <= end) {
            last.begin = std::min(last.begin, address);
            last.end = uint32_t(std::max<uint64_t>(last.end, end));
            return;
        }
    }
    codeEdits_.push_back(Range{address, uint32_t(end)});
}
std::vector<Range> Memory::takeCodeEdits() {
    std::vector<Range> edits;
    edits.swap(codeEdits_);
    if (edits.size() > 1) {
        std::sort(edits.begin(), edits.end(),
                  [](const Range& a, const Range& b) { return a.begin < b.begin; });
        std::vector<Range> merged;
        for (const Range& range : edits) {
            if (!merged.empty() && range.begin <= merged.back().end) {
                merged.back().end = std::max(merged.back().end, range.end);
                continue;
            }
            merged.push_back(range);
        }
        edits.swap(merged);
    }
    return edits;
}
void Memory::copyOut(uint32_t address, void* destination, size_t size, uint8_t permission) const {
    if (size && !destination) throw Error("MEMORY_ARGUMENT", "null destination", address);
    validate(address, size, permission);
    auto* out = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        const auto& s = at(current, permission);
        const size_t count = std::min(size - done, size_t(s.region.base) + s.region.size - current);
        std::memcpy(out + done, hostAt(s, current), count);
        done += count;
    }
}
void Memory::read(uint32_t address, void* destination, size_t size) const {
    copyOut(address, destination, size, Read);
}
void Memory::write(uint32_t address, const void* source, size_t size) {
    if (size && !source) throw Error("MEMORY_ARGUMENT", "null source", address);
    // Prevalidate every byte range before mutating; faults must not partially update a guest call.
    validate(address, size, Write);
    const auto* input = static_cast<const uint8_t*>(source);
    size_t done = 0;
    bool touchedCode = false;
    uint32_t codeBegin = 0, codeEnd = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        auto it = std::prev(storage_.upper_bound(current));
        const size_t count = std::min(
            size - done, size_t(it->second.region.base) + it->second.region.size - current);
        std::memcpy(hostAt(it->second, current), input + done, count);
        if (it->second.region.permissions & Execute) {
            if (!touchedCode) {
                touchedCode = true;
                codeBegin = current;
            }
            codeEnd = uint32_t(uint64_t(current) + count);
        }
        done += count;
    }
    if (size) ++revision_;
    if (touchedCode) noteCodeEdit(codeBegin, size_t(codeEnd) - codeBegin);
}
void Memory::patch(uint32_t address, const void* source, size_t size) {
    if (size && !source) throw Error("MEMORY_ARGUMENT", "null patch source", address);
    validate(address, size, Read);
    const auto* input = static_cast<const uint8_t*>(source);
    size_t done = 0;
    while (done < size) {
        const uint32_t current = uint32_t(uint64_t(address) + done);
        auto it = std::prev(storage_.upper_bound(current));
        const size_t count = std::min(
            size - done, size_t(it->second.region.base) + it->second.region.size - current);
#ifdef PODNATIVE_MEMORY_ARENA
        // A loader patch of a read-only (usually executable) region has to open the host
        // pages it touches and no others: an image binds hundreds of stubs, and reopening
        // the whole segment per stub would turn startup into a wall of syscalls.
        const bool reopen = arena_ && (it->second.region.permissions & Write) == 0;
        if (reopen) protectHost(arena_, current, count, PROT_READ | PROT_WRITE);
        std::memcpy(hostAt(it->second, current), input + done, count);
        if (reopen) applyArenaProtection(current, uint32_t(count));
#else
        std::memcpy(hostAt(it->second, current), input + done, count);
#endif
        done += count;
    }
    if (size) ++revision_;
    // A loader patch is always reported as a code edit, executable or not: the engine may
    // fold reads of guest pages the guest cannot write, and a patch is the one way such a
    // page changes.
    noteCodeEdit(address, size);
}
bool Memory::pageIsReadOnly(uint32_t address) const {
    const uint64_t pageBegin = address & ~uint64_t(PageSize - 1);
    const uint64_t pageEnd = pageBegin + PageSize;
    auto it = storage_.upper_bound(uint32_t(pageBegin));
    if (it == storage_.begin()) return false;
    --it;
    // The page must sit inside one mapping: a page shared by a writable and a read-only
    // region is not read-only, and neither is one that is not mapped at all.
    if (it->second.region.base > pageBegin) return false;
    if (uint64_t(it->second.region.base) + it->second.region.size < pageEnd) return false;
    return (it->second.region.permissions & Write) == 0;
}
void Memory::patch32(uint32_t address, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                             uint8_t(value >> 24)};
    patch(address, bytes, 4);
}
uint32_t Memory::read32(uint32_t address) const {
    uint8_t b[4];
    read(address, b, 4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
void Memory::write32(uint32_t address, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                             uint8_t(value >> 24)};
    write(address, bytes, 4);
}
uint32_t Memory::fetch32(uint32_t address) const {
    if (address % 4) throw Error("CPU_ALIGNMENT", "ARM instruction is not word-aligned", address);
    uint8_t b[4];
    copyOut(address, b, 4, Execute);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
uint16_t Memory::fetch16(uint32_t address) const {
    if (address % 2)
        throw Error("CPU_ALIGNMENT", "Thumb instruction is not halfword-aligned", address);
    uint8_t b[2];
    copyOut(address, b, 2, Execute);
    return uint16_t(b[0]) | uint16_t(b[1]) << 8;
}
std::vector<Region> Memory::regions() const {
    std::vector<Region> result;
    for (const auto& pair : storage_) result.push_back(pair.second.region);
    return result;
}
}  // namespace podnative::runtime
