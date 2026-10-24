#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cpu.h"
#include "dyld.h"
#include "exports.h"
#include "memory.h"
namespace podnative::runtime {
// One entry of a classic S_SYMBOL_STUBS section: the stub's address, the stub size from
// reserved2, and the symbol the indirect table says it forwards to. Stub dispatch is not
// dyld lazy binding: before the guest starts we rewrite each stub to branch straight at
// the resolved implementation (or at a trap gate that names the missing symbol), so no
// uninitialised lazy pointer is ever executed.
struct LegacyStub {
    uint32_t address = 0, stride = 0;
    std::string symbol;
    int ordinal = 0;
    bool weak = false;
    // A four-byte ARM "compact" stub is a single `ldr pc, [pc, #imm]` that reads its target
    // from a lazy/non-lazy pointer slot. It cannot hold a branch sequence, so it is
    // dispatched by binding the slot it names instead of being rewritten in place.
    bool compact = false;
    uint32_t slot = 0;
};
// A function symbol the image defines, kept only so a stop report can name the guest
// function that was executing. This is diagnosis, not a symbol table used by dyld.
struct FunctionSymbol {
    uint32_t address = 0;
    std::string name;
};
struct LoadedImage {
    std::unique_ptr<Memory> memory;
    std::array<uint32_t, 16> registers{};
    uint32_t cpsr = 0x10;  // ARM user mode; Thumb flag derives from the validated entry.
    std::vector<std::string> dependencies;
    bool requiresDyld = false;
    DyldPlan dyld;
    size_t legacyStubs = 0;
    std::vector<LegacyStub> stubs;
    // S_MOD_INIT_FUNC_POINTERS sections (`__mod_init_func` on iOS). Their function
    // pointers are read only after dyld rebases and binds the mapped image.
    std::vector<std::pair<uint32_t, uint32_t>> initializerSections;
    // Address ranges of the image's `__objc_classlist` sections. The pointer slots are
    // rebased by dyld; class addresses must not be read until those fixups have run.
    std::vector<std::pair<uint32_t, uint32_t>> objcClassListSections;
    std::vector<uint32_t> objcClassAddresses() const;
    std::vector<std::pair<std::string, ExportDefinition>> exports;
    uint32_t cpuSubtype = 9;  // CPU_SUBTYPE_ARM_V7 unless the header says otherwise.
    IsaArch arch = IsaArch::ArmV7;
    // Defined function symbols, sorted by address. Empty for stripped titles.
    std::vector<FunctionSymbol> symbols;
    // Nearest preceding function for a guest PC, or an empty string when the image is
    // stripped or the PC is further than 1 MiB past the last symbol.
    std::string nearestSymbol(uint32_t address) const;
    std::string summary() const;
};
// Fixed-address mapping at the un-slid vmaddrs. PIE images are accepted because every
// rebase is applied against the export graph before execution; a slide is simply zero.
// This does not execute code or resolve imports.
LoadedImage loadMachO(const uint8_t* data, size_t size, size_t budget = 256 * 1024 * 1024);
// Rewrites every recorded symbol stub. Unresolved required symbols are routed to `trap`
// so the guest stops on the exact missing API; unresolved weak symbols become null, which
// is what dyld binds and is checked by the caller in normal code.
void bindLegacyStubs(LoadedImage& image, const ExportGraph& graph, size_t requester,
                     const std::function<uint32_t(const std::string&)>& trap);
}  // namespace podnative::runtime
