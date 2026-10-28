#include "compat_runtime/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime {
namespace {
constexpr std::uint64_t kGuestAddressSpaceSize = std::uint64_t{1} << 32;
constexpr std::size_t kGuestPageBytes = 4096;
constexpr std::size_t kDirtyPageBytes = kGuestPageBytes;

bool isPowerOfTwo(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }

std::uint64_t alignUp(std::uint64_t value, std::size_t alignment) {
    if (!isPowerOfTwo(alignment))
        throw std::invalid_argument("alignment must be a non-zero power of two");
    const auto mask = static_cast<std::uint64_t>(alignment - 1);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
        throw std::overflow_error("guest address alignment overflow");
    return (value + mask) & ~mask;
}
} // namespace

GuestAddressSpace::GuestAddressSpace(std::size_t memoryLimit) : memoryLimit_(memoryLimit) {
    if (memoryLimit_ == 0 || memoryLimit_ > kGuestAddressSpaceSize)
        throw std::invalid_argument("guest memory limit must fit the 32-bit address space");
}

// Caches are local to one address-space instance: copied or moved maps must
// never inherit a pointer into the original map.
GuestAddressSpace::GuestAddressSpace(const GuestAddressSpace &other)
    : regions_(other.regions_), memoryLimit_(other.memoryLimit_), mappedBytes_(other.mappedBytes_),
      nextDynamicAddress_(other.nextDynamicAddress_), heapReady_(other.heapReady_),
      heapBase_(other.heapBase_), heapSize_(other.heapSize_), heapCursor_(other.heapCursor_) {}

GuestAddressSpace &GuestAddressSpace::operator=(const GuestAddressSpace &other) {
    if (this == &other)
        return *this;
    // Copy first so an allocation failure leaves this address space intact.
    auto copiedRegions = other.regions_;
    lastRegion_.store(nullptr, std::memory_order_relaxed);
    previousRegion_.store(nullptr, std::memory_order_relaxed);
    regions_ = std::move(copiedRegions);
    memoryLimit_ = other.memoryLimit_;
    mappedBytes_ = other.mappedBytes_;
    nextDynamicAddress_ = other.nextDynamicAddress_;
    heapReady_ = other.heapReady_;
    heapBase_ = other.heapBase_;
    heapSize_ = other.heapSize_;
    heapCursor_ = other.heapCursor_;
    return *this;
}

GuestAddressSpace::GuestAddressSpace(GuestAddressSpace &&other) noexcept
    : regions_(std::move(other.regions_)), memoryLimit_(other.memoryLimit_),
      mappedBytes_(other.mappedBytes_), nextDynamicAddress_(other.nextDynamicAddress_),
      heapReady_(other.heapReady_), heapBase_(other.heapBase_), heapSize_(other.heapSize_),
      heapCursor_(other.heapCursor_) {
    other.lastRegion_.store(nullptr, std::memory_order_relaxed);
    other.previousRegion_.store(nullptr, std::memory_order_relaxed);
}

GuestAddressSpace &GuestAddressSpace::operator=(GuestAddressSpace &&other) noexcept {
    if (this == &other)
        return *this;
    lastRegion_.store(nullptr, std::memory_order_relaxed);
    previousRegion_.store(nullptr, std::memory_order_relaxed);
    other.lastRegion_.store(nullptr, std::memory_order_relaxed);
    other.previousRegion_.store(nullptr, std::memory_order_relaxed);
    regions_ = std::move(other.regions_);
    memoryLimit_ = other.memoryLimit_;
    mappedBytes_ = other.mappedBytes_;
    nextDynamicAddress_ = other.nextDynamicAddress_;
    heapReady_ = other.heapReady_;
    heapBase_ = other.heapBase_;
    heapSize_ = other.heapSize_;
    heapCursor_ = other.heapCursor_;
    return *this;
}

GuestAddressSpace::Region *GuestAddressSpace::findRegion(GuestAddress address, std::size_t size) {
    return const_cast<Region *>(static_cast<const GuestAddressSpace *>(this)->findRegion(address, size));
}

const GuestAddressSpace::Region *GuestAddressSpace::findRegion(GuestAddress address,
                                                               std::size_t size) const {
    const std::uint64_t accessStart = address;
    const std::uint64_t accessEnd = accessStart + size;
    if (accessEnd < accessStart)
        return nullptr;
    // Two slots retain the common stack/data interleave without swapping on
    // every callback. A miss still uses the authoritative ordered map.
    for (const auto *cached : {lastRegion_.load(std::memory_order_relaxed),
                               previousRegion_.load(std::memory_order_relaxed)}) {
        if (cached) {
            const std::uint64_t start = cached->view.base;
            const std::uint64_t end = start + cached->view.size;
            // At a zero-size boundary, use the map so a mapping starting
            // there wins over the preceding mapping, just as it did before.
            if (accessStart >= start && accessStart < end && accessEnd <= end)
                return cached;
        }
    }
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin())
        return nullptr;
    --it;
    const std::uint64_t start = it->second.view.base;
    const std::uint64_t end = start + it->second.view.size;
    if (accessStart < start || accessEnd > end)
        return nullptr;
    previousRegion_.store(lastRegion_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    lastRegion_.store(&it->second, std::memory_order_relaxed);
    return &it->second;
}

GuestAddress GuestAddressSpace::mapAt(GuestAddress base, std::size_t size,
                                      MemoryPermission permissions, std::string name) {
    if (base == 0 || size == 0)
        throw std::invalid_argument("guest regions must have a non-zero base and size");
    const std::uint64_t end = static_cast<std::uint64_t>(base) + size;
    if (end > kGuestAddressSpaceSize)
        throw std::overflow_error("guest region exceeds the 32-bit address space");
    if (size > memoryLimit_ - std::min(memoryLimit_, mappedBytes_))
        throw std::length_error("guest address space memory limit exceeded");

    auto next = regions_.lower_bound(base);
    if (next != regions_.end() && end > next->first)
        throw std::runtime_error("guest region overlaps an existing mapping");
    if (next != regions_.begin()) {
        const auto previous = std::prev(next);
        const std::uint64_t previousEnd = static_cast<std::uint64_t>(previous->second.view.base) +
                                          previous->second.view.size;
        if (base < previousEnd)
            throw std::runtime_error("guest region overlaps an existing mapping");
    }

    Region region;
    region.view = GuestRegionView{base, size, permissions, std::move(name)};
    region.bytes.resize(size, 0);
    regions_.emplace(base, std::move(region));
    mappedBytes_ += size;
    return base;
}

GuestAddress GuestAddressSpace::mapAny(std::size_t size, MemoryPermission permissions,
                                       std::string name, std::size_t alignment) {
    if (size == 0)
        throw std::invalid_argument("guest mapping size must be non-zero");
    // The Dynarmic backend services guest accesses live through these
    // callbacks. Keep every dynamically allocated region page-shaped even when
    // a shim requested a four-byte cell or a short C string; the extra zeroed
    // tail is still inside the same bounded mapping and prevents an
    // engine-level permission or rounding check from turning an otherwise
    // valid guest allocation into a backend fault.
    const auto mappedSize = static_cast<std::size_t>(alignUp(size, kGuestPageBytes));
    std::uint64_t candidate = alignUp(nextDynamicAddress_, alignment);
    while (candidate + mappedSize <= 0xF0000000ULL) {
        const auto address = static_cast<GuestAddress>(candidate);
        auto next = regions_.lower_bound(address);
        bool available = next == regions_.end() || candidate + mappedSize <= next->first;
        if (available && next != regions_.begin()) {
            const auto previous = std::prev(next);
            const std::uint64_t previousEnd = static_cast<std::uint64_t>(previous->second.view.base) +
                                              previous->second.view.size;
            available = candidate >= previousEnd;
        }
        if (available) {
            const auto result = mapAt(address, mappedSize, permissions, std::move(name));
            nextDynamicAddress_ = static_cast<GuestAddress>(alignUp(candidate + mappedSize, alignment));
            return result;
        }
        if (next != regions_.end())
            candidate = alignUp(static_cast<std::uint64_t>(next->second.view.base) +
                                    next->second.view.size,
                                alignment);
        else
            break;
    }
    throw std::runtime_error("no free guest address range is available");
}

void GuestAddressSpace::unmap(GuestAddress base) {
    auto found = regions_.find(base);
    if (found == regions_.end())
        throw std::runtime_error("guest region is not mapped");
    if (heapReady_ && base == heapBase_)
        heapReady_ = false;
    mappedBytes_ -= found->second.view.size;
    if (lastRegion_.load(std::memory_order_relaxed) == &found->second)
        lastRegion_.store(nullptr, std::memory_order_relaxed);
    if (previousRegion_.load(std::memory_order_relaxed) == &found->second)
        previousRegion_.store(nullptr, std::memory_order_relaxed);
    regions_.erase(found);
}

void GuestAddressSpace::setPermissions(GuestAddress base, MemoryPermission permissions) {
    auto found = regions_.find(base);
    if (found == regions_.end())
        throw std::runtime_error("guest region is not mapped");
    found->second.view.permissions = permissions;
}

void GuestAddressSpace::configureHeap(GuestAddress base, std::size_t size) {
    if (heapReady_)
        throw std::runtime_error("guest heap is already configured");
    mapAt(base, size, MemoryPermission::Read | MemoryPermission::Write, "guest-heap");
    heapReady_ = true;
    heapBase_ = base;
    heapSize_ = size;
    heapCursor_ = 0;
}

GuestAddress GuestAddressSpace::allocateHeap(std::size_t size, std::size_t alignment) {
    if (!heapReady_)
        throw std::runtime_error("guest heap is not configured");
    if (size == 0)
        throw std::invalid_argument("guest heap allocations must be non-empty");
    const std::uint64_t aligned = alignUp(heapCursor_, alignment);
    if (aligned > heapSize_ || size > heapSize_ - static_cast<std::size_t>(aligned))
        throw std::bad_alloc();
    const std::uint64_t address = static_cast<std::uint64_t>(heapBase_) + aligned;
    if (address + size > kGuestAddressSpaceSize)
        throw std::overflow_error("guest heap address overflow");
    heapCursor_ = static_cast<std::size_t>(aligned) + size;
    std::memset(static_cast<std::uint8_t *>(guestToHost(static_cast<GuestAddress>(address), size,
                                                        MemoryPermission::Write)),
                0, size);
    return static_cast<GuestAddress>(address);
}

bool GuestAddressSpace::readAcross(GuestAddress address, void *destination, std::size_t size,
                                   MemoryPermission required) const {
    auto *cursor = static_cast<std::uint8_t *>(destination);
    std::size_t remaining = size;
    GuestAddress current = address;
    while (remaining > 0) {
        const auto *region = findRegion(current, 1);
        if (!region || !hasPermission(region->view.permissions, required))
            return false;
        const auto offset = static_cast<std::size_t>(current - region->view.base);
        const auto available = region->bytes.size() - offset;
        const auto chunk = std::min(available, remaining);
        std::memcpy(cursor, region->bytes.data() + offset, chunk);
        cursor += chunk;
        remaining -= chunk;
        current = static_cast<GuestAddress>(current + chunk);
    }
    return true;
}

bool GuestAddressSpace::writeAcross(GuestAddress address, const void *source, std::size_t size,
                                    bool markHostWrite) {
    const auto *cursor = static_cast<const std::uint8_t *>(source);
    std::size_t remaining = size;
    GuestAddress current = address;
    while (remaining > 0) {
        auto *region = findRegion(current, 1);
        if (!region || !hasPermission(region->view.permissions, MemoryPermission::Write))
            return false;
        const auto offset = static_cast<std::size_t>(current - region->view.base);
        const auto available = region->bytes.size() - offset;
        const auto chunk = std::min(available, remaining);
        std::memcpy(region->bytes.data() + offset, cursor, chunk);
        if (markHostWrite)
            markRegionDirty(*region, current, chunk);
        cursor += chunk;
        remaining -= chunk;
        current = static_cast<GuestAddress>(current + chunk);
    }
    return true;
}

bool GuestAddressSpace::read(GuestAddress address, void *destination, std::size_t size) const {
    if ((!destination && size != 0) || size == 0)
        return size == 0;
    const auto *region = findRegion(address, size);
    if (!region)
        return readAcross(address, destination, size, MemoryPermission::Read);
    if (!hasPermission(region->view.permissions, MemoryPermission::Read))
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(destination, region->bytes.data() + offset, size);
    return true;
}

bool GuestAddressSpace::write(GuestAddress address, const void *source, std::size_t size) {
    if ((!source && size != 0) || size == 0)
        return size == 0;
    auto *region = findRegion(address, size);
    if (!region)
        return writeAcross(address, source, size, true);
    if (!hasPermission(region->view.permissions, MemoryPermission::Write))
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(region->bytes.data() + offset, source, size);
    markRegionDirty(*region, address, size);
    return true;
}

bool GuestAddressSpace::writeFromGuest(GuestAddress address, const void *source, std::size_t size) {
    if ((!source && size != 0) || size == 0)
        return size == 0;
    auto *region = findRegion(address, size);
    if (!region)
        return writeAcross(address, source, size, false);
    if (!hasPermission(region->view.permissions, MemoryPermission::Write))
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(region->bytes.data() + offset, source, size);
    return true;
}

void GuestAddressSpace::markDirty(GuestAddress address, std::size_t size) {
    if (size == 0)
        return;
    auto *region = findRegion(address, size);
    if (region)
        markRegionDirty(*region, address, size);
}

// The caller already checked this region and its bounds. Reuse it instead of
// looking it up in the ordered map a second time on every host-to-guest write.
void GuestAddressSpace::markRegionDirty(Region &region, GuestAddress address, std::size_t size) {
    if (size == 0)
        return;
    const auto offset = static_cast<std::size_t>(address - region.view.base);
    if (offset >= region.bytes.size() || size > region.bytes.size() - offset)
        return;
    const auto firstPage = offset / kDirtyPageBytes;
    const auto lastPage = (offset + size - 1) / kDirtyPageBytes;
    if (region.dirtyPages.size() < lastPage + 1)
        region.dirtyPages.resize(lastPage + 1, 0);
    for (auto page = firstPage; page <= lastPage; ++page)
        region.dirtyPages[page / 64] |= std::uint64_t{1} << (page % 64);
    region.dirty = true;
}

void GuestAddressSpace::clearDirty() {
    for (auto &entry : regions_) {
        std::fill(entry.second.dirtyPages.begin(), entry.second.dirtyPages.end(), 0);
        entry.second.dirty = false;
    }
}

bool GuestAddressSpace::drainDirtyPages(
    const std::function<bool(GuestAddress, const std::uint8_t *, std::size_t)> &visit) {
    for (auto &entry : regions_) {
        auto &region = entry.second;
        if (!region.dirty)
            continue;
        std::vector<std::uint64_t> pages = region.dirtyPages;
        for (std::size_t page = 0; page < pages.size(); ++page) {
            if ((pages[page / 64] & (std::uint64_t{1} << (page % 64))) == 0)
                continue;
            const auto offset = page * kDirtyPageBytes;
            if (offset >= region.bytes.size())
                continue;
            const auto bytes = std::min(kDirtyPageBytes, region.bytes.size() - offset);
            if (!visit(region.view.base + static_cast<GuestAddress>(offset),
                       region.bytes.data() + offset, bytes))
                return false;
        }
        std::fill(region.dirtyPages.begin(), region.dirtyPages.end(), 0);
        region.dirty = false;
    }
    return true;
}

bool GuestAddressSpace::initialize(GuestAddress address, const void *source, std::size_t size) {
    if ((!source && size != 0) || size == 0)
        return size == 0;
    auto *region = findRegion(address, size);
    if (!region)
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(region->bytes.data() + offset, source, size);
    markRegionDirty(*region, address, size);
    return true;
}

void *GuestAddressSpace::guestToHost(GuestAddress address, std::size_t size,
                                     MemoryPermission required) {
    auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, required))
        return nullptr;
    // A writable host view may be written in place, so the pages are queued for
    // the engine even though the shim might only read through this pointer.
    if (hasPermission(required, MemoryPermission::Write))
        markRegionDirty(*region, address, size);
    return region->bytes.data() + static_cast<std::size_t>(address - region->view.base);
}

const void *GuestAddressSpace::guestToHost(GuestAddress address, std::size_t size,
                                           MemoryPermission required) const {
    const auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, required))
        return nullptr;
    return region->bytes.data() + static_cast<std::size_t>(address - region->view.base);
}

std::optional<GuestAddress> GuestAddressSpace::hostToGuest(const void *pointer) const {
    if (!pointer)
        return std::nullopt;
    const auto host = reinterpret_cast<std::uintptr_t>(pointer);
    for (const auto &entry : regions_) {
        const auto begin = reinterpret_cast<std::uintptr_t>(entry.second.bytes.data());
        const auto end = begin + entry.second.bytes.size();
        if (host >= begin && host < end) {
            const auto guest = static_cast<std::uint64_t>(entry.second.view.base) + (host - begin);
            if (guest < kGuestAddressSpaceSize)
                return static_cast<GuestAddress>(guest);
        }
    }
    return std::nullopt;
}

bool GuestAddressSpace::contains(GuestAddress address, std::size_t size,
                                 MemoryPermission required) const {
    const auto *region = findRegion(address, size);
    return region && hasPermission(region->view.permissions, required);
}

std::vector<GuestRegionView> GuestAddressSpace::regions() const {
    std::vector<GuestRegionView> result;
    result.reserve(regions_.size());
    for (const auto &entry : regions_)
        result.push_back(entry.second.view);
    return result;
}

GuestMemoryCallbacks GuestAddressSpace::callbacks() {
    return GuestMemoryCallbacks{
        [this](GuestAddress address, void *destination, std::size_t size) {
            return read(address, destination, size);
        },
        [this](GuestAddress address, const void *source, std::size_t size) {
            return write(address, source, size);
        },
        [this](GuestAddress address, const void *source, std::size_t size) {
            return writeFromGuest(address, source, size);
        },
        [this]() { return regions(); },
        [this]() { clearDirty(); },
        [this](const std::function<bool(GuestAddress, const std::uint8_t *, std::size_t)> &visit) {
            return drainDirtyPages(visit);
        },
        {},
    };
}

} // namespace radek::compat_runtime
