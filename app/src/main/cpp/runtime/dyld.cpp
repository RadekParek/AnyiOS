#include "dyld.h"

#include <limits>
#include <set>
#include <sstream>

#include "error.h"
namespace podnative::runtime {
namespace {
struct Stream {
    const uint8_t* b;
    size_t n, p = 0;
    uint8_t byte() {
        if (p == n) throw Error("DYLD_TRUNCATED", "incomplete opcode operand");
        return b[p++];
    }
    uint64_t leb(bool signedValue = false) {
        uint64_t value = 0;
        uint8_t part = 0;
        unsigned shift = 0;
        for (unsigned i = 0; i < 10; i++) {
            part = byte();
            if (i == 9 &&
                (signedValue ? ((part & 0x7f) != 0 && (part & 0x7f) != 0x7f) : (part & 0x7e) != 0))
                throw Error("DYLD_LEB", "integer overflow");
            value |= uint64_t(part & 0x7f) << shift;
            shift += 7;
            if (!(part & 0x80)) {
                if (signedValue && shift < 64 && (part & 0x40)) value |= ~uint64_t(0) << shift;
                return value;
            }
        }
        throw Error("DYLD_LEB", "unterminated integer");
    }
    std::string string() {
        std::string s;
        for (;;) {
            auto c = byte();
            if (!c) return s;
            if (s.size() >= 4096) throw Error("DYLD_SYMBOL", "symbol name exceeds limit");
            s += char(c);
        }
    }
};
}  // namespace
DyldPlan decodeDyld(const uint8_t* bytes, size_t size, const std::vector<DyldSegment>& segments,
                    bool rebasing, bool lazy, bool weakStream) {
    if (size > 16 * 1024 * 1024 || (!bytes && size))
        throw Error("DYLD_LIMIT", "stream exceeds budget");
    Stream s{bytes, size};
    DyldPlan plan;
    plan.needsCoalescing = weakStream;
    size_t segment = segments.size();
    uint64_t offset = 0;
    int type = 1, ordinal = weakStream ? -3 : 0;
    std::string symbol;
    bool weak = false;
    int64_t addend = 0;
    size_t operations = 0, nameBytes = 0;
    bool done = false;
    // Strides are pure cursor arithmetic. A stream may legitimately advance before it
    // selects a segment (the cursor is simply not anchored yet) and may run past the end
    // of the last fixup, because only emit() dereferences the cursor. Failing here rejected
    // valid streams from this binary; the precise complaint now comes from emit(), and the
    // only remaining hard failure is real 64-bit arithmetic overflow.
    auto advance = [&](uint64_t amount) {
        if (amount > UINT64_MAX - offset) throw Error("DYLD_ADDRESS", "cursor arithmetic overflow");
        offset += amount;
    };
    auto emit = [&] {
        if (type != 1)
            throw Error("DYLD_TYPE_UNSUPPORTED", "only ARM32 pointer fixups are implemented");
        if (segment >= segments.size())
            throw Error("DYLD_SEGMENT", "fixup emitted before a segment was selected");
        if (offset > segments[segment].size || segments[segment].size - offset < 4 ||
            uint64_t(segments[segment].address) + offset + 4 > 0x100000000ULL ||
            !(segments[segment].permissions & Write) || offset % 4)
            throw Error("DYLD_TARGET", "invalid, unaligned or non-writable fixup target");
        if (!rebasing && symbol.empty()) throw Error("DYLD_SYMBOL", "binding has no symbol");
        if (plan.fixups.size() >= 100000) throw Error("DYLD_LIMIT", "too many fixups");
        if (symbol.size() > 8 * 1024 * 1024 - nameBytes)
            throw Error("DYLD_LIMIT", "symbol allocation budget exceeded");
        nameBytes += symbol.size();
        plan.fixups.push_back({uint32_t(segments[segment].address + offset), rebasing, weak, lazy,
                               ordinal, addend, symbol});
    };
    while (s.p < s.n) {
        if (++operations > 1000000) throw Error("DYLD_LIMIT", "opcode work budget exceeded");
        uint8_t b = s.byte(), op = b & 0xf0, imm = b & 15;
        if (op == 0) {
            done = true;
            if (imm) throw Error("DYLD_OPCODE", "invalid DONE immediate");
            if (!lazy) {
                while (s.p < s.n)
                    if (s.byte()) throw Error("DYLD_TRAILING", "nonzero bytes after DONE");
                return plan;
            }
            segment = segments.size();
            offset = 0;
            type = 1;
            ordinal = 0;
            symbol.clear();
            weak = false;
            addend = 0;
            continue;
        }
        done = false;
        if (rebasing) {
            switch (op) {
                case 0x10:
                    type = imm;
                    break;
                case 0x20:
                    segment = imm;
                    offset = 0;
                    advance(s.leb());
                    break;
                case 0x30:
                    advance(s.leb());
                    break;
                case 0x40:
                    advance(uint64_t(imm) * 4);
                    break;
                case 0x50:
                case 0x60: {
                    uint64_t count = op == 0x50 ? imm : s.leb();
                    if (count > 100000 - plan.fixups.size())
                        throw Error("DYLD_LIMIT", "rebase repeat count");
                    for (uint64_t i = 0; i < count; i++) {
                        emit();
                        advance(4);
                    }
                    break;
                }
                case 0x70: {
                    auto skip = s.leb();
                    emit();
                    advance(4);
                    advance(skip);
                    break;
                }
                case 0x80: {
                    auto count = s.leb(), skip = s.leb();
                    if (count > 100000 - plan.fixups.size())
                        throw Error("DYLD_LIMIT", "rebase repeat count");
                    for (uint64_t i = 0; i < count; i++) {
                        emit();
                        advance(4);
                        advance(skip);
                    }
                    break;
                }
                default:
                    throw Error("DYLD_OPCODE", "unsupported rebase opcode");
            }
        } else {
            if (weakStream && (op == 0x10 || op == 0x20 || op == 0x30))
                throw Error("DYLD_WEAK_OPCODE", "weak stream cannot change library ordinal");
            switch (op) {
                case 0x10:
                    ordinal = imm;
                    break;
                case 0x20: {
                    auto n = s.leb();
                    if (n > INT32_MAX) throw Error("DYLD_ORDINAL", "ordinal exceeds range");
                    ordinal = int(n);
                    break;
                }
                case 0x30:
                    ordinal = imm ? int(imm) - 16 : 0;
                    if (ordinal < -3) throw Error("DYLD_ORDINAL", "unknown special ordinal");
                    break;
                case 0x40:
                    if (imm & ~9) throw Error("DYLD_SYMBOL", "unsupported symbol flags");
                    weak = imm & 1;
                    symbol = s.string();
                    if (weakStream && (imm & 8)) {
                        if (plan.strongDefinitions.size() >= 100000 ||
                            symbol.size() > 8 * 1024 * 1024 - nameBytes)
                            throw Error("DYLD_LIMIT", "weak declaration budget exceeded");
                        nameBytes += symbol.size();
                        plan.strongDefinitions.push_back(symbol);
                    }
                    break;
                case 0x50:
                    type = imm;
                    break;
                case 0x60:
                    addend = int64_t(s.leb(true));
                    break;
                case 0x70:
                    segment = imm;
                    offset = 0;
                    advance(s.leb());
                    break;
                case 0x80:
                    advance(s.leb());
                    break;
                case 0x90:
                    emit();
                    advance(4);
                    break;
                case 0xa0: {
                    auto skip = s.leb();
                    emit();
                    advance(4);
                    advance(skip);
                    break;
                }
                case 0xb0:
                    emit();
                    advance(uint64_t(imm) * 4 + 4);
                    break;
                case 0xc0: {
                    auto count = s.leb(), skip = s.leb();
                    if (count > 100000 - plan.fixups.size())
                        throw Error("DYLD_LIMIT", "binding repeat count");
                    for (uint64_t i = 0; i < count; i++) {
                        emit();
                        advance(4);
                        advance(skip);
                    }
                    break;
                }
                default:
                    throw Error("DYLD_OPCODE_UNSUPPORTED", "threaded/unknown binding opcode");
            }
        }
    }
    if (size && !done) throw Error("DYLD_TRUNCATED", "stream is missing DONE");
    return plan;
}
void DyldPlan::apply(
    Memory& memory, uint32_t slide,
    const std::function<std::optional<uint32_t>(int, const std::string&, bool)>& resolve) const {
    if (needsCoalescing)
        throw Error("DYLD_COALESCING_UNIMPLEMENTED",
                    "weak-definition coalescing requires a dependency/export graph");
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::set<uint32_t> used;
    for (const auto& f : fixups) {
        if (!used.insert(f.address).second || !memory.permits(f.address, 4, Write))
            throw Error("DYLD_TARGET", "duplicate or inaccessible target", f.address);
        int64_t value;
        if (f.rebase)
            value = int64_t(memory.read32(f.address)) + slide;
        else {
            auto found = resolve ? resolve(f.ordinal, f.symbol, f.weak) : std::optional<uint32_t>{};
            if (!found && !f.weak)
                throw Error("DYLD_UNRESOLVED", "required symbol has no resolver result");
            if (found && f.addend > INT64_MAX - int64_t(*found))
                throw Error("DYLD_VALUE", "signed addend overflow");
            value = found ? int64_t(*found) + f.addend : 0;
        }
        if (value < 0 || value > UINT32_MAX) throw Error("DYLD_VALUE", "pointer value overflow");
        writes.emplace_back(f.address, uint32_t(value));
    }
    for (auto [address, value] : writes) memory.write32(address, value);
}
void DyldPlan::applyWithGraph(Memory& memory, uint32_t slide, const ExportGraph& graph,
                              size_t requester,
                              const std::function<uint32_t(const std::string&)>& trap) const {
    for (const auto& name : strongDefinitions)
        if (!graph.hasStrong(name))
            throw Error("DYLD_WEAK_DEFINITION",
                        "strong declaration has no registered strong definition");
    DyldPlan normalized;
    std::map<uint32_t, size_t> targets;
    for (const auto& fixup : fixups) {
        auto old = targets.find(fixup.address);
        if (old == targets.end()) {
            targets.emplace(fixup.address, normalized.fixups.size());
            normalized.fixups.push_back(fixup);
            continue;
        }
        auto& previous = normalized.fixups[old->second];
        // Two streams that name the same slot with the same target are not a conflict:
        // a link editor may list a lazy pointer in both the lazy and the plain binding
        // stream. Writing the same value twice is idempotent, so merge and keep the
        // eager one (a lazy marker only defers work that we would otherwise repeat).
        const bool sameTarget =
            previous.rebase == fixup.rebase && previous.symbol == fixup.symbol &&
            previous.addend == fixup.addend &&
            (previous.ordinal == fixup.ordinal || previous.ordinal == -2 || fixup.ordinal == -2 ||
             previous.ordinal == -3 || fixup.ordinal == -3);
        if (sameTarget) {
            const bool coalescing = needsCoalescing && fixup.ordinal == -3;
            if (coalescing)
                previous = fixup;
            else
                previous.lazy = previous.lazy && fixup.lazy;
            continue;
        }
        throw Error("DYLD_TARGET", "conflicting duplicate fixup", fixup.address);
    }
    // Coalescing already ran above. apply() must not reject the normalized plan
    // merely because its original stream was the weak-binding stream.
    normalized.apply(memory, slide, [&](int ordinal, const std::string& name, bool weak) {
        if (auto found = graph.resolve(requester, ordinal, name)) return found;
        if (trap && !weak) return std::optional<uint32_t>(trap(name));
        return std::optional<uint32_t>{};
    });
}
std::string DyldPlan::summary() const {
    size_t rebases = 0, bindings = 0;
    std::ostringstream out;
    for (const auto& f : fixups) f.rebase ? ++rebases : ++bindings;
    out << "DYLD_PLAN: " << rebases << " pointer rebases, " << bindings
        << " pointer bindings. Decoded, not resolved or applied.";
    size_t shown = 0;
    for (const auto& f : fixups) {
        if (f.rebase || shown++ >= 120) continue;
        out << "\n" << (f.weak ? "weak " : "required ") << "ordinal=" << f.ordinal << " ";
        for (unsigned char c : f.symbol) out << (c >= 32 && c < 127 ? char(c) : '?');
        out << (f.lazy ? " (lazy)" : "");
    }
    if (needsCoalescing)
        out << "\nWeak-definition coalescing requires the registered export graph; unresolved definitions still block application.";
    if (bindings > 120) out << "\nDisplay limited to 120 bindings.";
    return out.str();
}
}  // namespace podnative::runtime
