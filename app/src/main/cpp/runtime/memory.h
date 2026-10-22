#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace podnative::runtime {
enum Permission : uint8_t { Read = 1, Write = 2, Execute = 4 };
struct Region {
    uint32_t base;
    uint32_t size;
    uint8_t permissions;
    std::string label;
};
// A closed-open guest address range.
struct Range {
    uint32_t begin;
    uint32_t end;
};
// Guest execute permission is metadata only for host-side accesses: these are never
// executable host mappings. Single CPU-owner thread; graphics receives copied/validated
// bytes, never backing pointers.
//
// Two backing stores, one set of semantics:
//
//  * region vectors (the default) — every mapping owns a `std::vector<uint8_t>`.
//  * a flat direct-access arena — one host reservation covering the whole 32-bit guest
//    address space, where `host == arena + guest`, requested by the CPU with
//    `enableDirectAccess()`. It lets the JIT compile guest loads and stores into direct
//    host memory accesses instead of a callback per access, and it commits pages lazily,
//    so a mapped-but-untouched guest region costs no host memory or zeroing.
//
// Permission checking, the mapping budget, region bookkeeping and every error code are
// identical in both modes: the arena changes how bytes are reached, never what the guest
// is allowed to do.
class Memory final {
   public:
    static constexpr uint32_t PageSize = 4096;
    // One page-table entry per 4 KiB guest page across the 32-bit address space. This is
    // the layout the pinned CPU engine's `absolute_offset_page_table` mode expects.
    static constexpr size_t PageTableBits = 12;
    static constexpr size_t PageTableEntries = size_t(1) << (32 - PageTableBits);
    using PageTable = std::array<uint8_t*, PageTableEntries>;
    explicit Memory(size_t budget = 256 * 1024 * 1024) : budget_(budget) {}
    ~Memory();
    // The arena is a host mapping owned by this instance; copying it would hand two owners
    // the same reservation.
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    void map(Region region, const uint8_t* initial = nullptr, size_t initialSize = 0);
    void read(uint32_t address, void* destination, size_t size) const;
    void write(uint32_t address, const void* source, size_t size);
    // Loader-only mutation that does not require guest write permission: used to rewrite
    // an image's symbol stubs inside its executable segment before the guest starts. The
    // range must still be mapped and readable, so a bad pointer faults instead of
    // corrupting host memory.
    void patch(uint32_t address, const void* source, size_t size);
    void patch32(uint32_t address, uint32_t value);
    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    uint32_t fetch32(uint32_t address) const;
    uint16_t fetch16(uint32_t address) const;
    bool permits(uint32_t address, size_t size, uint8_t permission) const;
    std::vector<Region> regions() const;
    size_t allocated() const { return allocated_; }
    // Any guest-visible mutation. Kept for callers that reason about data changes.
    uint64_t revision() const { return revision_; }
    // Mutations of bytes inside an execute-permitted mapping. This is the only revision a
    // dynamic recompiler has to care about: translated code goes stale when instructions
    // change, not when the guest writes to its own heap.
    uint64_t codeRevision() const { return codeRevision_; }
    // Execute-permitted ranges mutated since the previous call, merged and sorted. A
    // recompiler invalidates exactly these instead of discarding everything it translated.
    std::vector<Range> takeCodeEdits();

    // Asks for the flat direct-access arena and migrates already-mapped regions into it.
    // Returns false without changing anything when the host cannot reserve the address
    // space, so the caller keeps the region-vector path (today's behavior).
    bool enableDirectAccess();
    bool directAccess() const { return arena_ != nullptr; }
    // Base of the flat arena, or null. Only the CPU engine may use it, and only while this
    // instance is alive: `host == directBase() + guest` for every mapped guest address.
    uint8_t* directBase() const { return arena_; }
    // True when guest page protections map one-to-one onto host page protections. Only then
    // may the engine use hardware fault recovery for a store into a read-only guest page;
    // otherwise such stores must stay on the checked callback path.
    bool directProtectionIsExact() const { return exactProtection_; }
    // Page table for the engine's fast path: the arena base for every guest page that is
    // mapped writable and not executable, null everywhere else. A null entry means "use the
    // checked callback", which is what keeps unmapped pages faulting, read-only pages
    // refusing writes, and guest stores into executable pages invalidating translated code.
    PageTable* pageTable() { return pageTable_.get(); }
    // True when some mapping is both writable and executable. Guest stores into such a page
    // have to be seen by the runtime so translated code can be invalidated, so the CPU
    // engine keeps hardware fast paths off while one exists.
    bool hasWritableExecutable() const { return writableExecutable_; }
    // True when the whole 4 KiB guest page holding `address` lies inside one mapping the
    // guest cannot write. The engine uses it to fold constant reads (ARM literal pools).
    bool pageIsReadOnly(uint32_t address) const;

   private:
    struct Storage {
        Region region;
        // Empty once the region lives in the arena.
        std::vector<uint8_t> bytes;
    };
    std::map<uint32_t, Storage> storage_;
    size_t budget_;
    size_t allocated_ = 0;
    uint64_t revision_ = 0;
    uint64_t codeRevision_ = 0;
    std::vector<Range> codeEdits_;
    uint8_t* arena_ = nullptr;
    size_t arenaSize_ = 0;
    bool exactProtection_ = false;
    std::unique_ptr<PageTable> pageTable_;
    bool writableExecutable_ = false;
    const Storage& at(uint32_t address, uint8_t permission) const;
    void validate(uint32_t address, size_t size, uint8_t permission) const;
    void copyOut(uint32_t address, void* destination, size_t size, uint8_t permission) const;
    // Host pointer for a validated address inside one region.
    uint8_t* hostAt(const Storage& storage, uint32_t address) const;
    void noteCodeEdit(uint32_t address, size_t size);
    void publishPage(const Storage& storage);
    void applyArenaProtection(uint32_t base, uint32_t size);
};
}  // namespace podnative::runtime
