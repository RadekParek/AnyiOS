#include "exports.h"

#include "error.h"

namespace podnative::runtime {
void ExportGraph::text(const std::string& name) {
    if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos ||
        name.size() > 8 * 1024 * 1024 - nameBytes)
        throw Error("EXPORT_LIMIT", "invalid name or export graph name budget exceeded");
    nameBytes += name.size();
}
size_t ExportGraph::addImage(const std::string& name) {
    if (images.size() >= 256 || names.count(name))
        throw Error("EXPORT_IMAGE", "too many or duplicate images");
    text(name);
    size_t id = images.size();
    images.push_back({});
    names.emplace(name, id);
    return id;
}
void ExportGraph::setMainExecutable(size_t image) {
    if (image >= images.size()) throw Error("EXPORT_IMAGE", "invalid main executable");
    main = image;
}
void ExportGraph::alias(size_t image, const std::string& name) {
    if (image >= images.size() || names.count(name) || names.size() >= 1024)
        throw Error("EXPORT_ALIAS", "invalid or duplicate alias");
    text(name);
    names.emplace(name, image);
}
void ExportGraph::dependencies(size_t image, std::vector<std::string> deps) {
    if (image >= images.size() || deps.size() > 256 || !images[image].dependencies.empty())
        throw Error("EXPORT_DEPENDENCIES", "invalid dependency list");
    for (const auto& name : deps) text(name);
    images[image].dependencies = std::move(deps);
}
void ExportGraph::define(size_t image, const std::string& name, ExportDefinition definition) {
    if (image >= images.size() || definitions >= 100000)
        throw Error("EXPORT_LIMIT", "invalid image or too many definitions");
    auto& symbols = images[image].symbols;
    if (symbols.count(name)) throw Error("EXPORT_DUPLICATE", "duplicate definition in one image");
    text(name);
    symbols.emplace(name, definition);
    definitions++;
}
bool ExportGraph::strong(size_t image, const std::string& name) const {
    if (image >= images.size()) throw Error("EXPORT_IMAGE", "invalid image");
    auto it = images[image].symbols.find(name);
    return it != images[image].symbols.end() && !it->second.weak;
}
bool ExportGraph::hasStrong(const std::string& name) const {
    for (const auto& image : images) {
        auto it = image.symbols.find(name);
        if (it != image.symbols.end() && !it->second.weak) return true;
    }
    return false;
}
std::optional<uint32_t> ExportGraph::resolve(size_t requester, int ordinal,
                                             const std::string& name) const {
    if (requester >= images.size() || ordinal < -3)
        throw Error("EXPORT_ORDINAL", "invalid requester or special ordinal");
    auto lookup = [&](size_t image) -> std::optional<uint32_t> {
        auto it = images[image].symbols.find(name);
        return it == images[image].symbols.end() ? std::optional<uint32_t>{} : it->second.address;
    };
    if (ordinal == 0) return lookup(requester);
    if (ordinal == -1) return lookup(main);
    if (ordinal > 0) {
        const auto& deps = images[requester].dependencies;
        if (size_t(ordinal) > deps.size())
            throw Error("EXPORT_ORDINAL", "missing dependency ordinal");
        auto it = names.find(deps[size_t(ordinal) - 1]);
        return it == names.end() ? std::optional<uint32_t>{} : lookup(it->second);
    }
    // Flat lookup is first definition. Weak coalescing instead prefers the first
    // strong definition, then the first weak one, independent of host ASLR.
    std::optional<uint32_t> weak;
    for (const auto& image : images) {
        auto it = image.symbols.find(name);
        if (it == image.symbols.end()) continue;
        if (ordinal == -2 || !it->second.weak) return it->second.address;
        if (!weak) weak = it->second.address;
    }
    return weak;
}
}  // namespace podnative::runtime
